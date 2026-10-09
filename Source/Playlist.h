#pragma once

#include "Studio.h"
#include "Widgets.h"
#include "Panel.h"

// FL-style playlist: lanes of pattern clips and audio clips over a bar ruler.
//   Pencil (P): click empty = place current pattern, drag clip = move, drag edges = resize/trim
//   Brush (B): drag to paint clips   Delete (D) / right-click: erase   Select (E): box select
//   Slice (C): cut a clip   Mute (T): toggle clip mute   Ruler: click = jump, drag = loop region
class Playlist : public juce::Component,
                 public juce::DragAndDropTarget,
                 public juce::FileDragAndDropTarget,
                 private juce::Timer
{
public:
    enum class Tool { pencil, brush, erase, select, slice, mute };
    std::function<void()> onShowChannelRack;

    explicit Playlist (Studio& s) : studio (s)
    {
        const std::tuple<Tool, const char*, const char*> tools[] = {
            { Tool::pencil, "P", "Pencil (P): place, move and resize clips" },
            { Tool::brush,  "B", "Brush (B): drag to paint clips" },
            { Tool::erase,  "D", "Delete (D): click or drag over clips to erase" },
            { Tool::select, "E", "Select (E): drag a box to select clips" },
            { Tool::slice,  "C", "Slice (C): click a clip to cut it" },
            { Tool::mute,   "T", "Mute (T): click clips to mute/unmute" },
        };
        for (auto& [toolType, letter, hintText] : tools)
        {
            auto* b = toolButtons.add (new ToolButton (toolType, letter, hintText));
            b->onClick = [this, t = toolType] { setTool (t); };
            addAndMakeVisible (b);
        }

        snapBox.addItemList ({ "(none)", "Step", "1/2 beat", "Beat", "Bar" }, 1);
        snapBox.setSelectedItemIndex (4, juce::dontSendNotification);
        snapBox.setTooltip ("Snap to grid");
        addAndMakeVisible (snapBox);

        viewport.setViewedComponent (&grid, false);
        viewport.setScrollBarsShown (true, true);
        viewport.onScroll = [this] { ruler.repaint(); lanes.repaint(); };
        addAndMakeVisible (viewport);
        addAndMakeVisible (ruler);
        addAndMakeVisible (lanes);

        setTool (Tool::pencil);
        Widgets::noFocus (*this);
        updateGridSize();
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (12.0f));
        const auto pattern = studio.getCurrentPatternState();
        g.drawText ("Placing: " + pattern[IDs::name].toString(), patternLabelArea, juce::Justification::centredLeft, true);
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
        snapBox.setBounds (bar.removeFromLeft (100));
        bar.removeFromLeft (12);
        patternLabelArea = bar;

        auto rulerRow = r.removeFromTop (rulerHeight);
        ruler.setBounds (rulerRow.withTrimmedLeft (lanesWidth));
        lanes.setBounds (r.removeFromLeft (lanesWidth));
        viewport.setBounds (r);
        updateGridSize();
    }

    bool handleKey (const juce::KeyPress& key)
    {
        const auto mods = key.getModifiers();
        const int code = key.getKeyCode();
        if (! mods.isCommandDown() && ! mods.isAltDown())
        {
            if (code == 'P') { setTool (Tool::pencil); return true; }
            if (code == 'B') { setTool (Tool::brush);  return true; }
            if (code == 'D') { setTool (Tool::erase);  return true; }
            if (code == 'E') { setTool (Tool::select); return true; }
            if (code == 'C') { setTool (Tool::slice);  return true; }
            if (code == 'T') { setTool (Tool::mute);   return true; }
        }
        if (mods.isCommandDown() && code == 'A') { selection.clear(); for (auto c : studio.getPlaylist()) selection.add (c); grid.repaint(); return true; }
        if (mods.isCommandDown() && code == 'B') { duplicateSelection(); return true; }
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            studio.undo().beginNewTransaction ("Delete clips");
            for (auto c : juce::Array<juce::ValueTree> (selection))
                deleteClip (c);
            return true;
        }
        if (key == juce::KeyPress::escapeKey) { selection.clear(); grid.repaint(); return true; }
        return false;
    }

    static juce::String reservedKeys() { return "pbdect"; }

    // Drops: sounds from the browser or Finder become audio clips where you drop them.
    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString().startsWith ("file:"); }
    void itemDropped (const SourceDetails& d) override
    {
        dropFile (juce::File (d.description.toString().fromFirstOccurrenceOf ("file:", false, false)), d.localPosition);
    }
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        for (int i = 0; i < files.size(); ++i)
            dropFile (juce::File (files[i]), { x, y + i * laneHeight });
    }

