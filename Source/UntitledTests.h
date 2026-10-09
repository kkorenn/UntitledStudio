#pragma once

#include "Effects.h"

// Self-tests for the Untitled effects (part of --selftest). DSP runs on detached plugin instances
// (created from the edit's plugin cache but not inserted anywhere), so the audio thread never touches them.
namespace UntitledTests
{
    using Check = std::function<bool (bool, const juce::String&)>;

    struct Rig
    {
        te::Plugin::Ptr plugin;
        double sr, time = 0.0;

        Rig (Studio& s, const juce::String& type, double sampleRate = 44100.0)
            : plugin (s.getEdit().getPluginCache().createNewPlugin (type, {})), sr (sampleRate)
        {
            if (plugin != nullptr)
                plugin->baseClassInitialise ({ te::TimePosition(), sr, 512 });
        }

        ~Rig()
        {
            if (plugin != nullptr)
                plugin->baseClassDeinitialise();
        }

        UFX::Plugin& fx() const { return *dynamic_cast<UFX::Plugin*> (plugin.get()); }

        Rig& set (const juce::String& id, float value)
        {
            if (auto* p = fx().param (id))
                p->setParameter (value, juce::sendNotificationSync);
            else
                jassertfalse;
            return *this;
        }

        // Processes in uneven block sizes (exercises the chunking) with the transport playing from 0.
        juce::AudioBuffer<float> run (juce::AudioBuffer<float> buffer, int maxBlock = 437)
        {
            juce::Random rng (99);
            for (int pos = 0; pos < buffer.getNumSamples();)
            {
                const int n = juce::jmin (maxBlock > 0 ? maxBlock : 1 + rng.nextInt (1500), buffer.getNumSamples() - pos);
                te::PluginRenderContext rc (&buffer, pos, n, nullptr, 0.0,
                                            te::TimeRange (te::TimePosition::fromSeconds (time), te::TimeDuration::fromSeconds (n / sr)),
                                            true, false, true, false);
                plugin->applyToBuffer (rc);
                time += n / sr;
                pos += n;
            }
            return buffer;
        }
    };

