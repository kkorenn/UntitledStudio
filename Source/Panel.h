#pragma once

#include "Theme.h"

// A floating window inside the workspace, like FL's Channel Rack / Playlist / Mixer windows.
// Drag by the title bar (snaps to edges), resize from the corner, close with the x,
// double-click the title to maximize, right-click it to detach into its own window.
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

    ~Panel() override
    {
        // The detached window only borrows the content.
        if (detached != nullptr)
            detached->clearContentComponent();
    }

    // Keys typed in a detached window go to the main window's shortcuts.
    static inline std::function<bool (const juce::KeyPress&)> forwardKey;
    static inline std::function<bool()> forwardKeyState;

    juce::Component& getContent() { return *content; }
    const juce::String& getTitle() const { return title; }
    void setTitle (const juce::String& t)
    {
        title = t;
        repaint();
        if (detached != nullptr)
            detached->setName (t);
    }

    // Visible either in the workspace or as a detached window.
    bool isOpen() const { return detached != nullptr ? detached->isVisible() : isVisible(); }
    bool isDetached() const { return detached != nullptr; }
    bool isDetachedAndActive() const { return detached != nullptr && detached->isActiveWindow(); }

    void toggle()
    {
        if (isOpen()) close();
        else          show();
    }

    void show()
    {
        if (detached != nullptr)
        {
            detached->setVisible (true);
            detached->toFront (true);
            return;
        }
        setVisible (true);
        toFront (false);
    }

    void close()
    {
        if (detached != nullptr) detached->setVisible (false);
        else                     setVisible (false);
    }

    //==========================================================================
    // FL: double-click the title bar to maximize / restore inside the workspace.
    bool isMaximised() const { return ! restoreBounds.isEmpty(); }

    void setMaximised (bool shouldBe)
    {
        if (shouldBe == isMaximised() || getParentComponent() == nullptr)
            return;
        if (shouldBe)
        {
            restoreBounds = getBounds();
            setBounds (getParentComponent()->getLocalBounds());
        }
        else
        {
            setBounds (std::exchange (restoreBounds, {}));
        }
    }

    // Bounds to remember across launches (the un-maximized ones).
    juce::Rectangle<int> getNormalBounds() const { return isMaximised() ? restoreBounds : getBounds(); }

    void parentSizeChanged() override
    {
        if (isMaximised())
            setBounds (getParentComponent()->getLocalBounds());
    }

    //==========================================================================
    // Detach: the content moves into a native window; closing that window docks it back.
    void setDetached (bool shouldBe)
    {
        if (shouldBe == isDetached())
            return;

        if (shouldBe)
        {
            const auto size = content->getBounds();
            setMaximised (false);
            removeChildComponent (content.get());
            detached = std::make_unique<DetachedWindow> (*this);
            detached->setContentNonOwned (content.get(), false);
            detached->setResizable (true, false);
            detached->setResizeLimits (200, 120, 8000, 8000);
            detached->setSize (juce::jmax (200, size.getWidth()), juce::jmax (120, size.getHeight()));
            if (auto* parent = getParentComponent())
                detached->setTopLeftPosition (parent->localPointToGlobal (getPosition()) + juce::Point<int> (30, 30));
            setVisible (false);
            detached->setVisible (true);
            detached->toFront (true);
        }
        else
        {
            detached->clearContentComponent();
            detached = nullptr;
            addAndMakeVisible (*content);
            resized();
            show();
        }
    }

    //==========================================================================
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
        g.drawText (title, bar.reduced (8, 0).withTrimmedRight (titleHeight), juce::Justification::centredLeft, true);

        g.setColour (closeHover ? Theme::accent : Theme::textDim);
        g.drawText (juce::String::fromUTF8 ("\xc3\x97"), getCloseArea(), juce::Justification::centred); // ×

        g.setColour (Theme::outline);
        g.drawRect (getLocalBounds());
    }

    void resized() override
    {
        if (! isDetached())
            content->setBounds (getLocalBounds().reduced (1).withTrimmedTop (titleHeight - 1));
        resizer.setBounds (getWidth() - 14, getHeight() - 14, 14, 14);
        resizer.toFront (false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        toFront (false);
        if (getParentComponent() != nullptr)
            getParentComponent()->repaint();

        const bool onTitle = e.eventComponent == this && e.y < titleHeight && ! getCloseArea().contains (e.getPosition());
        if (onTitle && e.mods.isPopupMenu())
            return showTitleMenu();

        if (onTitle && ! isMaximised())
        {
            // Screen-based: the panel also listens to its own events, so each drag can arrive twice;
            // start + mouse offset gives the same answer however often it runs (no jumping).
            draggingTitle = true;
            dragStartPos = getPosition();
            dragStartMouse = e.getScreenPosition();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingTitle)
            setTopLeftPosition (snapped (dragStartPos + (e.getScreenPosition() - dragStartMouse)));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == this && ! draggingTitle && ! e.mods.isPopupMenu() && getCloseArea().contains (e.getPosition()))
            setVisible (false);
        draggingTitle = false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == this && e.y < titleHeight && ! getCloseArea().contains (e.getPosition()))
            setMaximised (! isMaximised());
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

    static constexpr int titleHeight = 24, snapDistance = 8;

