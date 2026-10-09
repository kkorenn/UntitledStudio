#pragma once

#include "ParamKnob.h"
#include "UntitledEffects2.h"

// Window bodies for the Untitled effects: a key visual on top, grouped parameter controls below.
namespace UFX
{
    inline juce::Rectangle<float> lcdArea (juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        auto r = bounds.toFloat().reduced (6.0f);
        g.setColour (Theme::lcd);
        g.fillRoundedRectangle (r, 4.0f);
        return r.reduced (4.0f);
    }

    inline float logX (float f, juce::Rectangle<float> r)  { return r.getX() + r.getWidth() * std::log10 (f / 20.0f) / 3.0f; }
    inline float xToFreq (float x, juce::Rectangle<float> r) { return 20.0f * std::pow (10.0f, 3.0f * (x - r.getX()) / r.getWidth()); }

    inline void drawFreqGrid (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setFont (Theme::font (10.0f));
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            const float x = logX (f, r);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            g.setColour (Theme::textDim);
            g.drawText (f >= 1000 ? juce::String ((int) f / 1000) + "k" : juce::String ((int) f), (int) x + 2, (int) r.getBottom() - 12, 30, 11,
                        juce::Justification::left);
        }
    }

    // Vertical bar meter; `amount` 0..1 from the top (gain reduction) or bottom (level).
    inline void drawBar (juce::Graphics& g, juce::Rectangle<float> r, float amount, juce::Colour c, bool fromTop)
    {
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (r);
        const float h = r.getHeight() * juce::jlimit (0.0f, 1.0f, amount);
        g.setColour (c);
        g.fillRect (fromTop ? r.withHeight (h) : r.withTrimmedTop (r.getHeight() - h));
    }

    //==============================================================================
    // Base for visuals: polls the plugin ~30 times a second.
    class Visual : public juce::Component, private juce::Timer
    {
    public:
        explicit Visual (Plugin& p) : plugin (&p) { startTimerHz (30); }

    protected:
        virtual void tick() { repaint(); }
        float take (int meter) { return plugin->meters[(size_t) meter].exchange (0.0f, std::memory_order_relaxed); }
        float v (int index) const { return plugin->value (index); }

        // Cheap "did any parameter change" check for param-only visuals.
        bool paramsChanged()
        {
            float sig = 0.0f;
            int k = 1;
            for (auto* p : plugin->getAutomatableParameters())
                sig += p->getCurrentValue() * (float) k++;
            if (juce::exactlyEqual (sig, lastSig)) return false;
            lastSig = sig;
            return true;
        }

        juce::ReferenceCountedObjectPtr<Plugin> plugin;

    private:
        void timerCallback() override { if (isShowing()) tick(); }
        float lastSig = -1.0e9f;
    };

    // Parameter-only drawings repaint when something changed.
    class StaticVisual : public Visual
    {
    public:
        using Visual::Visual;
    protected:
        void tick() override { if (paramsChanged()) repaint(); }
    };

    //==============================================================================
    // Parameter controls: a knob, or a drop-down for choice parameters. Right-click = automation menu.
    class Cell : public juce::Component
    {
    public:
        static constexpr int width = 68, height = 80;

        Cell (Studio& s, Plugin& p, int index) : param (p.param (index)), spec (p.getSpecs()[(size_t) index])
        {
            knob.bind (s, param);
            if (spec.choices.isEmpty())
                addAndMakeVisible (knob);
            else
            {
                box.addItemList (spec.choices, 1);
                box.setSelectedItemIndex (juce::roundToInt (param->getCurrentValue()), juce::dontSendNotification);
                box.onChange = [this, &s]
                {
                    s.undo().beginNewTransaction (spec.name);
                    param->setParameter ((float) box.getSelectedItemIndex(), juce::sendNotificationSync);
                };
                box.setTooltip (spec.name + " (right-click: automation / controller)");
                box.addMouseListener (this, true);
                addAndMakeVisible (box);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && e.eventComponent != &knob)
                knob.showMenu();
        }

        void sync()
        {
            knob.sync();
            if (! spec.choices.isEmpty() && box.getSelectedItemIndex() != juce::roundToInt (param->getCurrentValue()))
                box.setSelectedItemIndex (juce::roundToInt (param->getCurrentValue()), juce::dontSendNotification);
            const auto text = param->getCurrentValueAsString();
            if (text != valueText) { valueText = text; repaint(); }
        }

        void paint (juce::Graphics& g) override
        {
            if (isCompact()) return;
            auto r = getLocalBounds();
            g.setColour (Theme::text);
            g.setFont (Theme::font (11.0f));
            g.drawFittedText (spec.name, r.removeFromBottom (26).removeFromTop (13), juce::Justification::centred, 1);
            if (spec.choices.isEmpty())
            {
                g.setColour (Theme::lcdText.withAlpha (0.85f));
                g.drawFittedText (valueText, getLocalBounds().removeFromBottom (13), juce::Justification::centred, 1);
            }
        }

        void resized() override
        {
            if (isCompact())
                return box.setBounds (getLocalBounds());
            auto r = getLocalBounds().withTrimmedBottom (26);
            knob.setBounds (r.reduced (12, 2));
            box.setBounds (r.withSizeKeepingCentre (getWidth() - 6, 22));
        }

    private:
        bool isCompact() const { return getHeight() < 40; } // drop-down only

        te::AutomatableParameter::Ptr param;
        Spec spec;
        ParamKnob knob;
        juce::ComboBox box;
        juce::String valueText;
    };

    struct Section { juce::String title; juce::StringArray ids; };

    // Effect body: visual on top, sections of cells flowing below.
    class View : public juce::Component, private juce::Timer
    {
    public:
        View (Studio& s, Plugin& p, std::unique_ptr<juce::Component> vis, int visualHeight, std::vector<Section> secs, int w = 560)
            : visual (std::move (vis)), sections (std::move (secs)), visualH (visualHeight)
        {
            if (visual != nullptr)
                addAndMakeVisible (*visual);
            for (auto& sec : sections)
                for (auto& id : sec.ids)
                    for (int i = 0; i < (int) p.getSpecs().size(); ++i)
                        if (p.getSpecs()[(size_t) i].id == id)
                            addAndMakeVisible (cells.add (new Cell (s, p, i)));
            setSize (w, visualH + layout (w, false) + 6);
            startTimerHz (15);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::panelDark);
            g.setFont (Theme::font (11.0f, true));
            for (auto& [title, box] : boxes)
            {
                g.setColour (Theme::panel);
                g.fillRoundedRectangle (box.toFloat(), 4.0f);
                g.setColour (Theme::accent.withAlpha (0.8f));
                g.drawText (title.toUpperCase(), box.withHeight (16).withTrimmedLeft (8), juce::Justification::centredLeft);
            }
        }

        void resized() override
        {
            if (visual != nullptr)
                visual->setBounds (0, 0, getWidth(), visualH);
            layout (getWidth(), true);
        }

    private:
        int layout (int w, bool place)
        {
            boxes.clear();
            int x = 6, y = visualH + 4, rowH = 0, cell = 0;
            for (auto& sec : sections)
            {
                const int boxW = sec.ids.size() * Cell::width + 8, boxH = Cell::height + 20;
                if (x + boxW > w - 6 && x > 6) { x = 6; y += rowH + 6; rowH = 0; }
                boxes.push_back ({ sec.title, { x, y, boxW, boxH } });
                for (int k = 0; k < sec.ids.size() && cell < cells.size(); ++k, ++cell)
                    if (place)
                        cells[cell]->setBounds (x + 4 + k * Cell::width, y + 18, Cell::width, Cell::height);
                x += boxW + 6;
                rowH = juce::jmax (rowH, boxH);
            }
            return y + rowH - visualH;
        }

        void timerCallback() override { for (auto* c : cells) c->sync(); }

        std::unique_ptr<juce::Component> visual;
        std::vector<Section> sections;
        int visualH;
        juce::OwnedArray<Cell> cells;
        std::vector<std::pair<juce::String, juce::Rectangle<int>>> boxes;
    };

    //==============================================================================
    // FFT of a StereoTap (mono sum), smoothed for display.
    struct Spectrum
    {
        static constexpr int order = 11, size = 1 << order;
        juce::dsp::FFT fft { order };
        juce::dsp::WindowingFunction<float> window { (size_t) size, juce::dsp::WindowingFunction<float>::hann };
        std::array<float, size> input {};
        std::array<float, size / 2> smoothed {};
        int pos = 0;

        void pull (StereoTap& tap, bool compute)
        {
            std::array<float, 4096> l, r;
            const int n = tap.read (l.data(), r.data(), (int) l.size());
            for (int i = 0; i < n; ++i)
            {
                input[(size_t) pos] = 0.5f * (l[(size_t) i] + r[(size_t) i]);
                pos = (pos + 1) % size;
            }
            if (! compute) return;
            std::array<float, size * 2> data {};
            for (int i = 0; i < size; ++i)
                data[(size_t) i] = input[(size_t) ((pos + i) % size)];
            window.multiplyWithWindowingTable (data.data(), size);
            fft.performFrequencyOnlyForwardTransform (data.data());
            for (int i = 0; i < size / 2; ++i)
                smoothed[(size_t) i] = juce::jmax (data[(size_t) i] / (float) size * 4.0f, smoothed[(size_t) i] * 0.85f);
        }

        float dbAt (float freq, double sr) const
        {
            const int bin = juce::jlimit (1, size / 2 - 1, (int) (freq / sr * size));
            return juce::Decibels::gainToDecibels (smoothed[(size_t) bin], -100.0f);
        }
    };

    //==============================================================================
    // EQ: spectrum + response curve with draggable band dots.
    class EqDisplay : public Visual, public juce::SettableTooltipClient
    {
    public:
        explicit EqDisplay (EqPlugin& p) : Visual (p), eq (p)
        {
            setTooltip ("Drag a dot: frequency / gain. Wheel: Q. Right-click: band type. Double-click a dot: reset gain; empty space: add a band.");
        }

        static juce::Colour bandColour (int b)
        {
            static const juce::Colour c[] { juce::Colour (0xffd85f5f), juce::Colour (0xffd88a4c), juce::Colour (0xffd8c64c), juce::Colour (0xff63b56d),
                                            juce::Colour (0xff4cb8d8), juce::Colour (0xff5f8fd8), juce::Colour (0xffb46ed0) };
            return c[b % 7];
        }

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            area = r;
            drawFreqGrid (g, r);
            for (float db : { -18.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 18.0f })
            {
                g.setColour (juce::Colours::white.withAlpha (db == 0.0f ? 0.16f : 0.05f));
                g.drawHorizontalLine ((int) dbToY (db), r.getX(), r.getRight());
            }

            const double sr = eq.getRate() > 0 ? eq.getRate() : 44100.0;
            juce::Path spec;
            for (int px = 0; px <= (int) r.getWidth(); px += 2)
            {
                const float x = r.getX() + (float) px;
                const float y = r.getBottom() - juce::jmap (juce::jlimit (-90.0f, 0.0f, spectrum.dbAt (xToFreq (x, r), sr)), -90.0f, 0.0f, 0.0f, r.getHeight());
                if (px == 0) spec.startNewSubPath (x, r.getBottom());
                spec.lineTo (x, y);
            }
            spec.lineTo (r.getRight(), r.getBottom());
            spec.closeSubPath();
            g.setColour (juce::Colour (0xff7a8c99).withAlpha (0.28f));
            g.fillPath (spec);

            std::array<UDsp::Coeffs, EqPlugin::numBands> coeffs;
            for (int b = 0; b < EqPlugin::numBands; ++b)
                coeffs[(size_t) b] = eq.designFromParams (b, sr);

            juce::Path curve, fill;
            for (int px = 0; px <= (int) r.getWidth(); px += 2)
            {
                const float x = r.getX() + (float) px, f = xToFreq (x, r);
                double mag = 1.0;
                for (int b = 0; b < EqPlugin::numBands; ++b)
                    if (eq.choiceValue (EqPlugin::shapeParam (b)) != 0)
                        mag *= coeffs[(size_t) b].magnitude (f, sr);
                const float y = juce::jlimit (r.getY(), r.getBottom(), dbToY ((float) juce::Decibels::gainToDecibels (mag, -60.0) + v (EqPlugin::outputParam)));
                if (px == 0) { curve.startNewSubPath (x, y); fill.startNewSubPath (x, dbToY (0)); }
                curve.lineTo (x, y);
                fill.lineTo (x, y);
            }
            fill.lineTo (r.getRight(), dbToY (0));
            fill.closeSubPath();
            g.setColour (Theme::accent.withAlpha (0.12f));
            g.fillPath (fill);
            g.setColour (Theme::accent);
            g.strokePath (curve, juce::PathStrokeType (2.0f));

            g.setFont (Theme::font (10.0f, true));
            for (int b = 0; b < EqPlugin::numBands; ++b)
            {
                const auto c = dot (b);
                const bool on = eq.choiceValue (EqPlugin::shapeParam (b)) != 0;
                g.setColour (bandColour (b).withAlpha (on ? (dragging == b ? 1.0f : 0.9f) : 0.3f));
                g.fillEllipse (c.x - 7.0f, c.y - 7.0f, 14.0f, 14.0f);
                g.setColour (juce::Colours::black);
                g.drawText (juce::String (b + 1), juce::Rectangle<float> (c.x - 7.0f, c.y - 7.0f, 14.0f, 14.0f), juce::Justification::centred);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragging = bandAt (e.position);
            if (dragging < 0) return;
            if (e.mods.isPopupMenu())
            {
                juce::PopupMenu m;
                const int b = dragging;
                for (int s = 0; s < EqPlugin::shapeNames().size(); ++s)
                    m.addItem (EqPlugin::shapeNames()[s], true, eq.choiceValue (EqPlugin::shapeParam (b)) == s,
                               [this, b, s] { set (EqPlugin::shapeParam (b), (float) s); });
                m.showMenuAsync ({});
                dragging = -1;
                return;
            }
            for (int k = 0; k < 4; ++k) eq.param (dragging * 4 + k)->parameterChangeGestureBegin();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragging < 0) return;
            set (EqPlugin::freqParam (dragging), juce::jlimit (20.0f, 20000.0f, xToFreq (e.position.x, area)));
            if (hasGain (dragging))
                set (EqPlugin::gainParam (dragging), juce::jlimit (-24.0f, 24.0f, yToDb (e.position.y)));
            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragging >= 0)
                for (int k = 0; k < 4; ++k) eq.param (dragging * 4 + k)->parameterChangeGestureEnd();
            dragging = -1;
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (const int b = bandAt (e.position); b >= 0)
                return set (EqPlugin::gainParam (b), 0.0f);
            for (int b = 0; b < EqPlugin::numBands; ++b)
                if (eq.choiceValue (EqPlugin::shapeParam (b)) == 0)
                {
                    set (EqPlugin::shapeParam (b), 1.0f);
                    set (EqPlugin::freqParam (b), xToFreq (e.position.x, area));
                    set (EqPlugin::gainParam (b), juce::jlimit (-24.0f, 24.0f, yToDb (e.position.y)));
                    return;
                }
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            if (const int b = bandAt (e.position); b >= 0)
                set (EqPlugin::qParam (b), juce::jlimit (0.1f, 18.0f, v (EqPlugin::qParam (b)) * (w.deltaY > 0 ? 1.12f : 1.0f / 1.12f)));
        }

    private:
        void tick() override
        {
            spectrum.pull (eq.tap, true);
            repaint();
        }

        void set (int index, float value) { eq.param (index)->setParameter (value, juce::sendNotificationSync); }
        bool hasGain (int b) const { const int s = eq.choiceValue (EqPlugin::shapeParam (b)); return s >= 1 && s <= 3; }
        float dbToY (float db) const { return area.getCentreY() - db / 24.0f * area.getHeight() * 0.5f; }
        float yToDb (float y) const  { return (area.getCentreY() - y) / (area.getHeight() * 0.5f) * 24.0f; }

        juce::Point<float> dot (int b) const
        {
            return { logX (v (EqPlugin::freqParam (b)), area), dbToY (hasGain (b) ? v (EqPlugin::gainParam (b)) : 0.0f) };
        }

        int bandAt (juce::Point<float> p) const
        {
            for (int b = EqPlugin::numBands; --b >= 0;)
                if (dot (b).getDistanceFrom (p) < 10.0f)
                    return b;
            return -1;
        }

        EqPlugin& eq;
        Spectrum spectrum;
        juce::Rectangle<float> area { 0, 0, 100, 100 };
        int dragging = -1;
    };

    //==============================================================================
    // Scrolling input / output level and gain reduction, optional transfer curve (compressor).
    class DynamicsDisplay : public Visual
    {
    public:
        DynamicsDisplay (Plugin& p, bool transfer) : Visual (p), showTransfer (transfer) {}

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            auto bar = r.removeFromRight (22.0f);
            juce::Rectangle<float> curveArea;
            if (showTransfer)
            {
                r.removeFromRight (6.0f);
                curveArea = r.removeFromRight (r.getHeight());
                r.removeFromRight (6.0f);
            }

            auto yFor = [&r] (float db) { return r.getY() + r.getHeight() * juce::jlimit (0.0f, 1.0f, -db / 48.0f); };
            g.setFont (Theme::font (10.0f));
            for (float db : { 0.0f, -12.0f, -24.0f, -36.0f })
            {
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.drawHorizontalLine ((int) yFor (db), r.getX(), r.getRight());
                g.setColour (Theme::textDim);
                g.drawText (juce::String ((int) db), (int) r.getX() + 2, (int) yFor (db), 30, 11, juce::Justification::left);
            }

            const int n = (int) history.size();
            const float step = r.getWidth() / (float) (n - 1);
            juce::Path in, out, gr;
            for (int i = 0; i < n; ++i)
            {
                const auto& h = history[(size_t) ((head + i) % n)];
                const float x = r.getX() + step * (float) i;
                if (i == 0) { in.startNewSubPath (x, r.getBottom()); out.startNewSubPath (x, yFor (h.out)); gr.startNewSubPath (x, r.getY()); }
                in.lineTo (x, yFor (h.in));
                out.lineTo (x, yFor (h.out));
                gr.lineTo (x, r.getY() + r.getHeight() * juce::jlimit (0.0f, 1.0f, h.gr / 24.0f));
            }
            in.lineTo (r.getRight(), r.getBottom());
            in.closeSubPath();
            gr.lineTo (r.getRight(), r.getY());
            gr.closeSubPath();
            g.setColour (juce::Colour (0xff7a8c99).withAlpha (0.35f));
            g.fillPath (in);
            g.setColour (Theme::lcdText);
            g.strokePath (out, juce::PathStrokeType (1.3f));
            g.setColour (juce::Colour (0xffe0533a).withAlpha (0.55f));
            g.fillPath (gr);

            const auto& last = history[(size_t) ((head + n - 1) % n)];
            drawBar (g, bar, last.gr / 24.0f, juce::Colour (0xffe0533a), true);
            g.setColour (Theme::text);
            g.setFont (Theme::font (10.0f, true));
            g.drawText ("GR", bar.withHeight (12).translated (0, bar.getHeight() - 12), juce::Justification::centred);
            g.drawText (juce::String (-last.gr, 1), juce::Rectangle<float> (r.getRight() - 60, r.getY() + 2, 56, 12), juce::Justification::right);

            if (showTransfer)
                drawTransfer (g, curveArea);
        }

    private:
        struct Frame { float in = -100, out = -100, gr = 0; };

        void tick() override
        {
            history[(size_t) head] = { UDsp::gainToDb (take (1)), UDsp::gainToDb (take (2)), take (0) };
            head = (head + 1) % (int) history.size();
            repaint();
        }

        void drawTransfer (juce::Graphics& g, juce::Rectangle<float> c)
        {
            g.setColour (juce::Colours::white.withAlpha (0.05f));
            g.fillRect (c);
            g.drawLine (c.getX(), c.getBottom(), c.getRight(), c.getY(), 1.0f);
            const float thr = v (CompressorPlugin::threshold), rat = v (CompressorPlugin::ratio), kn = v (CompressorPlugin::knee), mk = v (CompressorPlugin::makeup);
            juce::Path p;
            for (int i = 0; i <= 60; ++i)
            {
                const float inDb = -60.0f + (float) i;
                const float outDb = inDb - UDsp::CompCore::reductionDb (inDb, thr, rat, kn) + mk;
                const float x = c.getX() + c.getWidth() * (float) i / 60.0f, y = c.getBottom() - c.getHeight() * juce::jlimit (0.0f, 1.0f, (outDb + 60.0f) / 60.0f);
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            g.setColour (Theme::accent);
            g.strokePath (p, juce::PathStrokeType (2.0f));
            const float tx = c.getX() + c.getWidth() * (thr + 60.0f) / 60.0f;
            g.setColour (Theme::accent.withAlpha (0.3f));
            g.drawVerticalLine ((int) tx, c.getY(), c.getBottom());
        }

        bool showTransfer;
        std::array<Frame, 160> history {};
        int head = 0;
    };

    //==============================================================================
    // Maximizer: bands on a frequency axis (drag the crossovers), band GR bars, limiter GR.
    class MaximizerDisplay : public Visual
    {
    public:
        using Visual::Visual;

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            auto right = r.removeFromRight (60.0f);
            r.removeFromRight (6.0f);
            area = r;
            drawFreqGrid (g, r);
            const float x1 = logX (v (MaximizerPlugin::xLow), r), x2 = logX (v (MaximizerPlugin::xHigh), r);
            const float edges[] { r.getX(), x1, x2, r.getRight() };
            const juce::Colour colours[] { juce::Colour (0xffd88a4c), juce::Colour (0xff63b56d), juce::Colour (0xff5f8fd8) };
            const char* names[] { "LOW", "MID", "HIGH" };
            g.setFont (Theme::font (11.0f, true));
            for (int k = 0; k < 3; ++k)
            {
                auto band = juce::Rectangle<float> (edges[k], r.getY(), edges[k + 1] - edges[k], r.getHeight() - 14);
                g.setColour (colours[k].withAlpha (0.12f));
                g.fillRect (band);
                const float thrY = band.getY() + band.getHeight() * (-v (MaximizerPlugin::lowThresh + 3 * k) / 48.0f);
                g.setColour (colours[k].withAlpha (0.8f));
                g.drawHorizontalLine ((int) thrY, band.getX(), band.getRight());
                auto grBar = band.withSizeKeepingCentre (juce::jmin (24.0f, band.getWidth() * 0.4f), band.getHeight() - 20).withY (band.getY() + 18);
                drawBar (g, grBar, gr[(size_t) k] / 24.0f, colours[k], true);
                g.setColour (Theme::text);
                g.drawText (names[k], band.withHeight (16), juce::Justification::centred);
            }
            g.setColour (Theme::accent);
            for (float x : { x1, x2 })
                g.drawLine (x, r.getY(), x, r.getBottom() - 14, 2.0f);

            auto limBar = right.removeFromLeft (26.0f).withTrimmedBottom (14);
            drawBar (g, limBar, gr[3] / 12.0f, juce::Colour (0xffe0533a), true);
            auto outBar = right.removeFromRight (26.0f).withTrimmedBottom (14);
            drawBar (g, outBar, 1.0f + UDsp::gainToDb (outPeak) / 48.0f, Theme::lcdText, false);
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (10.0f));
            g.drawText ("LIM", limBar.withY (limBar.getBottom()).withHeight (12), juce::Justification::centred);
            g.drawText ("OUT", outBar.withY (outBar.getBottom()).withHeight (12), juce::Justification::centred);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const float d1 = std::abs (e.position.x - logX (v (MaximizerPlugin::xLow), area));
            const float d2 = std::abs (e.position.x - logX (v (MaximizerPlugin::xHigh), area));
            dragging = juce::jmin (d1, d2) > 8.0f ? -1 : (d1 < d2 ? MaximizerPlugin::xLow : MaximizerPlugin::xHigh);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragging < 0) return;
            auto* p = plugin->param (dragging);
            p->setParameter (p->valueRange.snapToLegalValue (xToFreq (e.position.x, area)), juce::sendNotificationSync);
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const bool near = std::abs (e.position.x - logX (v (MaximizerPlugin::xLow), area)) < 8.0f
                           || std::abs (e.position.x - logX (v (MaximizerPlugin::xHigh), area)) < 8.0f;
            setMouseCursor (near ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
        }

    private:
        void tick() override
        {
            for (int k = 0; k < 4; ++k)
                gr[(size_t) k] = juce::jmax (take (k), gr[(size_t) k] * 0.8f);
            outPeak = juce::jmax (take (4), outPeak * 0.8f);
            repaint();
        }

        std::array<float, 4> gr {};
        float outPeak = 0.0f;
        juce::Rectangle<float> area { 0, 0, 100, 100 };
        int dragging = -1;
    };

    //==============================================================================
    class ReverbDisplay : public StaticVisual
    {
    public:
        using StaticVisual::StaticVisual;

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            const float pre = v (ReverbPlugin::predelay) * 0.001f, decay = v (ReverbPlugin::decay);
            const float total = juce::jmax (1.0f, pre + decay * 1.1f);
            auto tx = [&] (float t) { return r.getX() + r.getWidth() * t / total; };

            g.setFont (Theme::font (10.0f));
            for (float t = 0.0f; t < total; t += total > 8 ? 2.0f : (total > 3 ? 0.5f : 0.25f))
            {
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.drawVerticalLine ((int) tx (t), r.getY(), r.getBottom());
                g.setColour (Theme::textDim);
                g.drawText (juce::String (t, 2) + "s", (int) tx (t) + 2, (int) r.getBottom() - 12, 40, 11, juce::Justification::left);
            }

            g.setColour (Theme::text);
            g.fillRect (tx (0) - 1.0f, r.getY() + 10, 3.0f, r.getHeight() - 24);

            // Early reflections then the exponential tail (-60 dB at `decay`); damping darkens the highs faster.
            juce::Random rng (7);
            g.setColour (Theme::accent.withAlpha (0.7f));
            const float size = v (ReverbPlugin::size);
            for (int k = 0; k < 14; ++k)
            {
                const float t = pre + (0.004f + 0.05f * size) * (float) k / 14.0f * (1.0f + rng.nextFloat());
                const float a = 0.8f * std::pow (10.0f, -3.0f * (t - pre) / decay) * (0.5f + 0.5f * rng.nextFloat());
                g.drawLine (tx (t), r.getBottom() - 14, tx (t), r.getBottom() - 14 - a * (r.getHeight() - 30), 1.5f);
            }
            for (int pass = 0; pass < 2; ++pass)
            {
                const float rate = pass == 0 ? 1.0f : 1.0f + 3.0f * v (ReverbPlugin::damping);
                juce::Path p;
                p.startNewSubPath (tx (pre), r.getBottom() - 14);
                for (int i = 0; i <= 100; ++i)
                {
                    const float t = pre + (total - pre) * (float) i / 100.0f;
                    const float a = 0.7f * std::pow (10.0f, -3.0f * (t - pre) * rate / decay);
                    p.lineTo (tx (t), r.getBottom() - 14 - a * (r.getHeight() - 30));
                }
                p.lineTo (r.getRight(), r.getBottom() - 14);
                p.closeSubPath();
                g.setColour ((pass == 0 ? Theme::accent : juce::Colour (0xff5fb6d8)).withAlpha (pass == 0 ? 0.25f : 0.35f));
                g.fillPath (p);
            }
            g.setColour (Theme::text);
            g.setFont (Theme::font (11.0f, true));
            g.drawText ("RT60 " + juce::String (decay, 1) + " s   pre " + juce::String (juce::roundToInt (pre * 1000)) + " ms   width "
                            + juce::String (juce::roundToInt (v (ReverbPlugin::width) * 100)) + "%",
                        r.reduced (8, 4).removeFromTop (14), juce::Justification::right);
        }
    };

    //==============================================================================
    class DelayDisplay : public Visual
    {
    public:
        explicit DelayDisplay (DelayPlugin& p) : Visual (p), delay (p) {}

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            const double bpm = delay.lastBpm.load(), d = delay.delaySeconds (bpm);
            const float fb = v (DelayPlugin::feedback), wet = v (DelayPlugin::wet);
            const bool pp = delay.choiceValue (DelayPlugin::pingpong) != 0;
            const double total = juce::jmax (1.0, d * 8.5);
            auto tx = [&] (double t) { return r.getX() + 8.0f + (r.getWidth() - 16.0f) * (float) (t / total); };
            const float mid = r.getCentreY(), half = r.getHeight() * 0.5f - 16.0f;

            g.setColour (juce::Colours::white.withAlpha (0.1f));
            g.drawHorizontalLine ((int) mid, r.getX(), r.getRight());
            g.setFont (Theme::font (10.0f));
            g.setColour (Theme::textDim);
            g.drawText ("L", r.withHeight (14).withTrimmedLeft (4), juce::Justification::left);
            g.drawText ("R", r.withTrimmedTop (r.getHeight() - 14).withTrimmedLeft (4), juce::Justification::left);

            g.setColour (Theme::text);
            g.fillRect (tx (0) - 1.5f, mid - half, 3.0f, half * 2.0f);
            float a = wet;
            for (int k = 1; k <= 8 && a > 0.01f; ++k)
            {
                const float x = tx (d * k);
                g.setColour (Theme::accent.withAlpha (0.9f));
                const bool left = ! pp || (k % 2 == 1), right = ! pp || (k % 2 == 0);
                if (left)  g.fillRect (x - 2.0f, mid - half * a, 4.0f, half * a);
                if (right) g.fillRect (x - 2.0f, mid, 4.0f, half * a);
                a *= fb;
            }

            const bool synced = delay.choiceValue (DelayPlugin::sync) != 0;
            g.setColour (Theme::text);
            g.setFont (Theme::font (12.0f, true));
            juce::String label = juce::String (juce::roundToInt (d * 1000.0)) + " ms";
            if (synced)
                label << "  (" << UDsp::divisionNames()[delay.choiceValue (DelayPlugin::division)] << " at " << juce::String (bpm, 1) << " BPM)";
            if (pp) label << "  ping-pong";
            g.drawText (label, r.reduced (8, 2).removeFromTop (16), juce::Justification::right);
        }

    private:
        void tick() override { if (paramsChanged() || ! juce::exactlyEqual (delay.lastBpm.load(), shownBpm)) { shownBpm = delay.lastBpm.load(); repaint(); } }

        DelayPlugin& delay;
        float shownBpm = 0.0f;
    };

    //==============================================================================
    class FilterDisplay : public Visual
    {
    public:
        using Visual::Visual;

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            drawFreqGrid (g, r);
            const int mode = plugin->choiceValue (FilterPlugin::shape);
            const bool steep = plugin->choiceValue (FilterPlugin::slope) == 1;
            const float k = FilterPlugin::kFor (v (FilterPlugin::reso)), mix = v (FilterPlugin::mix);
            auto dbToY = [&r] (float db) { return r.getY() + r.getHeight() * juce::jlimit (0.0f, 1.0f, (24.0f - db) / 72.0f); };
            g.setColour (juce::Colours::white.withAlpha (0.12f));
            g.drawHorizontalLine ((int) dbToY (0), r.getX(), r.getRight());

            auto response = [&] (float cutoff)
            {
                juce::Path p;
                for (int px = 0; px <= (int) r.getWidth(); px += 2)
                {
                    const float x = r.getX() + (float) px, f = xToFreq (x, r);
                    std::complex<double> s (0.0, f / cutoff);
                    auto stage = [&] (float kk)
                    {
                        const auto den = s * s + (double) kk * s + 1.0;
                        switch (mode)
                        {
                            case 1:  return s * s / den;
                            case 2:  return (double) kk * s / den;
                            case 3:  return (s * s + 1.0) / den;
                            default: return 1.0 / den;
                        }
                    };
                    auto h = stage (k);
                    if (steep) h *= stage (1.414f);
                    const float mag = (float) std::abs ((1.0 - mix) + (double) mix * h);
                    const float y = dbToY (juce::Decibels::gainToDecibels (mag, -60.0f));
                    if (px == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
                }
                return p;
            };

            g.setColour (Theme::textDim.withAlpha (0.6f));
            g.strokePath (response (v (FilterPlugin::cutoff)), juce::PathStrokeType (1.0f));
            const float modulated = modCutoff > 0.0f ? modCutoff : v (FilterPlugin::cutoff);
            g.setColour (Theme::accent);
            g.strokePath (response (modulated), juce::PathStrokeType (2.0f));
            g.setColour (Theme::accent.withAlpha (0.4f));
            g.drawVerticalLine ((int) logX (modulated, r), r.getY(), r.getBottom());

            // LFO phase dot
            auto lfoBox = r.removeFromTop (34).removeFromRight (90).reduced (4);
            juce::Path wave;
            const int shapeIndex = plugin->choiceValue (FilterPlugin::lfoShape);
            for (int i = 0; i <= 40; ++i)
            {
                const float x = lfoBox.getX() + lfoBox.getWidth() * (float) i / 40.0f;
                const float y = lfoBox.getCentreY() - UDsp::lfo (shapeIndex, i / 40.0) * lfoBox.getHeight() * 0.45f;
                if (i == 0) wave.startNewSubPath (x, y); else wave.lineTo (x, y);
            }
            g.setColour (Theme::textDim);
            g.strokePath (wave, juce::PathStrokeType (1.0f));
            const float px = lfoBox.getX() + lfoBox.getWidth() * phase;
            g.setColour (v (FilterPlugin::lfoDepth) > 0.0f ? Theme::lcdText : Theme::textDim);
            g.fillEllipse (px - 3.0f, lfoBox.getCentreY() - UDsp::lfo (shapeIndex, phase) * lfoBox.getHeight() * 0.45f - 3.0f, 6.0f, 6.0f);
        }

    private:
        void tick() override
        {
            modCutoff = plugin->meters[0].load();
            phase = plugin->meters[1].load();
            repaint();
        }
        float modCutoff = 0.0f, phase = 0.0f;
    };

    //==============================================================================
    // Transfer curve (input -> output) for the distortion and the soft clipper.
    class CurveDisplay : public StaticVisual
    {
    public:
        CurveDisplay (Plugin& p, std::function<float (float)> f) : StaticVisual (p), fn (std::move (f)) {}

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            auto sq = r.withSizeKeepingCentre (r.getHeight() * 1.6f, r.getHeight());
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawHorizontalLine ((int) sq.getCentreY(), sq.getX(), sq.getRight());
            g.drawVerticalLine ((int) sq.getCentreX(), sq.getY(), sq.getBottom());
            g.drawLine (sq.getX(), sq.getBottom(), sq.getRight(), sq.getY(), 1.0f);
            for (float lvl : { -1.0f, 1.0f })
                g.drawHorizontalLine ((int) (sq.getCentreY() - lvl * sq.getHeight() * 0.4f), sq.getX(), sq.getRight());

            juce::Path p;
            for (int i = 0; i <= 200; ++i)
            {
                const float in = -1.25f + 2.5f * (float) i / 200.0f;
                const float out = juce::jlimit (-1.25f, 1.25f, fn (in));
                const float x = sq.getX() + sq.getWidth() * (float) i / 200.0f, y = sq.getCentreY() - out * sq.getHeight() * 0.4f;
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            g.setColour (Theme::accent);
            g.strokePath (p, juce::PathStrokeType (2.0f));
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (10.0f));
            g.drawText ("in", sq.removeFromBottom (12).removeFromRight (30), juce::Justification::right);
        }

    private:
        std::function<float (float)> fn;
    };

    //==============================================================================
    // L/R level bars with a balance indicator.
    class BalanceDisplay : public Visual
    {
    public:
        using Visual::Visual;

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            auto bars = r.removeFromLeft (60.0f).reduced (6.0f, 2.0f);
            drawBar (g, bars.removeFromLeft (20.0f), 1.0f + UDsp::gainToDb (level[0]) / 60.0f, Theme::lcdText, false);
            bars.removeFromLeft (8.0f);
            drawBar (g, bars.removeFromLeft (20.0f), 1.0f + UDsp::gainToDb (level[1]) / 60.0f, Theme::lcdText, false);

            const float pan = v (BalancePlugin::pan);
            auto arc = r.reduced (20.0f).withSizeKeepingCentre (r.getHeight() * 1.4f, r.getHeight() * 0.8f);
            juce::Path track;
            track.addCentredArc (arc.getCentreX(), arc.getBottom(), arc.getWidth() * 0.45f, arc.getHeight() * 0.9f, 0.0f, -1.2f, 1.2f, true);
            g.setColour (juce::Colours::white.withAlpha (0.15f));
            g.strokePath (track, juce::PathStrokeType (6.0f));
            const float angle = pan * 1.2f;
            const auto tip = juce::Point<float> (arc.getCentreX() + std::sin (angle) * arc.getWidth() * 0.45f,
                                                 arc.getBottom() - std::cos (angle) * arc.getHeight() * 0.9f);
            g.setColour (Theme::accent);
            g.fillEllipse (tip.x - 7.0f, tip.y - 7.0f, 14.0f, 14.0f);
            g.setColour (Theme::text);
            g.setFont (Theme::font (13.0f, true));
            g.drawText (plugin->param (BalancePlugin::pan)->getCurrentValueAsString() + "   " + plugin->param (BalancePlugin::volume)->getCurrentValueAsString(),
                        r.removeFromBottom (18), juce::Justification::centred);
        }

    private:
        void tick() override
        {
            for (int k = 0; k < 2; ++k)
                level[k] = juce::jmax (take (k), level[k] * 0.85f);
            repaint();
        }
        float level[2] {};
    };

    //==============================================================================
    // Goniometer (mid up, side across) + correlation.
    class Vectorscope : public Visual
    {
    public:
        explicit Vectorscope (StereoPlugin& p) : Visual (p), stereo (p) {}

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            auto corrBar = r.removeFromBottom (14.0f);
            auto sq = r.withSizeKeepingCentre (r.getHeight(), r.getHeight());
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawLine (sq.getX(), sq.getCentreY(), sq.getRight(), sq.getCentreY());
            g.drawLine (sq.getCentreX(), sq.getY(), sq.getCentreX(), sq.getBottom());
            g.drawLine (sq.getX(), sq.getY(), sq.getRight(), sq.getBottom());
            g.drawLine (sq.getX(), sq.getBottom(), sq.getRight(), sq.getY());
            g.setColour (Theme::lcdText.withAlpha (0.5f));
            for (int i = 0; i < count; ++i)
            {
                const float side = (pointsL[(size_t) i] - pointsR[(size_t) i]) * 0.7071f, mid = (pointsL[(size_t) i] + pointsR[(size_t) i]) * 0.7071f;
                const float x = sq.getCentreX() + juce::jlimit (-1.0f, 1.0f, side) * sq.getWidth() * 0.48f;
                const float y = sq.getCentreY() - juce::jlimit (-1.0f, 1.0f, mid) * sq.getHeight() * 0.48f;
                g.fillRect (x, y, 1.5f, 1.5f);
            }
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (10.0f));
            g.drawText ("L", sq.withSize (14, 14), juce::Justification::centred);
            g.drawText ("R", sq.withLeft (sq.getRight() - 14).withHeight (14), juce::Justification::centred);

            auto track = corrBar.withSizeKeepingCentre (juce::jmin (300.0f, corrBar.getWidth()), 6.0f);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.fillRect (track);
            const float cx = track.getCentreX() + correlation * track.getWidth() * 0.5f;
            g.setColour (correlation < 0 ? juce::Colour (0xffe0533a) : Theme::lcdText);
            g.fillRect (juce::jmin (cx, track.getCentreX()), track.getY(), std::abs (cx - track.getCentreX()) + 1.0f, track.getHeight());
            g.setColour (Theme::textDim);
            g.drawText ("-1", track.withX (track.getX() - 22).withWidth (20).expanded (0, 4), juce::Justification::right);
            g.drawText ("+1", track.withX (track.getRight() + 2).withWidth (20).expanded (0, 4), juce::Justification::left);
        }

    private:
        void tick() override
        {
            std::array<float, 4096> l, r;
            const int n = stereo.tap.read (l.data(), r.data(), (int) l.size());
            count = juce::jmin (n, (int) pointsL.size());
            double lr = 0, ll = 0, rr = 0;
            for (int i = 0; i < count; ++i)
            {
                pointsL[(size_t) i] = l[(size_t) (n - count + i)];
                pointsR[(size_t) i] = r[(size_t) (n - count + i)];
                lr += pointsL[(size_t) i] * pointsR[(size_t) i];
                ll += pointsL[(size_t) i] * pointsL[(size_t) i];
                rr += pointsR[(size_t) i] * pointsR[(size_t) i];
            }
            const float c = ll * rr > 1.0e-12 ? (float) (lr / std::sqrt (ll * rr)) : 0.0f;
            correlation += (c - correlation) * 0.3f;
            repaint();
        }

        StereoPlugin& stereo;
        std::array<float, 1024> pointsL {}, pointsR {};
        int count = 0;
        float correlation = 0.0f;
    };

    //==============================================================================
    // LFO shape with the current phase (chorus, flanger, phaser).
    class LfoDisplay : public Visual
    {
    public:
        LfoDisplay (Plugin& p, int shapeIndex, juce::String caption) : Visual (p), shape (shapeIndex), text (std::move (caption)) {}

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds()).reduced (10.0f, 14.0f);
            juce::Path wave;
            for (int cycle = 0; cycle < 2; ++cycle)
                for (int i = 0; i <= 100; ++i)
                {
                    const float x = r.getX() + r.getWidth() * ((float) cycle + (float) i / 100.0f) * 0.5f;
                    const float y = r.getCentreY() - UDsp::lfo (shape, i / 100.0) * r.getHeight() * 0.45f;
                    if (cycle == 0 && i == 0) wave.startNewSubPath (x, y); else wave.lineTo (x, y);
                }
            g.setColour (juce::Colours::white.withAlpha (0.1f));
            g.drawHorizontalLine ((int) r.getCentreY(), r.getX(), r.getRight());
            g.setColour (Theme::accent.withAlpha (0.8f));
            g.strokePath (wave, juce::PathStrokeType (2.0f));
            for (int cycle = 0; cycle < 2; ++cycle)
            {
                const float x = r.getX() + r.getWidth() * ((float) cycle + phase) * 0.5f;
                g.setColour (Theme::lcdText);
                g.fillEllipse (x - 5.0f, r.getCentreY() - UDsp::lfo (shape, phase) * r.getHeight() * 0.45f - 5.0f, 10.0f, 10.0f);
            }
            const bool synced = plugin->choiceValue (1) != 0;
            g.setColour (Theme::text);
            g.setFont (Theme::font (12.0f, true));
            g.drawText (text + "   " + (synced ? "sync " + UDsp::divisionNames()[plugin->choiceValue (2)] : plugin->param (0)->getCurrentValueAsString()),
                        getLocalBounds().reduced (14, 8).removeFromTop (16), juce::Justification::right);
        }

    private:
        void tick() override { phase = plugin->meters[0].load(); repaint(); }
        int shape;
        juce::String text;
        float phase = 0.0f;
    };

    //==============================================================================
    // Gate: draw the 16 steps (drag across), envelope line and playhead.
    class StepEditor : public Visual, public juce::SettableTooltipClient
    {
    public:
        explicit StepEditor (GatePlugin& p) : Visual (p), gate (p)
        {
            setTooltip ("Draw the volume shape: click / drag over the steps. Right-click a step: off / full.");
        }

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            area = r;
            const float w = r.getWidth() / GatePlugin::numSteps;
            for (int s = 0; s < GatePlugin::numSteps; ++s)
            {
                auto cell = juce::Rectangle<float> (r.getX() + w * (float) s, r.getY(), w, r.getHeight());
                g.setColour (juce::Colours::white.withAlpha ((s / 4) % 2 == 0 ? 0.04f : 0.02f));
                g.fillRect (cell);
                const float h = v (s) * r.getHeight();
                g.setColour (Theme::accent.withAlpha (0.35f));
                g.fillRect (cell.reduced (2.0f, 0.0f).withTrimmedTop (r.getHeight() - h));
            }
            juce::Path env;
            for (int px = 0; px <= (int) r.getWidth(); ++px)
            {
                const float y = r.getBottom() - gate.envelopeAt ((double) px / r.getWidth()) * r.getHeight();
                if (px == 0) env.startNewSubPath (r.getX(), y); else env.lineTo (r.getX() + (float) px, y);
            }
            g.setColour (Theme::accent);
            g.strokePath (env, juce::PathStrokeType (2.0f));
            g.setColour (Theme::lcdText.withAlpha (0.8f));
            g.drawVerticalLine ((int) (r.getX() + r.getWidth() * phase), r.getY(), r.getBottom());
            const int mode = gate.choiceValue (GatePlugin::timeMode);
            if (mode != 0)
            {
                g.setColour (Theme::text);
                g.setFont (Theme::font (11.0f, true));
                g.drawText (gate.param (GatePlugin::timeMode)->getCurrentValueAsString().toUpperCase(), r.reduced (6, 4).removeFromTop (14), juce::Justification::right);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu())
            {
                const int s = stepAt (e.position.x);
                set (s, v (s) > 0.5f ? 0.0f : 1.0f);
                return;
            }
            mouseDrag (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) return;
            set (stepAt (e.position.x), juce::jlimit (0.0f, 1.0f, (area.getBottom() - e.position.y) / area.getHeight()));
        }

    private:
        void tick() override { phase = plugin->meters[0].load(); repaint(); }
        int stepAt (float x) const { return juce::jlimit (0, GatePlugin::numSteps - 1, (int) ((x - area.getX()) / area.getWidth() * GatePlugin::numSteps)); }
        void set (int s, float value) { gate.param (s)->setParameter (value, juce::sendNotificationSync); }

        GatePlugin& gate;
        juce::Rectangle<float> area { 0, 0, 100, 100 };
        float phase = 0.0f;
    };

    //==============================================================================
    class EnhancerDisplay : public Visual
    {
    public:
        using Visual::Visual;

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            auto dial = r.removeFromLeft (r.getHeight()).reduced (8.0f);
            const float amt = v (0);
            juce::Path track, value;
            const float a0 = -2.4f, a1 = 2.4f;
            track.addCentredArc (dial.getCentreX(), dial.getCentreY(), dial.getWidth() * 0.45f, dial.getWidth() * 0.45f, 0.0f, a0, a1, true);
            value.addCentredArc (dial.getCentreX(), dial.getCentreY(), dial.getWidth() * 0.45f, dial.getWidth() * 0.45f, 0.0f, a0, a0 + (a1 - a0) * amt, true);
            g.setColour (juce::Colours::white.withAlpha (0.1f));
            g.strokePath (track, juce::PathStrokeType (10.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (Theme::accent);
            g.strokePath (value, juce::PathStrokeType (10.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (Theme::text);
            g.setFont (Theme::font (22.0f, true));
            g.drawText (juce::String (juce::roundToInt (amt * 100)) + "%", dial, juce::Justification::centred);
            g.setFont (Theme::font (12.0f, true));
            g.drawText (plugin->param (1)->getCurrentValueAsString().toUpperCase(), dial.withTrimmedTop (dial.getHeight() * 0.65f), juce::Justification::centredTop);

            r.removeFromLeft (16.0f);
            const char* names[] { "LOW", "MID", "HIGH" };
            g.setFont (Theme::font (10.0f, true));
            for (int k = 0; k < 3; ++k)
            {
                auto col = r.removeFromLeft (r.getWidth() / (float) (3 - k)).reduced (12.0f, 0.0f);
                auto label = col.removeFromBottom (14.0f);
                drawBar (g, col.withSizeKeepingCentre (22.0f, col.getHeight()), gr[k] / 12.0f, juce::Colour (0xffe0533a), true);
                g.setColour (Theme::textDim);
                g.drawText (names[k], label, juce::Justification::centred);
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            auto* p = plugin->param (0);
            if (e.mouseWasDraggedSinceMouseDown())
                p->setParameter (juce::jlimit (0.0f, 1.0f, startAmount - (float) e.getDistanceFromDragStartY() / 200.0f), juce::sendNotificationSync);
        }
        void mouseDown (const juce::MouseEvent&) override { startAmount = v (0); }

    private:
        void tick() override
        {
            for (int k = 0; k < 3; ++k)
                gr[k] = juce::jmax (take (k), gr[k] * 0.85f);
            repaint();
        }
        float gr[3] {}, startAmount = 0.0f;
    };

    //==============================================================================
    class LoudnessDisplay : public Visual
    {
    public:
        explicit LoudnessDisplay (LoudnessPlugin& p) : Visual (p), meter (p)
        {
            history.fill (-100.0f);
            reset.onClick = [this] { meter.requestReset(); history.fill (-100.0f); };
            reset.setTooltip ("Restart the integrated measurement");
            addAndMakeVisible (reset);
        }

        void resized() override { reset.setBounds (getWidth() - 74, 14, 60, 22); }

        void paint (juce::Graphics& g) override
        {
            auto r = lcdArea (g, getLocalBounds());
            auto readouts = r.removeFromTop (70.0f);
            const float target = v (LoudnessPlugin::target);
            const char* labels[] { "MOMENTARY", "SHORT-TERM", "INTEGRATED", "TRUE PEAK" };
            const float values[] { plugin->meters[0].load(), plugin->meters[1].load(), plugin->meters[2].load(),
                                   UDsp::gainToDb (plugin->meters[3].load()) };
            readouts.removeFromRight (70.0f);
            for (int k = 0; k < 4; ++k)
            {
                auto box = readouts.removeFromLeft (readouts.getWidth() / (float) (4 - k)).reduced (4.0f);
                g.setColour (Theme::textDim);
                g.setFont (Theme::font (10.0f, true));
                g.drawText (labels[k], box.removeFromTop (14), juce::Justification::centred);
                const bool tp = k == 3;
                const bool hot = tp ? values[k] > -1.0f : (k == 2 && values[k] > target + 1.0f);
                g.setColour (hot ? juce::Colour (0xffe0533a) : (k == 2 ? Theme::accent : Theme::lcdText));
                g.setFont (Theme::font (k == 2 ? 26.0f : 22.0f, true));
                g.drawText (values[k] <= -99.0f ? juce::String ("--") : juce::String (values[k], 1), box.removeFromTop (30), juce::Justification::centred);
                g.setColour (Theme::textDim);
                g.setFont (Theme::font (10.0f));
                g.drawText (tp ? "dBTP" : "LUFS", box, juce::Justification::centredTop);
            }

            auto graph = r.reduced (2.0f, 4.0f);
            auto yFor = [&graph] (float lufs) { return graph.getY() + graph.getHeight() * juce::jlimit (0.0f, 1.0f, (0.0f - lufs) / 48.0f); };
            g.setFont (Theme::font (10.0f));
            for (float l : { -6.0f, -12.0f, -18.0f, -24.0f, -36.0f })
            {
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.drawHorizontalLine ((int) yFor (l), graph.getX(), graph.getRight());
                g.setColour (Theme::textDim);
                g.drawText (juce::String ((int) l), (int) graph.getX() + 2, (int) yFor (l) - 11, 30, 11, juce::Justification::left);
            }
            g.setColour (Theme::accent.withAlpha (0.7f));
            g.drawHorizontalLine ((int) yFor (target), graph.getX(), graph.getRight());
            g.drawText ("target " + juce::String (target, 0), graph.withHeight (12).withY (yFor (target) - 12).withTrimmedRight (4), juce::Justification::right);

            juce::Path p;
            const int n = (int) history.size();
            bool started = false;
            for (int i = 0; i < n; ++i)
            {
                const float val = history[(size_t) ((head + i) % n)];
                if (val <= -99.0f) { started = false; continue; }
                const float x = graph.getX() + graph.getWidth() * (float) i / (float) (n - 1);
                if (! started) { p.startNewSubPath (x, yFor (val)); started = true; } else p.lineTo (x, yFor (val));
            }
            g.setColour (Theme::lcdText);
            g.strokePath (p, juce::PathStrokeType (1.5f));
        }

    private:
        void tick() override
        {
            if (++frames % 6 == 0) // 5 points a second = 60 s of short-term history
            {
                history[(size_t) head] = plugin->meters[1].load();
                head = (head + 1) % (int) history.size();
            }
            repaint();
        }

        LoudnessPlugin& meter;
        juce::TextButton reset { "Reset" };
        std::array<float, 300> history {};
        int head = 0, frames = 0;
    };

    //==============================================================================
    // EQ window body: display + one column per band.
    class EqView : public juce::Component, private juce::Timer
    {
    public:
        EqView (Studio& s, EqPlugin& p)
        {
            display = std::make_unique<EqDisplay> (p);
            addAndMakeVisible (*display);
            for (int i = 0; i <= EqPlugin::outputParam; ++i)
                addAndMakeVisible (cells.add (new Cell (s, p, i)));
            setSize (620, displayHeight + 32 + 3 * Cell::height + 10);
            startTimerHz (15);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::panelDark);
            for (int b = 0; b < EqPlugin::numBands; ++b)
            {
                auto col = column (b).toFloat();
                g.setColour (Theme::panel);
                g.fillRoundedRectangle (col, 4.0f);
                g.setColour (EqDisplay::bandColour (b));
                g.fillRoundedRectangle (col.removeFromTop (4.0f), 2.0f);
            }
        }

        void resized() override
        {
            display->setBounds (0, 0, getWidth(), displayHeight);
            for (int b = 0; b < EqPlugin::numBands; ++b)
            {
                auto col = column (b).withTrimmedTop (8);
                cells[b * 4]->setBounds (col.removeFromTop (24).reduced (4, 0));
                col.removeFromTop (4);
                for (int k = 1; k < 4; ++k)
                    cells[b * 4 + k]->setBounds (col.removeFromTop (Cell::height).withSizeKeepingCentre (Cell::width, Cell::height));
            }
            cells[EqPlugin::outputParam]->setBounds (getWidth() - Cell::width - 4, displayHeight + 8, Cell::width, Cell::height);
        }

    private:
        juce::Rectangle<int> column (int b) const
        {
            const int w = (getWidth() - 80) / EqPlugin::numBands;
            return { 4 + b * w, displayHeight + 2, w - 4, getHeight() - displayHeight - 6 };
        }

        void timerCallback() override { for (auto* c : cells) c->sync(); }

        static constexpr int displayHeight = 280;
        std::unique_ptr<EqDisplay> display;
        juce::OwnedArray<Cell> cells;
    };

    //==============================================================================
    inline std::unique_ptr<juce::Component> createView (Studio& s, Plugin& p)
    {
        auto view = [&] (std::unique_ptr<juce::Component> vis, int h, std::vector<Section> secs, int width = 560)
        {
            return std::make_unique<View> (s, p, std::move (vis), h, std::move (secs), width);
        };
        const auto type = p.getPluginType();
        const Section lfo { "LFO", { "rate", "sync", "division" } };

        if (auto* eq = dynamic_cast<EqPlugin*> (&p))
            return std::make_unique<EqView> (s, *eq);
        if (type == LimiterPlugin::xmlTypeName)
            return view (std::make_unique<DynamicsDisplay> (p, false), 190, { { "Limiter", { "gain", "ceiling", "release" } } });
        if (type == CompressorPlugin::xmlTypeName)
            return view (std::make_unique<DynamicsDisplay> (p, true), 190, { { "Dynamics", { "threshold", "ratio", "knee", "attack", "release" } },
                                                                              { "Output", { "makeup", "mix" } } });
        if (type == MaximizerPlugin::xmlTypeName)
            return view (std::make_unique<MaximizerDisplay> (p), 180, { { "Low", { "lowThresh", "lowRatio", "lowGain" } },
                                                                          { "Mid", { "midThresh", "midRatio", "midGain" } },
                                                                          { "High", { "highThresh", "highRatio", "highGain" } },
                                                                          { "Crossover", { "xLow", "xHigh" } }, { "Timing", { "attack", "release" } },
                                                                          { "Master", { "drive", "ceiling", "limRelease" } } }, 680);
        if (type == SoftClipperPlugin::xmlTypeName)
            return view (std::make_unique<CurveDisplay> (p, [&p] (float x)
                         {
                             return SoftClipperPlugin::clip (x, UDsp::dbToGain (p.value (SoftClipperPlugin::threshold))) * UDsp::dbToGain (p.value (SoftClipperPlugin::post));
                         }), 190, { { "Clipper", { "threshold", "post" } } });
        if (type == BalancePlugin::xmlTypeName)
            return view (std::make_unique<BalanceDisplay> (p), 150, { { "Balance", { "volume", "pan" } } });
        if (auto* st = dynamic_cast<StereoPlugin*> (&p))
            return view (std::make_unique<Vectorscope> (*st), 220, { { "Stereo", { "width", "pan", "offset", "invert" } } });
        if (auto* lm = dynamic_cast<LoudnessPlugin*> (&p))
            return view (std::make_unique<LoudnessDisplay> (*lm), 230, { { "Reference", { "target" } } });
        if (type == ReverbPlugin::xmlTypeName)
            return view (std::make_unique<ReverbDisplay> (p), 170, { { "Room", { "predelay", "size", "decay", "damping" } },
                                                                       { "Tone", { "lowcut", "width" } }, { "Mix", { "dry", "wet" } } });
        if (auto* d = dynamic_cast<DelayPlugin*> (&p))
            return view (std::make_unique<DelayDisplay> (*d), 160, { { "Time", { "sync", "division", "time" } },
                                                                       { "Feedback", { "feedback", "pingpong", "lowcut", "highcut" } },
                                                                       { "Mix", { "dry", "wet" } } }, 680);
        if (type == FilterPlugin::xmlTypeName)
            return view (std::make_unique<FilterDisplay> (p), 180, { { "Filter", { "shape", "slope", "cutoff", "reso", "mix" } },
                                                                       { "LFO", { "lfoDepth", "lfoShape", "lfoSync", "lfoRate", "lfoDiv" } },
                                                                       { "Envelope", { "envDepth", "envAttack", "envRelease" } } });
        if (type == DistortionPlugin::xmlTypeName)
            return view (std::make_unique<CurveDisplay> (p, [&p] (float x)
                         {
                             const int c = p.choiceValue (DistortionPlugin::curve);
                             const float d = p.value (DistortionPlugin::drive), m = p.value (DistortionPlugin::mix);
                             const float y = DistortionPlugin::shape (c, x * DistortionPlugin::gainFor (c, d), DistortionPlugin::bitsFor (d));
                             return (x + (y - x) * m) * UDsp::dbToGain (p.value (DistortionPlugin::output));
                         }), 190, { { "Distortion", { "drive", "curve", "tone", "mix", "output" } } });
        if (type == EnhancerPlugin::xmlTypeName)
            return view (std::make_unique<EnhancerDisplay> (p), 170, { { "Enhancer", { "amount", "mode", "output" } } });
        if (type == ChorusPlugin::xmlTypeName)
            return view (std::make_unique<LfoDisplay> (p, 0, "CHORUS"), 130, { lfo, { "Chorus", { "depth", "delay", "voices", "spread", "mix" } } }, 660);
        if (type == FlangerPlugin::xmlTypeName)
            return view (std::make_unique<LfoDisplay> (p, 1, "FLANGER"), 130, { lfo, { "Flanger", { "depth", "delay", "feedback", "spread", "mix" } } }, 660);
        if (type == PhaserPlugin::xmlTypeName)
            return view (std::make_unique<LfoDisplay> (p, 0, "PHASER"), 130, { lfo, { "Phaser", { "stages", "freq", "depth", "feedback", "spread", "mix" } } }, 660);
        if (auto* gate = dynamic_cast<GatePlugin*> (&p))
            return view (std::make_unique<StepEditor> (*gate), 170, { { "Gate", { "length", "smooth", "fade", "depth" } },
                                                                        { "Time FX", { "timeMode", "timeDiv", "timeMix" } } });

        juce::StringArray all;
        for (auto& spec : p.getSpecs()) all.add (spec.id);
        return view (nullptr, 0, { { p.getName(), all } });
    }
}
