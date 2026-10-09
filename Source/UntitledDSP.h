#pragma once

// DSP building blocks for the Untitled effects. Our own code from textbook formulas
// (RBJ audio-EQ cookbook biquads, Zavalishin TPT filters, Hadamard FDN, ITU-R BS.1770).
// Everything is real-time safe once prepare() has run.
namespace UDsp
{
    constexpr double pi = juce::MathConstants<double>::pi;

    inline float dbToGain (float db)  { return std::pow (10.0f, db * 0.05f); }
    inline float gainToDb (float g)   { return 20.0f * std::log10 (juce::jmax (g, 1.0e-9f)); }

    // One-pole smoothing coefficient that reaches ~99% in `seconds`.
    inline float coefFor (double seconds, double sampleRate)
    {
        return (float) (1.0 - std::exp (-4.6 / juce::jmax (1.0, seconds * sampleRate)));
    }

    //==============================================================================
    enum class Shape { off, bell, lowShelf, highShelf, lowCut, highCut, notch, bandPass };

    struct Coeffs
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

        static Coeffs make (Shape s, double freq, double gainDb, double q, double sr)
        {
            if (s == Shape::off)
                return {};
            const double w0 = 2.0 * pi * juce::jlimit (5.0, sr * 0.49, freq) / sr;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / (2.0 * juce::jmax (0.05, q));
            const double A = std::pow (10.0, gainDb / 40.0), sa = 2.0 * std::sqrt (A) * alpha;
            double b[3], a[3];
            switch (s)
            {
                case Shape::bell:      b[0] = 1 + alpha * A; b[1] = -2 * cw; b[2] = 1 - alpha * A; a[0] = 1 + alpha / A; a[1] = -2 * cw; a[2] = 1 - alpha / A; break;
                case Shape::lowShelf:
                    b[0] = A * ((A + 1) - (A - 1) * cw + sa); b[1] = 2 * A * ((A - 1) - (A + 1) * cw); b[2] = A * ((A + 1) - (A - 1) * cw - sa);
                    a[0] = (A + 1) + (A - 1) * cw + sa;       a[1] = -2 * ((A - 1) + (A + 1) * cw);     a[2] = (A + 1) + (A - 1) * cw - sa;
                    break;
                case Shape::highShelf:
                    b[0] = A * ((A + 1) + (A - 1) * cw + sa); b[1] = -2 * A * ((A - 1) + (A + 1) * cw); b[2] = A * ((A + 1) + (A - 1) * cw - sa);
                    a[0] = (A + 1) - (A - 1) * cw + sa;       a[1] = 2 * ((A - 1) - (A + 1) * cw);       a[2] = (A + 1) - (A - 1) * cw - sa;
                    break;
                case Shape::lowCut:    b[0] = (1 + cw) / 2; b[1] = -(1 + cw); b[2] = (1 + cw) / 2; a[0] = 1 + alpha; a[1] = -2 * cw; a[2] = 1 - alpha; break;
                case Shape::highCut:   b[0] = (1 - cw) / 2; b[1] = 1 - cw;    b[2] = (1 - cw) / 2; a[0] = 1 + alpha; a[1] = -2 * cw; a[2] = 1 - alpha; break;
                case Shape::notch:     b[0] = 1; b[1] = -2 * cw; b[2] = 1;              a[0] = 1 + alpha; a[1] = -2 * cw; a[2] = 1 - alpha; break;
                case Shape::bandPass:  b[0] = alpha; b[1] = 0; b[2] = -alpha;           a[0] = 1 + alpha; a[1] = -2 * cw; a[2] = 1 - alpha; break;
                case Shape::off:       return {};
            }
            return { b[0] / a[0], b[1] / a[0], b[2] / a[0], a[1] / a[0], a[2] / a[0] };
        }

