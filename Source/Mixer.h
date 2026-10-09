#pragma once

#include "Effects.h"

// Peak meter fed by a Tracktion LevelMeterPlugin.
class PeakMeter : public juce::Component, private juce::Timer
{
public:
    explicit PeakMeter (te::LevelMeterPlugin* p) : plugin (p)
    {
        if (plugin != nullptr)
            plugin->measurer.addClient (client);
        startTimerHz (30);
    }

    ~PeakMeter() override
    {
        if (plugin != nullptr)
            plugin->measurer.removeClient (client);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Theme::lcd);
        g.fillRect (r);
        const float w = r.getWidth() / 2.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float level = juce::jlimit (0.0f, 1.0f, (levels[ch] + 60.0f) / 66.0f); // -60..+6 dB
            auto bar = juce::Rectangle<float> (r.getX() + (float) ch * w + 1.0f, r.getBottom() - level * r.getHeight(), w - 2.0f, level * r.getHeight());
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4fd16a), 0.0f, r.getBottom(),
                                                     juce::Colour (0xffe8d24a), 0.0f, r.getY() + r.getHeight() * 0.25f, false));
            g.fillRect (bar);
            if (levels[ch] > 0.0f)
            {
                g.setColour (juce::Colour (0xffe0483e));
                g.fillRect (bar.withHeight (3.0f));
            }
        }
    }

private:
    void timerCallback() override
    {
        bool changed = false;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float db = client.getAndClearAudioLevel (ch).dB;
            const float decayed = levels[ch] - 1.5f;
            const float next = juce::jmax (db, decayed, -60.0f);
            changed |= std::abs (next - levels[ch]) > 0.01f;
            levels[ch] = next;
        }
        if (changed)
            repaint();
    }

    juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> plugin;
    te::LevelMeasurer::Client client;
    float levels[2] { -60.0f, -60.0f };
};

// FL-style mixer: master + inserts as strips; the selected insert's effect slots and sends on the right.
class Mixer : public juce::Component, private juce::Timer
{
public:
    std::function<void (te::Plugin&)> onOpenEffect;

    explicit Mixer (Studio& s) : studio (s)
    {
        viewport.setViewedComponent (&stripHolder, false);
        viewport.setScrollBarsShown (false, true);
        addAndMakeVisible (viewport);
        addAndMakeVisible (slots);
        studio.editAboutToChange.push_back ([this] { strips.clear(); slots.clear(); });
        rebuild();
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panelDark); }

    void resized() override
    {
        auto r = getLocalBounds();
        slots.setBounds (r.removeFromRight (230));
        viewport.setBounds (r);
        layoutStrips();
    }

    int getSelectedInsert() const { return selected; }

