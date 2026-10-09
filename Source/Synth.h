#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

// Parameters written by the UI thread, read by the audio thread.
struct SynthParams
{
    enum Waveform { sine = 0, saw, square, triangle };

    std::atomic<int> waveform { saw };
    std::atomic<float> attack { 0.005f }, decay { 0.3f }, sustain { 0.7f }, release { 0.3f };
};

// Lock-free single-producer / single-consumer queue of short MIDI messages.
// Use one instance per producer thread (UI keyboard, MIDI device).
class MidiFifo
{
public:
    void push (const juce::MidiMessage& m)
    {
        const int size = m.getRawDataSize();
        if (size > 3) // ignore sysex
            return;

        const auto scope = fifo.write (1);
        if (scope.blockSize1 > 0)
        {
            auto& slot = slots[(size_t) scope.startIndex1];
            slot.size = size;
            std::memcpy (slot.bytes, m.getRawData(), (size_t) size);
        }
    }

    void drainInto (juce::MidiBuffer& out)
    {
        const auto scope = fifo.read (fifo.getNumReady());
        scope.forEach ([&] (int i) { out.addEvent (slots[(size_t) i].bytes, slots[(size_t) i].size, 0); });
    }

private:
    struct Slot { juce::uint8 bytes[3]; int size; };
    static constexpr int capacity = 512;
    juce::AbstractFifo fifo { capacity };
    std::array<Slot, capacity> slots {};
};

struct SynthSound : juce::SynthesiserSound
{
    bool appliesToNote (int) override    { return true; }
    bool appliesToChannel (int) override { return true; }
};

class SynthVoice : public juce::SynthesiserVoice
{
public:
    explicit SynthVoice (const SynthParams& p) : params (p) {}

    bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<SynthSound*> (s) != nullptr; }

    void startNote (int note, float velocity, juce::SynthesiserSound*, int pitchWheel) override
    {
        baseIncrement = juce::MidiMessage::getMidiNoteInHertz (note) / getSampleRate();
        pitchWheelMoved (pitchWheel);
        phase = 0.0;
        level = velocity * 0.15f;

        adsr.setSampleRate (getSampleRate());
        adsr.setParameters ({ params.attack.load(), params.decay.load(), params.sustain.load(), params.release.load() });
        adsr.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            adsr.noteOff();
        }
        else
        {
            adsr.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved (int value) override
    {
        const double semitones = (value - 8192) / 8192.0 * 2.0; // +-2 semitone bend
        increment = baseIncrement * std::pow (2.0, semitones / 12.0);
    }

    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (! isVoiceActive())
            return;

        const int wave = params.waveform.load();

        for (int i = start; i < start + num; ++i)
        {
            const float sample = oscillator (wave) * level * adsr.getNextSample();

            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.addSample (ch, i, sample);

            if (! adsr.isActive())
            {
                clearCurrentNote();
                break;
            }
        }
    }

private:
    // PolyBLEP smooths the hard edges of saw/square so high notes don't alias (sound harsh/metallic).
    static double polyBlep (double t, double dt)
    {
        if (t < dt)       { t /= dt;               return t + t - t * t - 1.0; }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt;    return t * t + t + t + 1.0; }
        return 0.0;
    }

    float oscillator (int wave)
    {
        const double t = phase, dt = increment;
        double out = 0.0;

        switch (wave)
        {
            case SynthParams::sine:     out = std::sin (juce::MathConstants<double>::twoPi * t); break;
            case SynthParams::saw:      out = 2.0 * t - 1.0 - polyBlep (t, dt); break;
            case SynthParams::square:   out = (t < 0.5 ? 1.0 : -1.0) + polyBlep (t, dt) - polyBlep (std::fmod (t + 0.5, 1.0), dt); break;
            case SynthParams::triangle: out = 4.0 * std::abs (t - 0.5) - 1.0; break; // ponytail: naive triangle, mild aliasing; integrate BLEP square if it ever sounds harsh
            default: break;
        }

        phase += dt;
        if (phase >= 1.0)
            phase -= 1.0;

        return (float) out;
    }

    const SynthParams& params;
    juce::ADSR adsr;
    double phase = 0.0, baseIncrement = 0.0, increment = 0.0;
    float level = 0.0f;
};
