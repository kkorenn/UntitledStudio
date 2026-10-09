#pragma once

#include "PluginEditor.h"
#include "Studio.h"
#include "ParamKnob.h"
#include "Effects.h"

// FL note names: MIDI 60 = C5.
inline juce::String noteName (int note) { return juce::MidiMessage::getMidiNoteName (note, true, true, 5); }

// Sampler controls: waveform with draggable start/end markers, root note, replace sample.
class SamplerView : public juce::Component,
                    public juce::DragAndDropTarget,
                    public juce::FileDragAndDropTarget,
                    private juce::ChangeListener,
                    private juce::Timer
{
public:
    SamplerView (Studio& s, int channel, te::SamplerPlugin& p) : studio (s), channelId (channel), sampler (&p)
    {
        thumbnail.addChangeListener (this);

        root.format = [] (double v) { return noteName ((int) v); };
        root.setTooltip ("Root note: the key that plays the sample at its original pitch");
        root.onChange = [this] (double v)
        {
            studio.undo().beginNewTransaction ("Root note");
            sampler->setSoundParams (0, (int) v, 0, 127);
        };
        addAndMakeVisible (root);

        replaceButton.setTooltip ("Load a different sample (or drag one here from the browser)");
        replaceButton.onClick = [this] { browseForSample(); };
        addAndMakeVisible (replaceButton);

        loadThumbnail();
        startTimerHz (10);
    }

    ~SamplerView() override { thumbnail.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panelDark);
        const auto channel = studio.getChannel (channelId);
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (24);
        g.setColour (Theme::text);
        g.setFont (Theme::font (14.0f, true));
        g.drawText (channel[IDs::name].toString(), top.withTrimmedRight (260), juce::Justification::centredLeft, true);
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (12.0f));
        g.drawText ("ROOT", rootLabelArea, juce::Justification::centredRight);

        const auto wave = getWaveArea();
        g.setColour (Theme::lcd);
        g.fillRect (wave);

        const double total = thumbnail.getTotalLength();
        if (total > 0.0)
        {
            g.setColour (Theme::lcdText.withAlpha (0.85f));
            thumbnail.drawChannels (g, wave.reduced (0, 4), 0.0, total, 1.0f);

            // Dim what's outside start..end.
            const int sx = timeToX (getStart()), ex = timeToX (getEnd());
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRect (wave.withRight (sx));
            g.fillRect (wave.withLeft (ex));
            g.setColour (Theme::accent);
            g.fillRect (sx - 1, wave.getY(), 2, wave.getHeight());
            g.fillRect (ex - 1, wave.getY(), 2, wave.getHeight());
        }
        else
        {
            g.setColour (Theme::textDim);
            g.drawText ("Drop a sample here", wave, juce::Justification::centred);
        }

        if (dragOver)
        {
            g.setColour (Theme::accent);
            g.drawRect (getLocalBounds(), 2);
        }
    }

    void resized() override
    {
        auto top = getLocalBounds().reduced (10).removeFromTop (24);
        replaceButton.setBounds (top.removeFromRight (130));
        top.removeFromRight (10);
        root.setBounds (top.removeFromRight (56));
        rootLabelArea = top.removeFromRight (40);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        draggingMarker = 0;
        if (thumbnail.getTotalLength() <= 0.0 || ! getWaveArea().contains (e.getPosition()))
            return;

        const int sx = timeToX (getStart()), ex = timeToX (getEnd());
        draggingMarker = std::abs (e.x - sx) <= std::abs (e.x - ex) ? 1 : 2;
        studio.undo().beginNewTransaction ("Sample start/end");
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingMarker == 0)
            return;

        const double t = xToTime (e.x);
        double start = getStart(), end = getEnd();
        if (draggingMarker == 1) start = juce::jlimit (0.0, end - 0.005, t);
        else                     end = juce::jlimit (start + 0.005, thumbnail.getTotalLength(), t);

        const bool wholeEnd = std::abs (end - thumbnail.getTotalLength()) < 1.0e-4;
        sampler->setSoundExcerpt (0, start, wholeEnd && start <= 0.0 ? 0.0 : end - start);
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        studio.undo().beginNewTransaction ("Reset sample start/end");
        sampler->setSoundExcerpt (0, 0.0, 0.0);
        repaint();
    }

    // Drops from the browser or Finder replace the sample.
    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString().startsWith ("file:"); }
    void itemDragEnter (const SourceDetails&) override             { dragOver = true; repaint(); }
    void itemDragExit (const SourceDetails&) override              { dragOver = false; repaint(); }
    void itemDropped (const SourceDetails& d) override
    {
        dragOver = false;
        replace (juce::File (d.description.toString().fromFirstOccurrenceOf ("file:", false, false)));
    }

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override { replace (juce::File (files[0])); }

