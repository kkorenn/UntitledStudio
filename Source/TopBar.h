#pragma once

#include "Studio.h"
#include "Widgets.h"

// Pieces of the main top bar (FL-style layout, our own drawing).
namespace TopBar
{
    inline void drawPanel (juce::Graphics& g, juce::Rectangle<int> r)
    {
        g.setColour (Theme::panelDark);
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
        g.setColour (Theme::outline.withAlpha (0.7f));
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.0f);
    }

    //==========================================================================
    // Small square button with a drawn icon. Lit (accent) when its toggle state is on.
    class IconButton : public juce::Button
    {
    public:
        enum Icon { playlist, pianoRoll, channelRack, mixer, browser, pluginPicker,
                    metronome, waitInput, countIn, blend, loopRecord, typingKeys, stepEdit, loop };

        IconButton (Icon i, const juce::String& hint) : juce::Button (hint), icon (i) { setTooltip (hint); }

        std::function<void()> onRightClick;

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && onRightClick) onRightClick();
            else juce::Button::mouseDown (e);
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            const bool on = getToggleState();
            g.setColour (down ? Theme::outline : (on ? Theme::panel.brighter (0.08f) : (over ? Theme::titleActive : Theme::panel)));
            g.fillRoundedRectangle (r, 2.5f);
            g.setColour (Theme::outline);
            g.drawRoundedRectangle (r, 2.5f, 1.0f);
            if (on)
            {
                g.setColour (Theme::accent);
                g.fillRect (r.reduced (3.0f, 0.0f).removeFromBottom (2.0f));
            }

            const float size = juce::jmin (r.getWidth(), r.getHeight()) * 0.66f;
            const auto box = r.withSizeKeepingCentre (size, size).translated (0.0f, on ? -0.5f : 0.0f);
            const auto colour = on ? Theme::accent : (over ? Theme::text : Theme::text.withAlpha (0.72f));
            drawIcon (g, box, colour);
        }

    private:
        // Icons are drawn in a 16 x 16 box, then scaled.
        void drawIcon (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour c) const
        {
            juce::Path stroke, fill;
            switch (icon)
            {
                case playlist: // clips on lanes
                    fill.addRoundedRectangle (1.0f, 2.0f, 8.0f, 3.0f, 0.8f);
                    fill.addRoundedRectangle (5.0f, 6.5f, 10.0f, 3.0f, 0.8f);
                    fill.addRoundedRectangle (2.0f, 11.0f, 6.0f, 3.0f, 0.8f);
                    fill.addRoundedRectangle (10.0f, 11.0f, 5.0f, 3.0f, 0.8f);
                    break;
                case pianoRoll: // keys on the left, notes on the grid
                    stroke.addRectangle (1.0f, 1.5f, 4.0f, 13.0f);
                    fill.addRectangle (1.0f, 4.0f, 2.5f, 1.6f);
                    fill.addRectangle (1.0f, 8.0f, 2.5f, 1.6f);
                    fill.addRectangle (1.0f, 11.5f, 2.5f, 1.6f);
                    fill.addRoundedRectangle (7.0f, 3.0f, 5.0f, 2.2f, 0.6f);
                    fill.addRoundedRectangle (9.5f, 7.0f, 5.5f, 2.2f, 0.6f);
                    fill.addRoundedRectangle (7.0f, 11.0f, 4.0f, 2.2f, 0.6f);
                    break;
                case channelRack: // two rows of step buttons
                    for (int row = 0; row < 3; ++row)
                        for (int col = 0; col < 4; ++col)
                        {
                            const auto cell = juce::Rectangle<float> (1.0f + (float) col * 3.75f, 2.0f + (float) row * 4.5f, 2.8f, 3.4f);
                            if ((row + col) % 3 == 0) fill.addRoundedRectangle (cell, 0.5f);
                            else                      stroke.addRoundedRectangle (cell.reduced (0.3f), 0.5f);
                        }
                    break;
                case mixer: // three faders
                    for (int i = 0; i < 3; ++i)
                    {
                        const float x = 3.0f + (float) i * 5.0f, y = i == 0 ? 9.0f : (i == 1 ? 4.0f : 7.0f);
                        stroke.startNewSubPath (x, 1.5f);
                        stroke.lineTo (x, 14.5f);
                        fill.addRoundedRectangle (x - 2.0f, y, 4.0f, 2.6f, 0.6f);
                    }
                    break;
                case browser: // folder tree
                    stroke.startNewSubPath (3.0f, 3.0f);
                    stroke.lineTo (3.0f, 13.0f);
                    for (float y : { 6.5f, 13.0f })
                    {
                        stroke.startNewSubPath (3.0f, y);
                        stroke.lineTo (6.0f, y);
                    }
                    fill.addRoundedRectangle (1.0f, 1.0f, 6.0f, 3.5f, 0.6f);
                    fill.addRoundedRectangle (7.0f, 5.0f, 8.0f, 3.2f, 0.6f);
                    fill.addRoundedRectangle (7.0f, 11.5f, 8.0f, 3.2f, 0.6f);
                    break;
                case pluginPicker: // plug
                    fill.addRoundedRectangle (4.0f, 5.0f, 8.0f, 6.0f, 1.5f);
                    fill.addRectangle (5.5f, 1.0f, 1.6f, 4.5f);
                    fill.addRectangle (8.9f, 1.0f, 1.6f, 4.5f);
                    stroke.startNewSubPath (8.0f, 11.0f);
                    stroke.quadraticTo (8.0f, 15.0f, 13.0f, 14.5f);
                    break;
                case metronome:
                    stroke.startNewSubPath (5.5f, 1.5f);
                    stroke.lineTo (10.5f, 1.5f);
                    stroke.lineTo (14.0f, 14.5f);
                    stroke.lineTo (2.0f, 14.5f);
                    stroke.closeSubPath();
                    stroke.startNewSubPath (8.0f, 11.5f);
                    stroke.lineTo (12.5f, 3.5f);
                    fill.addEllipse (6.6f, 10.2f, 2.8f, 2.8f);
                    break;
                case waitInput: // hourglass
                    stroke.startNewSubPath (3.0f, 1.5f);
                    stroke.lineTo (13.0f, 1.5f);
                    stroke.lineTo (3.0f, 14.5f);
                    stroke.lineTo (13.0f, 14.5f);
                    stroke.closeSubPath();
                    fill.addTriangle (5.5f, 12.8f, 10.5f, 12.8f, 8.0f, 9.5f);
                    break;
                case countIn: // stopwatch counting down
                    stroke.addEllipse (2.0f, 3.0f, 12.0f, 12.0f);
                    stroke.startNewSubPath (6.5f, 1.0f);
                    stroke.lineTo (9.5f, 1.0f);
                    fill.addPieSegment (3.5f, 4.5f, 9.0f, 9.0f, 0.0f, juce::MathConstants<float>::pi * 1.3f, 0.0f);
                    break;
                case blend: // overlapping takes
                    stroke.addEllipse (1.0f, 3.5f, 9.0f, 9.0f);
                    stroke.addEllipse (6.0f, 3.5f, 9.0f, 9.0f);
                    break;
                case loopRecord: // loop arrow around a record dot
                    stroke.addCentredArc (8.0f, 8.0f, 6.0f, 6.0f, 0.0f, 0.4f, juce::MathConstants<float>::twoPi - 0.2f, true);
                    fill.addTriangle (8.4f, 0.0f, 8.4f, 4.4f, 11.6f, 2.2f);
                    fill.addEllipse (5.5f, 5.5f, 5.0f, 5.0f);
                    break;
                case typingKeys: // computer keyboard
                    stroke.addRoundedRectangle (0.8f, 3.5f, 14.4f, 9.5f, 1.5f);
                    for (int row = 0; row < 2; ++row)
                        for (int col = 0; col < 4; ++col)
                            fill.addRectangle (2.6f + (float) col * 3.0f, 5.3f + (float) row * 2.6f, 1.8f, 1.5f);
                    fill.addRectangle (4.5f, 10.5f, 7.0f, 1.3f);
                    break;
                case stepEdit: // stairs with a note on top
                    stroke.startNewSubPath (1.0f, 14.5f);
                    stroke.lineTo (5.0f, 14.5f);
                    stroke.lineTo (5.0f, 10.5f);
                    stroke.lineTo (9.0f, 10.5f);
                    stroke.lineTo (9.0f, 6.5f);
                    stroke.lineTo (15.0f, 6.5f);
                    fill.addEllipse (9.5f, 1.0f, 4.0f, 3.2f);
                    fill.addRectangle (12.4f, -1.0f, 1.1f, 3.6f);
                    break;
                case loop: // two arrows chasing each other
                    stroke.addCentredArc (8.0f, 8.0f, 6.0f, 5.0f, 0.0f, 0.3f, juce::MathConstants<float>::pi - 0.3f, true);
                    stroke.addCentredArc (8.0f, 8.0f, 6.0f, 5.0f, 0.0f, juce::MathConstants<float>::pi + 0.3f, juce::MathConstants<float>::twoPi - 0.3f, true);
                    fill.addTriangle (12.5f, 10.0f, 15.5f, 8.0f, 15.5f, 12.4f);
                    fill.addTriangle (3.5f, 6.0f, 0.5f, 8.0f, 0.5f, 3.6f);
                    break;
            }

            const float scale = box.getWidth() / 16.0f;
            const auto t = juce::AffineTransform::scale (scale).translated (box.getX(), box.getY());
            g.setColour (c);
            g.fillPath (fill, t);
            g.strokePath (stroke, juce::PathStrokeType (juce::jmax (1.0f, 1.3f * scale), juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded), t);
        }

        Icon icon;
    };

    //==========================================================================
    // FL's PAT / SONG switch: two labelled halves, the active one lit.
    class PatSongSwitch : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        std::function<void (bool song)> onChange;

        PatSongSwitch() { setTooltip ("Pattern / song mode (L): PAT plays the current pattern, SONG plays the playlist"); }

        void setSong (bool s)
        {
            if (s != song) { song = s; repaint(); }
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            for (int i = 0; i < 2; ++i)
            {
                auto half = (i == 0 ? r.removeFromTop (r.getHeight() * 0.5f) : r).reduced (0.5f);
                const bool lit = (i == 1) == song;
                g.setColour (lit ? Theme::panel.brighter (0.06f) : Theme::panelDark);
                g.fillRoundedRectangle (half, 2.0f);
                g.setColour (Theme::outline);
                g.drawRoundedRectangle (half, 2.0f, 1.0f);
                auto led = half.removeFromLeft (13.0f).withSizeKeepingCentre (6.0f, 6.0f);
                g.setColour (lit ? (i == 0 ? Theme::accent : Theme::led) : Theme::ledOff);
                g.fillEllipse (led);
                g.setColour (lit ? Theme::text : Theme::textDim);
                g.setFont (Theme::font (10.5f, true));
                g.drawText (i == 0 ? "PAT" : "SONG", half.withTrimmedRight (3.0f), juce::Justification::centredLeft);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const bool clickedSong = e.y >= getHeight() / 2;
            if (clickedSong != song && onChange)
                onChange (clickedSong);
        }

    private:
        bool song = false;
    };

    //==========================================================================
    // Song position LCD. Click toggles bar:step:tick / minutes:seconds:centiseconds.
    class TimeDisplay : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        TimeDisplay() { updateTooltip(); }

        bool showsTime() const { return timeFormat; }

        void set (double beats, double barBeats, double seconds)
        {
            juce::String t;
            if (timeFormat)
            {
                const int cs = juce::jmax (0, (int) std::floor (seconds * 100.0));
                t = juce::String::formatted ("%d:%02d:%02d", cs / 6000, (cs / 100) % 60, cs % 100);
            }
            else
            {
                const int bar = (int) std::floor (beats / barBeats) + 1;
                const double inBar = std::fmod (beats, barBeats);
                const int step = (int) std::floor (inBar / Studio::stepBeats) + 1;
                const int tick = (int) std::floor (std::fmod (inBar, Studio::stepBeats) / Studio::stepBeats * 24.0);
                t = juce::String::formatted ("%d:%02d:%02d", bar, step, tick);
            }
            if (t != text) { text = t; repaint(); }
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (Theme::lcd);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (8.5f, true));
            g.drawText (timeFormat ? "M:S:CS" : "B:S:T", r.reduced (4.0f, 2.0f), juce::Justification::topRight);
            g.setColour (Theme::lcdText);
            g.setFont (Theme::font (juce::jmin (r.getHeight() * 0.56f, (r.getWidth() - 6.0f) / (0.62f * (float) juce::jmax (4, text.length()))), true));
            g.drawText (text, r.withTrimmedTop (r.getHeight() * 0.18f), juce::Justification::centred);
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            timeFormat = ! timeFormat;
            text = {};
            updateTooltip();
        }

    private:
        void updateTooltip()
        {
            setTooltip (timeFormat ? "Song position in minutes:seconds:centiseconds. Click for bar:step:tick"
                                   : "Song position in bar:step:tick. Click for minutes:seconds");
        }

        bool timeFormat = false;
        juce::String text;
    };

    //==========================================================================
    // Pattern selector: colour swatch + name, arrows for previous / next, click for the menu.
    class PatternSelector : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        std::function<void (int delta)> onStep;
        std::function<void()> onMenu;

        PatternSelector() { setTooltip ("Pattern selector: click to pick / add / clone / rename, arrows or wheel = previous / next (numpad -/+)"); }

        void set (const juce::String& n, juce::Colour c)
        {
            if (n != name || c != colour) { name = n; colour = c; repaint(); }
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (Theme::lcd);
            g.fillRoundedRectangle (r, 3.0f);

            const float arrowW = r.getHeight() * 0.8f;
            auto left = r.removeFromLeft (arrowW), right = r.removeFromRight (arrowW);
            g.setColour (Theme::textDim);
            for (auto [area, dir] : { std::pair { left, -1.0f }, { right, 1.0f } })
            {
                const auto c = area.getCentre();
                const float s = r.getHeight() * 0.18f;
                juce::Path p;
                p.addTriangle (c.x - s * dir, c.y - s * 1.2f, c.x - s * dir, c.y + s * 1.2f, c.x + s * dir, c.y);
                g.fillPath (p);
            }

            auto swatch = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.22f);
            g.setColour (colour);
            g.fillRoundedRectangle (swatch, 2.0f);
            g.setColour (Theme::text);
            g.setFont (Theme::font (juce::jmin (13.0f, r.getHeight() * 0.6f)));
            g.drawText (name, r.reduced (3.0f, 0.0f), juce::Justification::centredLeft, true);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int arrowW = juce::roundToInt ((float) getHeight() * 0.8f);
            if (e.x < arrowW)                    { if (onStep) onStep (-1); }
            else if (e.x > getWidth() - arrowW)  { if (onStep) onStep (1); }
            else if (onMenu)                     onMenu();
        }

        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
        {
            if (onStep && std::abs (w.deltaY) > 0.0f) onStep (w.deltaY > 0 ? -1 : 1);
        }

    private:
        juce::String name;
        juce::Colour colour { Theme::accent };
    };

    //==========================================================================
    // Output monitor: scrolling peak history of the master output (mirrored, scope-like)
    // and a stereo peak meter. Reads the master LevelMeterPlugin, so nothing extra runs on the audio thread.
    class OutputMonitor : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        OutputMonitor()
        {
            setTooltip ("Output monitor: master peak level over the last few seconds");
            history.fill (-100.0f);
            startTimerHz (30);
        }

        ~OutputMonitor() override { setMeter (nullptr); }

        void setMeter (te::LevelMeterPlugin* p)
        {
            if (p == meter.get())
                return;
            if (meter != nullptr) meter->measurer.removeClient (client);
            meter = p;
            if (meter != nullptr) meter->measurer.addClient (client);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (Theme::lcd);
            g.fillRoundedRectangle (r, 3.0f);
            r.reduce (2.0f, 2.0f);

            auto meterArea = r.removeFromRight (8.0f);
            r.removeFromRight (2.0f);
            const float mid = r.getCentreY(), half = r.getHeight() * 0.5f;
            g.setColour (Theme::lcdText.withAlpha (0.18f));
            g.drawHorizontalLine ((int) mid, r.getX(), r.getRight());

            juce::Path wave;
            const float dx = r.getWidth() / (float) (history.size() - 1);
            for (size_t i = 0; i < history.size(); ++i)
            {
                const float x = r.getX() + (float) i * dx;
                const float y = mid - toHeight (history[(head + i) % history.size()]) * half;
                if (i == 0) wave.startNewSubPath (x, y); else wave.lineTo (x, y);
            }
            for (size_t i = history.size(); i-- > 0;)
                wave.lineTo (r.getX() + (float) i * dx, mid + toHeight (history[(head + i) % history.size()]) * half);
            wave.closeSubPath();
            g.setColour (Theme::lcdText.withAlpha (0.75f));
            g.fillPath (wave);

            for (int ch = 0; ch < 2; ++ch)
            {
                auto bar = meterArea.removeFromLeft (4.0f).reduced (0.5f, 0.0f);
                const float level = toHeight (levels[ch]);
                g.setColour (Theme::outline);
                g.fillRect (bar);
                g.setColour (levels[ch] > 0.0f ? juce::Colour (0xffe0483e) : (levels[ch] > -6.0f ? juce::Colour (0xffe8d24a) : juce::Colour (0xff4fd16a)));
                g.fillRect (bar.removeFromBottom (level * bar.getHeight()));
            }
        }

    private:
        static float toHeight (float db) { return juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 48.0f); }

        void timerCallback() override
        {
            float peak = -100.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float db = meter != nullptr ? client.getAndClearAudioLevel (ch).dB : -100.0f;
                levels[ch] = juce::jmax (db, levels[ch] - 1.5f, -60.0f);
                peak = juce::jmax (peak, db);
            }
            history[head] = peak;
            head = (head + 1) % history.size();
            repaint();
        }

        juce::ReferenceCountedObjectPtr<te::LevelMeterPlugin> meter;
        te::LevelMeasurer::Client client;
        std::array<float, 90> history {};
        size_t head = 0;
        float levels[2] { -60.0f, -60.0f };
    };

    //==========================================================================
    // CPU / RAM / polyphony readouts, with a CPU history graph (about the last 10 seconds).
    class PerfPanel : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        PerfPanel() { setTooltip ("CPU load (graph: last 10 seconds), memory used and voices playing"); history.fill (0.0f); }

        void push (float cpu, int ramMb, int voiceCount)
        {
            history[head] = juce::jlimit (0.0f, 1.0f, cpu);
            head = (head + 1) % history.size();
            ram = ramMb;
            voices = voiceCount;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            auto top = r.removeFromTop (r.getHeight() * 0.58f);
            r.removeFromTop (2.0f);

            g.setColour (Theme::lcd);
            g.fillRoundedRectangle (top, 3.0f);
            auto graph = top.reduced (2.0f);
            juce::Path p;
            const float dx = graph.getWidth() / (float) (history.size() - 1);
            p.startNewSubPath (graph.getX(), graph.getBottom());
            for (size_t i = 0; i < history.size(); ++i)
                p.lineTo (graph.getX() + (float) i * dx, graph.getBottom() - history[(head + i) % history.size()] * graph.getHeight());
            p.lineTo (graph.getRight(), graph.getBottom());
            p.closeSubPath();
            const float cpu = history[(head + history.size() - 1) % history.size()];
            const auto colour = cpu > 0.8f ? juce::Colour (0xffe0483e) : (cpu > 0.5f ? juce::Colour (0xffe8d24a) : Theme::lcdText);
            g.setColour (colour.withAlpha (0.35f));
            g.fillPath (p);

            g.setFont (Theme::font (10.0f, true));
            g.setColour (Theme::textDim);
            g.drawText ("CPU", top.reduced (4.0f, 0.0f), juce::Justification::centredLeft);
            g.setColour (colour);
            g.drawText (juce::String (juce::roundToInt (cpu * 100.0f)) + "%", top.reduced (4.0f, 0.0f), juce::Justification::centredRight);

            auto left = r.removeFromLeft (r.getWidth() * 0.58f);
            r.removeFromLeft (2.0f);
            for (auto [area, label, value] : { std::tuple { left, "MB", juce::String (ram) }, { r, "V", juce::String (voices) } })
            {
                g.setColour (Theme::lcd);
                g.fillRoundedRectangle (area, 3.0f);
                g.setColour (Theme::textDim);
                g.drawText (label, area.reduced (4.0f, 0.0f), juce::Justification::centredLeft);
                g.setColour (Theme::lcdText);
                g.drawText (value, area.reduced (4.0f, 0.0f), juce::Justification::centredRight);
            }
        }

    private:
        std::array<float, 40> history;
        size_t head = 0;
        int ram = 0, voices = 0;
    };
}
