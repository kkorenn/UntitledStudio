#pragma once

// Our own 4-operator FM (phase modulation) synth. Operator 4 has self-feedback.
// Audio thread: no allocation/locks; all parameter changes are smoothed per sample.
namespace FM
{
    constexpr int numOps = 4, numAlgorithms = 8, maxVoices = 16;
    enum OpParam { ratio, fine, level, attack, decay, sustain, release, numOpParams };
    enum Global { algorithm = numOps * numOpParams, feedback, cutoff, resonance, velocity, volume, numParams };

    // Per algorithm: which operators modulate each operator (bit i = op i+1), and which are carriers.
    // Modulators always have a higher number than their target, so ops render 4 -> 1.
    struct Algorithm { const char* name; std::array<int, numOps> mods; int carriers; };

    inline const std::array<Algorithm, numAlgorithms>& algorithms()
    {
        static const std::array<Algorithm, numAlgorithms> list { {
            { "Stack 4>3>2>1",     { 0b0010, 0b0100, 0b1000, 0 }, 0b0001 },
            { "Two stacks",        { 0b0010, 0,      0b1000, 0 }, 0b0101 },
            { "Three into one",    { 0b1110, 0,      0,      0 }, 0b0001 },
            { "Additive",          { 0,      0,      0,      0 }, 0b1111 },
            { "Branch (4>3)+2>1",  { 0b0110, 0,      0b1000, 0 }, 0b0001 },
            { "One into three",    { 0b1000, 0b1000, 0b1000, 0 }, 0b0111 },
            { "Stack 4>3>2 + 1",   { 0,      0b0100, 0b1000, 0 }, 0b0011 },
            { "4>1 + 2 + 3",       { 0b1000, 0,      0,      0 }, 0b0111 },
        } };
        return list;
    }

    struct Spec { juce::String id, name; juce::NormalisableRange<float> range; float def; };

    inline const std::vector<Spec>& specs()
    {
        static const std::vector<Spec> list = []
        {
            std::vector<Spec> v;
            for (int op = 1; op <= numOps; ++op)
            {
                const auto n = juce::String (op), id = "op" + n;
                v.push_back ({ id + "Ratio",   "Op " + n + " Ratio",   { 0.5f, 16.0f, 0.5f }, 1.0f });
                v.push_back ({ id + "Fine",    "Op " + n + " Fine",    { -100.0f, 100.0f }, 0.0f });
                v.push_back ({ id + "Level",   "Op " + n + " Level",   { 0.0f, 1.0f }, op == 1 ? 1.0f : (op == 2 ? 0.35f : 0.0f) });
                v.push_back ({ id + "Attack",  "Op " + n + " Attack",  { 0.001f, 10.0f, 0.0f, 0.3f }, 0.003f });
                v.push_back ({ id + "Decay",   "Op " + n + " Decay",   { 0.001f, 10.0f, 0.0f, 0.3f }, 1.0f });
                v.push_back ({ id + "Sustain", "Op " + n + " Sustain", { 0.0f, 1.0f }, 0.6f });
                v.push_back ({ id + "Release", "Op " + n + " Release", { 0.001f, 10.0f, 0.0f, 0.3f }, 0.3f });
            }
            v.push_back ({ "algorithm", "Algorithm",   { 0.0f, (float) numAlgorithms - 1.0f, 1.0f }, 0.0f });
            v.push_back ({ "feedback",  "Feedback",    { 0.0f, 1.0f }, 0.0f });
            v.push_back ({ "cutoff",    "Cutoff",      { 20.0f, 20000.0f, 0.0f, 0.25f }, 20000.0f });
            v.push_back ({ "resonance", "Resonance",   { 0.0f, 1.0f }, 0.1f });
            v.push_back ({ "velocity",  "Velocity",    { 0.0f, 1.0f }, 0.7f });
            v.push_back ({ "volume",    "Volume",      { -48.0f, 6.0f }, 0.0f });
            return v;
        }();
        return list;
    }