private:
    double getStart() const { return sampler->getNumSounds() > 0 ? sampler->getSoundStartTime (0) : 0.0; }

    double getEnd() const
    {
        const double length = sampler->getNumSounds() > 0 ? sampler->getSoundLength (0) : 0.0;
        return length > 0.0 ? juce::jmin (thumbnail.getTotalLength(), getStart() + length) : thumbnail.getTotalLength();
    }

    juce::Rectangle<int> getWaveArea() const { return getLocalBounds().reduced (10).withTrimmedTop (34); }

    int timeToX (double t) const
    {
        const auto w = getWaveArea();
        return w.getX() + (int) (w.getWidth() * t / juce::jmax (1.0e-6, thumbnail.getTotalLength()));
    }

    double xToTime (int x) const
    {
        const auto w = getWaveArea();
        return juce::jlimit (0.0, thumbnail.getTotalLength(), (x - w.getX()) / (double) w.getWidth() * thumbnail.getTotalLength());
    }

    void loadThumbnail()
    {
        currentFile = juce::File (studio.getChannel (channelId)[IDs::file].toString());
        thumbnail.setSource (currentFile.existsAsFile() ? new juce::FileInputSource (currentFile) : nullptr);
        repaint();
    }

    void replace (const juce::File& f)
    {
        if (f.existsAsFile())
        {
            studio.replaceSample (channelId, f);
            loadThumbnail();
        }
    }

    void browseForSample()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a sample", DrumKit::getPacksFolder(),
                                                       studio.engine.getAudioFileFormatManager().readFormatManager.getWildcardForAllFormats());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { replace (fc.getResult()); });
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    void timerCallback() override
    {
        // Follow undo/redo and external changes.
        if (sampler->getNumSounds() > 0)
            root.setValue (sampler->getKeyNote (0));
        if (juce::File (studio.getChannel (channelId)[IDs::file].toString()) != currentFile)
            loadThumbnail();
    }

    Studio& studio;
    const int channelId;
    juce::ReferenceCountedObjectPtr<te::SamplerPlugin> sampler;
    juce::AudioThumbnailCache cache { 4 };
    juce::AudioThumbnail thumbnail { 256, studio.engine.getAudioFileFormatManager().readFormatManager, cache };
    juce::File currentFile;
    Widgets::DragNumber root { 0.0, 127.0, 0.25 };
    juce::Rectangle<int> rootLabelArea;
    juce::TextButton replaceButton { "Replace sample..." };
    std::unique_ptr<juce::FileChooser> chooser;
    int draggingMarker = 0;
    bool dragOver = false;
};

