#pragma once

#include "Theme.h"

// A floating window inside the workspace, like FL's Channel Rack / Playlist / Mixer windows.
// Drag by the title bar, resize from the corner, close with the x.
class Panel : public juce::Component
{
public:
    Panel (const juce::String& panelTitle, std::unique_ptr<juce::Component> contentComponent)
        : title (panelTitle), content (std::move (contentComponent))
    {
        addAndMakeVisible (*content);
        addAndMakeVisible (resizer);
        constrainer.setMinimumSize (200, 120);
        constrainer.setMinimumOnscreenAmounts (titleHeight, 60, 40, 60);
        addMouseListener (this, true); // any click inside brings the window to front
    }

    juce::Component& getContent() { return *content; }
    const juce::String& getTitle() const { return title; }
    void setTitle (const juce::String& t) { title = t; repaint(); }

    void toggle()
    {
        setVisible (! isVisible());
        if (isVisible())
            toFront (false);
    }

    void show()
    {
        setVisible (true);
        toFront (false);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (Theme::panel);
        g.fillRect (r);

        auto bar = r.removeFromTop (titleHeight);
        const bool front = getParentComponent() != nullptr
                           && getParentComponent()->getChildren().getLast() == this;
        g.setColour (front ? Theme::titleActive : Theme::titleBar);
        g.fillRect (bar);

        g.setColour (Theme::text);
        g.setFont (Theme::font (13.0f, true));
        g.drawText (title, bar.reduced (8, 0), juce::Justification::centredLeft);

        g.setColour (closeHover ? Theme::accent : Theme::textDim);
        g.drawText (juce::String::fromUTF8 ("\xc3\x97"), getCloseArea(), juce::Justification::centred); // ×

        g.setColour (Theme::outline);
        g.drawRect (getLocalBounds());
    }

    void resized() override
    {
        content->setBounds (getLocalBounds().reduced (1).withTrimmedTop (titleHeight - 1));
        resizer.setBounds (getWidth() - 14, getHeight() - 14, 14, 14);
        resizer.toFront (false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        toFront (false);
        if (getParentComponent() != nullptr)
            getParentComponent()->repaint();

        if (e.eventComponent == this && e.y < titleHeight && ! getCloseArea().contains (e.getPosition()))
        {
            draggingTitle = true;
            dragger.startDraggingComponent (this, e.getEventRelativeTo (this));
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingTitle)
            dragger.dragComponent (this, e.getEventRelativeTo (this), &constrainer);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == this && ! draggingTitle && getCloseArea().contains (e.getPosition()))
            setVisible (false);
        draggingTitle = false;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool hover = e.eventComponent == this && getCloseArea().contains (e.getPosition());
        if (hover != closeHover)
        {
            closeHover = hover;
            repaint (getCloseArea());
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (closeHover)
        {
            closeHover = false;
            repaint (getCloseArea());
        }
    }

    static constexpr int titleHeight = 24;

private:
    juce::Rectangle<int> getCloseArea() const { return { getWidth() - titleHeight, 0, titleHeight, titleHeight }; }

    juce::String title;
    std::unique_ptr<juce::Component> content;
    juce::ComponentDragger dragger;
    juce::ComponentBoundsConstrainer constrainer;
    juce::ResizableCornerComponent resizer { this, &constrainer };
    bool draggingTitle = false, closeHover = false;
};
