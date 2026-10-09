#pragma once

#include "UntitledEffects.h"

namespace UFX
{
    // Shared LFO clock: synced to the song position (beats) or free-running in Hz.
    struct LfoClock
    {
        double freePhase = 0.0;

        // Returns the start phase for this block and the per-sample increment, both in cycles.
        std::pair<double, double> begin (const Block& b, bool sync, double divBeats, double hz, double sr) const
        {
            if (sync)
                return { b.beat / divBeats, b.beatsPerSample / divBeats };
            return { freePhase, hz / sr };
        }
        void end (double phase) { freePhase = phase - std::floor (phase); }
    };

    //==============================================================================
    // 8-line feedback delay network reverb (covers the role of FL's Reeverb 2).
    class ReverbPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_reverb";
        static const char* getPluginName() { return "Untitled Reverb"; }
        enum { predelay, size, decay, damping, lowcut, width, dry, wet };

        ReverbPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), {
                  knob ("predelay", "Predelay", 0.0f, 250.0f, 20.0f, "ms", 40.0f),
                  knob ("size", "Size", 0.0f, 1.0f, 0.6f, "%"),
                  knob ("decay", "Decay", 0.2f, 20.0f, 2.5f, "s", 2.5f),
                  knob ("damping", "Damping", 0.0f, 1.0f, 0.5f, "%"),
                  knob ("lowcut", "Low Cut", 20.0f, 1000.0f, 120.0f, "Hz", 200.0f),
                  knob ("width", "Width", 0.0f, 1.0f, 1.0f, "%"),
                  knob ("dry", "Dry", 0.0f, 1.0f, 1.0f, "%"),
                  knob ("wet", "Wet", 0.0f, 1.0f, 0.3f, "%") }) {}

        double getTailLength() const override { return value (decay) * 1.2 + value (predelay) * 0.001; }

    protected:
        static constexpr int lines = 8;
        static constexpr std::array<int, lines> baseLengths { 1433, 1601, 1867, 2053, 2251, 2399, 2617, 2797 };
        static constexpr std::array<int, 8> diffuserLengths { 142, 107, 379, 277, 151, 113, 397, 263 };

        struct Allpass
        {
            std::vector<float> buf;
            int pos = 0;
            float process (float x, float g) noexcept
            {
                const float d = buf[(size_t) pos];
                const float w = x + g * d;
                buf[(size_t) pos] = w;
                pos = (pos + 1) % (int) buf.size();
                return d - g * w;
            }
        };

        void prepare (double sr) override
        {
            const double scale = sr / 44100.0;
            pre[0].prepare ((int) (sr * 0.26));
            pre[1].prepare ((int) (sr * 0.26));
            for (int k = 0; k < lines; ++k)
            {
                delays[(size_t) k].prepare ((int) (baseLengths[(size_t) k] * 1.35 * scale) + 64);
                damp[(size_t) k] = 0.0f;
                length[(size_t) k] = targetLength (k);
            }
            for (int k = 0; k < 8; ++k)
            {
                diffusers[(size_t) k].buf.assign ((size_t) juce::jmax (1, (int) (diffuserLengths[(size_t) k] * scale)), 0.0f);
                diffusers[(size_t) k].pos = 0;
            }
            lowcutFilter.reset();
            predelayS.reset (sr, value (predelay) * 0.001f * (float) sr, 0.1);
            dryS.reset (sr, value (dry));
            wetS.reset (sr, value (wet));
            widthS.reset (sr, value (width));
            modPhase = 0.0;
        }

        void process (const Block& b) override
        {
            lowcutFilter.c = UDsp::Coeffs::make (UDsp::Shape::lowCut, value (lowcut), 0.0, 0.7071, rate);
            predelayS.target (value (predelay) * 0.001f * (float) rate);
            dryS.target (value (dry));
            wetS.target (value (wet));
            widthS.target (value (width));
            const float rt60 = value (decay);
            const float dampCoef = (float) (1.0 - std::exp (-2.0 * UDsp::pi * 20000.0 * std::pow (0.06, (double) value (damping)) / rate));
            const float glide = UDsp::coefFor (0.3, rate);
            std::array<float, lines> target, gain;
            for (int k = 0; k < lines; ++k)
            {
                target[(size_t) k] = targetLength (k);
                gain[(size_t) k] = std::pow (10.0f, -3.0f * length[(size_t) k] / ((float) rate * rt60));
            }
            const float modDepth = (float) (rate * 0.0004);
            const double modInc = 0.7 / rate;

            for (int i = 0; i < b.n; ++i)
            {
                const float inL = b.l[i], inR = b.r[i];
                pre[0].push (inL);
                pre[1].push (inR);
                const float pd = predelayS.next();
                float dl = lowcutFilter.process (pd < 1.0f ? inL : pre[0].read (pd), 0);
                float dr = lowcutFilter.process (pd < 1.0f ? inR : pre[1].read (pd), 1);
                for (int k = 0; k < 4; ++k)
                {
                    dl = diffusers[(size_t) k].process (dl, 0.6f);
                    dr = diffusers[(size_t) k + 4].process (dr, 0.6f);
                }

                const float ms = (float) std::sin (2.0 * UDsp::pi * modPhase), mc = (float) std::cos (2.0 * UDsp::pi * modPhase);
                modPhase += modInc;
                if (modPhase >= 1.0) modPhase -= 1.0;

                std::array<float, lines> o;
                for (int k = 0; k < lines; ++k)
                {
                    auto& len = length[(size_t) k];
                    len += (target[(size_t) k] - len) * glide;
                    const float mod = modDepth * ((k & 1) ? ms : mc) * ((k & 2) ? 1.0f : -1.0f);
                    float s = delays[(size_t) k].read (len + mod);
                    damp[(size_t) k] += (s - damp[(size_t) k]) * dampCoef;
                    o[(size_t) k] = damp[(size_t) k];
                }
                const float outL = (o[0] + o[2] + o[4] + o[6]) * 0.3f;
                const float outR = (o[1] + o[3] + o[5] + o[7]) * 0.3f;

                std::array<float, lines> v;
                for (int k = 0; k < lines; ++k)
                    v[(size_t) k] = o[(size_t) k] * gain[(size_t) k];
                for (int h = 1; h < lines; h <<= 1)
                    for (int s = 0; s < lines; s += h * 2)
                        for (int j = s; j < s + h; ++j)
                        {
                            const float a = v[(size_t) j], c = v[(size_t) (j + h)];
                            v[(size_t) j] = a + c;
                            v[(size_t) (j + h)] = a - c;
                        }
                for (int k = 0; k < lines; ++k)
                    delays[(size_t) k].push (v[(size_t) k] * 0.35355339f + ((k & 1) ? dr : dl) * 0.5f);

                const float w = widthS.next();
                const float mid = 0.5f * (outL + outR), side = 0.5f * (outL - outR) * w;
                const float d = dryS.next(), wt = wetS.next();
                b.l[i] = inL * d + (mid + side) * wt;
                b.r[i] = inR * d + (mid - side) * wt;
            }
        }

    private:
        float targetLength (int k) const
        {
            return (float) (baseLengths[(size_t) k] * (0.3 + value (size)) * rate / 44100.0);
        }

        std::array<UDsp::DelayLine, lines> delays;
        std::array<float, lines> damp {}, length {};
        std::array<Allpass, 8> diffusers;
        UDsp::DelayLine pre[2];
        UDsp::Biquad lowcutFilter;
        Smooth predelayS, dryS, wetS, widthS;
        double modPhase = 0.0;
    };

    //==============================================================================
    // Tempo-synced stereo / ping-pong delay with filters in the feedback (covers the roles of FL's Delay 2/3).
    class DelayPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_delay";
        static const char* getPluginName() { return "Untitled Delay"; }
        enum { sync, division, time, feedback, pingpong, lowcut, highcut, dry, wet };

        DelayPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), {
                  toggle ("sync", "Tempo Sync", true),
                  choice ("division", "Time (sync)", UDsp::divisionNames(), UDsp::divisionIndex ("1/4")),
                  knob ("time", "Time (ms)", 1.0f, 3000.0f, 375.0f, "ms", 300.0f),
                  knob ("feedback", "Feedback", 0.0f, 1.0f, 0.4f, "%"),
                  toggle ("pingpong", "Ping-pong", false),
                  knob ("lowcut", "FB Low Cut", 20.0f, 2000.0f, 80.0f, "Hz", 300.0f),
                  knob ("highcut", "FB High Cut", 1000.0f, 20000.0f, 9000.0f, "Hz", 5000.0f),
                  knob ("dry", "Dry", 0.0f, 1.0f, 1.0f, "%"),
                  knob ("wet", "Wet", 0.0f, 1.0f, 0.35f, "%") }) {}

        static constexpr double maxSeconds = 8.0;

        // Delay time in seconds at a tempo (also used by the window).
        double delaySeconds (double bpm) const
        {
            const double s = choiceValue (sync) != 0 ? UDsp::divisionBeats (value (division)) * 60.0 / bpm : value (time) * 0.001;
            return juce::jlimit (0.001, maxSeconds - 0.01, s);
        }

        double getTailLength() const override
        {
            const double fb = juce::jmin (0.98f, value (feedback));
            const double d = juce::jlimit (0.001, maxSeconds, choiceValue (sync) != 0 ? UDsp::divisionBeats (value (division)) * 0.5 : value (time) * 0.001);
            return juce::jmin (30.0, d * (fb > 0.01 ? 1.0 + 3.0 / -std::log10 (fb) : 1.0));
        }

        std::atomic<float> lastBpm { 120.0f };

    protected:
        void prepare (double sr) override
        {
            lineL.prepare ((int) (sr * maxSeconds));
            lineR.prepare ((int) (sr * maxSeconds));
            lp.reset();
            hp.reset();
            first = true;
            dryS.reset (sr, value (dry));
            wetS.reset (sr, value (wet));
            fbS.reset (sr, value (feedback));
        }

        void process (const Block& b) override
        {
            lastBpm = (float) b.bpm;
            const float target = (float) (delaySeconds (b.bpm) * rate);
            if (first) { current = target; first = false; }
            const float glide = UDsp::coefFor (0.08, rate);
            lp.set (value (highcut), 1.414f, rate);
            hp.set (value (lowcut), 1.414f, rate);
            dryS.target (value (dry));
            wetS.target (value (wet));
            fbS.target (juce::jmin (0.98f, value (feedback)));
            const bool pp = choiceValue (pingpong) != 0;

            for (int i = 0; i < b.n; ++i)
            {
                current += (target - current) * glide;
                const float oL = filter (lineL.read (current - 1.0f), 0); // read before this sample's push
                const float oR = filter (lineR.read (current - 1.0f), 1);
                const float fb = fbS.next(), l = b.l[i], r = b.r[i];
                if (pp)
                {
                    lineL.push (0.5f * (l + r) + fb * oR);
                    lineR.push (fb * oL);
                }
                else
                {
                    lineL.push (l + fb * oL);
                    lineR.push (r + fb * oR);
                }
                const float d = dryS.next(), w = wetS.next();
                b.l[i] = l * d + oL * w;
                b.r[i] = r * d + oR * w;
            }
        }

    private:
        float filter (float x, int ch) noexcept { return hp.tick (lp.tick (x, ch).lp, ch).hp; }

        UDsp::DelayLine lineL, lineR;
        UDsp::Svf lp, hp;
        Smooth dryS, wetS, fbS;
        float current = 1000.0f;
        bool first = true;
    };

    //==============================================================================
    // Resonant multimode filter with tempo-synced LFO and envelope follower on the cutoff
    // (covers the roles of FL's Filter / Love Philter / Free Filter). meters: 0 modulated cutoff Hz, 1 LFO phase.
    class FilterPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_filter";
        static const char* getPluginName() { return "Untitled Filter"; }
        enum { shape, slope, cutoff, reso, lfoDepth, lfoShape, lfoSync, lfoRate, lfoDiv, envDepth, envAttack, envRelease, mix };

        FilterPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), {
                  choice ("shape", "Type", { "Low pass", "High pass", "Band pass", "Notch" }),
                  choice ("slope", "Slope", { "12 dB", "24 dB" }),
                  knob ("cutoff", "Cutoff", 20.0f, 20000.0f, 1000.0f, "Hz", 1000.0f),
                  knob ("reso", "Resonance", 0.0f, 1.0f, 0.2f, "%"),
                  knob ("lfoDepth", "LFO Depth", 0.0f, 4.0f, 0.0f, "oct"),
                  choice ("lfoShape", "LFO Shape", UDsp::lfoShapes()),
                  toggle ("lfoSync", "LFO Sync", true),
                  knob ("lfoRate", "LFO Rate", 0.02f, 20.0f, 1.0f, "Hz", 1.0f),
                  choice ("lfoDiv", "LFO Time", UDsp::divisionNames(), UDsp::divisionIndex ("1 bar")),
                  knob ("envDepth", "Env Depth", -4.0f, 4.0f, 0.0f, "oct"),
                  knob ("envAttack", "Env Attack", 0.1f, 200.0f, 5.0f, "ms", 20.0f),
                  knob ("envRelease", "Env Release", 5.0f, 2000.0f, 200.0f, "ms", 200.0f),
                  knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, "%") }) {}

        static float kFor (float resonance) { return 1.414f * (1.0f - resonance) + 0.05f * resonance; }

    protected:
        void prepare (double sr) override
        {
            stage1.reset();
            stage2.reset();
            env = 0.0f;
            logCutoff.reset (sr, std::log2 (value (cutoff)), 0.02);
            mixS.reset (sr, value (mix));
        }

        void process (const Block& b) override
        {
            logCutoff.target (std::log2 (value (cutoff)));
            mixS.target (value (mix));
            const int mode = choiceValue (shape), lfoWave = choiceValue (lfoShape);
            const bool steep = choiceValue (slope) == 1;
            const float k = kFor (value (reso)), depth = value (lfoDepth), envAmt = value (envDepth);
            const float att = UDsp::coefFor (value (envAttack) * 0.001, rate), rel = UDsp::coefFor (value (envRelease) * 0.001, rate);
            auto [phase, inc] = clock.begin (b, choiceValue (lfoSync) != 0, UDsp::divisionBeats (value (lfoDiv)), value (lfoRate), rate);
            const float maxLog = std::log2 ((float) rate * 0.45f);
            float fc = 0.0f;

            for (int i = 0; i < b.n; ++i)
            {
                const float l = b.l[i], r = b.r[i];
                const float level = juce::jmax (std::abs (l), std::abs (r));
                env += (level - env) * (level > env ? att : rel);
                const float mod = depth * UDsp::lfo (lfoWave, phase) + envAmt * juce::jmin (1.0f, env * 1.5f);
                phase += inc;
                fc = std::exp2 (juce::jlimit (4.3f, maxLog, logCutoff.next() + mod));
                stage1.set (fc, k, rate);
                float yl = pick (mode, stage1.tick (l, 0), k), yr = pick (mode, stage1.tick (r, 1), k);
                if (steep)
                {
                    stage2.g = stage1.g;
                    stage2.k = 1.414f;
                    stage2.a1 = 1.0f / (1.0f + stage2.g * (stage2.g + stage2.k));
                    stage2.a2 = stage2.g * stage2.a1;
                    stage2.a3 = stage2.g * stage2.a2;
                    yl = pick (mode, stage2.tick (yl, 0), stage2.k);
                    yr = pick (mode, stage2.tick (yr, 1), stage2.k);
                }
                const float m = mixS.next();
                b.l[i] = l + (yl - l) * m;
                b.r[i] = r + (yr - r) * m;
            }
            clock.end (phase);
            meters[0] = fc;
            meters[1] = (float) (phase - std::floor (phase));
        }

    private:
        static float pick (int mode, UDsp::Svf::Out o, float k) noexcept
        {
            switch (mode)
            {
                case 1:  return o.hp;
                case 2:  return o.bp * k; // unity gain at the centre
                case 3:  return o.lp + o.hp;
                default: return o.lp;
            }
        }

        UDsp::Svf stage1, stage2;
        LfoClock clock;
        Smooth logCutoff, mixS;
        float env = 0.0f;
    };

    //==============================================================================
    // Oversampled distortion with several curves (covers the roles of FL's Blood Overdrive / Fast Dist).
    class DistortionPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_distortion";
        static const char* getPluginName() { return "Untitled Distortion"; }
        enum { drive, curve, tone, mix, output };

        DistortionPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), {
                  knob ("drive", "Drive", 0.0f, 48.0f, 12.0f, "dB"),
                  choice ("curve", "Curve", { "Soft", "Hard", "Tube", "Fold", "Crush" }),
                  knob ("tone", "Tone", 500.0f, 20000.0f, 12000.0f, "Hz", 4000.0f),
                  knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, "%"),
                  knob ("output", "Output", -24.0f, 6.0f, -6.0f, "dB") }) {}

        // The waveshaper (after drive). `bits` only matters for Crush.
        static float shape (int curveType, float x, float bits) noexcept
        {
            switch (curveType)
            {
                case 1:  return juce::jlimit (-1.0f, 1.0f, x);
                case 2:  return x >= 0.0f ? 1.0f - std::exp (-x) : (std::exp (1.4f * x) - 1.0f) / 1.4f;
                case 3:  return std::sin (x * juce::MathConstants<float>::halfPi);
                case 4:
                {
                    const float q = std::exp2 (bits - 1.0f);
                    return std::round (std::tanh (x) * q) / q;
                }
                default: return std::tanh (x);
            }
        }
        static float bitsFor (float driveDb)  { return 12.0f - 9.0f * driveDb / 48.0f; }
        static float gainFor (int curveType, float driveDb) { return curveType == 4 ? 1.0f + driveDb / 48.0f : UDsp::dbToGain (driveDb); }

    protected:
        void prepare (double sr) override
        {
            oversampler.prepare();
            latencySamples = oversampler.latency();
            dryL.prepare (oversampler.latency() + 8);
            dryR.prepare (oversampler.latency() + 8);
            dryL.reset();
            dryR.reset();
            toneFilter.reset();
            dcL = dcR = dcInL = dcInR = 0.0f;
            driveS.reset (sr * 4, std::log2 (gainFor (choiceValue (curve), value (drive))));
            mixS.reset (sr, value (mix));
            outS.reset (sr, UDsp::dbToGain (value (output)));
            holdCount = 0;
        }

        void process (const Block& b) override
        {
            const int c = choiceValue (curve);
            const float dr = value (drive), bits = bitsFor (dr);
            const int holdLen = c == 4 ? 1 + juce::roundToInt (dr / 48.0f * 31.0f) : 1;
            driveS.target (std::log2 (gainFor (c, dr)));
            mixS.target (value (mix));
            outS.target (UDsp::dbToGain (value (output)));
            toneFilter.c = UDsp::Coeffs::make (UDsp::Shape::highCut, value (tone), 0.0, 0.7071, rate);

            std::array<float, maxChunk> drySaveL, drySaveR;
            const int lat = oversampler.latency();
            for (int i = 0; i < b.n; ++i)
            {
                dryL.push (b.l[i]);
                dryR.push (b.r[i]);
                drySaveL[(size_t) i] = dryL.at (lat);
                drySaveR[(size_t) i] = dryR.at (lat);
            }

            oversampler.process (b.l, b.r, b.n, [&] (float& l, float& r)
            {
                const float g = std::exp2 (driveS.next());
                if (++holdCount >= holdLen)
                {
                    holdCount = 0;
                    heldL = shape (c, l * g, bits);
                    heldR = shape (c, r * g, bits);
                }
                l = heldL;
                r = heldR;
            });

            const float dcCoef = 1.0f - (float) (2.0 * UDsp::pi * 10.0 / rate);
            for (int i = 0; i < b.n; ++i)
            {
                float l = b.l[i], r = b.r[i];
                dcL = l - dcInL + dcCoef * dcL; dcInL = l;
                dcR = r - dcInR + dcCoef * dcR; dcInR = r;
                l = toneFilter.process (dcL, 0);
                r = toneFilter.process (dcR, 1);
                const float m = mixS.next(), o = outS.next();
                b.l[i] = (drySaveL[(size_t) i] + (l - drySaveL[(size_t) i]) * m) * o;
                b.r[i] = (drySaveR[(size_t) i] + (r - drySaveR[(size_t) i]) * m) * o;
            }
        }

    private:
        Oversampler oversampler;
        UDsp::DelayLine dryL, dryR;
        UDsp::Biquad toneFilter;
        Smooth driveS, mixS, outS;
        float dcL = 0, dcR = 0, dcInL = 0, dcInR = 0, heldL = 0, heldR = 0;
        int holdCount = 0;
    };

    //==============================================================================
    // Modulation effects share rate / sync / time params. meters: 0 LFO phase (cycles).
    inline std::vector<Spec> withLfo (std::vector<Spec> specs, float rateHz, const char* div)
    {
        specs.insert (specs.begin(), {
            knob ("rate", "Rate", 0.02f, 10.0f, rateHz, "Hz", 1.0f),
            toggle ("sync", "Tempo Sync", false),
            choice ("division", "Time (sync)", UDsp::divisionNames(), UDsp::divisionIndex (div)) });
        return specs;
    }

    // Multi-voice chorus (covers the role of FL's Chorus).
    class ChorusPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_chorus";
        static const char* getPluginName() { return "Untitled Chorus"; }
        enum { rate_, sync, division, depth, delay, voices, spread, mix };

        ChorusPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), withLfo ({
                  knob ("depth", "Depth", 0.0f, 10.0f, 3.0f, "ms"),
                  knob ("delay", "Delay", 3.0f, 40.0f, 15.0f, "ms"),
                  choice ("voices", "Voices", { "1", "2", "3", "4" }, 2),
                  knob ("spread", "Stereo", 0.0f, 1.0f, 0.8f, "%"),
                  knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%") }, 0.8f, "1 bar")) {}

    protected:
        void prepare (double sr) override
        {
            lineL.prepare ((int) (sr * 0.06));
            lineR.prepare ((int) (sr * 0.06));
            delayS.reset (sr, value (delay));
            depthS.reset (sr, value (depth));
            mixS.reset (sr, value (mix));
        }

        void process (const Block& b) override
        {
            delayS.target (value (delay));
            depthS.target (value (depth));
            mixS.target (value (mix));
            const int v = choiceValue (voices) + 1;
            const float off = value (spread) * 0.25f, msToSamples = (float) rate * 0.001f, norm = 1.0f / std::sqrt ((float) v);
            auto [phase, inc] = clock.begin (b, choiceValue (sync) != 0, UDsp::divisionBeats (value (division)), value (rate_), rate);
            for (int i = 0; i < b.n; ++i)
            {
                const float l = b.l[i], r = b.r[i];
                lineL.push (l);
                lineR.push (r);
                const float base = delayS.next(), dep = depthS.next();
                float wl = 0.0f, wr = 0.0f;
                for (int k = 0; k < v; ++k)
                {
                    const double p = phase + (double) k / v;
                    wl += lineL.read ((base + dep * 0.5f * (1.0f + UDsp::lfo (0, p))) * msToSamples);
                    wr += lineR.read ((base + dep * 0.5f * (1.0f + UDsp::lfo (0, p + off))) * msToSamples);
                }
                phase += inc;
                const float m = mixS.next();
                b.l[i] = l * (1.0f - m) + wl * norm * m;
                b.r[i] = r * (1.0f - m) + wr * norm * m;
            }
            clock.end (phase);
            meters[0] = (float) (phase - std::floor (phase));
        }

    private:
        UDsp::DelayLine lineL, lineR;
        LfoClock clock;
        Smooth delayS, depthS, mixS;
    };

    // Flanger with feedback (covers the role of FL's Flanger).
    class FlangerPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_flanger";
        static const char* getPluginName() { return "Untitled Flanger"; }
        enum { rate_, sync, division, depth, delay, feedback, spread, mix };

        FlangerPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), withLfo ({
                  knob ("depth", "Depth", 0.0f, 1.0f, 0.7f, "%"),
                  knob ("delay", "Delay", 0.1f, 10.0f, 1.0f, "ms", 2.0f),
                  knob ("feedback", "Feedback", -0.95f, 0.95f, 0.5f, "%"),
                  knob ("spread", "Stereo", 0.0f, 1.0f, 0.25f, "%"),
                  knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%") }, 0.25f, "2 bars")) {}

    protected:
        void prepare (double sr) override
        {
            lineL.prepare ((int) (sr * 0.02));
            lineR.prepare ((int) (sr * 0.02));
            lastL = lastR = 0.0f;
            delayS.reset (sr, value (delay));
            depthS.reset (sr, value (depth));
            fbS.reset (sr, value (feedback));
            mixS.reset (sr, value (mix));
        }

        void process (const Block& b) override
        {
            delayS.target (value (delay));
            depthS.target (value (depth));
            fbS.target (value (feedback));
            mixS.target (value (mix));
            const float off = value (spread) * 0.5f, msToSamples = (float) rate * 0.001f;
            auto [phase, inc] = clock.begin (b, choiceValue (sync) != 0, UDsp::divisionBeats (value (division)), value (rate_), rate);
            for (int i = 0; i < b.n; ++i)
            {
                const float l = b.l[i], r = b.r[i], fb = fbS.next();
                lineL.push (l + fb * lastL);
                lineR.push (r + fb * lastR);
                const float base = delayS.next(), sweep = depthS.next() * 5.0f;
                lastL = lineL.read (juce::jmax (2.0f, (base + sweep * 0.5f * (1.0f + UDsp::lfo (1, phase))) * msToSamples));
                lastR = lineR.read (juce::jmax (2.0f, (base + sweep * 0.5f * (1.0f + UDsp::lfo (1, phase + off))) * msToSamples));
                phase += inc;
                const float m = mixS.next();
                b.l[i] = l * (1.0f - m) + lastL * m;
                b.r[i] = r * (1.0f - m) + lastR * m;
            }
            clock.end (phase);
            meters[0] = (float) (phase - std::floor (phase));
        }

    private:
        UDsp::DelayLine lineL, lineR;
        LfoClock clock;
        Smooth delayS, depthS, fbS, mixS;
        float lastL = 0.0f, lastR = 0.0f;
    };

    // Allpass-chain phaser (covers the role of FL's Phaser).
    class PhaserPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_phaser";
        static const char* getPluginName() { return "Untitled Phaser"; }
        enum { rate_, sync, division, stages, freq, depth, feedback, spread, mix };
        static constexpr int maxStages = 12;

        PhaserPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), withLfo ({
                  choice ("stages", "Stages", { "2", "4", "6", "8", "10", "12" }, 1),
                  knob ("freq", "Centre", 100.0f, 8000.0f, 800.0f, "Hz", 800.0f),
                  knob ("depth", "Depth", 0.0f, 1.0f, 0.6f, "%"),
                  knob ("feedback", "Feedback", -0.9f, 0.9f, 0.5f, "%"),
                  knob ("spread", "Stereo", 0.0f, 1.0f, 0.25f, "%"),
                  knob ("mix", "Mix", 0.0f, 1.0f, 0.5f, "%") }, 0.4f, "1 bar")) {}

    protected:
        void prepare (double sr) override
        {
            for (auto& s : state1) s = {};
            last = {};
            freqS.reset (sr, std::log2 (value (freq)));
            mixS.reset (sr, value (mix));
            fbS.reset (sr, value (feedback));
        }

        void process (const Block& b) override
        {
            freqS.target (std::log2 (value (freq)));
            mixS.target (value (mix));
            fbS.target (value (feedback));
            const int n = (choiceValue (stages) + 1) * 2;
            const float range = value (depth) * 3.0f, off = value (spread) * 0.5f;
            const float maxLog = std::log2 ((float) rate * 0.45f);
            auto [phase, inc] = clock.begin (b, choiceValue (sync) != 0, UDsp::divisionBeats (value (division)), value (rate_), rate);
            for (int i = 0; i < b.n; ++i)
            {
                const float centre = freqS.next(), fb = fbS.next(), m = mixS.next();
                for (int ch = 0; ch < 2; ++ch)
                {
                    float& x = ch == 0 ? b.l[i] : b.r[i];
                    const float f = std::exp2 (juce::jlimit (4.3f, maxLog, centre + range * UDsp::lfo (0, phase + (ch == 0 ? 0.0f : off))));
                    const float t = (float) std::tan (UDsp::pi * f / rate), a = (t - 1.0f) / (t + 1.0f);
                    float y = x + fb * last[(size_t) ch];
                    for (int s = 0; s < n; ++s)
                    {
                        auto& st = state1[(size_t) ch][(size_t) s];
                        const float out = a * y + st;
                        st = y - a * out;
                        y = out;
                    }
                    last[(size_t) ch] = y;
                    x = x * (1.0f - m) + y * m;
                }
                phase += inc;
            }
            clock.end (phase);
            meters[0] = (float) (phase - std::floor (phase));
        }

    private:
        std::array<std::array<float, maxStages>, 2> state1 {};
        std::array<float, 2> last {};
        LfoClock clock;
        Smooth freqS, mixS, fbS;
    };

    //==============================================================================
    // Tempo-synced 16-point volume envelope / trance gate + buffer time effects
    // (stutter, half speed, tape stop). Covers the role of FL's Gross Beat. meters: 0 position in cycle (0..1).
    class GatePlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_gate";
        static const char* getPluginName() { return "Untitled Gate & Time FX"; }
        static constexpr int numSteps = 16;
        enum { length = numSteps, smooth, fade, depth, timeMode, timeDiv, timeMix };

        GatePlugin (te::PluginCreationInfo info) : Plugin (info, xmlTypeName, getPluginName(), makeSpecs()) {}

        static std::vector<Spec> makeSpecs()
        {
            std::vector<Spec> v;
            for (int s = 0; s < numSteps; ++s)
                v.push_back (knob ("s" + juce::String (s + 1), "Step " + juce::String (s + 1), 0.0f, 1.0f, 1.0f, "%"));
            v.push_back (choice ("length", "Length", { "1 beat", "2 beats", "1 bar", "2 bars" }, 2));
            v.push_back (knob ("smooth", "Smooth", 0.0f, 1.0f, 0.0f, "%"));
            v.push_back (knob ("fade", "Declick", 0.5f, 20.0f, 2.0f, "ms"));
            v.push_back (knob ("depth", "Gate Depth", 0.0f, 1.0f, 1.0f, "%"));
            v.push_back (choice ("timeMode", "Time FX", { "Off", "Stutter", "Half speed", "Tape stop" }));
            v.push_back (choice ("timeDiv", "Slice", UDsp::divisionNames(), UDsp::divisionIndex ("1/16")));
            v.push_back (knob ("timeMix", "Time Mix", 0.0f, 1.0f, 1.0f, "%"));
            return v;
        }

        static double lengthBeats (int index) { const double b[] { 1.0, 2.0, 4.0, 8.0 }; return b[juce::jlimit (0, 3, index)]; }

        // Envelope level at a position (0..1) in the cycle; used by audio and the editor.
        float envelopeAt (double pos01) const
        {
            const double p = (pos01 - std::floor (pos01)) * numSteps;
            const int i = juce::jlimit (0, numSteps - 1, (int) p);
            const float frac = (float) (p - i), sm = value (smooth);
            const float a = value (i), next = value ((i + 1) % numSteps);
            if (sm <= 0.0f) return a;
            const float t = juce::jlimit (0.0f, 1.0f, (frac - (1.0f - sm)) / sm);
            return a + (next - a) * t;
        }

    protected:
        void prepare (double sr) override
        {
            ringSize = juce::nextPowerOfTwo ((int) (sr * 8.0));
            ringL.assign ((size_t) ringSize, 0.0f);
            ringR.assign ((size_t) ringSize, 0.0f);
            written = 0;
            prevSrc = -1.0;
            fadeLeft = 0;
            gateGain = 1.0f;
            mixS.reset (sr, value (timeMix));
        }

        void process (const Block& b) override
        {
            const int mode = choiceValue (timeMode);
            const double cycle = lengthBeats (choiceValue (length)), slice = UDsp::divisionBeats (value (timeDiv));
            const double samplesPerBeat = 1.0 / b.beatsPerSample;
            const float dep = value (depth), declick = UDsp::coefFor (value (fade) * 0.001, rate);
            const int fadeLen = juce::jmax (16, (int) (rate * 0.004));
            const double maxBack = ringSize - 4.0;
            mixS.target (value (timeMix));
            std::array<float, numSteps> steps;
            for (int s = 0; s < numSteps; ++s) steps[(size_t) s] = value (s);
            const float sm = value (smooth);

            for (int i = 0; i < b.n; ++i)
            {
                const double beat = b.beat + i * b.beatsPerSample;
                const auto w = (size_t) (written & (ringSize - 1));
                ringL[w] = b.l[i];
                ringR[w] = b.r[i];
                ++written;

                float l = b.l[i], r = b.r[i];
                if (mode != 0)
                {
                    double src = beat;
                    if (mode == 1)
                    {
                        const double group = slice * 4.0, gs = std::floor (beat / group) * group;
                        src = gs + std::fmod (beat - gs, slice);
                    }
                    else
                    {
                        const double gs = std::floor (beat / cycle) * cycle, t = beat - gs;
                        src = mode == 2 ? gs + t * 0.5 : gs + t - t * t / (2.0 * cycle);
                    }
                    double back = (beat - src) * samplesPerBeat;
                    if (back > maxBack || back < 0.0) back = 0.0;
                    const double pos = (double) (written - 1) - back;
                    if (prevSrc >= 0.0 && std::abs (pos - prevSrc) > 2.0)
                    {
                        fadeFrom = prevSrc + 1.0;
                        fadeLeft = fadeLen;
                    }
                    prevSrc = pos;
                    float tl = readRing (ringL, pos), tr = readRing (ringR, pos);
                    if (fadeLeft > 0)
                    {
                        const float t = (float) fadeLeft / (float) fadeLen;
                        tl += (readRing (ringL, fadeFrom) - tl) * t;
                        tr += (readRing (ringR, fadeFrom) - tr) * t;
                        fadeFrom = juce::jmin (fadeFrom + 1.0, (double) (written - 1));
                        --fadeLeft;
                    }
                    const float m = mixS.next();
                    l += (tl - l) * m;
                    r += (tr - r) * m;
                }
                else
                {
                    prevSrc = -1.0;
                    mixS.next();
                }

                const double p = (beat / cycle - std::floor (beat / cycle)) * numSteps;
                const int s = juce::jlimit (0, numSteps - 1, (int) p);
                float env = steps[(size_t) s];
                if (sm > 0.0f)
                {
                    const float t = juce::jlimit (0.0f, 1.0f, ((float) (p - s) - (1.0f - sm)) / sm);
                    env += (steps[(size_t) ((s + 1) % numSteps)] - env) * t;
                }
                gateGain += ((1.0f - dep * (1.0f - env)) - gateGain) * declick;
                b.l[i] = l * gateGain;
                b.r[i] = r * gateGain;
            }
            const double end = b.beat + b.n * b.beatsPerSample;
            meters[0] = (float) (end / cycle - std::floor (end / cycle));
        }

    private:
        float readRing (const std::vector<float>& ring, double pos) const noexcept
        {
            const auto i = (juce::int64) std::floor (pos);
            const float t = (float) (pos - (double) i);
            const float a = ring[(size_t) (i & (ringSize - 1))], c = ring[(size_t) ((i + 1) & (ringSize - 1))];
            return a + (c - a) * t;
        }

        std::vector<float> ringL, ringR;
        int ringSize = 1;
        juce::int64 written = 0;
        double prevSrc = -1.0, fadeFrom = 0.0;
        int fadeLeft = 0;
        float gateGain = 1.0f;
        Smooth mixS;
    };

    //==============================================================================
    // One-knob enhancer: 3-band gentle compression + saturation + high-band width
    // (covers the role of FL's Soundgoodizer). meters: 0..2 band GR dB.
    class EnhancerPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_enhancer";
        static const char* getPluginName() { return "Untitled Enhancer"; }
        enum { amount, mode, output };

        EnhancerPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), {
                  knob ("amount", "Amount", 0.0f, 1.0f, 0.5f, "%"),
                  choice ("mode", "Character", { "Clean", "Warm", "Bright", "Loud" }),
                  knob ("output", "Output", -12.0f, 6.0f, 0.0f, "dB") }) {}

    protected:
        struct Character { float sat[3], tilt[3], ratio, width; };

        static const Character& character (int m)
        {
            static const Character c[4] {
                { { 0.5f, 0.3f, 0.3f }, {  0.0f, 0.0f,  0.5f }, 2.0f, 0.4f },
                { { 1.5f, 0.8f, 0.4f }, {  2.0f, 0.5f, -1.0f }, 2.5f, 0.3f },
                { { 0.5f, 0.6f, 1.2f }, {  0.0f, 0.5f,  3.0f }, 2.0f, 0.7f },
                { { 1.5f, 1.2f, 1.2f }, {  1.5f, 1.0f,  1.5f }, 4.0f, 0.5f },
            };
            return c[juce::jlimit (0, 3, m)];
        }

        void prepare (double sr) override
        {
            for (auto* x : { &split1, &split2, &lowAllpass }) { x->freq = -1.0; x->reset(); }
            split1.set (200.0, sr);
            split2.set (4000.0, sr);
            lowAllpass.set (4000.0, sr);
            for (auto& c : comps) { c.reset(); c.setTimes (10.0f, 150.0f, sr); }
            amountS.reset (sr, value (amount), 0.05);
            outS.reset (sr, UDsp::dbToGain (value (output)));
        }

        void process (const Block& b) override
        {
            const auto& ch = character (choiceValue (mode));
            amountS.target (value (amount));
            outS.target (UDsp::dbToGain (value (output)));
            float gr[3] {};
            for (int i = 0; i < b.n; ++i)
            {
                const float a = amountS.next();
                float band[3][2];
                for (int c = 0; c < 2; ++c)
                {
                    float lo, rest;
                    split1.split (c == 0 ? b.l[i] : b.r[i], c, lo, rest);
                    band[0][c] = lowAllpass.allpass (lo, c);
                    split2.split (rest, c, band[1][c], band[2][c]);
                }
                const float threshold = -6.0f - 18.0f * a, makeup = UDsp::dbToGain (a * 3.0f);
                float outL = 0.0f, outR = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float det = juce::jmax (std::abs (band[k][0]), std::abs (band[k][1]));
                    const float red = comps[(size_t) k].process (det, threshold, 1.0f + (ch.ratio - 1.0f) * a, 6.0f);
                    gr[k] = juce::jmax (gr[k], red);
                    const float g = UDsp::dbToGain (ch.tilt[k] * a - red) * makeup;
                    const float drive = 1.0f + ch.sat[k] * a * 2.0f;
                    float l = std::tanh (band[k][0] * g * drive) / drive, r = std::tanh (band[k][1] * g * drive) / drive;
                    const float sideGain = k == 0 ? 1.0f - 0.5f * a : (k == 2 ? 1.0f + ch.width * a : 1.0f);
                    const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * sideGain;
                    outL += mid + side;
                    outR += mid - side;
                }
                const float o = outS.next();
                b.l[i] = outL * o;
                b.r[i] = outR * o;
            }
            for (int k = 0; k < 3; ++k)
                meterMax (k, gr[k]);
        }

    private:
        UDsp::Crossover split1, split2, lowAllpass;
        std::array<UDsp::CompCore, 3> comps;
        Smooth amountS, outS;
    };

    //==============================================================================
    struct Info { const char* xmlType; const char* name; const char* category; const char* hint; };

    // Catalog order = menu order within each category.
    inline const std::vector<Info>& catalog()
    {
        static const std::vector<Info> list {
            { EqPlugin::xmlTypeName,          EqPlugin::getPluginName(),          "Mixing",     "7-band EQ with live spectrum" },
            { CompressorPlugin::xmlTypeName,  CompressorPlugin::getPluginName(),  "Mixing",     "Compressor with knee, mix and sidechain" },
            { LimiterPlugin::xmlTypeName,     LimiterPlugin::getPluginName(),     "Mixing",     "Look-ahead brickwall limiter" },
            { MaximizerPlugin::xmlTypeName,   MaximizerPlugin::getPluginName(),   "Mixing",     "3-band compressor + master limiter" },
            { SoftClipperPlugin::xmlTypeName, SoftClipperPlugin::getPluginName(), "Mixing",     "Smooth peak clipping, oversampled" },
            { StereoPlugin::xmlTypeName,      StereoPlugin::getPluginName(),      "Mixing",     "Width, pan, L/R delay and polarity" },
            { BalancePlugin::xmlTypeName,     BalancePlugin::getPluginName(),     "Mixing",     "Volume and balance" },
            { ReverbPlugin::xmlTypeName,      ReverbPlugin::getPluginName(),      "Space",      "Smooth algorithmic reverb" },
            { DelayPlugin::xmlTypeName,       DelayPlugin::getPluginName(),       "Space",      "Tempo-synced stereo / ping-pong delay" },
            { DistortionPlugin::xmlTypeName,  DistortionPlugin::getPluginName(),  "Color",      "Overdrive, fold and crush, oversampled" },
            { EnhancerPlugin::xmlTypeName,    EnhancerPlugin::getPluginName(),    "Color",      "One knob: fuller, louder, wider" },
            { ChorusPlugin::xmlTypeName,      ChorusPlugin::getPluginName(),      "Modulation", "Multi-voice chorus" },
            { FlangerPlugin::xmlTypeName,     FlangerPlugin::getPluginName(),     "Modulation", "Jet flanger with feedback" },
            { PhaserPlugin::xmlTypeName,      PhaserPlugin::getPluginName(),      "Modulation", "Sweeping phaser" },
            { GatePlugin::xmlTypeName,        GatePlugin::getPluginName(),        "Modulation", "Trance gate, volume shapes, stutter" },
            { FilterPlugin::xmlTypeName,      FilterPlugin::getPluginName(),      "Filter",     "Resonant filter with LFO and envelope" },
            { LoudnessPlugin::xmlTypeName,    LoudnessPlugin::getPluginName(),    "Utility",    "LUFS loudness + true peak" },
        };
        return list;
    }

    inline void registerAll (te::PluginManager& pm)
    {
        pm.createBuiltInType<EqPlugin>();
        pm.createBuiltInType<CompressorPlugin>();
        pm.createBuiltInType<LimiterPlugin>();
        pm.createBuiltInType<MaximizerPlugin>();
        pm.createBuiltInType<SoftClipperPlugin>();
        pm.createBuiltInType<StereoPlugin>();
        pm.createBuiltInType<BalancePlugin>();
        pm.createBuiltInType<ReverbPlugin>();
        pm.createBuiltInType<DelayPlugin>();
        pm.createBuiltInType<DistortionPlugin>();
        pm.createBuiltInType<EnhancerPlugin>();
        pm.createBuiltInType<ChorusPlugin>();
        pm.createBuiltInType<FlangerPlugin>();
        pm.createBuiltInType<PhaserPlugin>();
        pm.createBuiltInType<GatePlugin>();
        pm.createBuiltInType<FilterPlugin>();
        pm.createBuiltInType<LoudnessPlugin>();
    }

    //==============================================================================
    // Factory presets (parameter id -> value).
    inline const std::map<juce::String, std::vector<Presets::Preset>>& factoryPresets()
    {
        using V = Presets::Values;
        auto steps = [] (std::initializer_list<float> s, V extra)
        {
            V v;
            int i = 1;
            for (auto x : s) v.push_back ({ "s" + juce::String (i++), x });
            v.insert (v.end(), extra.begin(), extra.end());
            return v;
        };
        const float q = (float) UDsp::divisionIndex ("1/4"), e = (float) UDsp::divisionIndex ("1/8"), ed = (float) UDsp::divisionIndex ("1/8D"),
                    s16 = (float) UDsp::divisionIndex ("1/16"), s32 = (float) UDsp::divisionIndex ("1/32"), bar = (float) UDsp::divisionIndex ("1 bar"),
                    bar2 = (float) UDsp::divisionIndex ("2 bars"), half = (float) UDsp::divisionIndex ("1/2");

        static const std::map<juce::String, std::vector<Presets::Preset>> all {
            { EqPlugin::xmlTypeName, {
                { "Flat", V { { "b2Gain", 0 }, { "b3Gain", 0 }, { "b4Gain", 0 }, { "b5Gain", 0 }, { "b6Gain", 0 }, { "b1Shape", 0 }, { "b7Shape", 0 } } },
                { "Low cut 80 Hz", V { { "b1Shape", 4 }, { "b1Freq", 80 }, { "b1Q", 0.71f } } },
                { "Vocal presence", V { { "b1Shape", 4 }, { "b1Freq", 100 }, { "b3Freq", 300 }, { "b3Gain", -3 }, { "b3Q", 1.2f },
                                        { "b5Freq", 3500 }, { "b5Gain", 3 }, { "b5Q", 0.8f }, { "b6Freq", 10000 }, { "b6Gain", 2.5f } } },
                { "Kick punch", V { { "b2Freq", 60 }, { "b2Gain", 4 }, { "b3Freq", 350 }, { "b3Gain", -5 }, { "b3Q", 1.5f }, { "b5Freq", 4000 }, { "b5Gain", 3 } } },
                { "Telephone", V { { "b1Shape", 4 }, { "b1Freq", 400 }, { "b1Q", 1 }, { "b7Shape", 5 }, { "b7Freq", 3000 }, { "b7Q", 1 }, { "b4Gain", 6 } } },
                { "Air", V { { "b6Freq", 12000 }, { "b6Gain", 4 }, { "b6Q", 0.6f } } },
            } },
            { CompressorPlugin::xmlTypeName, {
                { "Gentle", V { { "threshold", -18 }, { "ratio", 2 }, { "attack", 20 }, { "release", 200 }, { "knee", 10 }, { "makeup", 2 } } },
                { "Vocal", V { { "threshold", -22 }, { "ratio", 4 }, { "attack", 5 }, { "release", 120 }, { "knee", 6 }, { "makeup", 5 } } },
                { "Drum smash", V { { "threshold", -30 }, { "ratio", 10 }, { "attack", 1 }, { "release", 60 }, { "knee", 0 }, { "makeup", 10 }, { "mix", 0.5f } } },
                { "Bus glue", V { { "threshold", -14 }, { "ratio", 2 }, { "attack", 30 }, { "release", 300 }, { "knee", 12 }, { "makeup", 1.5f } } },
                { "Sidechain pump", V { { "threshold", -36 }, { "ratio", 12 }, { "attack", 0.5f }, { "release", 180 }, { "knee", 0 } } },
            } },
            { LimiterPlugin::xmlTypeName, {
                { "Safety", V { { "gain", 0 }, { "ceiling", -0.3f }, { "release", 80 } } },
                { "Master +3 dB", V { { "gain", 3 }, { "ceiling", -0.3f }, { "release", 100 } } },
                { "Loud", V { { "gain", 8 }, { "ceiling", -0.1f }, { "release", 50 } } },
                { "Transparent", V { { "gain", 2 }, { "ceiling", -1 }, { "release", 300 } } },
            } },
            { MaximizerPlugin::xmlTypeName, {
                { "Gentle master", V { { "lowThresh", -12 }, { "midThresh", -12 }, { "highThresh", -12 }, { "lowRatio", 2 }, { "midRatio", 2 }, { "highRatio", 2 }, { "drive", 2 } } },
                { "Loud master", V { { "lowThresh", -18 }, { "midThresh", -16 }, { "highThresh", -16 }, { "lowRatio", 4 }, { "midRatio", 3 }, { "highRatio", 3 }, { "drive", 6 }, { "ceiling", -0.1f } } },
                { "Tight low end", V { { "lowThresh", -24 }, { "lowRatio", 6 }, { "lowGain", 2 }, { "xLow", 120 }, { "attack", 10 } } },
                { "De-harsh", V { { "highThresh", -30 }, { "highRatio", 4 }, { "xHigh", 5000 }, { "highGain", -1 } } },
            } },
            { SoftClipperPlugin::xmlTypeName, {
                { "Default", V { { "threshold", 0 }, { "post", 0 } } },
                { "Glue -3 dB", V { { "threshold", -3 }, { "post", 2 } } },
                { "Hot", V { { "threshold", -8 }, { "post", 6 } } },
            } },
            { StereoPlugin::xmlTypeName, {
                { "Default", V { { "width", 1 }, { "pan", 0 }, { "offset", 0 }, { "invert", 0 } } },
                { "Mono", V { { "width", 0 } } },
                { "Wide", V { { "width", 1.6f } } },
                { "Haas widen", V { { "width", 1.2f }, { "offset", 12 } } },
                { "Swap polarity R", V { { "invert", 2 } } },
            } },
            { BalancePlugin::xmlTypeName, {
                { "Default", V { { "volume", 0 }, { "pan", 0 } } },
                { "-6 dB", V { { "volume", -6 } } },
                { "Hard left", V { { "pan", -1 } } },
                { "Hard right", V { { "pan", 1 } } },
            } },
            { ReverbPlugin::xmlTypeName, {
                { "Small room", V { { "size", 0.25f }, { "decay", 0.8f }, { "predelay", 5 }, { "damping", 0.6f }, { "wet", 0.25f } } },
                { "Vocal plate", V { { "size", 0.5f }, { "decay", 1.8f }, { "predelay", 30 }, { "damping", 0.4f }, { "lowcut", 200 }, { "wet", 0.25f } } },
                { "Hall", V { { "size", 0.8f }, { "decay", 3.5f }, { "predelay", 40 }, { "damping", 0.5f }, { "wet", 0.3f } } },
                { "Huge", V { { "size", 1 }, { "decay", 12 }, { "predelay", 60 }, { "damping", 0.3f }, { "wet", 0.4f } } },
                { "Wet only (send)", V { { "dry", 0 }, { "wet", 1 }, { "decay", 2.5f } } },
            } },
            { DelayPlugin::xmlTypeName, {
                { "Quarter echo", V { { "sync", 1 }, { "division", q }, { "feedback", 0.4f }, { "pingpong", 0 }, { "wet", 0.3f } } },
                { "Ping-pong 1/8", V { { "sync", 1 }, { "division", e }, { "feedback", 0.45f }, { "pingpong", 1 }, { "wet", 0.35f } } },
                { "Dotted eighth", V { { "sync", 1 }, { "division", ed }, { "feedback", 0.5f }, { "pingpong", 1 }, { "highcut", 5000 }, { "wet", 0.3f } } },
                { "Slapback", V { { "sync", 0 }, { "time", 110 }, { "feedback", 0.1f }, { "wet", 0.3f } } },
                { "Dub", V { { "sync", 1 }, { "division", q }, { "feedback", 0.75f }, { "lowcut", 300 }, { "highcut", 2500 }, { "wet", 0.45f } } },
            } },
            { FilterPlugin::xmlTypeName, {
                { "Low pass", V { { "shape", 0 }, { "cutoff", 1200 }, { "reso", 0.2f }, { "lfoDepth", 0 }, { "envDepth", 0 } } },
                { "Wobble 1/4", V { { "shape", 0 }, { "slope", 1 }, { "cutoff", 400 }, { "reso", 0.6f }, { "lfoDepth", 3 }, { "lfoSync", 1 }, { "lfoDiv", q } } },
                { "Slow sweep", V { { "shape", 0 }, { "cutoff", 800 }, { "reso", 0.4f }, { "lfoDepth", 2.5f }, { "lfoSync", 1 }, { "lfoDiv", bar2 }, { "lfoShape", 1 } } },
                { "Auto-wah", V { { "shape", 2 }, { "cutoff", 400 }, { "reso", 0.7f }, { "envDepth", 3 }, { "envAttack", 5 }, { "envRelease", 150 } } },
                { "High pass build", V { { "shape", 1 }, { "slope", 1 }, { "cutoff", 300 }, { "reso", 0.3f } } },
            } },
            { DistortionPlugin::xmlTypeName, {
                { "Warm drive", V { { "curve", 0 }, { "drive", 8 }, { "tone", 9000 }, { "output", -4 } } },
                { "Overdrive", V { { "curve", 2 }, { "drive", 20 }, { "tone", 6000 }, { "output", -8 } } },
                { "Fuzz", V { { "curve", 1 }, { "drive", 36 }, { "tone", 5000 }, { "output", -10 } } },
                { "Wavefold", V { { "curve", 3 }, { "drive", 18 }, { "output", -6 } } },
                { "Bitcrush", V { { "curve", 4 }, { "drive", 30 }, { "output", -3 } } },
            } },
            { EnhancerPlugin::xmlTypeName, {
                { "Subtle", V { { "amount", 0.3f }, { "mode", 0 } } },
                { "Warm", V { { "amount", 0.5f }, { "mode", 1 } } },
                { "Bright", V { { "amount", 0.5f }, { "mode", 2 } } },
                { "Loud", V { { "amount", 0.7f }, { "mode", 3 }, { "output", -2 } } },
            } },
            { ChorusPlugin::xmlTypeName, {
                { "Classic", V { { "rate", 0.8f }, { "depth", 3 }, { "delay", 15 }, { "voices", 2 }, { "mix", 0.5f } } },
                { "Wide ensemble", V { { "rate", 0.4f }, { "depth", 5 }, { "delay", 20 }, { "voices", 3 }, { "spread", 1 }, { "mix", 0.5f } } },
                { "Subtle double", V { { "rate", 0.2f }, { "depth", 1.5f }, { "delay", 12 }, { "voices", 0 }, { "mix", 0.35f } } },
                { "Synced 1 bar", V { { "sync", 1 }, { "division", bar }, { "depth", 4 }, { "mix", 0.5f } } },
            } },
            { FlangerPlugin::xmlTypeName, {
                { "Jet", V { { "rate", 0.15f }, { "depth", 0.9f }, { "delay", 0.5f }, { "feedback", 0.8f }, { "mix", 0.5f } } },
                { "Subtle", V { { "rate", 0.3f }, { "depth", 0.5f }, { "delay", 2 }, { "feedback", 0.3f }, { "mix", 0.35f } } },
                { "Synced 2 bars", V { { "sync", 1 }, { "division", bar2 }, { "depth", 0.8f }, { "feedback", 0.6f } } },
                { "Negative", V { { "rate", 0.25f }, { "feedback", -0.7f }, { "mix", 0.5f } } },
            } },
            { PhaserPlugin::xmlTypeName, {
                { "Classic 4", V { { "stages", 1 }, { "rate", 0.4f }, { "freq", 800 }, { "depth", 0.6f }, { "feedback", 0.5f } } },
                { "Deep 8", V { { "stages", 3 }, { "rate", 0.2f }, { "freq", 1000 }, { "depth", 0.8f }, { "feedback", 0.7f } } },
                { "Synced 1/2", V { { "sync", 1 }, { "division", half }, { "stages", 2 }, { "depth", 0.7f } } },
                { "Gentle", V { { "stages", 0 }, { "rate", 0.15f }, { "depth", 0.4f }, { "feedback", 0.2f }, { "mix", 0.4f } } },
            } },
            { GatePlugin::xmlTypeName, {
                { "Open", steps ({ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, { { "timeMode", 0 }, { "smooth", 0 } }) },
                { "Trance 1/16", steps ({ 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1 }, { { "length", 2 }, { "smooth", 0 }, { "timeMode", 0 } }) },
                { "Offbeat pump", steps ({ 0, 0.2f, 0.6f, 1, 0, 0.2f, 0.6f, 1, 0, 0.2f, 0.6f, 1, 0, 0.2f, 0.6f, 1 }, { { "length", 2 }, { "smooth", 1 }, { "timeMode", 0 } }) },
                { "Stutter 1/16", steps ({ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, { { "timeMode", 1 }, { "timeDiv", s16 } }) },
                { "Stutter 1/32 gated", steps ({ 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0 }, { { "length", 0 }, { "timeMode", 1 }, { "timeDiv", s32 } }) },
                { "Half speed", steps ({ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, { { "timeMode", 2 }, { "length", 2 } }) },
                { "Tape stop", steps ({ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.8f, 0.5f, 0 }, { { "timeMode", 3 }, { "length", 2 }, { "smooth", 1 } }) },
            } },
            { LoudnessPlugin::xmlTypeName, {
                { "Streaming -14", V { { "target", -14 } } },
                { "Broadcast -23", V { { "target", -23 } } },
                { "Club -8", V { { "target", -8 } } },
            } },
        };
        return all;
    }
}

//==============================================================================
// FL Studio stock plugin names (as stored in .flp files) -> our replacement's xml type.
// Used only to recognise plugins when importing; never shown as our product names.
namespace FlStockMap
{
    inline const std::vector<std::pair<const char*, const char*>>& table()
    {
        static const std::vector<std::pair<const char*, const char*>> t {
            { "Fruity Parametric EQ 2",  UFX::EqPlugin::xmlTypeName },
            { "Fruity Parametric EQ",    UFX::EqPlugin::xmlTypeName },
            { "Fruity 7 Band EQ",        UFX::EqPlugin::xmlTypeName },
            { "Fruity Limiter",          UFX::LimiterPlugin::xmlTypeName },
            { "Fruity Compressor",       UFX::CompressorPlugin::xmlTypeName },
            { "Fruity Multiband Compressor", UFX::MaximizerPlugin::xmlTypeName },
            { "Maximus",                 UFX::MaximizerPlugin::xmlTypeName },
            { "Fruity Reeverb 2",        UFX::ReverbPlugin::xmlTypeName },
            { "Fruity Reeverb",          UFX::ReverbPlugin::xmlTypeName },
            { "Fruity Delay 3",          UFX::DelayPlugin::xmlTypeName },
            { "Fruity Delay 2",          UFX::DelayPlugin::xmlTypeName },
            { "Fruity Delay",            UFX::DelayPlugin::xmlTypeName },
            { "Fruity Delay Bank",       UFX::DelayPlugin::xmlTypeName },
            { "Fruity Filter",           UFX::FilterPlugin::xmlTypeName },
            { "Fruity Love Philter",     UFX::FilterPlugin::xmlTypeName },
            { "Fruity Free Filter",      UFX::FilterPlugin::xmlTypeName },
            { "Fruity Blood Overdrive",  UFX::DistortionPlugin::xmlTypeName },
            { "Fruity Fast Dist",        UFX::DistortionPlugin::xmlTypeName },
            { "Fruity Squeeze",          UFX::DistortionPlugin::xmlTypeName },
            { "Fruity WaveShaper",       UFX::DistortionPlugin::xmlTypeName },
            { "Fruity Soft Clipper",     UFX::SoftClipperPlugin::xmlTypeName },
            { "Fruity Balance",          UFX::BalancePlugin::xmlTypeName },
            { "Fruity Stereo Enhancer",  UFX::StereoPlugin::xmlTypeName },
            { "Fruity Stereo Shaper",    UFX::StereoPlugin::xmlTypeName },
            { "Fruity Chorus",           UFX::ChorusPlugin::xmlTypeName },
            { "Fruity Flanger",          UFX::FlangerPlugin::xmlTypeName },
            { "Fruity Flangus",          UFX::FlangerPlugin::xmlTypeName },
            { "Fruity Phaser",           UFX::PhaserPlugin::xmlTypeName },
            { "Gross Beat",              UFX::GatePlugin::xmlTypeName },
            { "Soundgoodizer",           UFX::EnhancerPlugin::xmlTypeName },
            { "Wave Candy",              UFX::LoudnessPlugin::xmlTypeName },
            { "Fruity dB Meter",         UFX::LoudnessPlugin::xmlTypeName },
            { "Fruity Peak Controller",  nullptr }, // a controller, not audio: no substitute
        };
        return t;
    }

    // nullptr when there is no substitute (or the name is unknown).
    inline const char* untitledTypeFor (const juce::String& flPluginName)
    {
        for (auto& [fl, ours] : table())
            if (flPluginName.trim().equalsIgnoreCase (fl))
                return ours;
        return nullptr;
    }
}