// FL "FPC"-style 4x4 pads (pad 1 bottom-left). Click = play, drop a sample on a pad = replace it.
class DrumPadGrid : public juce::Component,
                    public juce::DragAndDropTarget,
                    public juce::FileDragAndDropTarget
{
public:
    DrumPadGrid (Studio& s, int channel, MultiSamplerPlugin& p) : studio (s), channelId (channel), plugin (&p) {}

    void paint (juce::Graphics& g) override
    {
        for (int i = 0; i < MultiSampler::numPads; ++i)
        {
            auto r = padBounds (i).toFloat();
            const auto region = findPad (i);
            const bool lit = i == playingPad || i == hoverPad;
            g.setColour (lit ? Theme::accent : Theme::panel);
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (Theme::outline);
            g.drawRoundedRectangle (r, 4.0f, 1.0f);

            auto label = region["name"].toString();
            if (label.isEmpty())
                label = juce::File (region["sample"].toString()).getFileNameWithoutExtension();
            g.setColour (lit ? juce::Colours::black : Theme::text);
            g.setFont (Theme::font (13.0f, true));
            g.drawText (label, r.reduced (6.0f), juce::Justification::centred, true);
            g.setColour (lit ? juce::Colours::black.withAlpha (0.6f) : Theme::textDim);
            g.setFont (Theme::font (10.0f));
            g.drawText (juce::String (i + 1) + "  " + noteName (MultiSampler::firstPadNote + i), r.reduced (5.0f, 3.0f),
                        juce::Justification::bottomLeft);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        playingPad = padAt (e.getPosition());
        if (playingPad < 0)
            return;
        studio.selectChannel (channelId);
        studio.playLiveNote (juce::MidiMessage::noteOn (1, MultiSampler::firstPadNote + playingPad, (juce::uint8) 110));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (playingPad >= 0)
            studio.playLiveNote (juce::MidiMessage::noteOff (1, MultiSampler::firstPadNote + playingPad));
        playingPad = -1;
        repaint();
    }

    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString().startsWith ("file:"); }
    void itemDragMove (const SourceDetails& d) override              { setHover (padAt (d.localPosition)); }
    void itemDragExit (const SourceDetails&) override                { setHover (-1); }
    void itemDropped (const SourceDetails& d) override
    {
        setHover (-1);
        replacePad (padAt (d.localPosition), juce::File (d.description.toString().fromFirstOccurrenceOf ("file:", false, false)));
    }

    bool isInterestedInFileDrag (const juce::StringArray&) override     { return true; }
    void fileDragMove (const juce::StringArray&, int x, int y) override { setHover (padAt ({ x, y })); }
    void fileDragExit (const juce::StringArray&) override               { setHover (-1); }
    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        setHover (-1);
        replacePad (padAt ({ x, y }), juce::File (files[0]));
    }

private:
    juce::Rectangle<int> padBounds (int i) const
    {
        const int w = getWidth() / 4, h = getHeight() / 4;
        return juce::Rectangle<int> ((i % 4) * w, (3 - i / 4) * h, w, h).reduced (3);
    }

    int padAt (juce::Point<int> p) const
    {
        for (int i = 0; i < MultiSampler::numPads; ++i)
            if (padBounds (i).contains (p))
                return i;
        return -1;
    }

    juce::ValueTree findPad (int i) const
    {
        for (auto r : plugin->getRegionStates())
            if ((int) r["lokey"] == MultiSampler::firstPadNote + i)
                return r;
        return {};
    }

    void setHover (int pad) { if (pad != hoverPad) { hoverPad = pad; repaint(); } }

    void replacePad (int pad, const juce::File& file)
    {
        auto region = findPad (pad);
        if (! region.isValid() || ! file.existsAsFile())
            return;
        studio.undo().beginNewTransaction ("Replace pad sample");
        region.setProperty ("sample", file.getFullPathName(), &studio.undo());
        region.setProperty ("name", file.getFileNameWithoutExtension(), &studio.undo());
        region.removeProperty ("tune", &studio.undo());
        repaint();
    }

    Studio& studio;
    const int channelId;
    juce::ReferenceCountedObjectPtr<MultiSamplerPlugin> plugin;
    int playingPad = -1, hoverPad = -1;
};

// Channel window for the multi-sampler: SFZ / SoundFont / WAV instruments, drum pads, slices.
class InstrumentView : public juce::Component, private juce::Timer
{
public:
    InstrumentView (Studio& s, int channel, MultiSamplerPlugin& p) : studio (s), channelId (channel), plugin (&p)
    {
        const std::pair<te::AutomatableParameter::Ptr, const char*> params[] = {
            { plugin->volume, "VOL" }, { plugin->attack, "ATT" }, { plugin->decay, "DEC" },
            { plugin->sustain, "SUS" }, { plugin->release, "REL" }, { plugin->tune, "TUNE" } };
        for (auto& [param, label] : params)
        {
            knobs[(size_t) knobLabels.size()].bind (studio, param);
            addAndMakeVisible (knobs[(size_t) knobLabels.size()]);
            knobLabels.add (label);
        }

        presets.setTooltip ("SoundFont preset");
        presets.onChange = [this]
        {
            if (presets.getSelectedItemIndex() >= 0 && presets.getSelectedItemIndex() != plugin->getProgram())
            {
                studio.undo().beginNewTransaction ("Change preset");
                plugin->setProgram (presets.getSelectedItemIndex());
            }
        };
        addChildComponent (presets);

        loop.setTooltip ("Loop the sample while the note is held");
        loop.onClick = [this] { setLoop (loop.getToggleState() ? "loop_continuous" : "no_loop"); };
        addChildComponent (loop);
        for (auto* n : { &loopStart, &loopEnd })
        {
            n->format = [] (double v) { return juce::String (v, 1) + "%"; };
            addChildComponent (*n);
        }
        loopStart.setTooltip ("Loop start (% of the sample)");
        loopEnd.setTooltip ("Loop end (% of the sample)");
        loopStart.onChange = [this] (double v) { setLoopPoint ("loop_start", v); };
        loopEnd.onChange = [this] (double v) { setLoopPoint ("loop_end", v); };

        if (isDrumPad())
        {
            pads = std::make_unique<DrumPadGrid> (studio, channelId, *plugin);
            addAndMakeVisible (*pads);
        }

        sync();
        startTimerHz (10);
    }