    // Shared per-sample values the voices read.
    struct Frame
    {
        int algorithm = 0;
        std::array<float, numOps> level {}, freqMul {};
        float feedback = 0.0f, bend = 1.0f;
        std::array<float, numOps> attackInc {}, decayCoef {}, sustain {}, releaseCoef {};
    };

    struct Envelope
    {
        enum Stage { off, attack, decay, release };
        Stage stage = off;
        float value = 0.0f;

        float next (const Frame& f, int op)
        {
            switch (stage)
            {
                case attack:
                    value += f.attackInc[(size_t) op];
                    if (value >= 1.0f) { value = 1.0f; stage = decay; }
                    break;
                case decay:   value += (f.sustain[(size_t) op] - value) * f.decayCoef[(size_t) op]; break;
                case release: value -= value * f.releaseCoef[(size_t) op]; break;
                case off:     break;
            }
            return value;
        }
    };

    struct Voice
    {
        int note = -1;
        bool keyDown = false, sustained = false;
        juce::uint32 age = 0;
        float baseInc = 0.0f, velGain = 1.0f;
        std::array<float, numOps> phase {}, out {};
        std::array<Envelope, numOps> env;
        float fb1 = 0.0f, fb2 = 0.0f;

        bool isActive() const { return note >= 0; }

        float carrierLevel (int carriers) const
        {
            float l = 0.0f;
            for (int op = 0; op < numOps; ++op)
                if (carriers & (1 << op))
                    l = juce::jmax (l, env[(size_t) op].value);
            return l;
        }

        void start (int midiNote, float velocity, float sens, double sampleRate, juce::uint32 newAge)
        {
            if (! isActive())  // stolen voices keep their phase so they don't click
            {
                phase = {};
                fb1 = fb2 = 0.0f;
            }
            note = midiNote;
            keyDown = true;
            sustained = false;
            age = newAge;
            baseInc = (float) (juce::MidiMessage::getMidiNoteInHertz (midiNote) / sampleRate);
            velGain = 1.0f - sens + sens * velocity;
            for (auto& e : env)
                e.stage = Envelope::attack;  // attack continues from the current level
        }

        void stop()
        {
            keyDown = false;
            for (auto& e : env)
                e.stage = Envelope::release;
        }

        float tick (const Frame& f)
        {
            const auto& alg = algorithms()[(size_t) f.algorithm];
            const float inc = baseInc * f.bend;
            float sum = 0.0f;
            for (int op = numOps; --op >= 0;)
            {
                float pm = 0.0f;
                for (int m = op + 1; m < numOps; ++m)
                    if (alg.mods[(size_t) op] & (1 << m))
                        pm += out[(size_t) m];
                if (op == numOps - 1)
                    pm += f.feedback * (fb1 + fb2) * 0.5f;

                const float e = env[(size_t) op].next (f, op);
                const float o = std::sin (juce::MathConstants<float>::twoPi * phase[(size_t) op] + pm * 4.0f)
                                    * e * f.level[(size_t) op] * velGain;
                out[(size_t) op] = o;
                phase[(size_t) op] += inc * f.freqMul[(size_t) op];
                phase[(size_t) op] -= std::floor (phase[(size_t) op]);
                if (alg.carriers & (1 << op))
                    sum += o;
            }
            fb2 = fb1;
            fb1 = out[numOps - 1];

            if (! keyDown && ! sustained && carrierLevel (alg.carriers) < 1.0e-4f)
                note = -1;
            return sum;
        }
    };
}

