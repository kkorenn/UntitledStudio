#pragma once

#include "Widgets.h"

// Shows every automatable parameter of a Tracktion plugin as a knob grid.
// Works for any built-in plugin until it gets a custom GUI (Phase 9).
class GenericPluginEditor : public juce::Component, private juce::Timer
{
public:
    explicit GenericPluginEditor (te::Plugin& p) : plugin (&p)
    {
        for (auto* param : p.getAutomatableParameters())
        {
            auto* cell = cells.add (new Cell (*param));
            grid.addAndMakeVisible (cell);
        }

        viewport.setViewedComponent (&grid, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
        Widgets::noFocus (*this);
        startTimerHz (15);
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        const int cellW = 74, cellH = 78;
        const int columns = juce::jmax (1, (getWidth() - viewport.getScrollBarThickness()) / cellW);
        const int rows = (cells.size() + columns - 1) / columns;
        grid.setSize (columns * cellW, rows * cellH + 8);

        for (int i = 0; i < cells.size(); ++i)
            cells[i]->setBounds ((i % columns) * cellW, (i / columns) * cellH + 4, cellW, cellH);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panelDark); }

private:
    struct Cell : public juce::Component
    {
        explicit Cell (te::AutomatableParameter& p) : param (&p)
        {
            Widgets::setupKnob (knob, p.getParameterName(), p.valueRange.start < 0.0f && p.valueRange.end > 0.0f);
            knob.setNormalisableRange ({ p.valueRange.start, p.valueRange.end, p.valueRange.interval, p.valueRange.skew });
            knob.setValue (p.getCurrentValue(), juce::dontSendNotification);
            if (auto def = p.getDefaultValue())
                knob.setDoubleClickReturnValue (true, *def);

            knob.onDragStart    = [this] { param->parameterChangeGestureBegin(); };
            knob.onDragEnd      = [this] { param->parameterChangeGestureEnd(); };
            knob.onValueChange  = [this] { param->setParameter ((float) knob.getValue(), juce::sendNotificationSync); updateHint(); };
            addAndMakeVisible (knob);
            updateHint();
        }

        void updateHint() { knob.setTooltip (param->getParameterName() + ": " + param->getCurrentValueAsStringWithLabel()); }

        void sync()
        {
            if (! knob.isMouseButtonDown() && std::abs (knob.getValue() - param->getCurrentValue()) > 1.0e-6)
            {
                knob.setValue (param->getCurrentValue(), juce::dontSendNotification);
                updateHint();
            }
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (11.0f));
            g.drawFittedText (param->getParameterName(), getLocalBounds().removeFromBottom (26).reduced (2, 0),
                              juce::Justification::centredTop, 2);
        }

        void resized() override { knob.setBounds (getLocalBounds().removeFromTop (getHeight() - 26).reduced (10, 4)); }

        te::AutomatableParameter::Ptr param;
        juce::Slider knob;
    };

    void timerCallback() override
    {
        for (auto* c : cells)
            c->sync();
    }

    te::Plugin::Ptr plugin;
    juce::Viewport viewport;
    juce::Component grid;
    juce::OwnedArray<Cell> cells;
};