    bool isDrumPad() const { return studio.getChannel (channelId)[IDs::type].toString() == ChannelType::drumpad; }

    // A plain audio file instrument: one region we can loop.
    bool isSingleSample() const { return plugin->usesRegionState() && plugin->getRegionStates().size() == 1; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panelDark);
        const auto& info = plugin->getInfo();
        auto r = getLocalBounds().reduced (10);
        g.setColour (Theme::text);
        g.setFont (Theme::font (14.0f, true));
        g.drawText (studio.getChannel (channelId)[IDs::name].toString(), r.removeFromTop (20), juce::Justification::centredLeft, true);

        juce::String details;
        if (isDrumPad())
            details = "Drum pad - 16 pads (C3..D#4). Drop samples on pads.";
        else
            details = plugin->getSource().getFileName();
        details << "   |   " << info.numRegions << (info.numRegions == 1 ? " region" : " regions");
        g.setColour (info.error.isNotEmpty() ? Theme::accent : Theme::textDim);
        g.setFont (Theme::font (12.0f));
        g.drawText (info.error.isNotEmpty() ? info.error : details, r.removeFromTop (18), juce::Justification::centredLeft, true);

        g.setFont (Theme::font (11.0f));
        g.setColour (Theme::textDim);
        for (size_t i = 0; i < knobs.size(); ++i)
            g.drawText (knobLabels[(int) i], knobs[i].getBounds().translated (0, knobs[i].getHeight()).withHeight (14), juce::Justification::centred);
        if (loop.isVisible())
        {
            g.drawText ("LOOP", loop.getBounds().translated (-40, 0).withWidth (36), juce::Justification::centredRight);
            g.drawText ("START", loopStart.getBounds().translated (-44, 0).withWidth (40), juce::Justification::centredRight);
            g.drawText ("END", loopEnd.getBounds().translated (-34, 0).withWidth (30), juce::Justification::centredRight);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto header = r.removeFromTop (38);
        presets.setBounds (header.removeFromRight (240).withHeight (24));

        r.removeFromTop (6);
        auto knobRow = r.removeFromTop (48);
        for (auto& k : knobs)
        {
            k.setBounds (knobRow.removeFromLeft (48).reduced (4, 0));
            knobRow.removeFromLeft (6);
        }
        r.removeFromTop (18);

        auto loopRow = r.removeFromTop (24);
        loopRow.removeFromLeft (44);
        loop.setBounds (loopRow.removeFromLeft (24));
        loopRow.removeFromLeft (60);
        loopStart.setBounds (loopRow.removeFromLeft (70));
        loopRow.removeFromLeft (50);
        loopEnd.setBounds (loopRow.removeFromLeft (70));

        if (pads != nullptr)
            pads->setBounds (r.withTrimmedTop (-24));
    }

private:
    juce::ValueTree singleRegion() const { return plugin->getRegionStates().getFirst(); }

    void setLoop (const juce::String& mode)
    {
        studio.undo().beginNewTransaction ("Loop");
        singleRegion().setProperty ("loop_mode", mode, &studio.undo());
    }

    void setLoopPoint (const juce::Identifier& id, double percent)
    {
        const auto length = plugin->getInfo().firstRegionLength;
        if (length <= 0)
            return;
        studio.undo().beginNewTransaction ("Loop point");
        auto region = singleRegion();
        const auto sample = (juce::int64) (percent / 100.0 * (double) length);
        region.setProperty (id, juce::jlimit<juce::int64> (0, length - 1, id == juce::Identifier ("loop_end") ? sample - 1 : sample), &studio.undo());
        if (region["loop_mode"].toString() != "loop_continuous" && region["loop_mode"].toString() != "loop_sustain")
            region.setProperty ("loop_mode", "loop_continuous", &studio.undo());
    }