private:
    class DetachedWindow : public juce::DocumentWindow
    {
    public:
        explicit DetachedWindow (Panel& p)
            : juce::DocumentWindow (p.getTitle(), Theme::panelDark, juce::DocumentWindow::closeButton), panel (p)
        {
            setUsingNativeTitleBar (true);
            setWantsKeyboardFocus (true);
        }

        void closeButtonPressed() override
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Panel> (&panel)]
            {
                if (safe != nullptr) safe->setDetached (false);
            });
        }

        bool keyPressed (const juce::KeyPress& key) override { return forwardKey != nullptr && forwardKey (key); }
        bool keyStateChanged (bool) override                  { return forwardKeyState != nullptr && forwardKeyState(); }

    private:
        Panel& panel;
    };

    void showTitleMenu()
    {
        juce::PopupMenu m;
        m.addItem (isMaximised() ? "Restore" : "Maximize", [this] { setMaximised (! isMaximised()); });
        m.addItem ("Detach (own window)", [this] { setDetached (true); });
        m.addSeparator();
        m.addItem ("Close", [this] { setVisible (false); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    // Snaps a proposed position's edges to the workspace edges and other windows (within a few pixels),
    // and keeps the title bar reachable inside the workspace.
    juce::Point<int> snapped (juce::Point<int> pos) const
    {
        auto* parent = getParentComponent();
        if (parent == nullptr)
            return pos;

        juce::Array<int> xs { 0, parent->getWidth() }, ys { 0, parent->getHeight() };
        for (auto* c : parent->getChildren())
            if (c != this && c->isVisible() && dynamic_cast<Panel*> (c) != nullptr)
            {
                xs.addArray ({ c->getX(), c->getRight() });
                ys.addArray ({ c->getY(), c->getBottom() });
            }

        auto best = [] (const juce::Array<int>& lines, int a, int b)
        {
            int shift = 0, distance = snapDistance + 1;
            for (int line : lines)
                for (int edge : { a, b })
                    if (std::abs (line - edge) < distance)
                    {
                        distance = std::abs (line - edge);
                        shift = line - edge;
                    }
            return shift;
        };
        pos += { best (xs, pos.x, pos.x + getWidth()), best (ys, pos.y, pos.y + getHeight()) };
        pos.x = juce::jlimit (60 - getWidth(), parent->getWidth() - 60, pos.x);
        pos.y = juce::jlimit (0, juce::jmax (0, parent->getHeight() - titleHeight), pos.y);
        return pos;
    }

    juce::Rectangle<int> getCloseArea() const { return { getWidth() - titleHeight, 0, titleHeight, titleHeight }; }

    juce::String title;
    std::unique_ptr<juce::Component> content;
    std::unique_ptr<DetachedWindow> detached; // after content: destroyed first
    juce::Point<int> dragStartPos, dragStartMouse;
    juce::ComponentBoundsConstrainer constrainer;
    juce::ResizableCornerComponent resizer { this, &constrainer };
    juce::Rectangle<int> restoreBounds;
    bool draggingTitle = false, closeHover = false;
};