class FMSynthPlugin : public te::Plugin
{
public:
    FMSynthPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        auto um = getUndoManager();
        const auto& specs = FM::specs();
        for (size_t i = 0; i < specs.size(); ++i)
        {
            values[i].referTo (state, juce::Identifier (specs[i].id), um, specs[i].def);
            params[i] = addParam (specs[i].id, specs[i].name, specs[i].range);
            params[i]->attachToCurrentValue (values[i]);
        }
    }

    ~FMSynthPlugin() override
    {
        notifyListenersOfDeletion();
        for (auto p : getAutomatableParameters())
            p->detachFromCurrentValue();
    }

    static const char* getPluginName()                  { return "FM Synth"; }
    static inline const char* xmlTypeName = "us_fmsynth";

    juce::String getName() const override               { return "FM Synth"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "FM"; }
    juce::String getSelectableDescription() override    { return "FM Synth"; }
    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool needsConstantBufferSize() override             { return false; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }

    te::AutomatableParameter* getParam (int index) const { return params[(size_t) index].get(); }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        rate = info.sampleRate;
        for (auto& v : voices) v = {};
        ic1 = ic2 = 0.0f;
        auto setup = [this] (juce::SmoothedValue<float>& s, float v) { s.reset (rate, 0.02); s.setCurrentAndTargetValue (v); };
        for (int op = 0; op < FM::numOps; ++op)
        {
            setup (level[(size_t) op], value (op * FM::numOpParams + FM::level));
            setup (freqMul[(size_t) op], ratioOf (op));
        }
        setup (feedbackS, value (FM::feedback));
        setup (cutoffS, value (FM::cutoff));
        setup (resonanceS, value (FM::resonance));
        setup (gainS, juce::Decibels::decibelsToGain (value (FM::volume)));
        setup (bendS, 1.0f);
    }

    void deinitialise() override {}

    void applyToBuffer (const te::PluginRenderContext& rc) override
    {
        if (rc.destBuffer == nullptr || rc.bufferNumSamples <= 0)
            return;

        rc.destBuffer->clear (rc.bufferStartSample, rc.bufferNumSamples);
        updateTargets();

        int done = 0;
        if (auto midi = rc.bufferForMidiMessages)
        {
            if (midi->isAllNotesOff)
                for (auto& v : voices) v.note = -1;

            for (auto& m : *midi)
            {
                const int at = juce::jlimit (0, rc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * rate));
                render (*rc.destBuffer, rc.bufferStartSample + done, at - done);
                done = juce::jmax (done, at);
                handleMidi (m);
            }
        }
        render (*rc.destBuffer, rc.bufferStartSample + done, rc.bufferNumSamples - done);
    }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyValueTree (state, v, getUndoManager());
        for (auto p : getAutomatableParameters())
            p->updateFromAttachedValue();
    }

    int getNumActiveVoices() const
    {
        int n = 0;
        for (auto& v : voices) n += v.isActive() ? 1 : 0;
        return n;
    }