    void sync()
    {
        for (auto& k : knobs)
            k.sync();

        const auto& info = plugin->getInfo();
        if (info.presetNames != shownPresets)
        {
            shownPresets = info.presetNames;
            presets.clear (juce::dontSendNotification);
            presets.addItemList (shownPresets, 1);
        }
        presets.setVisible (! shownPresets.isEmpty());
        presets.setSelectedItemIndex (plugin->getProgram(), juce::dontSendNotification);

        const bool single = isSingleSample() && info.firstRegionLength > 0;
        for (auto* c : std::initializer_list<juce::Component*> { &loop, &loopStart, &loopEnd })
            c->setVisible (single);
        if (single)
        {
            const auto region = singleRegion();
            const auto mode = region["loop_mode"].toString();
            loop.setToggleState (mode == "loop_continuous" || mode == "loop_sustain", juce::dontSendNotification);
            const double length = (double) info.firstRegionLength;
            loopStart.setValue (region.hasProperty ("loop_start") ? (double) region["loop_start"] / length * 100.0 : 0.0);
            loopEnd.setValue (region.hasProperty ("loop_end") ? ((double) region["loop_end"] + 1.0) / length * 100.0 : 100.0);
        }
        repaint();
    }

    void timerCallback() override { sync(); }

    Studio& studio;
    const int channelId;
    juce::ReferenceCountedObjectPtr<MultiSamplerPlugin> plugin;
    std::array<ParamKnob, 6> knobs;
    juce::StringArray knobLabels, shownPresets;
    juce::ComboBox presets;
    Widgets::LedButton loop { "Loop" };
    Widgets::DragNumber loopStart { 0.0, 100.0, 0.25 }, loopEnd { 0.0, 100.0, 0.25 };
    std::unique_ptr<DrumPadGrid> pads;
};

// FM Synth controls: algorithm picker + diagram, one knob row per operator, global row.
class FMSynthView : public juce::Component, private juce::Timer
{
public:
    FMSynthView (Studio& s, FMSynthPlugin& p) : studio (s), plugin (&p)
    {
        for (int i = 0; i < FM::numParams; ++i)
        {
            if (i == FM::algorithm)
                continue;
            knobs[(size_t) i].bind (studio, plugin->getParam (i));
            addAndMakeVisible (knobs[(size_t) i]);
        }

        for (int a = 0; a < FM::numAlgorithms; ++a)
            algorithm.addItem (juce::String (a + 1) + ": " + FM::algorithms()[(size_t) a].name, a + 1);
        algorithm.setTooltip ("Algorithm: how the 4 operators modulate each other (bottom row = heard, upper ops modulate the ones below)");
        algorithm.onChange = [this]
        {
            auto* param = plugin->getParam (FM::algorithm);
            if (algorithm.getSelectedItemIndex() >= 0 && algorithm.getSelectedItemIndex() != currentAlgorithm())
            {
                studio.undo().beginNewTransaction ("Algorithm");
                param->setParameter ((float) algorithm.getSelectedItemIndex(), juce::sendNotificationSync);
                studio.userChangedParam (*param);
                repaint();
            }
        };
        addAndMakeVisible (algorithm);
        sync();
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panelDark);
        g.setFont (Theme::font (11.0f));
        g.setColour (Theme::textDim);
        const char* columns[] = { "RATIO", "FINE", "LEVEL", "ATTACK", "DECAY", "SUSTAIN", "RELEASE" };
        for (int c = 0; c < FM::numOpParams; ++c)
            g.drawText (columns[c], knobs[(size_t) c].getBounds().withY (knobs[0].getY() - 16).withHeight (14).expanded (8, 0), juce::Justification::centred);

        const auto& alg = FM::algorithms()[(size_t) currentAlgorithm()];
        for (int op = 0; op < FM::numOps; ++op)
        {
            const auto k = knobs[(size_t) (op * FM::numOpParams)].getBounds();
            const bool carrier = alg.carriers & (1 << op);
            g.setColour (carrier ? Theme::accent : Theme::text);
            g.setFont (Theme::font (12.0f, true));
            g.drawText ("OP " + juce::String (op + 1), k.withX (10).withWidth (50), juce::Justification::centredLeft);
            g.setFont (Theme::font (10.0f));
            g.setColour (Theme::textDim);
            g.drawText (carrier ? "carrier" : (op == 3 ? "mod + fb" : "modulator"), k.withX (10).withWidth (60).translated (0, 14), juce::Justification::centredLeft);
        }

