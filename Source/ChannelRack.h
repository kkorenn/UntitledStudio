#pragma once

#include "Studio.h"
#include "Widgets.h"
#include "ParamKnob.h"

// FL's Channel Rack: one row per channel with mute LED, pan, volume, name and step buttons.
class ChannelRack : public juce::Component,
                    public juce::DragAndDropTarget,
                    public juce::FileDragAndDropTarget,
                    private juce::Timer
{
public:
    //==========================================================================
    // FL: drop a sound on a channel to replace it, or on empty space to add a new channel.

    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        return d.description.toString().startsWith ("file:") || d.description.toString().startsWith ("plugin:");
    }
    void itemDragMove (const SourceDetails& d) override              { setDropHighlight (d.localPosition); }
    void itemDragExit (const SourceDetails&) override                { setDropHighlight ({ -1, -1 }); }

    void itemDropped (const SourceDetails& d) override
    {
        setDropHighlight ({ -1, -1 });
        if (d.description.toString().startsWith ("plugin:"))
        {
            if (auto desc = studio.findKnownPlugin (d.description.toString().substring (7)); desc && desc->isInstrument)
                studio.selectChannel (studio.addPluginChannel (*desc));
            return;
        }
        dropFile (juce::File (d.description.toString().fromFirstOccurrenceOf ("file:", false, false)), d.localPosition);
    }

    bool isInterestedInFileDrag (const juce::StringArray&) override   { return true; }
    void fileDragMove (const juce::StringArray&, int x, int y) override { setDropHighlight ({ x, y }); }
    void fileDragExit (const juce::StringArray&) override            { setDropHighlight ({ -1, -1 }); }

    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        for (int i = 0; i < files.size(); ++i)
            dropFile (juce::File (files[i]), i == 0 ? juce::Point<int> (x, y) : juce::Point<int> (-1, -1));
    }

    std::function<void (int channelId)> onOpenChannel, onOpenPianoRoll;
    std::function<void()> onManagePlugins;

    explicit ChannelRack (Studio& s) : studio (s)
    {
        Widgets::setupKnob (swingKnob, "Swing: delays every 2nd step for a shuffled groove");
        swingKnob.setRange (0.0, 1.0);
        swingKnob.onDragStart = [this] { studio.undo().beginNewTransaction ("Swing"); };
        swingKnob.onValueChange = [this] { studio.setSwing (swingKnob.getValue()); };
        addAndMakeVisible (swingKnob);

        addButton.setTooltip ("Add a channel (sampler or synth)");
        addButton.onClick = [this] { showAddMenu(); };
        addAndMakeVisible (addButton);

        viewport.setViewedComponent (&rowHolder, false);
        viewport.setScrollBarsShown (true, true);
        addAndMakeVisible (viewport);

        Widgets::noFocus (*this);
        rebuildRows();
        studio.editAboutToChange.push_back ([this] { rows.clear(); });
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panelDark);
        auto header = getLocalBounds().removeFromTop (headerHeight);
        g.setColour (Theme::panel);
        g.fillRect (header);
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (12.0f));
        g.drawText ("SWING", header.withX (8).withWidth (44), juce::Justification::centredLeft);

        const auto pattern = studio.getCurrentPatternState();
        g.setColour (Theme::text);
        g.drawText (pattern[IDs::name].toString(), header.withTrimmedLeft (90).reduced (6, 0), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto header = r.removeFromTop (headerHeight);
        swingKnob.setBounds (header.withX (52).withWidth (headerHeight).reduced (2));

        auto footer = r.removeFromBottom (footerHeight);
        addButton.setBounds (footer.reduced (6, 4).withWidth (ChannelRow::nameRight - 6));
        viewport.setBounds (r);
        layoutRows();
    }

