#pragma once

#include "Studio.h"
#include "Widgets.h"
#include "Panel.h"

// FL-style piano roll for the selected channel in the current pattern.
//   Pencil (P): click empty = add note (then drag to move), drag note = move, drag right edge = resize
//   Brush (B): drag to paint notes    Delete (D): click/drag to erase    Select (E): drag a box
//   Slice (C): click a note to cut it  Right-click always erases. Ctrl+drag = box select.
class PianoRoll : public juce::Component, private juce::Timer
{
public:
    enum class Tool { pencil, brush, erase, select, slice };

    explicit PianoRoll (Studio& s) : studio (s)
    {
        const std::pair<Tool, const char*> tools[] = {
            { Tool::pencil, "Pencil (P): draw, move and resize notes" },
            { Tool::brush,  "Brush (B): drag to paint notes" },
            { Tool::erase,  "Delete (D): click or drag over notes to erase" },
            { Tool::select, "Select (E): drag a box to select notes" },
            { Tool::slice,  "Slice (C): click a note to cut it in two" },
        };
        for (auto& [toolType, hintText] : tools)
        {
            auto* b = toolButtons.add (new ToolButton (toolType, hintText));
            b->onClick = [this, t = toolType] { setTool (t); };
            addAndMakeVisible (b);
        }

        snapBox.addItem ("Main", 1);
        snapBox.addItemList (Studio::snapNames(), 2);
        snapBox.setSelectedItemIndex (0, juce::dontSendNotification);
        snapBox.setTooltip ("Snap to grid (Main = follow the main snap in the top bar)");
        addAndMakeVisible (snapBox);

        chordBox.addItem ("No chord", 1);
        for (int i = 0; i < (int) chords().size(); ++i)
            chordBox.addItem (chords()[(size_t) i].first, i + 2);
        chordBox.setSelectedId (1, juce::dontSendNotification);
        chordBox.setTooltip ("Chord stamp: new notes become this chord");
        addAndMakeVisible (chordBox);

        scaleRootBox.addItemList ({ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 1);
        scaleRootBox.setSelectedItemIndex (0, juce::dontSendNotification);
        scaleRootBox.setTooltip ("Scale root (for highlighting)");
        scaleRootBox.onChange = [this] { grid.repaint(); keys.repaint(); };
        addAndMakeVisible (scaleRootBox);

        scaleBox.addItem ("No scale", 1);
        for (int i = 0; i < (int) scales().size(); ++i)
            scaleBox.addItem (scales()[(size_t) i].first, i + 2);
        scaleBox.setSelectedId (1, juce::dontSendNotification);
        scaleBox.setTooltip ("Highlight a scale in the piano roll");
        scaleBox.onChange = [this] { grid.repaint(); keys.repaint(); };
        addAndMakeVisible (scaleBox);

        toolsMenuButton.setTooltip ("Quantize, humanize, select...");
        toolsMenuButton.onClick = [this] { showToolsMenu(); };
        addAndMakeVisible (toolsMenuButton);

        viewport.setViewedComponent (&grid, false);
        viewport.setScrollBarsShown (true, true);
        viewport.onScroll = [this] { keys.repaint(); velocity.repaint(); };
        addAndMakeVisible (viewport);
        addAndMakeVisible (keys);
        addAndMakeVisible (velocity);

        setTool (Tool::pencil);
        Widgets::noFocus (*this);
        updateGridSize();
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PianoRoll> (this)]
        {
            if (safe != nullptr) // start around C5, like FL
                safe->viewport.setViewPosition (0, safe->keyToY (Studio::defaultRootNote + 14));
        });
        startTimerHz (30);
    }

    juce::String getTitle() const
    {
        return "Piano roll - " + studio.getChannel (studio.getSelectedChannel())[IDs::name].toString();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        g.setColour (Theme::outline);
        g.drawHorizontalLine (toolbarHeight - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (toolbarHeight).reduced (4, 4);
        for (auto* b : toolButtons)
            b->setBounds (bar.removeFromLeft (bar.getHeight() + 4).reduced (2, 0));
        bar.removeFromLeft (8);
        snapBox.setBounds (bar.removeFromLeft (110));
        bar.removeFromLeft (6);
        chordBox.setBounds (bar.removeFromLeft (110));
        bar.removeFromLeft (6);
        scaleRootBox.setBounds (bar.removeFromLeft (56));
        scaleBox.setBounds (bar.removeFromLeft (120));
        bar.removeFromLeft (6);
        toolsMenuButton.setBounds (bar.removeFromLeft (70));

        auto lane = r.removeFromBottom (velocityHeight);
        velocity.setBounds (lane.withTrimmedLeft (keysWidth));
        keys.setBounds (r.removeFromLeft (keysWidth));
        viewport.setBounds (r);
        updateGridSize();
    }

    // Called by the main window when the piano roll is the active window.
    bool handleKey (const juce::KeyPress& key)
    {
        const auto mods = key.getModifiers();
        const bool cmd = mods.isCommandDown();
        const int code = key.getKeyCode();

        if (! cmd && ! mods.isAltDown())
        {
            if (code == 'P') { setTool (Tool::pencil); return true; }
            if (code == 'B') { setTool (Tool::brush);  return true; }
            if (code == 'D') { setTool (Tool::erase);  return true; }
            if (code == 'E') { setTool (Tool::select); return true; }
            if (code == 'C') { setTool (Tool::slice);  return true; }
        }

        if (cmd && code == 'A') { selectAll(); return true; }
        if (cmd && code == 'C') { copySelection(); return true; }
        if (cmd && code == 'X') { copySelection(); deleteSelection ("Cut"); return true; }
        if (cmd && code == 'V') { paste(); return true; }
        if (cmd && code == 'B') { duplicateSelection(); return true; }
        if (cmd && code == 'Q') { quantize(); return true; }

        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) { deleteSelection ("Delete notes"); return true; }
        if (key == juce::KeyPress::escapeKey) { selection.clear(); grid.repaint(); return true; }
        if (key.isKeyCode (juce::KeyPress::upKey))    { transposeSelection (mods.isShiftDown() ? 12 : 1); return true; }
        if (key.isKeyCode (juce::KeyPress::downKey))  { transposeSelection (mods.isShiftDown() ? -12 : -1); return true; }
        if (key.isKeyCode (juce::KeyPress::rightKey)) { shiftSelection (getSnap() > 0 ? getSnap() : Studio::stepBeats); return true; }
        if (key.isKeyCode (juce::KeyPress::leftKey))  { shiftSelection (-(getSnap() > 0 ? getSnap() : Studio::stepBeats)); return true; }
        return false;
    }

    // Grid in beats; "Main" follows the top bar's main snap, "Line" = steps here.
    double getSnap() const
    {
        const int i = snapBox.getSelectedItemIndex();
        return studio.snapBeats (i <= 0 ? studio.mainSnap : i - 1, Studio::stepBeats);
    }