        const char* globals[] = { "FEEDBACK", "CUTOFF", "RESONANCE", "VELOCITY", "VOLUME" };
        for (int i = 0; i < 5; ++i)
            g.drawText (globals[i], knobs[(size_t) (FM::feedback + i)].getBounds().withY (knobs[(size_t) FM::feedback].getY() - 16).withHeight (14).expanded (10, 0),
                        juce::Justification::centred);

        paintDiagram (g, diagramArea.toFloat(), alg);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        diagramArea = r.removeFromRight (150);
        algorithm.setBounds (diagramArea.removeFromTop (24));
        diagramArea.removeFromTop (6);
        diagramArea = diagramArea.withHeight (juce::jmin (diagramArea.getHeight(), 200));

        r.removeFromTop (16);
        for (int op = 0; op < FM::numOps; ++op)
        {
            auto row = r.removeFromTop (50);
            row.removeFromLeft (64);
            for (int c = 0; c < FM::numOpParams; ++c)
                knobs[(size_t) (op * FM::numOpParams + c)].setBounds (row.removeFromLeft (54).reduced (5, 2));
        }
        r.removeFromTop (22);
        auto row = r.removeFromTop (50);
        row.removeFromLeft (64);
        for (int i = FM::feedback; i < FM::numParams; ++i)
            knobs[(size_t) i].setBounds (row.removeFromLeft (70).reduced (12, 2));
    }

private:
    int currentAlgorithm() const { return juce::jlimit (0, FM::numAlgorithms - 1, juce::roundToInt (plugin->getParam (FM::algorithm)->getCurrentValue())); }

    // Boxes per op: carriers on the bottom row; a modulator sits one row above its highest target.
    static void paintDiagram (juce::Graphics& g, juce::Rectangle<float> area, const FM::Algorithm& alg)
    {
        g.setColour (Theme::lcd);
        g.fillRoundedRectangle (area, 4.0f);

        std::array<int, FM::numOps> row {};
        for (int op = 0; op < FM::numOps; ++op)
            for (int target = 0; target < op; ++target)
                if (alg.mods[(size_t) target] & (1 << op))
                    row[(size_t) op] = juce::jmax (row[(size_t) op], row[(size_t) target] + 1);

        const int rows = 1 + *std::max_element (row.begin(), row.end());
        std::array<juce::Rectangle<float>, FM::numOps> boxes;
        const float cellH = area.getHeight() / (float) juce::jmax (rows, 1);
        for (int rIndex = 0; rIndex < rows; ++rIndex)
        {
            juce::Array<int> ops;
            for (int op = 0; op < FM::numOps; ++op)
                if (row[(size_t) op] == rIndex) ops.add (op);
            const float cellW = area.getWidth() / (float) ops.size();
            for (int i = 0; i < ops.size(); ++i)
                boxes[(size_t) ops[i]] = juce::Rectangle<float> (area.getX() + cellW * (float) i, area.getBottom() - cellH * (float) (rIndex + 1), cellW, cellH)
                                             .withSizeKeepingCentre (30.0f, juce::jmin (24.0f, cellH - 8.0f));
        }

        g.setColour (Theme::textDim);
        for (int op = 0; op < FM::numOps; ++op)
            for (int target = 0; target < op; ++target)
                if (alg.mods[(size_t) target] & (1 << op))
                    g.drawLine ({ boxes[(size_t) op].getCentre().withY (boxes[(size_t) op].getBottom()),
                                  boxes[(size_t) target].getCentre().withY (boxes[(size_t) target].getY()) }, 1.5f);

        const auto fb = boxes[3];
        g.drawRoundedRectangle (fb.withX (fb.getRight() - 4).withWidth (12).expanded (0, 4), 3.0f, 1.0f);

        for (int op = 0; op < FM::numOps; ++op)
        {
            const bool carrier = alg.carriers & (1 << op);
            g.setColour (carrier ? Theme::accent : Theme::panel);
            g.fillRoundedRectangle (boxes[(size_t) op], 3.0f);
            g.setColour (carrier ? juce::Colours::black : Theme::text);
            g.setFont (Theme::font (12.0f, true));
            g.drawText (juce::String (op + 1), boxes[(size_t) op], juce::Justification::centred);
        }
    }

    void sync()
    {
        for (auto& k : knobs)
            if (k.getParam() != nullptr)
                k.sync();
        if (algorithm.getSelectedItemIndex() != currentAlgorithm())
        {
            algorithm.setSelectedItemIndex (currentAlgorithm(), juce::dontSendNotification);
            repaint();
        }
    }

    void timerCallback() override { sync(); }

    Studio& studio;
    juce::ReferenceCountedObjectPtr<FMSynthPlugin> plugin;
    std::array<ParamKnob, FM::numParams> knobs;
    juce::ComboBox algorithm;
    juce::Rectangle<int> diagramArea;
};

