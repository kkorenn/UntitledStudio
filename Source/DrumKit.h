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
}