private:
    //==========================================================================
    class ChannelRow : public juce::Component
    {
    public:
        static constexpr int stepsLeft = 230, stepWidth = 20, nameRight = 224;

        ChannelRow (ChannelRack& r, int id) : rack (r), studio (r.studio), channelId (id)
        {
            mute.onClick = [this]
            {
                studio.undo().beginNewTransaction ("Mute");
                if (auto t = track()) t->setMute (! mute.getToggleState());
            };
            mute.onRightClick = [this] { toggleSolo(); };
            addAndMakeVisible (mute);

            if (auto t = track())
            {
                if (auto vp = t->getVolumePlugin())
                {
                    pan.bind (studio, vp->panParam);
                    volume.bind (studio, vp->volParam);
                }
            }
            addAndMakeVisible (pan);
            addAndMakeVisible (volume);

            insert.format = [] (double v) { return v < 0.5 ? juce::String ("--") : juce::String ((int) v); };
            insert.setTooltip ("Mixer insert this channel plays through (drag up/down, -- = master)");
            insert.onChange = [this] (double v)
            {
                studio.undo().beginNewTransaction ("Route channel");
                studio.setChannelInsert (channelId, (int) std::round (v));
            };
            addAndMakeVisible (insert);

            name.setTooltip ("Left-click: open channel. Right-click: options");
            name.onClick = [this] { studio.selectChannel (channelId); if (rack.onOpenChannel) rack.onOpenChannel (channelId); };
            name.onRightClick = [this] { showMenu(); };
            addAndMakeVisible (name);

            select.setClickingTogglesState (false);
            select.onClick = [this] { studio.selectChannel (channelId); };
            addAndMakeVisible (select);
            sync();
        }

        int getChannelId() const { return channelId; }

        void sync()
        {
            auto channel = studio.getChannel (channelId);
            name.setButtonText (channel[IDs::name].toString());
            select.setToggleState (studio.getSelectedChannel() == channelId, juce::dontSendNotification);
            insert.setValue (studio.getChannelInsert (channelId));
            if (auto t = track())
                mute.setToggleState (! t->isMuted (false), juce::dontSendNotification);

            pan.sync();
            volume.sync();
            repaint();
        }

        bool isAudioChannel() const { return studio.getChannel (channelId)[IDs::type].toString() == ChannelType::audio; }

        void paint (juce::Graphics& g) override
        {
            if (isAudioChannel())
            {
                g.setColour (Theme::textDim);
                g.setFont (Theme::font (12.0f));
                g.drawText ("Audio clip - plays from the playlist", stepsLeft + 4, 0, 300, getHeight(), juce::Justification::centredLeft);
                return;
            }

            const auto pattern = studio.getCurrentPatternState();
            const int numSteps = rack.getNumSteps();
            const int playingStep = rack.getPlayingStep();

            for (int i = 0; i < numSteps; ++i)
            {
                auto r = getStepBounds (i).toFloat();
                const bool on = studio.findStepNote (pattern, channelId, i).isValid();
                g.setColour (on ? Theme::stepOn : ((i / 4) % 2 == 0 ? Theme::stepOffA : Theme::stepOffB));
                g.fillRoundedRectangle (r, 2.0f);

                if (i == playingStep)
                {
                    g.setColour (Theme::accent.withAlpha (on ? 0.9f : 0.45f));
                    g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.5f);
                }
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (0, 4);
            mute.setBounds (r.removeFromLeft (16).withTrimmedLeft (4).withSizeKeepingCentre (12, 12));
            r.removeFromLeft (4);
            pan.setBounds (r.removeFromLeft (24));
            volume.setBounds (r.removeFromLeft (24));
            r.removeFromLeft (4);
            insert.setBounds (r.removeFromLeft (28).reduced (0, 2));
            r.removeFromLeft (4);
            name.setBounds (r.withRight (nameRight));
            select.setBounds (getWidth() - 14, 7, 10, getHeight() - 14);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int step = stepAt (e.x);
            if (step < 0 || isAudioChannel())
                return;

            studio.undo().beginNewTransaction ("Steps");
            const auto pattern = studio.getCurrentPatternState();
            // FL: left-click toggles + paints, right-click erases.
            paintValue = e.mods.isPopupMenu() ? false : ! studio.findStepNote (pattern, channelId, step).isValid();
            studio.setStep (channelId, step, paintValue);
            lastStep = step;
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const int step = stepAt (e.x);
            if (step >= 0 && step != lastStep)
            {
                studio.setStep (channelId, step, paintValue);
                lastStep = step;
            }
        }

    private:
        te::AudioTrack* track() const { return studio.getTrack (channelId); }

        void toggleSolo()
        {
            auto t = track();
            if (t == nullptr)
                return;

            // FL: right-click the LED solos this channel; again un-solos.
            studio.undo().beginNewTransaction ("Solo");
            const bool soloThis = ! t->isSolo (false);
            for (auto channel : studio.getChannels())
                if (auto other = studio.getTrack (channel))
                    other->setSolo (soloThis && other == t);
        }

        void showMenu()
        {
            const auto channel = studio.getChannel (channelId);
            juce::PopupMenu m;
            m.addItem ("Piano roll", [this] { if (rack.onOpenPianoRoll) rack.onOpenPianoRoll (channelId); });
            m.addItem ("Rename...", [this] { rename(); });
            if (channel[IDs::type].toString() == ChannelType::sampler)
                m.addItem ("Replace sample...", [this] { if (rack.onOpenChannel) rack.onOpenChannel (channelId); });
            m.addSeparator();
            m.addItem ("Delete", studio.getChannels().getNumChildren() > 1,
                       false, [&s = studio, id = channelId] { s.deleteChannel (id); });
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (name));
        }

        void rename()
        {
            auto* w = new juce::AlertWindow ("Rename channel", {}, juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", studio.getChannel (channelId)[IDs::name].toString());
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int result)
            {
                if (result == 1)
                {
                    studio.undo().beginNewTransaction ("Rename");
                    studio.renameChannel (channelId, w->getTextEditorContents ("name"));
                }
            }), true);
        }

        juce::Rectangle<int> getStepBounds (int i) const
        {
            return { stepsLeft + i * stepWidth + 1, 4, stepWidth - 2, getHeight() - 8 };
        }

        int stepAt (int x) const
        {
            const int i = (x - stepsLeft) / stepWidth;
            return (x >= stepsLeft && i < rack.getNumSteps()) ? i : -1;
        }

        ChannelRack& rack;
        Studio& studio;
        const int channelId;
        Widgets::LedButton mute { "Mute (left-click) / Solo (right-click)" };
        ParamKnob pan, volume;
        Widgets::DragNumber insert { 0.0, (double) Studio::numInserts, 0.1 };
        struct NameButton : juce::TextButton
        {
            std::function<void()> onRightClick;
            void mouseDown (const juce::MouseEvent& e) override
            {
                if (e.mods.isPopupMenu()) onRightClick();
                else juce::TextButton::mouseDown (e);
            }
        } name;
        Widgets::LedButton select { "Select channel", Theme::led };
        bool paintValue = true;
        int lastStep = -1;
    };

    //==========================================================================
    int getNumSteps() const
    {
        return juce::jmax (16, (int) std::round (studio.getPatternLengthBeats (studio.getCurrentPatternState()) / Studio::stepBeats));
    }

    int getPlayingStep() const
    {
        if (! studio.isPlaying() || studio.isSongMode())
            return -1;
        return (int) std::floor (studio.getPositionBeats() / Studio::stepBeats) % getNumSteps();
    }

    void rebuildRows()
    {
        rows.clear();
        for (auto channel : studio.getChannels())
            rowHolder.addAndMakeVisible (rows.add (new ChannelRow (*this, channel[IDs::id])));
        Widgets::noFocus (rowHolder);
        layoutRows();
    }

    void layoutRows()
    {
        const int width = juce::jmax (viewport.getMaximumVisibleWidth(),
                                      ChannelRow::stepsLeft + getNumSteps() * ChannelRow::stepWidth + 20);
        rowHolder.setSize (width, rows.size() * rowHeight);
        for (int i = 0; i < rows.size(); ++i)
            rows[i]->setBounds (0, i * rowHeight, width, rowHeight);
    }

    bool rowsMatchModel() const
    {
        const auto channels = studio.getChannels();
        if (channels.getNumChildren() != rows.size())
            return false;
        for (int i = 0; i < rows.size(); ++i)
            if ((int) channels.getChild (i)[IDs::id] != rows[i]->getChannelId())
                return false;
        return true;
    }

    void timerCallback() override
    {
        const int playingStep = getPlayingStep();
        if (studio.getEditGeneration() != lastGeneration)
        {
            lastGeneration = studio.getEditGeneration();
            rebuildRows();
        }

        if (studio.getChangeCount() != lastChangeCount)
        {
            lastChangeCount = studio.getChangeCount();
            if (! rowsMatchModel())
                rebuildRows();
            else
                layoutRows();

            swingKnob.setValue (studio.getSwing(), juce::dontSendNotification);
            for (auto* row : rows)
                row->sync();
            repaint();
        }
        else if (++ticks % 6 == 0)
        {
            // Mute/volume can change outside the model (undo, automation): resync a few times a second.
            for (auto* row : rows)
                row->sync();
        }
        else if (playingStep != lastPlayingStep)
        {
            for (auto* row : rows)
                row->repaint();
        }
        lastPlayingStep = playingStep;
    }

    void showAddMenu()
    {
        juce::PopupMenu m;
        m.addItem ("Sampler", [this]
        {
            const int id = studio.addSamplerChannel (DrumKit::getDrumsFolder().getChildFile ("Kick.wav"));
            studio.selectChannel (id);
            if (onOpenChannel) onOpenChannel (id);
        });
        m.addItem ("4-OSC Synth", [this] { openNew (studio.addSynthChannel()); });
        m.addItem ("FM Synth", [this] { openNew (studio.addFmSynthChannel()); });

        // FL users pick a sound, not an engine.
        juce::PopupMenu sounds;
        const auto all = Presets::allSynthSounds();
        for (auto& category : Presets::synthCategories())
        {
            sounds.addSectionHeader (category);
            for (auto& sound : all)
                if (sound.preset->category == category)
                    sounds.addItem (sound.preset->name, [this, name = sound.preset->name] { openNew (studio.addPresetChannel (name)); });
        }
        m.addSubMenu ("Synth presets", sounds);
        m.addItem ("Instrument (SFZ / SoundFont)...", [this]
        {
            chooseFile ("Choose an instrument", "*.sfz;*.sf2", [this] (const juce::File& f) { return studio.addInstrumentChannel (f); });
        });
        m.addItem ("Drum pad", [this] { openNew (studio.addDrumPadChannel()); });
        m.addItem ("Slicer...", [this]
        {
            chooseFile ("Choose a loop to slice", studio.engine.getAudioFileFormatManager().readFormatManager.getWildcardForAllFormats(),
                        [this] (const juce::File& f) { return studio.addSlicerChannel (f); });
        });

        juce::PopupMenu plugins;
        for (auto& d : studio.getKnownPlugins (true))
            plugins.addItem (d.name + "  (" + d.manufacturerName + ")", [this, d]
            {
                const int id = studio.addPluginChannel (d);
                studio.selectChannel (id);
                if (auto p = studio.getInstrument (id)) p->showWindowExplicitly();
            });
        plugins.addSeparator();
        plugins.addItem ("Scan / manage plugins...", [this] { if (onManagePlugins) onManagePlugins(); });
        m.addSubMenu ("Plugins (VST3 / AU)", plugins);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addButton));
    }

    void openNew (int id)
    {
        if (id == 0)
            return;
        studio.selectChannel (id);
        if (onOpenChannel) onOpenChannel (id);
    }

    void chooseFile (const juce::String& title, const juce::String& wildcard, std::function<int (const juce::File&)> create)
    {
        chooser = std::make_unique<juce::FileChooser> (title, MultiSampler::getInstrumentsFolder(), wildcard);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, create] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult().existsAsFile())
                                      openNew (create (fc.getResult()));
                              });
    }

    int rowIndexAt (juce::Point<int> p) const
    {
        if (! viewport.getBounds().contains (p))
            return -1;
        const auto local = rowHolder.getLocalPoint (this, p);
        const int i = local.y / rowHeight;
        return local.y >= 0 && i < rows.size() ? i : -1;
    }

    void setDropHighlight (juce::Point<int> p)
    {
        const int index = rowIndexAt (p);
        for (int i = 0; i < rows.size(); ++i)
            rows[i]->setAlpha (index < 0 || i == index ? 1.0f : 0.55f);
    }

    void dropFile (const juce::File& file, juce::Point<int> position)
    {
        setDropHighlight ({ -1, -1 });
        if (! file.existsAsFile())
            return;

        if (MultiSampler::isInstrumentFile (file))
        {
            studio.selectChannel (studio.addInstrumentChannel (file));
            return;
        }

        const int index = rowIndexAt (position);
        if (index >= 0)
        {
            const int id = rows[index]->getChannelId();
            if (studio.getChannel (id)[IDs::type].toString() == ChannelType::sampler)
            {
                studio.replaceSample (id, file);
                return;
            }
        }

        studio.selectChannel (studio.addSamplerChannel (file));
    }

    static constexpr int headerHeight = 28, footerHeight = 30, rowHeight = 32;

    Studio& studio;
    juce::Slider swingKnob;
    juce::TextButton addButton { "+" };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Viewport viewport;
    juce::Component rowHolder;
    juce::OwnedArray<ChannelRow> rows;
    int lastChangeCount = -1, lastPlayingStep = -1, ticks = 0, lastGeneration = 0;
};