// FL's channel settings window: the instrument's controls + a playable keyboard at the bottom.
class ChannelSettings : public juce::Component, private juce::MidiKeyboardState::Listener
{
public:
    ChannelSettings (Studio& s, int channel) : studio (s), channelId (channel)
    {
        if (auto track = studio.getTrack (channelId))
        {
            if (auto plugin = track->pluginList.getPlugins().getFirst())
            {
                if (auto sampler = dynamic_cast<te::SamplerPlugin*> (plugin.get()))
                    editor = std::make_unique<SamplerView> (studio, channelId, *sampler);
                else if (auto ms = dynamic_cast<MultiSamplerPlugin*> (plugin.get()))
                    editor = std::make_unique<InstrumentView> (studio, channelId, *ms);
                else if (auto fm = dynamic_cast<FMSynthPlugin*> (plugin.get()))
                    editor = std::make_unique<FMSynthView> (studio, *fm);
                else
                    editor = std::make_unique<GenericPluginEditor> (studio, *plugin);
                addAndMakeVisible (*editor);

                if (Presets::isSynthType (plugin->getPluginType()))
                {
                    synth = plugin;
                    presetButton.setTooltip ("Factory sounds and your saved presets");
                    presetButton.onClick = [this] { Presets::showMenu (studio, synth, presetButton); };
                    addAndMakeVisible (presetButton);
                }
            }
        }

        keyboard.setAvailableRange (24, 108);
        keyboard.setLowestVisibleKey (48);
        keyboard.setVelocity (100.0f / 127.0f, true);
        keyboard.clearKeyMappings();
        keyboard.setOctaveForMiddleC (5); // FL naming
        keyboardState.addListener (this);
        addAndMakeVisible (keyboard);

        Widgets::noFocus (*this);
        const bool isSampler = dynamic_cast<SamplerView*> (editor.get()) != nullptr;
        if (auto iv = dynamic_cast<InstrumentView*> (editor.get()))
            setSize (560, iv->isDrumPad() ? 560 : 260);
        else if (dynamic_cast<FMSynthView*> (editor.get()) != nullptr)
            setSize (640, 500);
        else
            setSize (isSampler ? 560 : 640, isSampler ? 300 : 460);
    }

    ~ChannelSettings() override { keyboardState.removeListener (this); }

    int getChannelId() const { return channelId; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panelDark);
        if (synth != nullptr)
        {
            g.setColour (Theme::text);
            g.setFont (Theme::font (14.0f, true));
            g.drawText (studio.getChannel (channelId)[IDs::name].toString(), presetButton.getBounds().withX (10).withRight (presetButton.getX() - 10),
                        juce::Justification::centredLeft, true);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        keyboard.setBounds (r.removeFromBottom (70));
        if (synth != nullptr)
            presetButton.setBounds (r.removeFromTop (34).reduced (10, 5).removeFromRight (90));
        if (editor != nullptr)
            editor->setBounds (r);
    }

private:
    void handleNoteOn (juce::MidiKeyboardState*, int ch, int note, float vel) override
    {
        studio.selectChannel (channelId);
        studio.playLiveNote (juce::MidiMessage::noteOn (ch, note, vel));
    }

    void handleNoteOff (juce::MidiKeyboardState*, int ch, int note, float) override
    {
        studio.playLiveNote (juce::MidiMessage::noteOff (ch, note));
    }

    Studio& studio;
    const int channelId;
    std::unique_ptr<juce::Component> editor;
    te::Plugin::Ptr synth;
    juce::TextButton presetButton { "Presets" };
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard { keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard };
};