        double magnitude (double freq, double sr) const
        {
            const auto z = std::polar (1.0, -2.0 * pi * freq / sr);
            const auto num = b0 + z * (b1 + z * b2), den = 1.0 + z * (a1 + z * a2);
            return std::abs (num / den);
        }
    };

    // Stereo biquad, transposed direct form II in double (quiet at low frequencies / high rates).
    struct Biquad
    {
        Coeffs c;
        double s1[2] {}, s2[2] {};

        float process (float x, int ch) noexcept
        {
            const double y = c.b0 * x + s1[ch];
            s1[ch] = c.b1 * x - c.a1 * y + s2[ch];
            s2[ch] = c.b2 * x - c.a2 * y;
            return (float) y;
        }
        void reset() { s1[0] = s1[1] = s2[0] = s2[1] = 0.0; }
    };

    // 4th-order Linkwitz-Riley band split (low + high sum to an allpass).
    struct Crossover
    {
        Biquad lp[2], hp[2];
        double freq = -1.0;

        void set (double f, double sr)
        {
            if (juce::approximatelyEqual (f, freq)) return;
            freq = f;
            for (auto& b : lp) b.c = Coeffs::make (Shape::highCut, f, 0.0, 0.70710678, sr);
            for (auto& b : hp) b.c = Coeffs::make (Shape::lowCut, f, 0.0, 0.70710678, sr);
        }
        void split (float x, int ch, float& lo, float& hi) noexcept
        {
            lo = lp[1].process (lp[0].process (x, ch), ch);
            hi = hp[1].process (hp[0].process (x, ch), ch);
        }
        float allpass (float x, int ch) noexcept { float lo, hi; split (x, ch, lo, hi); return lo + hi; }
        void reset() { for (auto& b : lp) b.reset(); for (auto& b : hp) b.reset(); }
    };

    // Topology-preserving state-variable filter (Zavalishin). One instance = stereo.
    struct Svf
    {
        float g = 0.1f, k = 1.414f, a1 = 0, a2 = 0, a3 = 0;
        float ic1[2] {}, ic2[2] {};
        struct Out { float lp, bp, hp; };

        void set (float freq, float k_, double sr) noexcept
        {
            g = (float) std::tan (pi * juce::jlimit (10.0, sr * 0.49, (double) freq) / sr);
            k = k_;
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        Out tick (float x, int ch) noexcept
        {
            const float v3 = x - ic2[ch];
            const float v1 = a1 * ic1[ch] + a2 * v3;
            const float v2 = ic2[ch] + a2 * ic1[ch] + a3 * v3;
            ic1[ch] = 2.0f * v1 - ic1[ch];
            ic2[ch] = 2.0f * v2 - ic2[ch];
            return { v2, v1, x - k * v1 - v2 };
        }
        // |H| of the low/high/band/notch outputs (0..3), for drawing.
        static float magnitude (int mode, float freq, float cutoff, float k)
        {
            const std::complex<double> s (0.0, freq / juce::jmax (1.0f, cutoff));
            const auto den = s * s + (double) k * s + 1.0;
            std::complex<double> num = mode == 0 ? std::complex<double> (1.0) : mode == 1 ? s * s : mode == 2 ? (double) k * s : s * s + 1.0;
            return (float) std::abs (num / den);
        }
        void reset() { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0f; }
    };

    //==============================================================================
    inline float hermite (float xm1, float x0, float x1, float x2, float t) noexcept
    {
        const float c = (x1 - xm1) * 0.5f, v = x0 - x1, w = c + v;
        const float a = w + v + (x2 - x0) * 0.5f, b = w + a;
        return ((a * t - b) * t + c) * t + x0;
    }

    // Mono delay line with fractional (Hermite) reads. read(d) = the sample pushed d samples ago (0 = newest).
    struct DelayLine
    {
        std::vector<float> buf;
        int mask = 0, w = 0;

        void prepare (int maxDelay)
        {
            buf.assign ((size_t) juce::nextPowerOfTwo (maxDelay + 8), 0.0f);
            mask = (int) buf.size() - 1;
            w = 0;
        }
        void reset()                            { std::fill (buf.begin(), buf.end(), 0.0f); }
        void push (float x) noexcept            { buf[(size_t) w] = x; w = (w + 1) & mask; }
        float at (int back) const noexcept      { return buf[(size_t) ((w - 1 - back) & mask)]; }
        float maxDelay() const noexcept         { return (float) (mask - 4); }

        float read (float d) const noexcept
        {
            d = juce::jlimit (1.0f, maxDelay(), d);
            const int i = (int) d;
            const float t = d - (float) i;
            // Samples at back = i-1, i, i+1, i+2 (newer to older); t moves towards older.
            return hermite (at (i - 1), at (i), at (i + 1), at (i + 2), t);
        }
    };

    //==============================================================================
    // Tempo divisions for synced delays and LFOs (length in beats).
    struct Division { const char* name; double beats; };

    inline const std::vector<Division>& divisions()
    {
        static const std::vector<Division> list {
            { "1/64", 1.0 / 16 }, { "1/32", 1.0 / 8 }, { "1/16T", 1.0 / 6 }, { "1/16", 0.25 }, { "1/16D", 0.375 },
            { "1/8T", 1.0 / 3 }, { "1/8", 0.5 }, { "1/8D", 0.75 }, { "1/4T", 2.0 / 3 }, { "1/4", 1.0 }, { "1/4D", 1.5 },
            { "1/2T", 4.0 / 3 }, { "1/2", 2.0 }, { "1/2D", 3.0 }, { "1 bar", 4.0 }, { "2 bars", 8.0 }, { "4 bars", 16.0 },
        };
        return list;
    }

    inline juce::StringArray divisionNames()
    {
        juce::StringArray s;
        for (auto& d : divisions()) s.add (d.name);
        return s;
    }

    inline int divisionIndex (const char* name)
    {
        return divisionNames().indexOf (name);
    }

    inline double divisionBeats (float index)
    {
        return divisions()[(size_t) juce::jlimit (0, (int) divisions().size() - 1, juce::roundToInt (index))].beats;
    }

    inline juce::StringArray lfoShapes() { return { "Sine", "Triangle", "Saw up", "Saw down", "Square" }; }

    // Bipolar LFO value for a phase in cycles.
    inline float lfo (int shape, double phase) noexcept
    {
        const float p = (float) (phase - std::floor (phase));
        switch (shape)
        {
            case 1:  return 1.0f - 4.0f * std::abs (p - 0.5f);
            case 2:  return 2.0f * p - 1.0f;
            case 3:  return 1.0f - 2.0f * p;
            case 4:  return p < 0.5f ? 1.0f : -1.0f;
            default: return (float) std::sin (2.0 * pi * p);
        }
    }

    //==============================================================================
    // Feed-forward compressor gain computer with a peak follower (attack/release on the level).
    struct CompCore
    {
        float env = 0.0f, att = 1.0f, rel = 1.0f;

        void setTimes (float attackMs, float releaseMs, double sr)
        {
            att = coefFor (attackMs * 0.001, sr);
            rel = coefFor (releaseMs * 0.001, sr);
        }

        static float reductionDb (float inDb, float threshold, float ratio, float knee) noexcept
        {
            const float over = inDb - threshold, slope = 1.0f - 1.0f / juce::jmax (1.0f, ratio);
            if (knee > 0.0f && std::abs (over) * 2.0f <= knee)
            {
                const float x = over + knee * 0.5f;
                return slope * x * x / (2.0f * knee);
            }
            return over > 0.0f ? slope * over : 0.0f;
        }

        // Returns the gain reduction in dB (>= 0) for one detector sample (absolute level).
        float process (float level, float threshold, float ratio, float knee) noexcept
        {
            env += (level - env) * (level > env ? att : rel);
            return reductionDb (gainToDb (env), threshold, ratio, knee);
        }
        void reset() { env = 0.0f; }
    };

    // Look-ahead brickwall limiter: sliding-window minimum of the required gain, smoothed by a
    // moving average over the look-ahead (so the gain is fully down when the peak arrives), then
    // released exponentially. Output sample peaks never exceed the ceiling.
    struct LimiterCore
    {
        int lookahead = 1, window = 2, pos = 0, dqHead = 0, dqSize = 0, avgPos = 0;
        juce::int64 counter = 0;
        std::vector<float> dl, dr, avgRing, dqValue;
        std::vector<juce::int64> dqIndex;
        double avgSum = 0.0;
        float gain = 1.0f, minGain = 1.0f;

        void prepare (double sr, double seconds)
        {
            lookahead = juce::jmax (1, juce::roundToInt (sr * seconds));
            window = lookahead + 1;
            dl.assign ((size_t) lookahead, 0.0f);
            dr.assign ((size_t) lookahead, 0.0f);
            avgRing.assign ((size_t) lookahead, 1.0f);
            dqValue.assign ((size_t) window + 1, 1.0f);
            dqIndex.assign ((size_t) window + 1, 0);
            reset();
        }

        void reset()
        {
            std::fill (dl.begin(), dl.end(), 0.0f);
            std::fill (dr.begin(), dr.end(), 0.0f);
            std::fill (avgRing.begin(), avgRing.end(), 1.0f);
            avgSum = lookahead;
            pos = avgPos = dqHead = dqSize = 0;
            counter = 0;
            gain = 1.0f;
        }

        void process (float* l, float* r, int n, float ceiling, float releaseCoef) noexcept
        {
            const int cap = (int) dqValue.size();
            minGain = 1.0f;
            for (int i = 0; i < n; ++i)
            {
                const float peak = juce::jmax (std::abs (l[i]), std::abs (r[i]));
                const float need = peak > ceiling ? ceiling / peak : 1.0f;

                // Monotonic deque: front = minimum of the last `window` values.
                while (dqSize > 0 && dqValue[(size_t) ((dqHead + dqSize - 1) % cap)] >= need) --dqSize;
                const int slot = (dqHead + dqSize) % cap;
                dqValue[(size_t) slot] = need;
                dqIndex[(size_t) slot] = counter;
                ++dqSize;
                while (dqIndex[(size_t) dqHead] <= counter - window) { dqHead = (dqHead + 1) % cap; --dqSize; }
                ++counter;
                const float held = dqValue[(size_t) dqHead];

                avgSum += held - avgRing[(size_t) avgPos];
                avgRing[(size_t) avgPos] = held;
                avgPos = (avgPos + 1) % lookahead;
                const float target = (float) (avgSum / lookahead);

                gain = target < gain ? target : gain + (target - gain) * releaseCoef;
                minGain = juce::jmin (minGain, gain);

                const float outL = dl[(size_t) pos] * gain, outR = dr[(size_t) pos] * gain;
                dl[(size_t) pos] = l[i];
                dr[(size_t) pos] = r[i];
                pos = (pos + 1) % lookahead;
                l[i] = juce::jlimit (-ceiling, ceiling, outL);
                r[i] = juce::jlimit (-ceiling, ceiling, outR);
            }
        }
    };

    //==============================================================================
    // ITU-R BS.1770-4 loudness: K-weighting, 100 ms sub-blocks, momentary (400 ms), short-term (3 s),
    // gated integrated loudness (absolute -70 LUFS, relative -10 LU) kept in a fixed histogram.
    struct Loudness
    {
        static constexpr int histBins = 1000; // 0.1 LU bins from -70
        Biquad shelf, highPass;
        int subLength = 4410, subCount = 0, subsSeen = 0;
        double subSum = 0.0;
        std::array<double, 30> subs {};
        std::array<double, histBins> histEnergy {};
        std::array<int, histBins> histCount {};
        float momentary = -100.0f, shortTerm = -100.0f, integrated = -100.0f;

        void prepare (double sr)
        {
            // K-weighting stage 1 (high shelf) and stage 2 (high-pass), from the BS.1770 analog prototypes.
            {
                const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
                const double K = std::tan (pi * f0 / sr), Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
                const double a0 = 1.0 + K / Q + K * K;
                shelf.c = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                            2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
            }
            {
                const double f0 = 38.13547087602444, Q = 0.5003270373238773;
                const double K = std::tan (pi * f0 / sr), a0 = 1.0 + K / Q + K * K;
                highPass.c = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
            }
            subLength = juce::jmax (1, juce::roundToInt (sr * 0.1));
            reset();
        }

        void reset()
        {
            shelf.reset();
            highPass.reset();
            subCount = subsSeen = 0;
            subSum = 0.0;
            subs.fill (0.0);
            histEnergy.fill (0.0);
            histCount.fill (0);
            momentary = shortTerm = integrated = -100.0f;
        }

        static float toLufs (double meanSquare) { return meanSquare > 1.0e-12 ? (float) (-0.691 + 10.0 * std::log10 (meanSquare)) : -100.0f; }

        void process (const float* l, const float* r, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                const float kl = highPass.process (shelf.process (l[i], 0), 0);
                const float kr = highPass.process (shelf.process (r[i], 1), 1);
                subSum += (double) kl * kl + (double) kr * kr;
                if (++subCount >= subLength)
                    endSubBlock();
            }
        }

    private:
        void endSubBlock()
        {
            const int slot = subsSeen % (int) subs.size();
            subs[(size_t) slot] = subSum / subLength;
            ++subsSeen;
            subSum = 0.0;
            subCount = 0;

            auto meanOfLast = [this] (int count)
            {
                count = juce::jmin (count, subsSeen);
                double sum = 0.0;
                for (int k = 0; k < count; ++k)
                    sum += subs[(size_t) ((subsSeen - 1 - k + (int) subs.size() * 2) % (int) subs.size())];
                return sum / juce::jmax (1, count);
            };

            const double block = meanOfLast (4);
            momentary = subsSeen >= 4 ? toLufs (block) : -100.0f;
            shortTerm = subsSeen >= 30 ? toLufs (meanOfLast (30)) : momentary;

            if (subsSeen >= 4 && momentary > -70.0f)
            {
                const int bin = juce::jlimit (0, histBins - 1, (int) ((momentary + 70.0f) * 10.0f));
                histEnergy[(size_t) bin] += block;
                ++histCount[(size_t) bin];

                double sum = 0.0; int count = 0;
                for (int b = 0; b < histBins; ++b) { sum += histEnergy[(size_t) b]; count += histCount[(size_t) b]; }
                const float relative = toLufs (sum / juce::jmax (1, count)) - 10.0f;
                sum = 0.0; count = 0;
                for (int b = juce::jmax (0, (int) ((relative + 70.0f) * 10.0f)); b < histBins; ++b) { sum += histEnergy[(size_t) b]; count += histCount[(size_t) b]; }
                integrated = count > 0 ? toLufs (sum / count) : -100.0f;
            }
        }
    };

    // True-peak estimate: 4x polyphase windowed-sinc interpolation of the last 12 samples.
    struct TruePeak
    {
        static constexpr int taps = 12, phases = 4;
        std::array<std::array<float, taps>, phases> fir {};
        std::array<std::array<float, taps>, 2> hist {};
        int pos = 0;

        TruePeak()
        {
            for (int p = 0; p < phases; ++p)
            {
                double sum = 0.0;
                for (int t = 0; t < taps; ++t)
                {
                    const double x = (double) (t - taps / 2 + 1) - (double) p / phases;
                    const double sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (pi * x) / (pi * x);
                    const double win = 0.5 + 0.5 * std::cos (pi * x / (taps / 2 + 1));
                    fir[(size_t) p][(size_t) t] = (float) (sinc * win);
                    sum += sinc * win;
                }
                for (auto& c : fir[(size_t) p]) c = (float) (c / sum);
            }
        }

        float process (float l, float r) noexcept
        {
            hist[0][(size_t) pos] = l;
            hist[1][(size_t) pos] = r;
            pos = (pos + 1) % taps;
            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int p = 0; p < phases; ++p)
                {
                    float acc = 0.0f;
                    for (int t = 0; t < taps; ++t)
                        acc += fir[(size_t) p][(size_t) t] * hist[(size_t) ch][(size_t) ((pos + t) % taps)];
                    peak = juce::jmax (peak, std::abs (acc));
                }
            return peak;
        }
    };
}