    inline juce::AudioBuffer<float> sine (float freq, float amp, double seconds, double sr = 44100.0, float freqR = 0.0f)
    {
        juce::AudioBuffer<float> b (2, (int) (seconds * sr));
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            b.setSample (0, i, amp * (float) std::sin (2.0 * UDsp::pi * freq * i / sr));
            b.setSample (1, i, amp * (float) std::sin (2.0 * UDsp::pi * (freqR > 0 ? freqR : freq) * i / sr));
        }
        return b;
    }

    inline juce::AudioBuffer<float> noise (double seconds, float amp = 0.3f, double sr = 44100.0)
    {
        juce::AudioBuffer<float> b (2, (int) (seconds * sr));
        juce::Random rng (5);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (ch, i, amp * (rng.nextFloat() * 2.0f - 1.0f));
        return b;
    }

    inline float peakDb (const juce::AudioBuffer<float>& b, double from, double to, double sr = 44100.0)
    {
        return UDsp::gainToDb (b.getMagnitude ((int) (from * sr), (int) ((to - from) * sr)));
    }

    inline float rms (const juce::AudioBuffer<float>& b, double from, double to, double sr = 44100.0)
    {
        return juce::jmax (b.getRMSLevel (0, (int) (from * sr), (int) ((to - from) * sr)), b.getRMSLevel (1, (int) (from * sr), (int) ((to - from) * sr)));
    }

    inline bool finite (const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite (b.getSample (ch, i)))
                    return false;
        return true;
    }

    inline bool run (Studio& s, const Check& check)
    {
        bool ok = true;
        const double oldBpm = s.getBpm();
        s.setBpm (120.0);

        {
            int created = 0;
            for (auto& u : UFX::catalog())
                if (auto p = s.addEffect (6, u.xmlType))
                {
                    created += dynamic_cast<UFX::Plugin*> (p.get()) != nullptr ? 1 : 0;
                    s.removePlugin (*p);
                }
            ok &= check (created == (int) UFX::catalog().size(), "every Untitled effect is created via addEffect");

            bool allFinite = true;
            for (auto& u : UFX::catalog())
            {
                Rig rig (s, u.xmlType, 96000.0);
                allFinite &= finite (rig.run (noise (0.25, 0.5f, 96000.0), 0));
            }
            ok &= check (allFinite, "Untitled effects: no NaN/inf at 96 kHz with random block sizes (1..1500)");
        }

        {
            Rig eq (s, UFX::EqPlugin::xmlTypeName);
            eq.set ("b4Shape", 1).set ("b4Freq", 1000).set ("b4Gain", 12).set ("b4Q", 1);
            const float at1k = peakDb (eq.run (sine (1000, 0.1f, 0.5)), 0.3, 0.5);
            const float at100 = peakDb (eq.run (sine (100, 0.1f, 0.5)), 0.3, 0.5);
            ok &= check (std::abs (at1k - (-8.0f)) < 0.5f && std::abs (at100 - (-20.0f)) < 0.5f,
                         "EQ: +12 dB bell at 1 kHz boosts 1 kHz by 12 dB (" + juce::String (at1k + 20, 2) + "), not 100 Hz (" + juce::String (at100 + 20, 2) + ")");
        }

        {
            Rig lim (s, UFX::LimiterPlugin::xmlTypeName);
            lim.set ("gain", 12).set ("ceiling", -1);
            auto in = sine (997, 0.9f, 1.0);
            in.addFrom (0, 0, noise (1.0, 0.1f), 0, 0, in.getNumSamples()); // add transients
            const auto out = lim.run (in);
            UDsp::TruePeak tp;
            float truePeak = 0.0f;
            for (int i = 0; i < out.getNumSamples(); ++i)
                truePeak = juce::jmax (truePeak, tp.process (out.getSample (0, i), out.getSample (1, i)));
            const float samplePeak = peakDb (out, 0.0, 1.0);
            ok &= check (samplePeak <= -1.0f + 0.001f && UDsp::gainToDb (truePeak) < -1.0f + 0.6f && samplePeak > -1.5f,
                         "Limiter: +12 dB into -1 dB ceiling, sample peak " + juce::String (samplePeak, 2) + " dB, true peak " + juce::String (UDsp::gainToDb (truePeak), 2) + " dBTP");
            ok &= check (std::abs (lim.plugin->getLatencySeconds() - UFX::LimiterPlugin::lookaheadSeconds) < 1.0e-4, "Limiter reports its look-ahead as latency");
        }

        {
            Rig comp (s, UFX::CompressorPlugin::xmlTypeName);
            comp.set ("threshold", -20).set ("ratio", 4).set ("knee", 0).set ("attack", 1).set ("release", 200).set ("makeup", 0);
            const float out = peakDb (comp.run (sine (1000, 0.5f, 1.0)), 0.7, 1.0);
            ok &= check (std::abs (out - (-16.5f)) < 1.5f, "Compressor: -6 dB sine, -20 dB threshold, 4:1 -> " + juce::String (out, 2) + " dB (expected -16.5)");
        }

        {
            Rig mx (s, UFX::MaximizerPlugin::xmlTypeName);
            mx.set ("drive", 12).set ("ceiling", -0.5f);
            const auto out = mx.run (noise (1.0, 0.7f));
            ok &= check (finite (out) && peakDb (out, 0.0, 1.0) <= -0.5f + 0.001f && rms (out, 0.5, 1.0) > 0.1f, "Maximizer: loud noise stays under the ceiling");
        }

        {
            constexpr int start = 4410;
            auto impulse = [] (double seconds)
            {
                juce::AudioBuffer<float> b (2, (int) (seconds * 44100));
                b.clear();
                b.setSample (0, start, 1.0f); // after the dry/wet smoothing has settled
                b.setSample (1, start, 1.0f);
                return b;
            };
            auto argmax = [] (const juce::AudioBuffer<float>& b, int ch, int from)
            {
                int best = from;
                for (int i = from; i < b.getNumSamples(); ++i)
                    if (std::abs (b.getSample (ch, i)) > std::abs (b.getSample (ch, best))) best = i;
                return best;
            };
            Rig d (s, UFX::DelayPlugin::xmlTypeName);
            d.set ("sync", 1).set ("division", (float) UDsp::divisionIndex ("1/4")).set ("feedback", 0).set ("dry", 0).set ("wet", 1).set ("lowcut", 20).set ("highcut", 20000);
            const auto out = d.run (impulse (1.0));
            const int at = argmax (out, 0, 0);
            ok &= check (std::abs (at - (start + 22050)) <= 2, "Delay: 1/4 at 120 BPM echoes after 500 ms (" + juce::String ((at - start) / 44.1, 1) + " ms)");

            Rig pp (s, UFX::DelayPlugin::xmlTypeName);
            pp.set ("sync", 1).set ("division", (float) UDsp::divisionIndex ("1/8")).set ("feedback", 0.5f).set ("pingpong", 1).set ("dry", 0).set ("wet", 1)
              .set ("lowcut", 20).set ("highcut", 20000);
            const auto o2 = pp.run (impulse (1.0));
            const float l1 = std::abs (o2.getSample (0, start + 11025)), r1 = std::abs (o2.getSample (1, start + 11025));
            const float l2 = std::abs (o2.getSample (0, start + 22050)), r2 = std::abs (o2.getSample (1, start + 22050));
            ok &= check (l1 > 0.4f && r1 < 0.01f && r2 > 0.2f && l2 < 0.01f, "Delay: ping-pong alternates left then right");
        }

        {
            Rig rv (s, UFX::ReverbPlugin::xmlTypeName);
            rv.set ("dry", 0).set ("wet", 1).set ("decay", 1.0f).set ("predelay", 0);
            auto in = noise (2.5);
            in.clear ((int) (0.1 * 44100), in.getNumSamples() - (int) (0.1 * 44100));
            const auto out = rv.run (in);
            const float early = rms (out, 0.2, 0.4), mid = rms (out, 1.0, 1.2), late = rms (out, 2.2, 2.4);
            ok &= check (finite (out) && early > 1.0e-3f && mid < early * 0.5f && late < mid * 0.5f && early < 1.0f,
                         "Reverb: tail rings after the input stops and decays (" + juce::String (UDsp::gainToDb (early), 1) + " > "
                             + juce::String (UDsp::gainToDb (mid), 1) + " > " + juce::String (UDsp::gainToDb (late), 1) + " dB)");
        }

        {
            Rig f (s, UFX::FilterPlugin::xmlTypeName);
            f.set ("shape", 0).set ("cutoff", 1000).set ("reso", 0);
            const float hi = peakDb (f.run (sine (10000, 0.5f, 0.3)), 0.2, 0.3);
            const float lo = peakDb (f.run (sine (100, 0.5f, 0.3)), 0.2, 0.3);
            ok &= check (lo > -6.5f && hi < lo - 30.0f, "Filter: low-pass at 1 kHz passes 100 Hz, cuts 10 kHz by " + juce::String (lo - hi, 1) + " dB");

            f.set ("lfoDepth", 3).set ("lfoSync", 1).set ("lfoDiv", (float) UDsp::divisionIndex ("1/4"));
            const auto swept = f.run (noise (1.0));
            ok &= check (finite (swept) && std::abs (rms (swept, 0.0, 0.25) - rms (swept, 0.25, 0.5)) < 1.0f, "Filter: synced LFO sweep runs");
        }

        {
            Rig lm (s, UFX::LoudnessPlugin::xmlTypeName);
            lm.run (sine (1000, 0.1f, 3.5));
            const float m = lm.fx().meters[0].load(), st = lm.fx().meters[1].load(), in = lm.fx().meters[2].load();
            const float tp = UDsp::gainToDb (lm.fx().meters[3].load());
            ok &= check (std::abs (m + 20.0f) < 0.5f && std::abs (st + 20.0f) < 0.5f && std::abs (in + 20.0f) < 0.5f && std::abs (tp + 20.0f) < 0.3f,
                         "Loudness: -20 dBFS 1 kHz sine reads M " + juce::String (m, 2) + " / S " + juce::String (st, 2) + " / I " + juce::String (in, 2)
                             + " LUFS, TP " + juce::String (tp, 2) + " dBTP");
        }

        {
            Rig st (s, UFX::StereoPlugin::xmlTypeName);
            st.set ("width", 0);
            const auto out = st.run (sine (440, 0.5f, 0.3, 44100.0, 660));
            float diff = 0.0f;
            for (int i = (int) (0.15 * 44100); i < out.getNumSamples(); ++i)
                diff = juce::jmax (diff, std::abs (out.getSample (0, i) - out.getSample (1, i)));
            ok &= check (diff < 1.0e-6f && rms (out, 0.15, 0.3) > 0.1f, "Stereo Enhancer: width 0 makes L == R");

            Rig bal (s, UFX::BalancePlugin::xmlTypeName);
            bal.set ("pan", -1);
            const auto b = bal.run (sine (440, 0.5f, 0.3));
            ok &= check (b.getMagnitude (1, (int) (0.15 * 44100), (int) (0.15 * 44100)) < 1.0e-6f && b.getMagnitude (0, 0, b.getNumSamples()) > 0.4f,
                         "Balance: pan hard left silences the right channel");
        }

        {
            Rig sc (s, UFX::SoftClipperPlugin::xmlTypeName);
            sc.set ("threshold", -6);
            const float hot = peakDb (sc.run (sine (220, 1.0f, 0.3)), 0.1, 0.3);
            const float quiet = peakDb (sc.run (sine (220, 0.1f, 0.3)), 0.1, 0.3);
            ok &= check (hot < -5.7f && hot > -7.0f && std::abs (quiet + 20.0f) < 0.2f,
                         "Soft Clipper: 0 dB sine held at the -6 dB threshold (" + juce::String (hot, 2) + "), quiet signal untouched");

            bool curvesOk = true;
            for (int c = 0; c < 5; ++c)
            {
                Rig dist (s, UFX::DistortionPlugin::xmlTypeName);
                dist.set ("curve", (float) c).set ("drive", 24);
                const auto out = dist.run (sine (220, 0.5f, 0.3));
                curvesOk &= finite (out) && rms (out, 0.1, 0.3) > 0.02f;
            }
            ok &= check (curvesOk, "Distortion: every curve produces sound");
        }

        {
            bool modOk = true;
            for (auto type : { UFX::ChorusPlugin::xmlTypeName, UFX::FlangerPlugin::xmlTypeName, UFX::PhaserPlugin::xmlTypeName, UFX::EnhancerPlugin::xmlTypeName })
            {
                Rig m (s, type);
                const auto in = noise (0.5);
                const auto out = m.run (in);
                float diff = 0.0f;
                for (int i = 0; i < out.getNumSamples(); ++i)
                    diff = juce::jmax (diff, std::abs (out.getSample (0, i) - in.getSample (0, i)));
                const float ratio = rms (out, 0.1, 0.5) / rms (in, 0.1, 0.5);
                modOk &= finite (out) && diff > 0.01f && ratio > 0.25f && ratio < 4.0f;
            }
            ok &= check (modOk, "Chorus / Flanger / Phaser / Enhancer change the sound at sane levels");
        }

        {
            Rig g (s, UFX::GatePlugin::xmlTypeName);
            for (int k = 1; k <= 16; ++k)
                g.set ("s" + juce::String (k), (k % 2) == 1 ? 1.0f : 0.0f);
            g.set ("fade", 1);
            const auto out = g.run (sine (1000, 0.5f, 0.5)); // 1/16 at 120 BPM = 125 ms steps
            ok &= check (peakDb (out, 0.03, 0.1) > -6.5f && peakDb (out, 0.16, 0.24) < -60.0f, "Gate: trance pattern opens and closes on the sixteenths");

            Rig t (s, UFX::GatePlugin::xmlTypeName);
            t.set ("timeMode", 1).set ("timeDiv", (float) UDsp::divisionIndex ("1/8"));
            const auto in = noise (1.0);
            const auto st = t.run (in);
            const int slice = 11025; // 1/8 at 120 BPM
            float err = 0.0f;
            for (int i = 1000; i < 4000; ++i)
                err = juce::jmax (err, std::abs (st.getSample (0, slice + i) - in.getSample (0, i)));
            ok &= check (err < 1.0e-3f, "Time FX: stutter repeats the first slice of the beat");
        }

        {
            bool presetsOk = true;
            int total = 0;
            juce::String bad;
            for (auto& u : UFX::catalog())
            {
                Rig r (s, u.xmlType);
                const auto& presets = Presets::factory (u.xmlType);
                presetsOk &= presets.size() >= 3 && presets.size() <= 7;
                for (auto& preset : presets)
                {
                    ++total;
                    for (auto& [id, value] : preset.values)
                    {
                        auto* p = r.fx().param (id);
                        if (p == nullptr || value < p->valueRange.start || value > p->valueRange.end)
                        {
                            presetsOk = false;
                            bad << u.xmlType << ":" << preset.name << ":" << id << " ";
                        }
                    }
                }
            }
            ok &= check (presetsOk, "Untitled effects: " + juce::String (total) + " factory presets reference valid params " + bad);

            auto a = s.getEdit().getPluginCache().createNewPlugin (UFX::EqPlugin::xmlTypeName, {});
            dynamic_cast<UFX::Plugin*> (a.get())->param ("b4Gain")->setParameter (7.5f, juce::sendNotificationSync);
            auto b = s.getEdit().getPluginCache().createNewPlugin (a->state.createCopy());
            auto* restored = dynamic_cast<UFX::Plugin*> (b.get());
            ok &= check (restored != nullptr && std::abs (restored->param ("b4Gain")->getCurrentValue() - 7.5f) < 1.0e-4f, "Untitled effect parameters are saved in the plugin state");
        }

        {
            const char* names[] { "Fruity Parametric EQ 2", "Fruity Limiter", "Fruity Reeverb 2", "Fruity Delay 3", "Fruity Compressor", "Maximus",
                                  "Fruity Filter", "Fruity Love Philter", "Fruity Blood Overdrive", "Fruity Fast Dist", "Fruity Soft Clipper",
                                  "Fruity Balance", "Fruity Stereo Enhancer", "Fruity Stereo Shaper", "Fruity Chorus", "Fruity Flanger",
                                  "Fruity Phaser", "Gross Beat", "Soundgoodizer", "Wave Candy", "Fruity Delay 2", "Fruity Reeverb", "Fruity Free Filter" };
            bool mapped = true;
            for (auto* n : names)
            {
                const char* type = FlStockMap::untitledTypeFor (n);
                mapped &= type != nullptr && s.getEdit().getPluginCache().createNewPlugin (type, {}) != nullptr;
            }
            ok &= check (mapped && FlStockMap::untitledTypeFor ("Fruity Peak Controller") == nullptr,
                         "FL stock effect names map to registered Untitled effects");
        }

        s.setBpm (oldBpm);
        return ok;
    }

    // --fx-bench: CPU cost of each effect (10 s of stereo noise at 48 kHz, 512-sample blocks).
    inline void bench (Studio& s)
    {
        for (auto& u : UFX::catalog())
        {
            Rig rig (s, u.xmlType, 48000.0);
            const auto in = noise (10.0, 0.3f, 48000.0);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            rig.run (in, 512);
            const double ms = juce::Time::getMillisecondCounterHiRes() - start;
            std::cout << juce::String (u.name).paddedRight (' ', 28) << juce::String (ms / 100.0, 3) << "% of one core" << std::endl;
        }
    }
}