private:
    //==========================================================================
    class Strip : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        Strip (Mixer& m, int insertIndex) : index (insertIndex), mixer (m), studio (m.studio),
                                             meter (m.studio.getInsertMeter (insertIndex))
        {
            setTooltip (index == 0 ? "Master" : "Insert " + juce::String (index) + ": click to select, double-click to rename");

            if (auto vp = studio.getInsertVolume (index))
            {
                bind (fader, vp->volParam);
                bind (pan, vp->panParam);
            }
            fader.setSliderStyle (juce::Slider::LinearVertical);
            fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            Widgets::setupKnob (pan, "Pan", true);
            addAndMakeVisible (fader);
            addAndMakeVisible (pan);
            addAndMakeVisible (meter);

            if (index > 0)
            {
                mute.onClick = [this]
                {
                    studio.undo().beginNewTransaction ("Mute insert");
                    if (auto t = studio.getInsertTrack (index)) t->setMute (! mute.getToggleState());
                };
                mute.onRightClick = [this]
                {
                    studio.undo().beginNewTransaction ("Solo insert");
                    auto t = studio.getInsertTrack (index);
                    const bool soloThis = t != nullptr && ! t->isSolo (false);
                    for (int i = 1; i <= Studio::numInserts; ++i)
                        if (auto other = studio.getInsertTrack (i))
                            other->setSolo (soloThis && other == t);
                };
                addAndMakeVisible (mute);
            }
            Widgets::noFocus (*this);
            sync();
        }

        void sync()
        {
            if (auto t = studio.getInsertTrack (index))
                mute.setToggleState (! t->isMuted (false), juce::dontSendNotification);
            for (auto* s : { &fader, &pan })
                if (auto* p = static_cast<te::AutomatableParameter*> (s->getProperties()["param"].getObject()))
                    if (! s->isMouseButtonDown())
                        s->setValue (p->getCurrentValue(), juce::dontSendNotification);
        }

        void paint (juce::Graphics& g) override
        {
            const bool isSelected = mixer.selected == index;
            g.setColour (isSelected ? Theme::titleActive : (index == 0 ? Theme::panel.brighter (0.05f) : Theme::panel));
            g.fillRect (getLocalBounds().reduced (1, 0));
            g.setColour (Theme::outline);
            g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

            g.setColour (isSelected ? Theme::accent : Theme::text);
            g.setFont (Theme::font (11.0f, true));
            g.drawFittedText (studio.getInsertName (index), getLocalBounds().removeFromTop (30).reduced (3, 2), juce::Justification::centred, 2);

            g.setColour (Theme::textDim);
            g.setFont (Theme::font (11.0f));
            g.drawText (index == 0 ? "M" : juce::String (index), getLocalBounds().removeFromBottom (18), juce::Justification::centred);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (4, 0);
            r.removeFromTop (32);
            r.removeFromBottom (18);
            mute.setBounds (r.removeFromBottom (18).withSizeKeepingCentre (14, 14));
            pan.setBounds (r.removeFromBottom (34).withSizeKeepingCentre (30, 30));
            r.removeFromBottom (4);
            meter.setBounds (r.removeFromLeft (12));
            fader.setBounds (r);
        }

        void mouseDown (const juce::MouseEvent&) override { mixer.select (index); }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            if (index == 0)
                return;
            auto* w = new juce::AlertWindow ("Rename insert", {}, juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", studio.getInsertName (index));
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int r)
            {
                if (r == 1)
                {
                    studio.undo().beginNewTransaction ("Rename insert");
                    studio.renameInsert (index, w->getTextEditorContents ("name"));
                    repaint();
                }
            }), true);
        }

        const int index;

    private:
        void bind (juce::Slider& s, te::AutomatableParameter::Ptr param)
        {
            const auto range = param->valueRange;
            s.setNormalisableRange ({ range.start, range.end, 0.0, range.skew });
            s.setValue (param->getCurrentValue(), juce::dontSendNotification);
            if (auto def = param->getDefaultValue())
                s.setDoubleClickReturnValue (true, *def);
            s.getProperties().set ("param", param.get());
            s.onDragStart = [this, param] { mixer.select (index); studio.undo().beginNewTransaction ("Mixer"); param->parameterChangeGestureBegin(); };
            s.onDragEnd   = [param] { param->parameterChangeGestureEnd(); };
            s.onValueChange = [&s, param]
            {
                param->setParameter ((float) s.getValue(), juce::sendNotificationSync);
                s.setTooltip (param->getParameterName() + ": " + param->getCurrentValueAsStringWithLabel());
            };
            params.add (param);
        }

        Mixer& mixer;
        Studio& studio;
        juce::Slider fader, pan;
        PeakMeter meter;
        Widgets::LedButton mute { "Mute (left-click) / Solo (right-click)" };
        juce::Array<te::AutomatableParameter::Ptr> params;
    };

    //==========================================================================
    // Right side: 10 effect slots + sends for the selected insert.
    class Slots : public juce::Component
    {
    public:
        explicit Slots (Mixer& m) : mixer (m), studio (m.studio)
        {
            addSendButton.setTooltip ("Send this insert's audio to another insert too (e.g. a reverb bus)");
            addSendButton.onClick = [this] { showSendMenu(); };
            addAndMakeVisible (addSendButton);
        }

        void clear() { sendKnobs.clear(); sendParams.clear(); }

        void refresh()
        {
            effects.clearQuick();
            for (auto p : studio.getInsertEffects (mixer.selected))
                effects.add (p);

            sendKnobs.clear();
            sendParams.clear();
            for (auto send : studio.getInsertSends (mixer.selected))
            {
                auto* k = sendKnobs.add (new juce::Slider());
                Widgets::setupKnob (*k, "Send to " + send->getBusName());
                auto param = send->getAutomatableParameter (0);
                if (param != nullptr)
                {
                    k->setNormalisableRange ({ param->valueRange.start, param->valueRange.end });
                    k->setValue (param->getCurrentValue(), juce::dontSendNotification);
                    k->onDragStart = [this] { studio.undo().beginNewTransaction ("Send level"); };
                    k->onValueChange = [k, param] { param->setParameter ((float) k->getValue(), juce::sendNotificationSync); };
                    sendParams.add (param);
                }
                k->getProperties().set ("name", send->getBusName());
                addAndMakeVisible (k);
            }
            addSendButton.setVisible (mixer.selected > 0);
            Widgets::noFocus (*this);
            resized();
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::panel);
            g.setColour (Theme::outline);
            g.drawVerticalLine (0, 0.0f, (float) getHeight());

            g.setColour (Theme::accent);
            g.setFont (Theme::font (13.0f, true));
            g.drawText (studio.getInsertName (mixer.selected), 10, 4, getWidth() - 20, 22, juce::Justification::centredLeft);

            g.setFont (Theme::font (12.0f));
            for (int i = 0; i < Studio::maxEffectSlots; ++i)
            {
                const auto r = slotBounds (i);
                g.setColour (Theme::panelDark);
                g.fillRoundedRectangle (r.toFloat(), 3.0f);
                if (i < effects.size())
                {
                    auto* p = effects[i];
                    g.setColour (p->isEnabled() ? Theme::led : Theme::ledOff);
                    g.fillRoundedRectangle (r.withWidth (6).reduced (0, 4).toFloat().translated (5.0f, 0.0f), 2.0f);
                    g.setColour (Theme::text);
                    g.drawText (Effects::displayName (*p), r.withTrimmedLeft (18), juce::Justification::centredLeft, true);
                }
                else
                {
                    g.setColour (Theme::textDim.withAlpha (0.5f));
                    g.drawText ("Slot " + juce::String (i + 1), r.withTrimmedLeft (18), juce::Justification::centredLeft);
                }
            }

            g.setColour (Theme::textDim);
            g.setFont (Theme::font (12.0f, true));
            g.drawText ("SENDS", 10, sendsTop() - 22, 80, 20, juce::Justification::centredLeft);
            g.setFont (Theme::font (11.0f));
            for (auto* k : sendKnobs)
                g.drawText (k->getProperties()["name"].toString(), k->getRight() + 6, k->getY(), getWidth() - k->getRight() - 10, k->getHeight(),
                            juce::Justification::centredLeft, true);
        }

        void resized() override
        {
            int y = sendsTop();
            for (auto* k : sendKnobs)
            {
                k->setBounds (10, y, 28, 28);
                y += 32;
            }
            addSendButton.setBounds (10, y + 4, 110, 24);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            for (int i = 0; i < Studio::maxEffectSlots; ++i)
            {
                if (! slotBounds (i).contains (e.getPosition()))
                    continue;

                if (i >= effects.size())
                {
                    Effects::showAddMenu (*this, [this, insert = mixer.selected] (const juce::String& type)
                    {
                        if (auto p = studio.addEffect (insert, type))
                            if (mixer.onOpenEffect) mixer.onOpenEffect (*p);
                        refresh();
                    });
                    return;
                }

                te::Plugin::Ptr p = effects[i];
                if (e.mods.isPopupMenu() || e.x < slotBounds (i).getX() + 16)
                {
                    if (! e.mods.isPopupMenu())
                    {
                        studio.undo().beginNewTransaction ("Bypass");
                        p->setEnabled (! p->isEnabled());
                        repaint();
                        return;
                    }
                    juce::PopupMenu m;
                    m.addItem ("Open", [this, p] { if (mixer.onOpenEffect) mixer.onOpenEffect (*p); });
                    m.addItem (p->isEnabled() ? "Bypass" : "Enable", [this, p] { studio.undo().beginNewTransaction ("Bypass"); p->setEnabled (! p->isEnabled()); repaint(); });
                    m.addItem ("Remove", [this, p] { studio.removePlugin (*p); refresh(); });
                    m.showMenuAsync ({});
                    return;
                }

                if (mixer.onOpenEffect) mixer.onOpenEffect (*p);
                return;
            }
        }

    private:
        juce::Rectangle<int> slotBounds (int i) const { return { 8, 30 + i * 26, getWidth() - 16, 23 }; }
        int sendsTop() const                          { return 30 + Studio::maxEffectSlots * 26 + 30; }

        void showSendMenu()
        {
            juce::PopupMenu m;
            for (int i = 1; i <= Studio::numInserts; ++i)
                if (i != mixer.selected)
                    m.addItem (studio.getInsertName (i), [this, i] { studio.addSend (mixer.selected, i); refresh(); });
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addSendButton));
        }

        Mixer& mixer;
        Studio& studio;
        juce::Array<te::Plugin*> effects;
        juce::OwnedArray<juce::Slider> sendKnobs;
        juce::Array<te::AutomatableParameter::Ptr> sendParams;
        juce::TextButton addSendButton { "+ Add send" };
    };

    //==========================================================================
    void select (int index)
    {
        selected = index;
        for (auto* s : strips)
            s->repaint();
        slots.refresh();
    }

    void rebuild()
    {
        strips.clear();
        for (int i = 0; i <= Studio::numInserts; ++i)
            stripHolder.addAndMakeVisible (strips.add (new Strip (*this, i)));
        layoutStrips();
        slots.refresh();
    }

    void layoutStrips()
    {
        const int h = juce::jmax (200, viewport.getMaximumVisibleHeight());
        stripHolder.setSize (strips.size() * stripWidth, h);
        for (int i = 0; i < strips.size(); ++i)
            strips[i]->setBounds (i * stripWidth, 0, stripWidth, h);
    }

    void timerCallback() override
    {
        if (studio.getEditGeneration() != lastGeneration)
        {
            lastGeneration = studio.getEditGeneration();
            rebuild();
        }
        for (auto* s : strips)
            s->sync();

        // Effects can change from undo/redo.
        const auto fx = studio.getInsertEffects (selected);
        if (fx.size() != lastEffectCount)
        {
            lastEffectCount = fx.size();
            slots.refresh();
        }
        slots.repaint();
    }

    static constexpr int stripWidth = 62;

    Studio& studio;
    int selected = 0, lastGeneration = 0, lastEffectCount = -1;
    juce::Viewport viewport;
    juce::Component stripHolder;
    juce::OwnedArray<Strip> strips;
    Slots slots { *this };
};
