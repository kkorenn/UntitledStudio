#pragma once

#include "Theme.h"

namespace Widgets
{
    // Buttons/combos normally steal keyboard focus on click, which would break the FL typing
    // keyboard and shortcuts. Call this on any component tree after adding children.
    inline void noFocus (juce::Component& root)
    {
        if (dynamic_cast<juce::TextEditor*> (&root) == nullptr)
        {
            root.setWantsKeyboardFocus (false);
            root.setMouseClickGrabsKeyboardFocus (false);
        }
        for (auto* child : root.getChildren())
            noFocus (*child);
    }

    // FL-style small knob. Tooltip shows in the hint bar.
    inline void setupKnob (juce::Slider& s, const juce::String& hint, bool bipolar = false)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        s.setMouseDragSensitivity (180);
        s.setVelocityModeParameters (0.6, 1, 0.05, true, juce::ModifierKeys::ctrlModifier); // Ctrl = fine
        s.getProperties().set ("bipolar", bipolar);
        s.setTooltip (hint);
    }

    // Numeric display you drag up/down to change (FL tempo display). Double-click to type.
    class DragNumber : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        std::function<void (double)> onChange;
        std::function<juce::String (double)> format = [] (double v) { return juce::String (v, 3); };

        DragNumber (double minV, double maxV, double dragStep) : minValue (minV), maxValue (maxV), step (dragStep) {}

        void setValue (double v)
        {
            if (! editor && std::abs (v - value) > 1.0e-9)
            {
                value = v;
                repaint();
            }
        }

        double getValue() const { return value; }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Theme::lcd);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);
            g.setColour (Theme::lcdText);
            g.setFont (Theme::font ((float) getHeight() * 0.62f, true));
            g.drawText (format (value), getLocalBounds().reduced (4, 0), juce::Justification::centred);
        }

        void mouseDown (const juce::MouseEvent&) override { dragStartValue = value; }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const double fine = e.mods.isCtrlDown() || e.mods.isCommandDown() ? 0.1 : 1.0;
            change (dragStartValue - e.getDistanceFromDragStartY() * step * fine);
        }

        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
        {
            change (value + (w.deltaY > 0 ? step : -step) * 4.0);
        }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            editor = std::make_unique<juce::TextEditor>();
            editor->setText (juce::String (value), false);
            editor->setJustification (juce::Justification::centred);
            editor->selectAll();
            editor->onReturnKey = [this] { change (editor->getText().getDoubleValue()); closeEditor(); };
            editor->onEscapeKey = editor->onFocusLost = [this] { closeEditor(); };
            addAndMakeVisible (*editor);
            editor->setBounds (getLocalBounds());
            editor->grabKeyboardFocus();
        }

    private:
        void change (double v)
        {
            v = juce::jlimit (minValue, maxValue, v);
            if (v != value)
            {
                value = v;
                repaint();
                if (onChange) onChange (v);
            }
        }

        void closeEditor()
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<DragNumber> (this)]
            {
                if (safe != nullptr)
                    safe->editor.reset();
            });
        }

        double value = 0.0, dragStartValue = 0.0;
        const double minValue, maxValue, step;
        std::unique_ptr<juce::TextEditor> editor;
    };

    // Toggle drawn as an FL-style LED / lit button.
    class LedButton : public juce::Button
    {
    public:
        explicit LedButton (const juce::String& hint, juce::Colour onColour = Theme::led)
            : juce::Button (hint), colourOn (onColour)
        {
            setClickingTogglesState (true);
            setTooltip (hint);
        }

        void paintButton (juce::Graphics& g, bool over, bool) override
        {
            auto r = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (getToggleState() ? colourOn : Theme::ledOff);
            g.fillRoundedRectangle (r, 2.0f);
            if (over)
            {
                g.setColour (juce::Colours::white.withAlpha (0.15f));
                g.fillRoundedRectangle (r, 2.0f);
            }
        }

        std::function<void()> onRightClick;

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && onRightClick)
                onRightClick();
            else
                juce::Button::mouseDown (e);
        }

    private:
        juce::Colour colourOn;
    };
}
