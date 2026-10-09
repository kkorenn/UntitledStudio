#pragma once

// Synthesises the default drum kit as WAV files on first launch.
// All sounds are generated here, so we own them (no licensing issues).
namespace DrumKit
{
    inline juce::File getPacksFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("untitledstudio").getChildFile ("Packs");
    }

    inline juce::File getDrumsFolder() { return getPacksFolder().getChildFile ("Drums"); }

    namespace detail
    {
        constexpr double sampleRate = 44100.0;

        struct Rng
        {
            juce::uint32 state = 0x12345678;
            float next() { state = state * 1664525u + 1013904223u; return (float) (state >> 8) / 8388608.0f - 1.0f; }
        };

        inline float expEnv (double t, double decay) { return (float) std::exp (-t / decay); }

        inline void writeWav (const juce::File& file, const std::vector<float>& data)
        {
            juce::AudioBuffer<float> buffer (2, (int) data.size());
            for (int ch = 0; ch < 2; ++ch)
                buffer.copyFrom (ch, 0, data.data(), (int) data.size());

            file.getParentDirectory().createDirectory();
            file.deleteFile();

            if (auto stream = file.createOutputStream())
            {
                juce::WavAudioFormat wav;
                const auto options = juce::AudioFormatWriterOptions{}.withSampleRate (sampleRate)
                                                                    .withNumChannels (2)
                                                                    .withBitsPerSample (24);
                std::unique_ptr<juce::OutputStream> out (stream.release());
                if (auto writer = wav.createWriterFor (out, options))
                    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
            }
        }

        inline std::vector<float> render (double seconds, const std::function<float (double t, Rng&)>& fn)
        {
            Rng rng;
            std::vector<float> out ((size_t) (seconds * sampleRate));
            for (size_t i = 0; i < out.size(); ++i)
                out[i] = fn ((double) i / sampleRate, rng);

            // Short fade-out so nothing clicks at the end.
            const size_t fade = std::min<size_t> (out.size(), 256);
            for (size_t i = 0; i < fade; ++i)
                out[out.size() - 1 - i] *= (float) i / (float) fade;
            return out;
        }

        // One-pole filters for shaping noise.
        struct OnePole
        {
            float z = 0.0f;
            float lowpass (float x, float a)  { z += a * (x - z); return z; }
            float highpass (float x, float a) { return x - lowpass (x, a); }
        };
    }

    inline void writeKick (const juce::File& f)
    {
        using namespace detail;
        double phase = 0.0;
        writeWav (f, render (0.6, [&] (double t, Rng&)
        {
            const double freq = 48.0 + 180.0 * std::exp (-t / 0.035);
            phase += juce::MathConstants<double>::twoPi * freq / sampleRate;
            const float body = (float) std::sin (phase) * expEnv (t, 0.22);
            const float click = t < 0.004 ? (float) (1.0 - t / 0.004) * 0.4f : 0.0f;
            return std::tanh ((body + click) * 1.6f) * 0.9f;
        }));
    }

    inline void writeSnare (const juce::File& f)
    {
        using namespace detail;
        OnePole hp;
        double phase = 0.0;
        writeWav (f, render (0.35, [&] (double t, Rng& rng)
        {
            phase += juce::MathConstants<double>::twoPi * (185.0 + 60.0 * std::exp (-t / 0.02)) / sampleRate;
            const float tone = (float) std::sin (phase) * expEnv (t, 0.06) * 0.6f;
            const float noise = hp.highpass (rng.next(), 0.15f) * expEnv (t, 0.11) * 0.8f;
            return std::tanh ((tone + noise) * 1.3f) * 0.85f;
        }));
    }

    inline void writeClap (const juce::File& f)
    {
        using namespace detail;
        OnePole hp, lp;
        writeWav (f, render (0.4, [&] (double t, Rng& rng)
        {
            // Several quick bursts, then a tail: classic clap shape.
            float env = expEnv (t - 0.03, 0.12) * (t >= 0.03 ? 1.0f : 0.0f);
            for (double burst : { 0.0, 0.011, 0.022 })
                if (t >= burst && t < burst + 0.01)
                    env = juce::jmax (env, expEnv (t - burst, 0.004));

            const float n = lp.lowpass (hp.highpass (rng.next(), 0.25f), 0.5f);
            return n * env * 1.4f;
        }));
    }

    inline void writeHat (const juce::File& f, double decay)
    {
        using namespace detail;
        OnePole hp1, hp2;
        writeWav (f, render (decay * 6.0, [&] (double t, Rng& rng)
        {
            const float n = hp2.highpass (hp1.highpass (rng.next(), 0.45f), 0.45f);
            return n * expEnv (t, decay) * 0.7f;
        }));
    }

    inline void writeRim (const juce::File& f)
    {
        using namespace detail;
        double p1 = 0.0, p2 = 0.0;
        writeWav (f, render (0.12, [&] (double t, Rng&)
        {
            p1 += juce::MathConstants<double>::twoPi * 1700.0 / sampleRate;
            p2 += juce::MathConstants<double>::twoPi * 820.0 / sampleRate;
            return (float) (std::sin (p1) * 0.5 + std::sin (p2) * 0.5) * expEnv (t, 0.012) * 0.8f;
        }));
    }

    inline void write808 (const juce::File& f)
    {
        using namespace detail;
        double phase = 0.0;
        writeWav (f, render (1.6, [&] (double t, Rng&)
        {
            const double freq = 55.0 + 60.0 * std::exp (-t / 0.03);
            phase += juce::MathConstants<double>::twoPi * freq / sampleRate;
            return std::tanh ((float) std::sin (phase) * 2.2f) * expEnv (t, 0.55) * 0.8f;
        }));
    }

    // Writes any missing default sounds. Cheap to call on every launch.
    inline void ensureDefaultKit()
    {
        const auto dir = getDrumsFolder();
        const std::pair<const char*, std::function<void (const juce::File&)>> sounds[] = {
            { "Kick.wav",     writeKick },
            { "Clap.wav",     writeClap },
            { "Hat.wav",      [] (const juce::File& f) { writeHat (f, 0.035); } },
            { "Snare.wav",    writeSnare },
            { "Open Hat.wav", [] (const juce::File& f) { writeHat (f, 0.16); } },
            { "Rim.wav",      writeRim },
            { "808.wav",      write808 },
        };

        for (auto& [name, writer] : sounds)
            if (! dir.getChildFile (name).existsAsFile())
                writer (dir.getChildFile (name));
    }

    //==========================================================================
    // The bigger sound library: more drums, FX and BPM-tagged loops, also generated.
    namespace Library
    {
        using namespace detail;
        using Sound = std::vector<float>;
        constexpr double twoPi = juce::MathConstants<double>::twoPi;

        inline Sound kick (double endHz, double sweepHz, double sweepTime, double decay, float drive, float click)
        {
            double phase = 0.0;
            return render (decay * 3.0, [&] (double t, Rng&)
            {
                phase += twoPi * (endHz + sweepHz * std::exp (-t / sweepTime)) / sampleRate;
                const float body = (float) std::sin (phase) * expEnv (t, decay);
                const float c = t < 0.004 ? (float) (1.0 - t / 0.004) * click : 0.0f;
                return std::tanh ((body + c) * drive) * 0.9f;
            });
        }

        inline Sound snare (double toneHz, double toneDecay, double noiseDecay, float noiseLevel)
        {
            OnePole hp;
            double phase = 0.0;
            return render (noiseDecay * 4.0, [&] (double t, Rng& rng)
            {
                phase += twoPi * (toneHz + 60.0 * std::exp (-t / 0.02)) / sampleRate;
                const float tone = (float) std::sin (phase) * expEnv (t, toneDecay) * 0.6f;
                const float noise = hp.highpass (rng.next(), 0.15f) * expEnv (t, noiseDecay) * noiseLevel;
                return std::tanh ((tone + noise) * 1.3f) * 0.85f;
            });
        }

        inline Sound clap (double spacing, double tail)
        {
            OnePole hp, lp;
            return render (tail * 4.0, [&] (double t, Rng& rng)
            {
                const double last = spacing * 3.0;
                float env = expEnv (t - last, tail) * (t >= last ? 1.0f : 0.0f);
                for (int b = 0; b < 3; ++b)
                    if (t >= b * spacing && t < (b + 1) * spacing)
                        env = juce::jmax (env, expEnv (t - b * spacing, 0.004));
                return lp.lowpass (hp.highpass (rng.next(), 0.25f), 0.5f) * env * 1.4f;
            });
        }

        inline Sound hat (double decay)
        {
            OnePole hp1, hp2;
            return render (decay * 6.0, [&] (double t, Rng& rng)
            {
                return hp2.highpass (hp1.highpass (rng.next(), 0.45f), 0.45f) * expEnv (t, decay) * 0.7f;
            });
        }

        inline Sound shaker()
        {
            OnePole hp, lp;
            return render (0.18, [&] (double t, Rng& rng)
            {
                const float env = (float) juce::jmin (1.0, t / 0.025) * expEnv (t, 0.04);
                return lp.lowpass (hp.highpass (rng.next(), 0.5f), 0.6f) * env * 0.8f;
            });
        }

        inline Sound tom (double hz)
        {
            double phase = 0.0;
            return render (0.7, [&] (double t, Rng& rng)
            {
                phase += twoPi * hz * (1.0 + 0.5 * std::exp (-t / 0.05)) / sampleRate;
                const float noise = rng.next() * expEnv (t, 0.01) * 0.2f;
                return std::tanh (((float) std::sin (phase) * expEnv (t, 0.22) + noise) * 1.4f) * 0.85f;
            });
        }

        // Inharmonic square cluster through high-passes: the classic analog cymbal recipe.
        inline Sound metal (double seconds, double decay, float hpCoef, float noiseLevel)
        {
            OnePole hp1, hp2;
            std::array<double, 6> phases {};
            const double freqs[] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
            return render (seconds, [&] (double t, Rng& rng)
            {
                float sum = 0.0f;
                for (size_t i = 0; i < phases.size(); ++i)
                {
                    phases[i] = std::fmod (phases[i] + freqs[i] * 2.7 / sampleRate, 1.0);
                    sum += phases[i] < 0.5 ? 1.0f : -1.0f;
                }
                const float x = sum / 6.0f + rng.next() * noiseLevel;
                return hp2.highpass (hp1.highpass (x, hpCoef), hpCoef) * expEnv (t, decay) * 0.6f;
            });
        }

        inline Sound ride()
        {
            auto body = metal (2.5, 0.9, 0.35f, 0.1f);
            double p1 = 0.0, p2 = 0.0;
            auto bell = render (2.5, [&] (double t, Rng&)
            {
                p1 += twoPi * 2400.0 / sampleRate;
                p2 += twoPi * 3610.0 / sampleRate;
                return (float) (std::sin (p1) + 0.6 * std::sin (p2)) * expEnv (t, 0.5) * 0.12f;
            });
            for (size_t i = 0; i < body.size(); ++i)
                body[i] += bell[i];
            return body;
        }

        inline Sound perc()
        {
            double phase = 0.0;
            return render (0.25, [&] (double t, Rng&)
            {
                phase += twoPi * (520.0 + 400.0 * std::exp (-t / 0.008)) / sampleRate;
                return (float) std::sin (phase) * expEnv (t, 0.05) * 0.8f;
            });
        }

        // Band-limited squares (odd harmonics below Nyquist).
        inline Sound cowbell()
        {
            OnePole hp;
            double p1 = 0.0, p2 = 0.0;
            return render (0.45, [&] (double t, Rng&)
            {
                p1 += twoPi * 540.0 / sampleRate;
                p2 += twoPi * 800.0 / sampleRate;
                double sum = 0.0;
                for (int h = 1; h <= 9; h += 2)
                    sum += (std::sin (p1 * h) + std::sin (p2 * h)) / h;
                const float env = (float) (0.6 * std::exp (-t / 0.012) + 0.4 * std::exp (-t / 0.12));
                return hp.highpass ((float) sum * 0.35f, 0.08f) * env;
            });
        }

        inline Sound bass808 (double decay, double seconds, float drive)
        {
            double phase = 0.0;
            return render (seconds, [&] (double t, Rng&)
            {
                phase += twoPi * (52.0 + 55.0 * std::exp (-t / 0.03)) / sampleRate;
                return std::tanh ((float) std::sin (phase) * drive) * expEnv (t, decay) * 0.8f;
            });
        }

        // FX: noise + tone sweeps.
        inline Sound sweep (double seconds, bool up, float toneLevel)
        {
            OnePole lp1, lp2;
            double phase = 0.0;
            return render (seconds, [&] (double t, Rng& rng)
            {
                const double x = up ? t / seconds : 1.0 - t / seconds;
                const float cutoff = (float) (0.01 + 0.6 * x * x);
                phase += twoPi * (150.0 * std::pow (8.0, x)) / sampleRate;
                const float env = up ? (float) (x * x) : (float) std::pow (x, 1.5);
                const float n = lp2.lowpass (lp1.lowpass (rng.next(), cutoff), cutoff);
                return (n * 0.9f + (float) std::sin (phase) * toneLevel) * env * 0.7f;
            });
        }

        inline Sound noiseSweep()
        {
            OnePole lp1, lp2;
            const double seconds = 4.0;
            return render (seconds, [&] (double t, Rng& rng)
            {
                const double x = std::sin (juce::MathConstants<double>::pi * t / seconds);
                const float cutoff = (float) (0.02 + 0.5 * x * x);
                return lp2.lowpass (lp1.lowpass (rng.next(), cutoff), cutoff) * (float) x * 0.8f;
            });
        }

        inline Sound impact()
        {
            OnePole lp;
            double phase = 0.0;
            return render (3.0, [&] (double t, Rng& rng)
            {
                phase += twoPi * (38.0 + 90.0 * std::exp (-t / 0.08)) / sampleRate;
                const float boom = (float) std::sin (phase) * expEnv (t, 0.9);
                const float noise = lp.lowpass (rng.next(), 0.2f) * expEnv (t, 0.35) * 0.6f;
                return std::tanh ((boom + noise) * 1.8f) * 0.85f;
            });
        }

        // Mixes hits on a 16th grid: each track is (sound, gain, steps).
        struct Track { const Sound* sound; float gain; std::vector<int> steps; };

        inline Sound loop (double bpm, int bars, const std::vector<Track>& tracks)
        {
            const double stepSeconds = 60.0 / bpm / 4.0;
            Sound out ((size_t) (bars * 16 * stepSeconds * sampleRate), 0.0f);
            for (auto& tr : tracks)
                for (int step : tr.steps)
                {
                    const auto start = (size_t) (step * stepSeconds * sampleRate);
                    for (size_t i = 0; i < tr.sound->size() && start + i < out.size(); ++i)
                        out[start + i] += (*tr.sound)[i] * tr.gain;
                }
            for (auto& x : out)
                x = std::tanh (x * 0.8f);
            return out;
        }

        struct Entry { const char* path; std::function<Sound()> make; };

        inline const std::vector<Entry>& entries()
        {
            static const std::vector<Entry> list {
                { "Drums/Kick Punchy.wav",  [] { return kick (55.0, 220.0, 0.025, 0.16, 2.2f, 0.5f); } },
                { "Drums/Kick Deep.wav",    [] { return kick (42.0, 140.0, 0.05, 0.4, 1.4f, 0.2f); } },
                { "Drums/Kick Hard.wav",    [] { return kick (50.0, 300.0, 0.02, 0.2, 4.0f, 0.8f); } },
                { "Drums/Snare Tight.wav",  [] { return snare (230.0, 0.04, 0.07, 0.9f); } },
                { "Drums/Snare Fat.wav",    [] { return snare (160.0, 0.09, 0.18, 0.7f); } },
                { "Drums/Clap Tight.wav",   [] { return clap (0.008, 0.07); } },
                { "Drums/Clap Wide.wav",    [] { return clap (0.014, 0.2); } },
                { "Drums/Shaker.wav",       shaker },
                { "Drums/Tom Low.wav",      [] { return tom (90.0); } },
                { "Drums/Tom Mid.wav",      [] { return tom (130.0); } },
                { "Drums/Tom High.wav",     [] { return tom (185.0); } },
                { "Drums/Crash.wav",        [] { return metal (2.5, 0.7, 0.5f, 0.5f); } },
                { "Drums/Ride.wav",         ride },
                { "Drums/Perc.wav",         perc },
                { "Drums/Cowbell.wav",      cowbell },
                { "Drums/808 Long.wav",     [] { return bass808 (1.1, 3.0, 2.0f); } },
                { "Drums/808 Short.wav",    [] { return bass808 (0.22, 0.8, 3.0f); } },
                { "FX/Riser.wav",           [] { return sweep (4.0, true, 0.25f); } },
                { "FX/Downlifter.wav",      [] { return sweep (3.0, false, 0.25f); } },
                { "FX/Impact.wav",          impact },
                { "FX/Noise Sweep.wav",     noiseSweep },
                { "Loops/Drum Loop 120 BPM.wav", []
                {
                    const auto k = kick (48.0, 180.0, 0.035, 0.22, 1.6f, 0.4f), s = snare (185.0, 0.06, 0.11, 0.8f), h = hat (0.035);
                    return loop (120.0, 2, { { &k, 1.0f, { 0, 8, 10, 16, 24, 26 } }, { &s, 0.9f, { 4, 12, 20, 28 } },
                                             { &h, 0.6f, { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30 } } });
                } },
                { "Loops/Trap Loop 140 BPM.wav", []
                {
                    const auto k = kick (55.0, 220.0, 0.025, 0.16, 2.2f, 0.5f), s = snare (230.0, 0.04, 0.07, 0.9f), h = hat (0.03), b = bass808 (0.5, 1.2, 2.2f);
                    return loop (140.0, 2, { { &k, 0.9f, { 0, 10, 16, 19, 26 } }, { &b, 0.7f, { 0, 16 } }, { &s, 0.9f, { 8, 24 } },
                                             { &h, 0.5f, { 0, 2, 4, 6, 8, 10, 12, 13, 14, 15, 16, 18, 20, 22, 24, 26, 28, 29, 30, 31 } } });
                } },
                { "Loops/House Loop 124 BPM.wav", []
                {
                    const auto k = kick (50.0, 200.0, 0.03, 0.2, 1.8f, 0.4f), c = clap (0.011, 0.12), o = hat (0.16), h = hat (0.03);
                    return loop (124.0, 2, { { &k, 1.0f, { 0, 4, 8, 12, 16, 20, 24, 28 } }, { &c, 0.8f, { 4, 12, 20, 28 } },
                                             { &o, 0.5f, { 2, 6, 10, 14, 18, 22, 26, 30 } }, { &h, 0.35f, { 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31 } } });
                } },
            };
            return list;
        }
    }

    // Writes any missing library sounds into `packs` (default: the user's Packs folder).
    inline void ensureLibrary (const juce::File& packs = getPacksFolder())
    {
        for (auto& e : Library::entries())
            if (auto f = packs.getChildFile (e.path); ! f.existsAsFile())
                detail::writeWav (f, e.make());
    }
}