private:
    float value (int index) const { return params[(size_t) index]->getCurrentValue(); }

    float ratioOf (int op) const
    {
        const int base = op * FM::numOpParams;
        return value (base + FM::ratio) * std::exp2 (value (base + FM::fine) / 1200.0f);
    }

    // Once per block: targets for smoothed values + envelope coefficients.
    void updateTargets()
    {
        const auto sr = (float) rate;
        auto coef = [sr] (float seconds) { return 1.0f - std::exp (-4.6f / juce::jmax (1.0f, seconds * sr)); };
        for (int op = 0; op < FM::numOps; ++op)
        {
            const int base = op * FM::numOpParams;
            level[(size_t) op].setTargetValue (value (base + FM::level));
            freqMul[(size_t) op].setTargetValue (ratioOf (op));
            frame.attackInc[(size_t) op] = 1.0f / juce::jmax (1.0f, value (base + FM::attack) * sr);
            frame.decayCoef[(size_t) op] = coef (value (base + FM::decay));
            frame.sustain[(size_t) op] = value (base + FM::sustain);
            frame.releaseCoef[(size_t) op] = coef (value (base + FM::release));
        }
        frame.algorithm = juce::jlimit (0, FM::numAlgorithms - 1, juce::roundToInt (value (FM::algorithm)));
        feedbackS.setTargetValue (value (FM::feedback));
        cutoffS.setTargetValue (value (FM::cutoff));
        resonanceS.setTargetValue (value (FM::resonance));
        gainS.setTargetValue (juce::Decibels::decibelsToGain (value (FM::volume), -47.9f));
    }

    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
        {
            auto& v = findVoice (m.getNoteNumber());
            v.start (m.getNoteNumber(), m.getFloatVelocity(), value (FM::velocity), rate, ++ageCounter);
        }
        else if (m.isNoteOff())
        {
            for (auto& v : voices)
                if (v.isActive() && v.keyDown && v.note == m.getNoteNumber())
                {
                    if (pedal) { v.keyDown = false; v.sustained = true; }
                    else       v.stop();
                }
        }
        else if (m.isPitchWheel())
        {
            bendS.setTargetValue (std::exp2 ((float) (m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f / 12.0f));
        }
        else if (m.isSustainPedalOn())
        {
            pedal = true;
        }
        else if (m.isSustainPedalOff())
        {
            pedal = false;
            for (auto& v : voices)
                if (v.isActive() && v.sustained)
                    v.stop();
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& v : voices)
                if (v.isActive()) v.stop();
        }
    }

    // Free voice, else the same note, else the quietest released voice, else the oldest.
    FM::Voice& findVoice (int note)
    {
        FM::Voice* best = nullptr;
        for (auto& v : voices)
            if (! v.isActive()) return v;
        for (auto& v : voices)
            if (v.note == note) return v;
        const int carriers = FM::algorithms()[(size_t) frame.algorithm].carriers;
        for (auto& v : voices)
            if (! v.keyDown && (best == nullptr || v.carrierLevel (carriers) < best->carrierLevel (carriers)))
                best = &v;
        if (best != nullptr)
            return *best;
        best = &voices[0];
        for (auto& v : voices)
            if (v.age < best->age) best = &v;
        return *best;
    }

    void render (juce::AudioBuffer<float>& buffer, int start, int num)
    {
        if (num <= 0)
            return;

        auto* left = buffer.getWritePointer (0, start);
        auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1, start) : nullptr;
        const float nyquistLimit = (float) rate * 0.45f;
        float g = 0.0f, k = 1.0f;
        bool coefsValid = false;

        for (int i = 0; i < num; ++i)
        {
            for (int op = 0; op < FM::numOps; ++op)
            {
                frame.level[(size_t) op] = level[(size_t) op].getNextValue();
                frame.freqMul[(size_t) op] = freqMul[(size_t) op].getNextValue();
            }
            frame.feedback = feedbackS.getNextValue();
            frame.bend = bendS.getNextValue();

            float mix = 0.0f;
            for (auto& v : voices)
                if (v.isActive())
                    mix += v.tick (frame);

            // TPT state-variable low-pass (coefficients only recomputed while smoothing).
            const bool smoothing = cutoffS.isSmoothing() || resonanceS.isSmoothing();
            const float fc = cutoffS.getNextValue(), res = resonanceS.getNextValue();
            if (smoothing || ! coefsValid)
            {
                g = std::tan (juce::MathConstants<float>::pi * juce::jmin (fc, nyquistLimit) / (float) rate);
                k = juce::jmap (res, 1.414f, 0.06f);
                coefsValid = true;
            }
            const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const float x = mix * 0.35f;
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;

            const float out = v2 * gainS.getNextValue();
            left[i] = out;
            if (right != nullptr) right[i] = out;
        }
    }

    std::array<juce::CachedValue<float>, FM::numParams> values;
    std::array<te::AutomatableParameter::Ptr, FM::numParams> params;

    double rate = 44100.0;
    std::array<FM::Voice, FM::maxVoices> voices;
    FM::Frame frame;
    std::array<juce::SmoothedValue<float>, FM::numOps> level, freqMul;
    juce::SmoothedValue<float> feedbackS, cutoffS, resonanceS, gainS, bendS;
    float ic1 = 0.0f, ic2 = 0.0f;
    juce::uint32 ageCounter = 0;
    bool pedal = false;
};
