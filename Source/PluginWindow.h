#pragma once

#include "Theme.h"

// Native window hosting a third-party plugin's own editor (VST3 / AU).
// Adapted from Tracktion Engine's example PluginWindow.
class PluginWindow : public juce::DocumentWindow
{
public:
    explicit PluginWindow (te::Plugin& p)
        : DocumentWindow (p.getName(), Theme::panelDark, DocumentWindow::closeButton, true), plugin (p)
    {
        getConstrainer()->setMinimumOnscreenAmounts (0x10000, 50, 30, 50);
        setResizeLimits (100, 50, 4000, 4000);
        setUsingNativeTitleBar (true);
        setAlwaysOnTop (true); // FL keeps plugin windows above the main window
        recreateEditor();
        setBoundsConstrained (getLocalBounds() + plugin.windowState->choosePositionForPluginWindow());
        updateStoredBounds = true;
    }

    ~PluginWindow() override
    {
        updateStoredBounds = false;
        plugin.edit.flushPluginStateIfNeeded (plugin);
        setEditor (nullptr);
    }

    static std::unique_ptr<juce::Component> create (te::Plugin& plugin)
    {
        if (auto external = dynamic_cast<te::ExternalPlugin*> (&plugin))
            if (external->getAudioPluginInstance() == nullptr)
                return {};

        auto w = std::make_unique<PluginWindow> (plugin);
        if (w->editor == nullptr)
            return {};

        w->setVisible (true);
        w->toFront (false);
        return w;
    }

    void recreateEditor()
    {
        setEditor (nullptr);
        setEditor (plugin.createEditor());
    }

    void recreateEditorAsync()
    {
        setEditor (nullptr);
        juce::Timer::callAfterDelay (50, [sp = juce::Component::SafePointer<PluginWindow> (this)]
        {
            if (sp != nullptr)
                sp->recreateEditor();
        });
    }

private:
    void setEditor (std::unique_ptr<te::Plugin::EditorComponent> newEditor)
    {
        JUCE_AUTORELEASEPOOL
        {
            setConstrainer (nullptr);
            editor.reset();
            if (newEditor != nullptr)
            {
                editor = std::move (newEditor);
                setContentNonOwned (editor.get(), true);
            }
            setResizable (editor == nullptr || editor->allowWindowResizing(), false);
            if (editor != nullptr && editor->allowWindowResizing())
                setConstrainer (editor->getBoundsConstrainer());
        }
    }

    void moved() override
    {
        if (updateStoredBounds)
        {
            plugin.windowState->lastWindowBounds = getBounds();
            plugin.edit.pluginChanged (plugin);
        }
    }

    void closeButtonPressed() override { plugin.windowState->closeWindowExplicitly(); }
    float getDesktopScaleFactor() const override { return 1.0f; }

    te::Plugin& plugin;
    std::unique_ptr<te::Plugin::EditorComponent> editor;
    bool updateStoredBounds = false;
};

struct StudioUIBehaviour : public te::UIBehaviour
{
    std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& state) override
    {
        if (auto ws = dynamic_cast<te::Plugin::WindowState*> (&state))
            return PluginWindow::create (ws->plugin);
        return {};
    }

    void recreatePluginWindowContentAsync (te::Plugin& p) override
    {
        if (auto* w = dynamic_cast<PluginWindow*> (p.windowState->pluginWindow.get()))
            return w->recreateEditorAsync();
        te::UIBehaviour::recreatePluginWindowContentAsync (p);
    }
};

struct StudioEngineBehaviour : public te::EngineBehaviour
{
    // Scans plugins in a child process, so a crashing plugin can't take the app down.
    bool canScanPluginsOutOfProcess() override { return true; }
};