private:
    static constexpr int toolbarHeight = 32, rulerHeight = 22, lanesWidth = 110, laneHeight = 34, numLanes = 64;

    double getSnap() const
    {
        switch (snapBox.getSelectedItemIndex())
        {
            case 0: return 0.0;
            case 1: return Studio::stepBeats;
            case 2: return 0.5;
            case 3: return 1.0;
            default: return studio.getBarBeats();
        }
    }

    double snapFloor (double b) const { const double s = getSnap(); return s > 0 ? std::floor (b / s + 1.0e-6) * s : b; }
    double snapRound (double b) const { const double s = getSnap(); return s > 0 ? std::round (b / s) * s : b; }

    int beatToX (double beat) const { return (int) std::round (beat * pxPerBeat); }
    double xToBeat (int x) const    { return juce::jmax (0.0, x / pxPerBeat); }
    int yToLane (int y) const       { return juce::jlimit (0, numLanes - 1, y / laneHeight); }

    juce::Rectangle<int> clipBounds (const juce::ValueTree& c) const
    {
        const int x = beatToX (c[IDs::start]);
        return { x, (int) c[IDs::lane] * laneHeight + 1, juce::jmax (4, beatToX ((double) c[IDs::start] + (double) c[IDs::length]) - x), laneHeight - 2 };
    }

    juce::ValueTree clipAt (juce::Point<int> p) const
    {
        const auto playlist = studio.getPlaylist();
        for (int i = playlist.getNumChildren(); --i >= 0;)
            if (clipBounds (playlist.getChild (i)).contains (p))
                return playlist.getChild (i);
        return {};
    }

    void updateGridSize()
    {
        const double beats = juce::jmax (studio.getSongLengthBeats(), 0.0) + studio.getBarBeats() * 16.0;
        grid.setSize (juce::jmax (viewport.getMaximumVisibleWidth(), beatToX (beats)), numLanes * laneHeight);
    }

    void setTool (Tool t)
    {
        tool = t;
        for (auto* b : toolButtons)
            b->setToggleState (b->tool == t, juce::dontSendNotification);
    }

    void deleteClip (const juce::ValueTree& c)
    {
        if (c.getParent().isValid())
            studio.getPlaylist().removeChild (c, &studio.undo());
        selection.removeAllInstancesOf (c);
    }

    juce::ValueTree placePattern (double start, int lane)
    {
        auto c = studio.addPatternClip (studio.getCurrentPattern(), lane, start);
        selection.clear();
        selection.add (c);
        return c;
    }

    void duplicateSelection()
    {
        if (selection.isEmpty())
            return;
        double start = 1.0e9, end = 0.0;
        for (auto& c : selection)
        {
            start = juce::jmin (start, (double) c[IDs::start]);
            end = juce::jmax (end, (double) c[IDs::start] + (double) c[IDs::length]);
        }
        studio.undo().beginNewTransaction ("Duplicate clips");
        juce::Array<juce::ValueTree> copies;
        for (auto& c : selection)
        {
            auto copy = c.createCopy();
            copy.setProperty (IDs::id, studio.newId(), nullptr);
            copy.setProperty (IDs::start, (double) c[IDs::start] + (end - start), nullptr);
            studio.getPlaylist().appendChild (copy, &studio.undo());
            copies.add (copy);
        }
        selection = copies;
    }

    void dropFile (const juce::File& file, juce::Point<int> posInThis)
    {
        if (! file.existsAsFile())
            return;
        const auto p = grid.getLocalPoint (this, posInThis);
        studio.undo().beginNewTransaction ("Add audio clip");
        studio.addAudioClip (file, yToLane (juce::jmax (0, p.y)), snapFloor (xToBeat (juce::jmax (0, p.x))));
    }

    //==========================================================================
    struct ToolButton : public juce::Button
    {
        ToolButton (Tool t, const juce::String& l, const juce::String& hint) : juce::Button (hint), tool (t), letter (l) { setTooltip (hint); }

        void paintButton (juce::Graphics& g, bool over, bool) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (getToggleState() ? Theme::accent : (over ? Theme::titleActive : Theme::panelDark));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (getToggleState() ? juce::Colours::black : Theme::text);
            g.setFont (Theme::font (13.0f, true));
            g.drawText (letter, r, juce::Justification::centred);
        }

        Tool tool;
        juce::String letter;
    };

    //==========================================================================
    struct Grid : public juce::Component, private juce::ChangeListener
    {
        explicit Grid (Playlist& p) : list (p) {}
        ~Grid() override { thumbnails.clear(); }

        void paint (juce::Graphics& g) override
        {
            const auto clip = g.getClipBounds();
            for (int lane = clip.getY() / laneHeight; lane <= clip.getBottom() / laneHeight; ++lane)
            {
                g.setColour (lane % 2 == 0 ? juce::Colour (0xff353d43) : juce::Colour (0xff323a40));
                g.fillRect (clip.getX(), lane * laneHeight, clip.getWidth(), laneHeight);
            }

            const double bar = list.studio.getBarBeats();
            for (double b = std::floor (list.xToBeat (clip.getX())); list.beatToX (b) <= clip.getRight(); b += 1.0)
            {
                const bool isBar = std::abs (std::fmod (b + 1.0e-6, bar)) < 1.0e-3;
                if (! isBar && list.pxPerBeat < 10.0)
                    continue;
                g.setColour (juce::Colours::black.withAlpha (isBar ? 0.5f : 0.2f));
                g.drawVerticalLine (list.beatToX (b), (float) clip.getY(), (float) clip.getBottom());
            }

            for (auto c : list.studio.getPlaylist())
            {
                const auto r = list.clipBounds (c);
                if (r.intersects (clip))
                    paintClip (g, c, r);
            }

            if (! list.lasso.isEmpty())
            {
                g.setColour (Theme::accent.withAlpha (0.15f));
                g.fillRect (list.lasso);
                g.setColour (Theme::accent);
                g.drawRect (list.lasso);
            }

            if (list.studio.isSongMode() && (list.studio.isPlaying() || list.studio.getPositionBeats() > 0.0))
            {
                g.setColour (Theme::accent);
                g.drawVerticalLine (list.beatToX (list.studio.getPositionBeats()), (float) clip.getY(), (float) clip.getBottom());
            }
        }

        void paintClip (juce::Graphics& g, const juce::ValueTree& c, juce::Rectangle<int> r)
        {
            const bool audio = Studio::isAudioClip (c);
            const bool muted = c[IDs::muted];
            const bool selected = list.selection.contains (c);
            const auto pattern = list.studio.getPattern (c[IDs::pattern]);
            auto colour = audio ? juce::Colour (0xff4a9a9a) : list.studio.getPatternColour (pattern);
            if (muted)
                colour = colour.withSaturation (0.0f).darker (0.4f);

            const auto rf = r.toFloat();
            g.setColour (colour.darker (0.55f));
            g.fillRoundedRectangle (rf, 3.0f);
            g.setColour (colour);
            g.fillRoundedRectangle (rf.withHeight (13.0f), 3.0f);
            g.setColour (selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.6f));
            g.drawRoundedRectangle (rf.reduced (0.5f), 3.0f, selected ? 1.6f : 1.0f);

            const auto name = audio ? list.studio.getChannel (c[IDs::channel])[IDs::name].toString() : pattern[IDs::name].toString();
            g.setColour (juce::Colours::black.withAlpha (0.85f));
            g.setFont (Theme::font (11.0f, true));
            g.drawText (name, r.withHeight (13).reduced (4, 0), juce::Justification::centredLeft, true);

            auto body = r.withTrimmedTop (14).reduced (2, 2);
            g.saveState();
            g.reduceClipRegion (body);
            if (audio)
            {
                if (auto* thumb = getThumbnail (juce::File (list.studio.getChannel (c[IDs::channel])[IDs::file].toString())))
                {
                    const double secondsPerBeat = 60.0 / list.studio.getBpm();
                    const double offsetSeconds = (double) c[IDs::offset] * secondsPerBeat;
                    g.setColour (colour.brighter (0.4f));
                    thumb->drawChannel (g, body, offsetSeconds, offsetSeconds + (double) c[IDs::length] * secondsPerBeat, 0, 1.0f);
                }
            }
            else
            {
                // Mini preview of the pattern's notes, like FL.
                int lo = 127, hi = 0;
                for (auto n : pattern)
                {
                    lo = juce::jmin (lo, (int) n[IDs::key]);
                    hi = juce::jmax (hi, (int) n[IDs::key]);
                }
                const double offset = c[IDs::offset];
                const double patternLength = list.studio.getPatternLengthBeats (pattern);
                g.setColour (colour.brighter (0.5f));
                for (double rep = 0.0; rep < (double) c[IDs::length] + offset; rep += patternLength)
                {
                    for (auto n : pattern)
                    {
                        const double start = rep + (double) n[IDs::start] - offset;
                        if (start + (double) n[IDs::length] < 0.0 || start >= (double) c[IDs::length])
                            continue;
                        const auto bf = body.toFloat();
                        const float y = hi == lo ? bf.getCentreY() - 1.0f
                                                 : bf.getBottom() - 2.0f - (float) ((int) n[IDs::key] - lo) / (float) (hi - lo) * (bf.getHeight() - 3.0f);
                        const float x = (float) (r.getX() + list.beatToX (start));
                        g.fillRect (x, y, juce::jmax (1.5f, (float) list.beatToX (n[IDs::length])), 2.0f);
                    }
                    if (patternLength <= 0.0)
                        break;
                }
            }
            g.restoreState();
        }

        juce::AudioThumbnail* getThumbnail (const juce::File& f)
        {
            auto& t = thumbnails[f.getFullPathName()];
            if (t == nullptr)
            {
                t = std::make_unique<juce::AudioThumbnail> (256, list.studio.engine.getAudioFileFormatManager().readFormatManager, cache);
                t->addChangeListener (this);
                t->setSource (new juce::FileInputSource (f));
            }
            return t.get();
        }

        void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            mode = Mode::none;
            downBeat = list.xToBeat (e.x);
            downLane = list.yToLane (e.y);
            auto hit = list.clipAt (e.getPosition());
            for (int i = list.selection.size(); --i >= 0;)
                if (! list.selection.getReference (i).getParent().isValid())
                    list.selection.remove (i);

            if (e.mods.isPopupMenu() || list.tool == Tool::erase)
            {
                list.studio.undo().beginNewTransaction ("Delete clips");
                mode = Mode::erase;
                if (hit.isValid()) list.deleteClip (hit);
                return;
            }

            if (list.tool == Tool::select || ((e.mods.isCtrlDown() || e.mods.isCommandDown()) && ! hit.isValid()))
            {
                mode = Mode::lasso;
                if (! e.mods.isShiftDown())
                    list.selection.clear();
                lassoStart = e.getPosition();
                return;
            }

            if (list.tool == Tool::mute)
            {
                list.studio.undo().beginNewTransaction ("Mute clip");
                mode = Mode::mute;
                if (hit.isValid())
                {
                    muteValue = ! (bool) hit[IDs::muted];
                    hit.setProperty (IDs::muted, muteValue, &list.studio.undo());
                }
                return;
            }

            if (list.tool == Tool::slice)
            {
                if (hit.isValid())
                    slice (hit, list.snapRound (downBeat));
                return;
            }

            list.studio.undo().beginNewTransaction ("Edit clips");

            if (hit.isValid())
            {
                if (! list.selection.contains (hit))
                {
                    if (! e.mods.isShiftDown())
                        list.selection.clear();
                    list.selection.add (hit);
                }
                if (! Studio::isAudioClip (hit))
                    list.studio.setCurrentPattern (hit[IDs::pattern]); // FL: clicking a clip picks its pattern

                const auto b = list.clipBounds (hit);
                const int edge = juce::jmin (8, b.getWidth() / 4);
                mode = e.x >= b.getRight() - edge ? Mode::resizeEnd : (e.x <= b.getX() + edge ? Mode::resizeStart : Mode::move);
            }
            else
            {
                list.placePattern (list.snapFloor (downBeat), downLane);
                mode = list.tool == Tool::brush ? Mode::brush : Mode::move;
                lastBrushEnd = list.snapFloor (downBeat) + (double) list.selection[0][IDs::length];
            }

            originals.clear();
            for (auto& c : list.selection)
                originals.add ({ c, c[IDs::start], c[IDs::length], c[IDs::offset], c[IDs::lane] });
            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const double beat = list.xToBeat (e.x);
            const double delta = list.snapRound (beat - downBeat);
            auto& um = list.studio.undo();

            switch (mode)
            {
                case Mode::erase:
                    if (auto hit = list.clipAt (e.getPosition()); hit.isValid())
                        list.deleteClip (hit);
                    break;

                case Mode::mute:
                    if (auto hit = list.clipAt (e.getPosition()); hit.isValid())
                        hit.setProperty (IDs::muted, muteValue, &um);
                    break;

                case Mode::lasso:
                    list.lasso = juce::Rectangle<int> (lassoStart, e.getPosition());
                    list.selection.clear();
                    for (auto c : list.studio.getPlaylist())
                        if (list.clipBounds (c).intersects (list.lasso))
                            list.selection.add (c);
                    break;

                case Mode::move:
                {
                    const int laneDelta = list.yToLane (e.y) - downLane;
                    for (auto& o : originals)
                    {
                        o.clip.setProperty (IDs::start, juce::jmax (0.0, o.start + delta), &um);
                        o.clip.setProperty (IDs::lane, juce::jlimit (0, numLanes - 1, o.lane + laneDelta), &um);
                    }
                    break;
                }

                case Mode::resizeEnd:
                {
                    const double minLength = list.getSnap() > 0 ? list.getSnap() : Studio::stepBeats;
                    for (auto& o : originals)
                        o.clip.setProperty (IDs::length, juce::jmax (minLength, o.length + delta), &um);
                    break;
                }

                case Mode::resizeStart:
                {
                    for (auto& o : originals)
                    {
                        const double d = juce::jlimit (-o.offset, o.length - Studio::stepBeats, delta);
                        o.clip.setProperty (IDs::start, juce::jmax (0.0, o.start + d), &um);
                        o.clip.setProperty (IDs::length, o.length - d, &um);
                        o.clip.setProperty (IDs::offset, o.offset + d, &um);
                    }
                    break;
                }

                case Mode::brush:
                {
                    const double cell = list.snapFloor (beat);
                    if (cell >= lastBrushEnd - 1.0e-6)
                    {
                        auto c = list.studio.addPatternClip (list.studio.getCurrentPattern(), downLane, cell);
                        lastBrushEnd = cell + (double) c[IDs::length];
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
            list.lasso = {};
            mode = Mode::none;
            repaint();
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            auto hit = list.clipAt (e.getPosition());
            if (hit.isValid() && ! Studio::isAudioClip (hit))
            {
                list.studio.setCurrentPattern (hit[IDs::pattern]);
                if (list.onShowChannelRack) list.onShowChannelRack();
            }
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const auto hit = list.clipAt (e.getPosition());
            if (! hit.isValid() || list.tool != Tool::pencil)
                return setMouseCursor (juce::MouseCursor::NormalCursor);
            const auto b = list.clipBounds (hit);
            const int edge = juce::jmin (8, b.getWidth() / 4);
            setMouseCursor (e.x >= b.getRight() - edge || e.x <= b.getX() + edge ? juce::MouseCursor::LeftRightResizeCursor
                                                                                 : juce::MouseCursor::DraggingHandCursor);
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            if (e.mods.isCtrlDown() || e.mods.isCommandDown())
            {
                const double beatAtMouse = list.xToBeat (e.x);
                const int mouseInView = e.x - list.viewport.getViewPositionX();
                list.pxPerBeat = juce::jlimit (2.0, 200.0, list.pxPerBeat * (w.deltaY > 0 ? 1.15 : 1.0 / 1.15));
                list.updateGridSize();
                list.viewport.setViewPosition (juce::jmax (0, list.beatToX (beatAtMouse) - mouseInView), list.viewport.getViewPositionY());
                list.ruler.repaint();
                repaint();
                return;
            }
            juce::Component::mouseWheelMove (e, w);
        }

    private:
        enum class Mode { none, move, resizeStart, resizeEnd, erase, lasso, brush, mute };
        struct Original { juce::ValueTree clip; double start, length, offset; int lane; };

        void slice (juce::ValueTree c, double at)
        {
            const double start = c[IDs::start], length = c[IDs::length], offset = c[IDs::offset];
            if (at <= start + 1.0e-6 || at >= start + length - 1.0e-6)
                return;
            list.studio.undo().beginNewTransaction ("Slice clip");
            auto second = c.createCopy();
            second.setProperty (IDs::id, list.studio.newId(), nullptr);
            second.setProperty (IDs::start, at, nullptr);
            second.setProperty (IDs::length, start + length - at, nullptr);
            second.setProperty (IDs::offset, offset + (at - start), nullptr);
            c.setProperty (IDs::length, at - start, &list.studio.undo());
            list.studio.getPlaylist().appendChild (second, &list.studio.undo());
        }

        Playlist& list;
        Mode mode = Mode::none;
        double downBeat = 0.0, lastBrushEnd = 0.0;
        int downLane = 0;
        bool muteValue = true;
        juce::Point<int> lassoStart;
        juce::Array<Original> originals;
        juce::AudioThumbnailCache cache { 32 };
        std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbnails;
    };

    //==========================================================================
    struct Ruler : public juce::Component, public juce::SettableTooltipClient
    {
        explicit Ruler (Playlist& p) : list (p) { setTooltip ("Click: jump here. Drag: set loop region. Double-click: clear loop"); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::panelDark);
            const int offset = list.viewport.getViewPositionX();
            const double bar = list.studio.getBarBeats();

            const auto loop = list.studio.getSongLoop();
            if (loop.getLength() > 1.0e-6)
            {
                g.setColour (Theme::accent.withAlpha (0.35f));
                g.fillRect (list.beatToX (loop.getStart()) - offset, 0, list.beatToX (loop.getLength()), getHeight());
            }

            g.setFont (Theme::font (11.0f));
            const int step = juce::jmax (1, (int) std::ceil (40.0 / (bar * list.pxPerBeat)));
            for (int b = (int) (list.xToBeat (offset) / bar); list.beatToX (b * bar) - offset < getWidth(); b += step)
            {
                const int x = list.beatToX (b * bar) - offset;
                g.setColour (Theme::textDim);
                g.drawVerticalLine (x, (float) getHeight() - 6.0f, (float) getHeight());
                g.drawText (juce::String (b + 1), x + 3, 0, 40, getHeight(), juce::Justification::centredLeft);
            }

            if (list.studio.isSongMode())
            {
                const int x = list.beatToX (list.studio.getPositionBeats()) - offset;
                juce::Path p;
                p.addTriangle ((float) x - 5.0f, 0.0f, (float) x + 5.0f, 0.0f, (float) x, 8.0f);
                g.setColour (Theme::accent);
                g.fillPath (p);
            }
        }

        double beatAt (int x) const { return list.snapRound (list.xToBeat (x + list.viewport.getViewPositionX())); }

        void mouseDown (const juce::MouseEvent& e) override { downBeat = beatAt (e.x); }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const double b = beatAt (e.x);
            if (std::abs (b - downBeat) > 1.0e-6)
                list.studio.setSongLoop ({ juce::jmin (b, downBeat), juce::jmax (b, downBeat) });
            repaint();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.getDistanceFromDragStart() < 3)
            {
                list.studio.setSongMode (true);
                list.studio.setPositionBeats (downBeat);
            }
        }

        void mouseDoubleClick (const juce::MouseEvent&) override { list.studio.setSongLoop ({}); repaint(); }

        Playlist& list;
        double downBeat = 0.0;
    };

    //==========================================================================
    struct Lanes : public juce::Component
    {
        explicit Lanes (Playlist& p) : list (p) {}

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::panelDark);
            const int offset = list.viewport.getViewPositionY();
            g.setFont (Theme::font (12.0f));
            for (int lane = offset / laneHeight; lane < numLanes && lane * laneHeight - offset < getHeight(); ++lane)
            {
                const int y = lane * laneHeight - offset;
                g.setColour (Theme::panel);
                g.fillRect (0, y + 1, getWidth() - 1, laneHeight - 2);
                g.setColour (Theme::textDim);
                g.drawText ("Track " + juce::String (lane + 1), 8, y, getWidth() - 12, laneHeight, juce::Justification::centredLeft);
            }
        }

        Playlist& list;
    };

    //==========================================================================
    void timerCallback() override
    {
        if (studio.getChangeCount() != lastChangeCount)
        {
            lastChangeCount = studio.getChangeCount();
            for (int i = selection.size(); --i >= 0;)
                if (! selection.getReference (i).getParent().isValid())
                    selection.remove (i);
            updateGridSize();
            grid.repaint();
            ruler.repaint();
            repaint (patternLabelArea);
        }
        else if (studio.isPlaying() && studio.isSongMode())
        {
            // Follow the playhead: page forward when it leaves the view.
            const int x = beatToX (studio.getPositionBeats());
            const auto view = viewport.getViewArea();
            if (x > view.getRight() - 20 || x < view.getX())
                viewport.setViewPosition (juce::jmax (0, x - 40), view.getY());
            grid.repaint();
            ruler.repaint();
        }
    }

    Studio& studio;
    Tool tool = Tool::pencil;
    double pxPerBeat = 20.0;
    juce::Array<juce::ValueTree> selection;
    juce::Rectangle<int> lasso, patternLabelArea;

    juce::OwnedArray<ToolButton> toolButtons;
    juce::ComboBox snapBox;
    Grid grid { *this };
    Ruler ruler { *this };
    Lanes lanes { *this };
    struct SyncViewport : juce::Viewport
    {
        std::function<void()> onScroll;
        void visibleAreaChanged (const juce::Rectangle<int>&) override { if (onScroll) onScroll(); }
    } viewport;
    int lastChangeCount = -1;
};
