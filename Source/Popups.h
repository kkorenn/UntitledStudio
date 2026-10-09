#pragma once

#include "Theme.h"

// F8: type to find a synth preset, instrument or plugin; Return / double-click adds it as a channel.
class SoundPicker : public juce::Component, private juce::ListBoxModel, private juce::KeyListener
{
public:
    struct Item { juce::String name, group; std::function<void()> add; };

    explicit SoundPicker (std::vector<Item> allItems) : items (std::move (allItems))
    {
        search.setTextToShowWhenEmpty ("Search sounds, instruments, plugins...", Theme::textDim);
        search.onTextChange = [this] { filter(); };
        search.onReturnKey = [this] { pick (list.getSelectedRow()); };
        search.onEscapeKey = [this] { dismiss(); };
        search.addKeyListener (this);
        addAndMakeVisible (search);

        list.setModel (this);
        list.setRowHeight (22);
        list.setColour (juce::ListBox::backgroundColourId, Theme::panelDark);
        addAndMakeVisible (list);
        filter();
        setSize (360, 420);
    }

    void visibilityChanged() override
    {
        if (isShowing())
            search.grabKeyboardFocus();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        search.setBounds (r.removeFromTop (28));
        list.setBounds (r.withTrimmedTop (4));
    }

private:
    void filter()
    {
        shown.clear();
        const auto words = juce::StringArray::fromTokens (search.getText(), true);
        for (int i = 0; i < (int) items.size(); ++i)
        {
            bool match = true;
            for (auto& w : words)
                match &= (items[(size_t) i].name + " " + items[(size_t) i].group).containsIgnoreCase (w);
            if (match)
                shown.add (i);
        }
        list.updateContent();
        list.selectRow (0);
        list.repaint();
    }

    void pick (int row)
    {
        if (! juce::isPositiveAndBelow (row, shown.size()))
            return;
        auto add = items[(size_t) shown[row]].add;
        dismiss();
        if (add) add();
    }

    void dismiss()
    {
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
    }

    using juce::Component::keyPressed;
    bool keyPressed (const juce::KeyPress& key, juce::Component*) override
    {
        const int delta = key.isKeyCode (juce::KeyPress::downKey) ? 1 : key.isKeyCode (juce::KeyPress::upKey) ? -1 : 0;
        if (delta == 0)
            return false;
        const int row = juce::jlimit (0, juce::jmax (0, shown.size() - 1), list.getSelectedRow() + delta);
        list.selectRow (row);
        list.scrollToEnsureRowIsOnscreen (row);
        return true;
    }

    int getNumRows() override { return shown.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, shown.size()))
            return;
        const auto& item = items[(size_t) shown[row]];
        if (selected)
        {
            g.setColour (Theme::accent.withAlpha (0.85f));
            g.fillRect (0, 0, w, h);
        }
        g.setFont (Theme::font (13.0f));
        g.setColour (selected ? juce::Colours::black : Theme::text);
        g.drawText (item.name, 8, 0, w - 130, h, juce::Justification::centredLeft, true);
        g.setColour (selected ? juce::Colours::black.withAlpha (0.6f) : Theme::textDim);
        g.drawText (item.group, w - 125, 0, 117, h, juce::Justification::centredRight, true);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { pick (row); }

    std::vector<Item> items;
    juce::Array<int> shown;
    juce::TextEditor search;
    juce::ListBox list;
};

//==============================================================================
// OPTIONS > Keyboard shortcuts.
namespace Shortcuts
{
    struct Entry { const char* keys; const char* what; };

    inline const std::vector<Entry>& list()
    {
        static const std::vector<Entry> entries {
            { "F5", "Playlist" },                     { "F6", "Channel rack" },
            { "F7", "Piano roll" },                   { "F8", "Add a sound / instrument / plugin (search)" },
            { "F9", "Mixer" },                        { "F10", "Audio settings" },
            { "F12", "Close all windows" },           { "Alt+F8", "Browser" },
            { "Tab", "Bring the next window to front" },
            { "Space", "Play / stop" },               { "L", "Pattern / song mode" },
            { "Numpad + / -, Ctrl+Up / Down", "Next / previous pattern" },
            { "Ctrl+N / O / S", "New / open / save" }, { "Ctrl+Shift+S", "Save as" },
            { "Ctrl+R", "Export" },                   { "Ctrl+Z", "Undo" },
            { "Ctrl+Y, Ctrl+Shift+Z", "Redo" },
            { "Z S X D C ... / Q 2 W 3 E ...", "Play notes on the selected channel (typing keyboard)" },
            { "P B D E C (T)", "Piano roll / playlist tools: pencil, brush, delete, select, slice (mute)" },
            { "Ctrl+A / C / X / V", "Select all / copy / cut / paste" },
            { "Ctrl+B / Ctrl+Q", "Duplicate / quantize" },
            { "Up / Down, Shift", "Transpose selected notes (semitone / octave)" },
            { "Left / Right", "Move selected notes by the snap" },
            { "Del", "Delete selection" },
            { "Double-click a window title", "Maximize / restore" },
            { "Right-click a window title", "Detach into its own window (close it to dock back)" },
        };
        return entries;
    }

    class Help : public juce::Component
    {
    public:
        Help() { setSize (620, rowHeight * (int) list().size() + 24); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::panelDark);
            int y = 12;
            for (auto& e : list())
            {
                g.setFont (Theme::font (13.0f, true));
                g.setColour (Theme::accent);
                g.drawText (juce::String (e.keys).replace ("Ctrl", ctrlName()), 16, y, 220, rowHeight, juce::Justification::centredLeft, true);
                g.setFont (Theme::font (13.0f));
                g.setColour (Theme::text);
                g.drawText (e.what, 244, y, getWidth() - 260, rowHeight, juce::Justification::centredLeft, true);
                y += rowHeight;
            }
        }

    private:
        static juce::String ctrlName()
        {
           #if JUCE_MAC
            return "Cmd";
           #else
            return "Ctrl";
           #endif
        }

        static constexpr int rowHeight = 22;
    };
}