private:
    //==========================================================================
    static constexpr int toolbarHeight = 32, keysWidth = 56, velocityHeight = 70, rowHeight = 13;

    double snapFloor (double beat) const { const double s = getSnap(); return s > 0 ? std::floor (beat / s + 1.0e-6) * s : beat; }
    double snapRound (double beat) const { const double s = getSnap(); return s > 0 ? std::round (beat / s) * s : beat; }

    static const std::vector<std::pair<juce::String, std::vector<int>>>& chords()
    {
        static const std::vector<std::pair<juce::String, std::vector<int>>> c {
            { "Major", { 0, 4, 7 } }, { "Minor", { 0, 3, 7 } }, { "Dim", { 0, 3, 6 } }, { "Aug", { 0, 4, 8 } },
            { "Sus2", { 0, 2, 7 } }, { "Sus4", { 0, 5, 7 } }, { "Major 7", { 0, 4, 7, 11 } }, { "Minor 7", { 0, 3, 7, 10 } },
            { "Dom 7", { 0, 4, 7, 10 } }, { "Minor 9", { 0, 3, 7, 10, 14 } }, { "Power", { 0, 7, 12 } },
        };
        return c;
    }

    static const std::vector<std::pair<juce::String, std::vector<int>>>& scales()
    {
        static const std::vector<std::pair<juce::String, std::vector<int>>> s {
            { "Major", { 0, 2, 4, 5, 7, 9, 11 } }, { "Minor", { 0, 2, 3, 5, 7, 8, 10 } },
            { "Harmonic minor", { 0, 2, 3, 5, 7, 8, 11 } }, { "Dorian", { 0, 2, 3, 5, 7, 9, 10 } },
            { "Phrygian", { 0, 1, 3, 5, 7, 8, 10 } }, { "Mixolydian", { 0, 2, 4, 5, 7, 9, 10 } },
            { "Pentatonic major", { 0, 2, 4, 7, 9 } }, { "Pentatonic minor", { 0, 3, 5, 7, 10 } }, { "Blues", { 0, 3, 5, 6, 7, 10 } },
        };
        return s;
    }

    bool isInScale (int key) const
    {
        const int index = scaleBox.getSelectedId() - 2;
        if (index < 0)
            return true;
        const int degree = ((key - scaleRootBox.getSelectedItemIndex()) % 12 + 12) % 12;
        const auto& steps = scales()[(size_t) index].second;
        return std::find (steps.begin(), steps.end(), degree) != steps.end();
    }

    static bool isBlackKey (int key) { const int n = key % 12; return n == 1 || n == 3 || n == 6 || n == 8 || n == 10; }

    // Geometry -----------------------------------------------------------------
    int beatToX (double beat) const { return (int) std::round (beat * pxPerBeat); }
    double xToBeat (int x) const    { return juce::jmax (0.0, x / pxPerBeat); }
    int keyToY (int key) const      { return (127 - key) * rowHeight; }
    int yToKey (int y) const        { return juce::jlimit (0, 127, 127 - y / rowHeight); }

    juce::Rectangle<int> noteBounds (const juce::ValueTree& n) const
    {
        const int x = beatToX (n[IDs::start]);
        const int w = juce::jmax (3, beatToX ((double) n[IDs::start] + (double) n[IDs::length]) - x);
        return { x, keyToY (n[IDs::key]), w, rowHeight };
    }

    juce::ValueTree pattern() const { return studio.getCurrentPatternState(); }
    int channel() const             { return studio.getSelectedChannel(); }

    juce::Array<juce::ValueTree> channelNotes() const
    {
        juce::Array<juce::ValueTree> out;
        for (auto n : pattern())
            if ((int) n[IDs::channel] == channel())
                out.add (n);
        return out;
    }

    juce::ValueTree noteAt (juce::Point<int> p) const
    {
        const auto notes = channelNotes();
        for (int i = notes.size(); --i >= 0;)
            if (noteBounds (notes[i]).contains (p))
                return notes[i];
        return {};
    }

    void updateGridSize()
    {
        const double beats = studio.getPatternLengthBeats (pattern()) + studio.getBarBeats() * 4.0;
        grid.setSize (juce::jmax (viewport.getMaximumVisibleWidth(), beatToX (beats)), 128 * rowHeight);
    }

    void setTool (Tool t)
    {
        tool = t;
        for (auto* b : toolButtons)
            b->setToggleState (b->tool == t, juce::dontSendNotification);
        grid.setMouseCursor (t == Tool::erase ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    }

    // Editing --------------------------------------------------------------------
    void cleanSelection()
    {
        for (int i = selection.size(); --i >= 0;)
            if (! selection.getReference (i).getParent().isValid() || (int) selection.getReference (i)[IDs::channel] != channel())
                selection.remove (i);
    }

    juce::ValueTree createNote (double start, int key)
    {
        juce::ValueTree first;
        const int chordIndex = chordBox.getSelectedId() - 2;
        const std::vector<int> intervals = chordIndex >= 0 ? chords()[(size_t) chordIndex].second : std::vector<int> { 0 };
        selection.clear();

        for (int interval : intervals)
        {
            auto n = studio.addNote (pattern(), channel(), start, lastLength, key + interval, lastVelocity);
            selection.add (n);
            if (! first.isValid())
                first = n;
        }
        studio.previewNote (key, lastVelocity);
        return first;
    }

    void deleteNote (const juce::ValueTree& n)
    {
        if (n.getParent().isValid())
            pattern().removeChild (n, &studio.undo());
        selection.removeAllInstancesOf (n);
    }

    void selectAll()
    {
        selection = channelNotes();
        grid.repaint();
    }

    juce::Array<juce::ValueTree> targetNotes() const { return selection.isEmpty() ? channelNotes() : selection; }

    void deleteSelection (const juce::String& name)
    {
        studio.undo().beginNewTransaction (name);
        for (auto n : juce::Array<juce::ValueTree> (selection))
            deleteNote (n);
        selection.clear();
        grid.repaint();
    }

    void copySelection()
    {
        clipboard.clear();
        for (auto& n : selection)
            clipboard.add (n.createCopy());
    }

    void paste()
    {
        if (clipboard.isEmpty())
            return;
        studio.undo().beginNewTransaction ("Paste");
        selection.clear();
        for (auto& n : clipboard)
            selection.add (studio.addNote (pattern(), channel(), n[IDs::start], n[IDs::length], n[IDs::key], n[IDs::velocity]));
        grid.repaint();
    }

    // FL Ctrl+B: copy the selection right after itself.
    void duplicateSelection()
    {
        if (selection.isEmpty())
            return;

        double start = 1.0e9, end = 0.0;
        for (auto& n : selection)
        {
            start = juce::jmin (start, (double) n[IDs::start]);
            end = juce::jmax (end, (double) n[IDs::start] + (double) n[IDs::length]);
        }
        const double bar = studio.getBarBeats();
        const double offset = juce::jmax (bar, std::ceil ((end - start) / bar - 1.0e-6) * bar);

        studio.undo().beginNewTransaction ("Duplicate");
        juce::Array<juce::ValueTree> copies;
        for (auto& n : selection)
            copies.add (studio.addNote (pattern(), channel(), (double) n[IDs::start] + offset, n[IDs::length], n[IDs::key], n[IDs::velocity]));
        selection = copies;
        grid.repaint();
    }

    void quantize()
    {
        const double snap = getSnap() > 0 ? getSnap() : Studio::stepBeats;
        studio.undo().beginNewTransaction ("Quantize");
        for (auto n : targetNotes())
            n.setProperty (IDs::start, std::round ((double) n[IDs::start] / snap) * snap, &studio.undo());
    }

    void humanize()
    {
        juce::Random rng;
        studio.undo().beginNewTransaction ("Humanize");
        for (auto n : targetNotes())
        {
            n.setProperty (IDs::start, juce::jmax (0.0, (double) n[IDs::start] + (rng.nextDouble() - 0.5) * 0.03), &studio.undo());
            n.setProperty (IDs::velocity, juce::jlimit (1, 127, (int) n[IDs::velocity] + rng.nextInt ({ -12, 13 })), &studio.undo());
        }
    }

    void transposeSelection (int semis)
    {
        studio.undo().beginNewTransaction ("Transpose");
        for (auto n : selection)
            n.setProperty (IDs::key, juce::jlimit (0, 127, (int) n[IDs::key] + semis), &studio.undo());
        if (selection.size() == 1)
            studio.previewNote (selection[0][IDs::key]);
    }

    void shiftSelection (double beats)
    {
        studio.undo().beginNewTransaction ("Move notes");
        for (auto n : selection)
            n.setProperty (IDs::start, juce::jmax (0.0, (double) n[IDs::start] + beats), &studio.undo());
    }

    void showToolsMenu()
    {
        juce::PopupMenu m;
        m.addItem ("Quantize (Ctrl+Q)", [this] { quantize(); });
        m.addItem ("Humanize", [this] { humanize(); });
        m.addSeparator();
        m.addItem ("Select all (Ctrl+A)", [this] { selectAll(); });
        m.addItem ("Duplicate (Ctrl+B)", ! selection.isEmpty(), false, [this] { duplicateSelection(); });
        m.addItem ("Delete selected", ! selection.isEmpty(), false, [this] { deleteSelection ("Delete notes"); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (toolsMenuButton));
    }

    //==========================================================================
    struct ToolButton : public juce::Button
    {
        ToolButton (Tool t, const juce::String& hint) : juce::Button (hint), tool (t) { setTooltip (hint); }

        void paintButton (juce::Graphics& g, bool over, bool) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (getToggleState() ? Theme::accent : (over ? Theme::titleActive : Theme::panelDark));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (getToggleState() ? juce::Colours::black : Theme::text);
            g.setFont (Theme::font (13.0f, true));
            const char* letters[] = { "P", "B", "D", "E", "C" };
            g.drawText (letters[(int) tool], r, juce::Justification::centred);
        }

        Tool tool;
    };

    //==========================================================================
    struct Grid : public juce::Component
    {
        explicit Grid (PianoRoll& r) : roll (r) {}

        void paint (juce::Graphics& g) override
        {
            const auto clip = g.getClipBounds();
            const int firstKey = roll.yToKey (clip.getBottom()), lastKey = roll.yToKey (clip.getY());

            // Rows: black keys darker, out-of-scale rows darkest.
            for (int key = firstKey; key <= lastKey; ++key)
            {
                auto c = isBlackKey (key) ? juce::Colour (0xff2f363b) : juce::Colour (0xff384046);
                if (! roll.isInScale (key))
                    c = c.darker (0.45f);
                g.setColour (c);
                g.fillRect (clip.getX(), roll.keyToY (key), clip.getWidth(), rowHeight);
                if (key % 12 == 0)
                {
                    g.setColour (Theme::outline);
                    g.drawHorizontalLine (roll.keyToY (key) + rowHeight - 1, (float) clip.getX(), (float) clip.getRight());
                }
            }

            // Vertical lines: steps faint, beats medium, bars strong.
            const double bar = roll.studio.getBarBeats();
            for (double b = std::floor (roll.xToBeat (clip.getX()) / Studio::stepBeats) * Studio::stepBeats;
                 roll.beatToX (b) <= clip.getRight(); b += Studio::stepBeats)
            {
                const bool isBar = std::abs (std::fmod (b + 1.0e-6, bar)) < 1.0e-3;
                const bool isBeat = std::abs (std::fmod (b + 1.0e-6, 1.0)) < 1.0e-3;
                if (roll.pxPerBeat * Studio::stepBeats < 6.0 && ! isBeat)
                    continue;
                g.setColour (juce::Colours::black.withAlpha (isBar ? 0.55f : (isBeat ? 0.3f : 0.14f)));
                g.drawVerticalLine (roll.beatToX (b), (float) clip.getY(), (float) clip.getBottom());
            }

            // Pattern end.
            const int endX = roll.beatToX (roll.studio.getPatternLengthBeats (roll.pattern()));
            g.setColour (juce::Colours::black.withAlpha (0.25f));
            g.fillRect (endX, clip.getY(), juce::jmax (0, clip.getRight() - endX), clip.getHeight());

            // Ghost notes from other channels.
            for (auto n : roll.pattern())
            {
                if ((int) n[IDs::channel] == roll.channel())
                    continue;
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillRoundedRectangle (roll.noteBounds (n).toFloat().reduced (0.5f), 2.0f);
            }

            // Notes: brighter = louder; selected = pink like FL.
            for (auto n : roll.channelNotes())
            {
                const auto r = roll.noteBounds (n).toFloat().reduced (0.5f);
                const bool selected = roll.selection.contains (n);
                const float vel = (float) (int) n[IDs::velocity] / 127.0f;
                const auto base = selected ? juce::Colour (0xffff6f8e) : juce::Colour (0xff7fd36b);
                g.setColour (base.withMultipliedBrightness (0.55f + 0.45f * vel));
                g.fillRoundedRectangle (r, 2.0f);
                g.setColour (juce::Colours::black.withAlpha (0.5f));
                g.drawRoundedRectangle (r, 2.0f, 1.0f);
                if (r.getWidth() > 26)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.7f));
                    g.setFont (Theme::font (10.0f));
                    g.drawText (juce::MidiMessage::getMidiNoteName (n[IDs::key], true, true, 5), r.reduced (3, 0),
                                juce::Justification::centredLeft, false);
                }
            }

            // Box selection.
            if (! roll.lasso.isEmpty())
            {
                g.setColour (Theme::accent.withAlpha (0.15f));
                g.fillRect (roll.lasso);
                g.setColour (Theme::accent);
                g.drawRect (roll.lasso);
            }

            // Playhead.
            if (roll.studio.isPlaying() && ! roll.studio.isSongMode())
            {
                const double loop = juce::jmax (1.0e-3, roll.studio.getLoopEndBeats());
                const int x = roll.beatToX (std::fmod (roll.studio.getPositionBeats(), loop));
                g.setColour (Theme::accent);
                g.drawVerticalLine (x, (float) clip.getY(), (float) clip.getBottom());
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            roll.cleanSelection();
            mode = Mode::none;
            downBeat = roll.xToBeat (e.x);
            downKey = roll.yToKey (e.y);
            auto hit = roll.noteAt (e.getPosition());

            if (e.mods.isPopupMenu() || roll.tool == Tool::erase)
            {
                roll.studio.undo().beginNewTransaction ("Delete notes");
                mode = Mode::erase;
                eraseAt (e.getPosition());
                return;
            }

            if (roll.tool == Tool::select || e.mods.isCtrlDown() || (e.mods.isCommandDown() && ! hit.isValid()))
            {
                if (! hit.isValid() || roll.tool == Tool::select)
                {
                    mode = Mode::lasso;
                    if (! e.mods.isShiftDown())
                        roll.selection.clear();
                    lassoStart = e.getPosition();
                    return;
                }
            }

            if (roll.tool == Tool::slice)
            {
                if (hit.isValid())
                    slice (hit, roll.snapRound (downBeat));
                return;
            }

            roll.studio.undo().beginNewTransaction ("Edit notes");

            if (hit.isValid())
            {
                if (! roll.selection.contains (hit))
                {
                    if (! e.mods.isShiftDown())
                        roll.selection.clear();
                    roll.selection.add (hit);
                }

                roll.lastLength = hit[IDs::length];
                roll.lastVelocity = hit[IDs::velocity];
                const auto b = roll.noteBounds (hit);
                mode = e.x >= b.getRight() - juce::jmin (6, b.getWidth() / 3) ? Mode::resize : Mode::move;
                roll.studio.previewNote (hit[IDs::key], hit[IDs::velocity]);
            }
            else
            {
                roll.createNote (roll.snapFloor (downBeat), downKey);
                mode = roll.tool == Tool::brush ? Mode::brush : Mode::move;
                lastBrushBeat = roll.snapFloor (downBeat);
            }

            originals.clear();
            for (auto& n : roll.selection)
                originals.add ({ n, n[IDs::start], n[IDs::length], n[IDs::key] });
            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const double beat = roll.xToBeat (e.x);
            const int key = roll.yToKey (e.y);

            switch (mode)
            {
                case Mode::erase:
                    eraseAt (e.getPosition());
                    break;

                case Mode::lasso:
                {
                    roll.lasso = juce::Rectangle<int> (lassoStart, e.getPosition());
                    roll.selection.clear();
                    for (auto n : roll.channelNotes())
                        if (roll.noteBounds (n).intersects (roll.lasso))
                            roll.selection.add (n);
                    break;
                }

                case Mode::move:
                {
                    const double delta = roll.snapRound (beat - downBeat);
                    const int keyDelta = key - downKey;
                    for (auto& o : originals)
                    {
                        o.note.setProperty (IDs::start, juce::jmax (0.0, o.start + delta), &roll.studio.undo());
                        o.note.setProperty (IDs::key, juce::jlimit (0, 127, o.key + keyDelta), &roll.studio.undo());
                    }
                    if (keyDelta != lastKeyDelta && originals.size() == 1)
                        roll.studio.previewNote (originals[0].key + keyDelta, roll.lastVelocity);
                    lastKeyDelta = keyDelta;
                    break;
                }

                case Mode::resize:
                {
                    const double delta = roll.snapRound (beat - downBeat);
                    const double minLength = roll.getSnap() > 0 ? roll.getSnap() : 1.0 / 64.0;
                    for (auto& o : originals)
                        o.note.setProperty (IDs::length, juce::jmax (minLength, o.length + delta), &roll.studio.undo());
                    if (! originals.isEmpty())
                        roll.lastLength = originals[0].note[IDs::length];
                    break;
                }

                case Mode::brush:
                {
                    const double cell = roll.snapFloor (beat);
                    if (cell >= lastBrushBeat + roll.lastLength - 1.0e-6)
                    {
                        roll.createNote (cell, downKey);
                        lastBrushBeat = cell;
                    }
                    break;
                }

                case Mode::none:
                    break;
            }
            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            roll.lasso = {};
            mode = Mode::none;
            lastKeyDelta = 0;
            repaint();
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            if (roll.tool != Tool::pencil && roll.tool != Tool::brush)
                return;
            const auto hit = roll.noteAt (e.getPosition());
            const bool edge = hit.isValid() && e.x >= roll.noteBounds (hit).getRight() - 6;
            setMouseCursor (edge ? juce::MouseCursor::LeftRightResizeCursor
                                 : (hit.isValid() ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor));
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            // Ctrl/Cmd + wheel zooms horizontally around the mouse (FL behaviour).
            if (e.mods.isCtrlDown() || e.mods.isCommandDown())
            {
                const double beatAtMouse = roll.xToBeat (e.x);
                const int mouseInView = e.x - roll.viewport.getViewPositionX();
                roll.pxPerBeat = juce::jlimit (12.0, 600.0, roll.pxPerBeat * (w.deltaY > 0 ? 1.15 : 1.0 / 1.15));
                roll.updateGridSize();
                roll.viewport.setViewPosition (juce::jmax (0, roll.beatToX (beatAtMouse) - mouseInView), roll.viewport.getViewPositionY());
                repaint();
                return;
            }
            juce::Component::mouseWheelMove (e, w);
        }

    private:
        enum class Mode { none, move, resize, erase, lasso, brush };

        struct Original { juce::ValueTree note; double start, length; int key; };

        void eraseAt (juce::Point<int> p)
        {
            auto hit = roll.noteAt (p);
            if (hit.isValid())
                roll.deleteNote (hit);
        }

        void slice (juce::ValueTree n, double at)
        {
            const double start = n[IDs::start], length = n[IDs::length];
            if (at <= start + 1.0e-6 || at >= start + length - 1.0e-6)
                return;
            roll.studio.undo().beginNewTransaction ("Slice");
            n.setProperty (IDs::length, at - start, &roll.studio.undo());
            roll.studio.addNote (roll.pattern(), roll.channel(), at, start + length - at, n[IDs::key], n[IDs::velocity]);
        }

        PianoRoll& roll;
        Mode mode = Mode::none;
        double downBeat = 0.0, lastBrushBeat = 0.0;
        int downKey = 60, lastKeyDelta = 0;
        juce::Point<int> lassoStart;
        juce::Array<Original> originals;
    };

    //==========================================================================
    struct Keys : public juce::Component
    {
        explicit Keys (PianoRoll& r) : roll (r) {}

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::panelDark);
            const int offset = roll.viewport.getViewPositionY();
            for (int key = 0; key < 128; ++key)
            {
                const int y = roll.keyToY (key) - offset;
                if (y + rowHeight < 0 || y > getHeight())
                    continue;
                const bool black = isBlackKey (key);
                auto row = juce::Rectangle<int> (0, y, getWidth(), rowHeight);
                g.setColour (black ? juce::Colour (0xff1e2226) : juce::Colour (0xffe6e8ea));
                g.fillRect (black ? row.withWidth (getWidth() * 3 / 5) : row);
                if (! roll.isInScale (key))
                {
                    g.setColour (juce::Colours::black.withAlpha (0.25f));
                    g.fillRect (row);
                }
                g.setColour (juce::Colours::black.withAlpha (0.3f));
                g.drawHorizontalLine (y + rowHeight - 1, 0.0f, (float) getWidth());
                if (key % 12 == 0)
                {
                    g.setColour (juce::Colours::black);
                    g.setFont (Theme::font (10.0f, true));
                    g.drawText (juce::MidiMessage::getMidiNoteName (key, true, true, 5), row.reduced (3, 0), juce::Justification::centredRight);
                }
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            roll.studio.previewNote (roll.yToKey (e.y + roll.viewport.getViewPositionY()));
        }

        PianoRoll& roll;
    };

    //==========================================================================
    // Velocity lane: drag to set velocities (selected notes, or the notes under the mouse).
    struct VelocityLane : public juce::Component, public juce::SettableTooltipClient
    {
        explicit VelocityLane (PianoRoll& r) : roll (r) { setTooltip ("Velocity: drag to change how hard notes are hit"); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::panelDark);
            g.setColour (Theme::outline);
            g.drawHorizontalLine (0, 0.0f, (float) getWidth());
            const int offset = roll.viewport.getViewPositionX();
            for (auto n : roll.channelNotes())
            {
                const int x = roll.beatToX (n[IDs::start]) - offset;
                const int h = (int) ((getHeight() - 6) * (int) n[IDs::velocity] / 127.0);
                const bool selected = roll.selection.contains (n);
                g.setColour (selected ? juce::Colour (0xffff6f8e) : juce::Colour (0xff7fd36b));
                g.fillRect (x, getHeight() - h, 3, h);
                g.fillEllipse ((float) x - 2.0f, (float) (getHeight() - h) - 3.0f, 7.0f, 7.0f);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            roll.studio.undo().beginNewTransaction ("Velocity");
            mouseDrag (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const int vel = juce::jlimit (1, 127, (int) ((getHeight() - e.y) / (double) (getHeight() - 6) * 127.0));
            const double beat = roll.xToBeat (e.x + roll.viewport.getViewPositionX());
            const double tolerance = 4.0 / roll.pxPerBeat;
            for (auto n : roll.channelNotes())
            {
                const bool underMouse = std::abs ((double) n[IDs::start] - beat) <= tolerance;
                if (underMouse && (roll.selection.isEmpty() || roll.selection.contains (n)))
                    n.setProperty (IDs::velocity, vel, &roll.studio.undo());
            }
            roll.lastVelocity = vel;
        }

        PianoRoll& roll;
    };

    //==========================================================================
    void timerCallback() override
    {
        const bool playing = studio.isPlaying();
        if (studio.getChangeCount() != lastChangeCount || studio.getSelectedChannel() != lastChannel)
        {
            if (studio.getSelectedChannel() != lastChannel)
                selection.clear();
            lastChangeCount = studio.getChangeCount();
            lastChannel = studio.getSelectedChannel();
            cleanSelection();
            updateGridSize();
            grid.repaint();
            velocity.repaint();
            if (auto* panel = findParentComponentOfClass<Panel>())
                panel->setTitle (getTitle());
        }
        else if (playing || wasPlaying)
        {
            // Repaint just the playhead's old and new columns (a full repaint per frame is costly).
            const int x = playing && ! studio.isSongMode()
                              ? beatToX (std::fmod (studio.getPositionBeats(), juce::jmax (1.0e-3, studio.getLoopEndBeats()))) : -10;
            if (x != lastPlayheadX)
            {
                grid.repaint (lastPlayheadX - 1, 0, 3, grid.getHeight());
                grid.repaint (x - 1, 0, 3, grid.getHeight());
                lastPlayheadX = x;
            }
        }
        wasPlaying = playing;
    }

    int lastPlayheadX = -10;

    Studio& studio;
    Tool tool = Tool::pencil;
    double pxPerBeat = 80.0, lastLength = Studio::stepBeats;
    int lastVelocity = 100;
    juce::Array<juce::ValueTree> selection;
    juce::Rectangle<int> lasso;
    static inline juce::Array<juce::ValueTree> clipboard;

    juce::OwnedArray<ToolButton> toolButtons;
    juce::ComboBox snapBox, chordBox, scaleRootBox, scaleBox;
    juce::TextButton toolsMenuButton { "Tools" };
    Grid grid { *this };
    Keys keys { *this };
    VelocityLane velocity { *this };
    struct SyncViewport : juce::Viewport
    {
        std::function<void()> onScroll;
        void visibleAreaChanged (const juce::Rectangle<int>&) override { if (onScroll) onScroll(); }
    } viewport;
    int lastChangeCount = -1, lastChannel = -1;
    bool wasPlaying = false;
};
