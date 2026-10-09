#pragma once

#include "Widgets.h"
#include "ParamKnob.h"

// Shows every automatable parameter of a Tracktion plugin as a knob grid.
// Works for any built-in plugin until it gets a custom GUI (Phase 9).
class GenericPluginEditor : public juce::Component, private juce::Timer
{
public:
    GenericPluginEditor (Studio& s, te::Plugin& p) : plugin (&p)
    {
        for (auto* param : p.getAutomatableParameters())
        {
            auto* cell = cells.add (new Cell (s, *param));
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
        Cell (Studio& s, te::AutomatableParameter& p) : param (&p)
        {
            knob.bind (s, &p);
            addAndMakeVisible (knob);
        }

        void sync() { knob.sync(); }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (11.0f));
            g.drawFittedText (param->getParameterName(), getLocalBounds().removeFromBottom (26).reduced (2, 0),
                              juce::Justification::centredTop, 2);
        }

        void resized() override { knob.setBounds (getLocalBounds().removeFromTop (getHeight() - 26).reduced (10, 4)); }

        te::AutomatableParameter::Ptr param;
        ParamKnob knob;
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
