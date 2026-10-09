#pragma once

#include "UntitledDSP.h"

// Our own stock effects ("Untitled ..."). One base class handles parameters (automatable, saved in
// the plugin state), tempo, sidechain and block chunking; each effect implements prepare() + process().
namespace UFX
{
    struct Spec
    {
        juce::String id, name;
        float min = 0.0f, max = 1.0f, def = 0.0f;
        juce::String unit;
        float centre = 0.0f;
        juce::StringArray choices;
    };

    inline Spec knob (const juce::String& id, const juce::String& name, float min, float max, float def,
                      const juce::String& unit = {}, float centre = 0.0f)
    {
        return { id, name, min, max, def, unit, centre, {} };
    }

    inline Spec choice (const juce::String& id, const juce::String& name, const juce::StringArray& items, int def = 0)
    {
        return { id, name, 0.0f, (float) items.size() - 1.0f, (float) def, {}, 0.0f, items };
    }

    inline Spec toggle (const juce::String& id, const juce::String& name, bool def)
    {
        return choice (id, name, { "Off", "On" }, def ? 1 : 0);
    }

    inline juce::String format (const Spec& s, float v)
    {
        if (! s.choices.isEmpty())
            return s.choices[juce::jlimit (0, s.choices.size() - 1, juce::roundToInt (v))];
        if (s.unit == "Hz")
            return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                                : (v >= 100.0f ? juce::String (juce::roundToInt (v)) : juce::String (v, v >= 10.0f ? 1 : 2)) + " Hz";
        if (s.unit == "%")    return juce::String (juce::roundToInt (v * 100.0f)) + "%";
        if (s.unit == "ms")   return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " s" : juce::String (v, v < 10.0f ? 1 : 0) + " ms";
        if (s.unit == ":1")   return juce::String (v, 1) + ":1";
        if (s.unit == "dB")   return (v <= -60.0f ? juce::String ("-inf") : juce::String (v, 1)) + " dB";
        if (s.unit == "pan")  return std::abs (v) < 0.005f ? juce::String ("C") : juce::String (juce::roundToInt (std::abs (v) * 100.0f)) + (v < 0 ? "L" : "R");
        if (s.unit == "LUFS" || s.unit == "oct") return juce::String (v, 1) + " " + s.unit;
        return juce::String (v, 2) + (s.unit.isEmpty() ? juce::String() : " " + s.unit);
    }

    struct Block
    {
        float* l;
        float* r;
        const float* scL;   // sidechain input, or nullptr
        const float* scR;
        int n;
        double beat, beatsPerSample, bpm;
        bool playing;
    };

    // Lock-free stereo sample tap for displays (spectrum, vectorscope). Drops samples when full.
    struct StereoTap
    {
        static constexpr int capacity = 1 << 13;
        juce::AbstractFifo fifo { capacity };
        std::array<float, capacity> left {}, right {};

        void write (const float* l, const float* r, int n) noexcept
        {
            const auto scope = fifo.write (juce::jmin (n, fifo.getFreeSpace()));
            std::copy_n (l, scope.blockSize1, left.begin() + scope.startIndex1);
            std::copy_n (r, scope.blockSize1, right.begin() + scope.startIndex1);
            std::copy_n (l + scope.blockSize1, scope.blockSize2, left.begin() + scope.startIndex2);
            std::copy_n (r + scope.blockSize1, scope.blockSize2, right.begin() + scope.startIndex2);
        }

        int read (float* l, float* r, int max) noexcept
        {
            const auto scope = fifo.read (juce::jmin (max, fifo.getNumReady()));
            std::copy_n (left.begin() + scope.startIndex1, scope.blockSize1, l);
            std::copy_n (right.begin() + scope.startIndex1, scope.blockSize1, r);
            std::copy_n (left.begin() + scope.startIndex2, scope.blockSize2, l + scope.blockSize1);
            std::copy_n (right.begin() + scope.startIndex2, scope.blockSize2, r + scope.blockSize1);
            return scope.blockSize1 + scope.blockSize2;
        }
    };

    //==============================================================================
    class Plugin : public te::Plugin
    {
    public:
        static constexpr int maxChunk = 256;

        Plugin (te::PluginCreationInfo info, const char* type, const char* name, std::vector<Spec> specList)
            : te::Plugin (info), typeName (type), displayName (name), specs (std::move (specList))
        {
            auto um = getUndoManager();
            for (auto& spec : specs)
            {
                auto* v = values.add (new juce::CachedValue<float>());
                v->referTo (state, juce::Identifier (spec.id), um, spec.def);
                juce::NormalisableRange<float> range (spec.min, spec.max, spec.choices.isEmpty() ? 0.0f : 1.0f);
                if (spec.centre > 0.0f)
                    range.setSkewForCentre (spec.centre);
                auto p = addParam (spec.id, spec.name, range,
                                   [spec] (float x) { return format (spec, x); },
                                   [] (const juce::String& t) { return t.getFloatValue(); });
                p->attachToCurrentValue (*v);
                params.push_back (p);
            }
        }

        ~Plugin() override
        {
            notifyListenersOfDeletion();
            for (auto p : getAutomatableParameters())
                p->detachFromCurrentValue();
        }

        juce::String getName() const override              { return displayName; }
        juce::String getPluginType() override              { return typeName; }
        juce::String getShortName (int) override           { return displayName.fromFirstOccurrenceOf ("Untitled ", false, false).substring (0, 10); }
        juce::String getSelectableDescription() override   { return displayName; }
        bool needsConstantBufferSize() override            { return false; }
        int getNumOutputChannelsGivenInputs (int) override { return 2; }
        BusLayout getBusses() const override               { return BusLayout::singleStereoInOut(); }
        double getLatencySeconds() override                { return latencySamples / rate; }

        void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
        {
            te::Plugin::getChannelNames (ins, outs);
            if (ins != nullptr && hasSidechainInput())
                ins->addArray (juce::StringArray { "Sidechain L", "Sidechain R" });
        }

        const std::vector<Spec>& getSpecs() const           { return specs; }
        te::AutomatableParameter* param (int index) const   { return params[(size_t) index].get(); }
        te::AutomatableParameter* param (const juce::String& id) const
        {
            for (auto& p : params)
                if (p->paramID == id)
                    return p.get();
            return nullptr;
        }
        float value (int index) const                       { return params[(size_t) index]->getCurrentValue(); }
        int choiceValue (int index) const                   { return juce::roundToInt (value (index)); }
        double getRate() const                              { return rate; }

        // Display values the audio thread publishes (meters, LFO phase...); meaning is per effect.
        std::array<std::atomic<float>, 8> meters {};

        void initialise (const te::PluginInitialisationInfo& info) override
        {
            rate = info.sampleRate;
            monoScratch.assign ((size_t) maxChunk, 0.0f);
            sidechainOn = getSidechainSourceID().isValid();
            prepare (rate);
        }

        void deinitialise() override {}

        void restorePluginStateFromValueTree (const juce::ValueTree& v) override
        {
            te::copyValueTree (state, v, getUndoManager());
            for (auto p : getAutomatableParameters())
                p->updateFromAttachedValue();
        }

        void applyToBuffer (const te::PluginRenderContext& rc) override
        {
            if (rc.destBuffer == nullptr || rc.bufferNumSamples <= 0 || rc.destBuffer->getNumChannels() == 0)
                return;

            juce::ScopedNoDenormals noDenormals;
            auto& buffer = *rc.destBuffer;
            const int channels = buffer.getNumChannels(), start = rc.bufferStartSample;
            float* l = buffer.getWritePointer (0, start);
            float* r = channels > 1 ? buffer.getWritePointer (1, start) : nullptr;
            const float* scL = nullptr;
            const float* scR = nullptr;
            if (sidechainOn.load (std::memory_order_relaxed) && channels > 2)
            {
                scL = buffer.getReadPointer (2, start);
                scR = channels > 3 ? buffer.getReadPointer (3, start) : scL;
            }

            tempoPos.set (rc.editTime.getStart());
            const double bpm = juce::jlimit (10.0, 999.0, tempoPos.getTempo());
            const double bps = bpm / 60.0 / rate;
            double beat = rc.isPlaying ? tempoPos.getBeats().inBeats() : freeBeat;

            for (int done = 0; done < rc.bufferNumSamples;)
            {
                const int n = juce::jmin (maxChunk, rc.bufferNumSamples - done);
                float* right = r != nullptr ? r + done : monoScratch.data();
                if (r == nullptr)
                    std::copy_n (l + done, n, right);
                process ({ l + done, right, scL != nullptr ? scL + done : nullptr, scR != nullptr ? scR + done : nullptr,
                           n, beat, bps, bpm, rc.isPlaying });
                beat += n * bps;
                done += n;
            }
            freeBeat = beat;
        }

    protected:
        virtual void prepare (double sampleRate) = 0;
        virtual void process (const Block&) = 0;
        virtual bool hasSidechainInput() const { return false; }

        void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
        {
            if (v == state && id == te::IDs::sidechainSourceID)
                sidechainOn = te::EditItemID::fromVar (v[id]).isValid();
            te::Plugin::valueTreePropertyChanged (v, id);
        }

        void meterMax (int index, float v) noexcept
        {
            auto& m = meters[(size_t) index];
            if (v > m.load (std::memory_order_relaxed))
                m.store (v, std::memory_order_relaxed);
        }

        double rate = 44100.0, latencySamples = 0.0;

    private:
        const char* typeName;
        juce::String displayName;
        std::vector<Spec> specs;
        juce::OwnedArray<juce::CachedValue<float>> values;
        std::vector<te::AutomatableParameter::Ptr> params;
        std::vector<float> monoScratch;
        te::tempo::Sequence::Position tempoPos { te::createPosition (edit.tempoSequence) };
        double freeBeat = 0.0;
        std::atomic<bool> sidechainOn { false };
    };

    // Smoothed parameter helper: target set once per chunk, read per sample.
    struct Smooth
    {
        juce::SmoothedValue<float> s;
        void reset (double sr, float v, double seconds = 0.03) { s.reset (sr, seconds); s.setCurrentAndTargetValue (v); }
        float next() noexcept               { return s.getNextValue(); }
        void target (float v) noexcept      { s.setTargetValue (v); }
    };

    //==============================================================================
    // 7-band parametric EQ (covers the role of FL's Parametric EQ 2).
    class EqPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_eq";
        static const char* getPluginName() { return "Untitled Parametric EQ"; }
        static constexpr int numBands = 7;

        static const juce::StringArray& shapeNames()
        {
            static const juce::StringArray s { "Off", "Bell", "Low shelf", "High shelf", "Low cut", "High cut", "Notch", "Band pass" };
            return s;
        }

        EqPlugin (te::PluginCreationInfo info) : Plugin (info, xmlTypeName, getPluginName(), makeSpecs()) {}

        static std::vector<Spec> makeSpecs()
        {
            struct D { int shape; float freq, q; };
            const D d[numBands] { { 0, 30, 0.71f }, { 2, 90, 0.71f }, { 1, 250, 1 }, { 1, 1000, 1 }, { 1, 3000, 1 }, { 3, 9000, 0.71f }, { 0, 16000, 0.71f } };
            std::vector<Spec> v;
            for (int b = 0; b < numBands; ++b)
            {
                const auto n = juce::String (b + 1);
                v.push_back (choice ("b" + n + "Shape", "Band " + n + " Type", shapeNames(), d[b].shape));
                v.push_back (knob ("b" + n + "Freq", "Band " + n + " Freq", 20.0f, 20000.0f, d[b].freq, "Hz", 1000.0f));
                v.push_back (knob ("b" + n + "Gain", "Band " + n + " Gain", -24.0f, 24.0f, 0.0f, "dB"));
                v.push_back (knob ("b" + n + "Q", "Band " + n + " Q", 0.1f, 18.0f, d[b].q, {}, 1.0f));
            }
            v.push_back (knob ("output", "Output", -24.0f, 12.0f, 0.0f, "dB"));
            return v;
        }

        static int shapeParam (int band) { return band * 4; }
        static int freqParam (int band)  { return band * 4 + 1; }
        static int gainParam (int band)  { return band * 4 + 2; }
        static int qParam (int band)     { return band * 4 + 3; }
        static constexpr int outputParam = numBands * 4;

        UDsp::Coeffs designFromParams (int band, double sr) const
        {
            return UDsp::Coeffs::make ((UDsp::Shape) choiceValue (shapeParam (band)), value (freqParam (band)),
                                       value (gainParam (band)), value (qParam (band)), sr);
        }

        StereoTap tap; // post-EQ audio for the spectrum

    protected:
        struct BandState { int shape = 0; float logFreq = 0, gain = 0, q = 1; };

        void prepare (double sr) override
        {
            for (int b = 0; b < numBands; ++b)
            {
                filters[(size_t) b].reset();
                current[(size_t) b] = targetOf (b);
                filters[(size_t) b].c = design (current[(size_t) b]);
            }
            output.reset (sr, UDsp::dbToGain (value (outputParam)));
            stepCoef = 1.0f - std::exp (-4.6f * (float) subBlock / (0.03f * (float) sr));
        }

        void process (const Block& blk) override
        {
            output.target (UDsp::dbToGain (value (outputParam)));
            for (int start = 0; start < blk.n; start += subBlock)
            {
                const int len = juce::jmin (subBlock, blk.n - start);
                for (int b = 0; b < numBands; ++b)
                {
                    auto& cur = current[(size_t) b];
                    const auto tgt = targetOf (b);
                    if (tgt.shape != cur.shape)
                        cur = tgt;
                    else if (std::abs (tgt.logFreq - cur.logFreq) < 1.0e-4f && std::abs (tgt.gain - cur.gain) < 1.0e-3f && std::abs (tgt.q - cur.q) < 1.0e-4f)
                        continue;
                    else
                    {
                        cur.logFreq += (tgt.logFreq - cur.logFreq) * stepCoef;
                        cur.gain += (tgt.gain - cur.gain) * stepCoef;
                        cur.q += (tgt.q - cur.q) * stepCoef;
                    }
                    filters[(size_t) b].c = design (cur);
                }

                for (int i = start; i < start + len; ++i)
                {
                    float l = blk.l[i], r = blk.r[i];
                    for (int b = 0; b < numBands; ++b)
                        if (current[(size_t) b].shape != 0)
                        {
                            l = filters[(size_t) b].process (l, 0);
                            r = filters[(size_t) b].process (r, 1);
                        }
                    const float g = output.next();
                    blk.l[i] = l * g;
                    blk.r[i] = r * g;
                }
            }
            tap.write (blk.l, blk.r, blk.n);
        }

    private:
        BandState targetOf (int b) const
        {
            return { choiceValue (shapeParam (b)), std::log (value (freqParam (b))), value (gainParam (b)), value (qParam (b)) };
        }

        UDsp::Coeffs design (const BandState& s) const
        {
            return UDsp::Coeffs::make ((UDsp::Shape) s.shape, std::exp (s.logFreq), s.gain, s.q, rate);
        }

        static constexpr int subBlock = 32;
        std::array<UDsp::Biquad, numBands> filters;
        std::array<BandState, numBands> current;
        Smooth output;
        float stepCoef = 0.1f;
    };

    //==============================================================================
    // Look-ahead brickwall limiter (covers the role of FL's Limiter). meters: 0 GR dB, 1 input peak, 2 output peak.
    class LimiterPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_limiter";
        static const char* getPluginName() { return "Untitled Limiter"; }
        enum { gain, ceiling, release };

        LimiterPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), {
                  knob ("gain", "Gain", -12.0f, 24.0f, 0.0f, "dB"),
                  knob ("ceiling", "Ceiling", -24.0f, 0.0f, -0.3f, "dB"),
                  knob ("release", "Release", 1.0f, 1000.0f, 80.0f, "ms", 80.0f) }) {}

        static constexpr double lookaheadSeconds = 0.003;

    protected:
        void prepare (double sr) override
        {
            core.prepare (sr, lookaheadSeconds);
            latencySamples = core.lookahead;
            input.reset (sr, UDsp::dbToGain (value (gain)));
        }

        void process (const Block& b) override
        {
            input.target (UDsp::dbToGain (value (gain)));
            float inPeak = 0.0f;
            for (int i = 0; i < b.n; ++i)
            {
                const float g = input.next();
                b.l[i] *= g;
                b.r[i] *= g;
                inPeak = juce::jmax (inPeak, std::abs (b.l[i]), std::abs (b.r[i]));
            }
            core.process (b.l, b.r, b.n, UDsp::dbToGain (value (ceiling)), UDsp::coefFor (value (release) * 0.001, rate));
            meterMax (0, -UDsp::gainToDb (core.minGain));
            meterMax (1, inPeak);
            meterMax (2, juce::jmax (juce::FloatVectorOperations::findMaximum (b.l, b.n), -juce::FloatVectorOperations::findMinimum (b.l, b.n),
                                     juce::FloatVectorOperations::findMaximum (b.r, b.n), -juce::FloatVectorOperations::findMinimum (b.r, b.n)));
        }

    private:
        UDsp::LimiterCore core;
        Smooth input;
    };

    //==============================================================================
    // Compressor with soft knee, parallel mix and sidechain input (covers the role of FL's Compressor).
    // meters: 0 GR dB, 1 input peak, 2 output peak.
    class CompressorPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_compressor";
        static const char* getPluginName() { return "Untitled Compressor"; }
        enum { threshold, ratio, attack, release, knee, makeup, mix };

        CompressorPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), {
                  knob ("threshold", "Threshold", -60.0f, 0.0f, -18.0f, "dB"),
                  knob ("ratio", "Ratio", 1.0f, 20.0f, 4.0f, ":1", 4.0f),
                  knob ("attack", "Attack", 0.1f, 200.0f, 10.0f, "ms", 10.0f),
                  knob ("release", "Release", 5.0f, 2000.0f, 120.0f, "ms", 150.0f),
                  knob ("knee", "Knee", 0.0f, 24.0f, 6.0f, "dB"),
                  knob ("makeup", "Makeup", -12.0f, 24.0f, 0.0f, "dB"),
                  knob ("mix", "Mix", 0.0f, 1.0f, 1.0f, "%") }) {}

    protected:
        bool hasSidechainInput() const override { return true; }

        void prepare (double sr) override
        {
            core.reset();
            thresholdS.reset (sr, value (threshold));
            makeupS.reset (sr, UDsp::dbToGain (value (makeup)));
            mixS.reset (sr, value (mix));
        }

        void process (const Block& b) override
        {
            core.setTimes (value (attack), value (release), rate);
            thresholdS.target (value (threshold));
            makeupS.target (UDsp::dbToGain (value (makeup)));
            mixS.target (value (mix));
            const float rat = value (ratio), kn = value (knee);
            float maxGr = 0.0f, inPeak = 0.0f, outPeak = 0.0f;
            for (int i = 0; i < b.n; ++i)
            {
                const float l = b.l[i], r = b.r[i];
                const float det = b.scL != nullptr ? juce::jmax (std::abs (b.scL[i]), std::abs (b.scR[i]))
                                                   : juce::jmax (std::abs (l), std::abs (r));
                const float gr = core.process (det, thresholdS.next(), rat, kn);
                const float m = mixS.next();
                const float g = (1.0f - m) + m * UDsp::dbToGain (-gr) * makeupS.next();
                b.l[i] = l * g;
                b.r[i] = r * g;
                maxGr = juce::jmax (maxGr, gr);
                inPeak = juce::jmax (inPeak, det);
                outPeak = juce::jmax (outPeak, std::abs (b.l[i]), std::abs (b.r[i]));
            }
            meterMax (0, maxGr);
            meterMax (1, inPeak);
            meterMax (2, outPeak);
        }

    private:
        UDsp::CompCore core;
        Smooth thresholdS, makeupS, mixS;
    };

    //==============================================================================
    // 3-band compressor + look-ahead master limiter (covers the role of FL's Maximus).
    // meters: 0..2 band GR dB, 3 limiter GR dB, 4 output peak.
    class MaximizerPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_maximizer";
        static const char* getPluginName() { return "Untitled Maximizer"; }
        enum { xLow, xHigh, lowThresh, lowRatio, lowGain, midThresh, midRatio, midGain, highThresh, highRatio, highGain,
               attack, release, drive, ceiling, limRelease };

        MaximizerPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), {
                  knob ("xLow", "Low/Mid", 40.0f, 1000.0f, 150.0f, "Hz", 200.0f),
                  knob ("xHigh", "Mid/High", 1000.0f, 12000.0f, 4000.0f, "Hz", 4000.0f),
                  knob ("lowThresh", "Low Thresh", -48.0f, 0.0f, -12.0f, "dB"),
                  knob ("lowRatio", "Low Ratio", 1.0f, 20.0f, 3.0f, ":1", 4.0f),
                  knob ("lowGain", "Low Gain", -12.0f, 12.0f, 0.0f, "dB"),
                  knob ("midThresh", "Mid Thresh", -48.0f, 0.0f, -12.0f, "dB"),
                  knob ("midRatio", "Mid Ratio", 1.0f, 20.0f, 3.0f, ":1", 4.0f),
                  knob ("midGain", "Mid Gain", -12.0f, 12.0f, 0.0f, "dB"),
                  knob ("highThresh", "High Thresh", -48.0f, 0.0f, -12.0f, "dB"),
                  knob ("highRatio", "High Ratio", 1.0f, 20.0f, 3.0f, ":1", 4.0f),
                  knob ("highGain", "High Gain", -12.0f, 12.0f, 0.0f, "dB"),
                  knob ("attack", "Attack", 0.1f, 100.0f, 5.0f, "ms", 10.0f),
                  knob ("release", "Release", 10.0f, 1000.0f, 150.0f, "ms", 150.0f),
                  knob ("drive", "Drive", 0.0f, 24.0f, 0.0f, "dB"),
                  knob ("ceiling", "Ceiling", -12.0f, 0.0f, -0.3f, "dB"),
                  knob ("limRelease", "Lim Release", 1.0f, 1000.0f, 60.0f, "ms", 80.0f) }) {}

    protected:
        void prepare (double sr) override
        {
            limiter.prepare (sr, LimiterPlugin::lookaheadSeconds);
            latencySamples = limiter.lookahead;
            for (auto* x : { &split1, &split2, &lowAllpass }) { x->freq = -1.0; x->reset(); }
            for (int k = 0; k < 3; ++k)
            {
                comps[(size_t) k].reset();
                bandGain[(size_t) k].reset (sr, UDsp::dbToGain (value (lowGain + 3 * k)));
            }
            driveS.reset (sr, UDsp::dbToGain (value (drive)));
        }

        void process (const Block& b) override
        {
            const float f1 = value (xLow), f2 = juce::jmax (value (xHigh), f1 * 1.5f);
            split1.set (f1, rate);
            split2.set (f2, rate);
            lowAllpass.set (f2, rate);
            float thr[3], rat[3], gr[3] {};
            for (int k = 0; k < 3; ++k)
            {
                comps[(size_t) k].setTimes (value (attack), value (release), rate);
                thr[k] = value (lowThresh + 3 * k);
                rat[k] = value (lowRatio + 3 * k);
                bandGain[(size_t) k].target (UDsp::dbToGain (value (lowGain + 3 * k)));
            }
            driveS.target (UDsp::dbToGain (value (drive)));

            for (int i = 0; i < b.n; ++i)
            {
                float band[3][2];
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float x = ch == 0 ? b.l[i] : b.r[i];
                    float lo, rest;
                    split1.split (x, ch, lo, rest);
                    band[0][ch] = lowAllpass.allpass (lo, ch);
                    split2.split (rest, ch, band[1][ch], band[2][ch]);
                }
                float outL = 0.0f, outR = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float det = juce::jmax (std::abs (band[k][0]), std::abs (band[k][1]));
                    const float red = comps[(size_t) k].process (det, thr[k], rat[k], 0.0f);
                    gr[k] = juce::jmax (gr[k], red);
                    const float g = UDsp::dbToGain (-red) * bandGain[(size_t) k].next();
                    outL += band[k][0] * g;
                    outR += band[k][1] * g;
                }
                const float d = driveS.next();
                b.l[i] = outL * d;
                b.r[i] = outR * d;
            }
            limiter.process (b.l, b.r, b.n, UDsp::dbToGain (value (ceiling)), UDsp::coefFor (value (limRelease) * 0.001, rate));
            for (int k = 0; k < 3; ++k)
                meterMax (k, gr[k]);
            meterMax (3, -UDsp::gainToDb (limiter.minGain));
            meterMax (4, juce::jmax (std::abs (juce::FloatVectorOperations::findMinimum (b.l, b.n)), juce::FloatVectorOperations::findMaximum (b.l, b.n)));
        }

    private:
        UDsp::Crossover split1, split2, lowAllpass;
        std::array<UDsp::CompCore, 3> comps;
        std::array<Smooth, 3> bandGain;
        Smooth driveS;
        UDsp::LimiterCore limiter;
    };

    //==============================================================================
    // Helper: 4x oversampling (linear-phase, integer latency) for the nonlinear effects.
    struct Oversampler
    {
        juce::dsp::Oversampling<float> os { 2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true };

        void prepare() { os.initProcessing ((size_t) Plugin::maxChunk); os.reset(); }
        int latency() const { return juce::roundToInt (os.getLatencyInSamples()); }

        template <typename Fn>
        void process (float* l, float* r, int n, Fn&& perSample)
        {
            float* chans[] = { l, r };
            juce::dsp::AudioBlock<float> block (chans, 2, (size_t) n);
            auto up = os.processSamplesUp (block);
            const int m = (int) up.getNumSamples();
            float* ul = up.getChannelPointer (0);
            float* ur = up.getChannelPointer (1);
            for (int i = 0; i < m; ++i)
                perSample (ul[i], ur[i]);
            os.processSamplesDown (block);
        }
    };

    // Soft clipper (covers the role of FL's Soft Clipper). meters: 0 output peak, 1 input peak.
    class SoftClipperPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_softclip";
        static const char* getPluginName() { return "Untitled Soft Clipper"; }
        enum { threshold, post };

        SoftClipperPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), {
                  knob ("threshold", "Threshold", -24.0f, 0.0f, 0.0f, "dB"),
                  knob ("post", "Post Gain", -24.0f, 12.0f, 0.0f, "dB") }) {}

        // Linear up to 2/3 of the threshold, then a smooth tanh shoulder that never passes it.
        static float clip (float x, float t) noexcept
        {
            const float a = t * (2.0f / 3.0f), ax = std::abs (x);
            if (ax <= a) return x;
            return std::copysign (a + (t - a) * std::tanh ((ax - a) / (t - a)), x);
        }

    protected:
        void prepare (double sr) override
        {
            oversampler.prepare();
            latencySamples = oversampler.latency();
            thresholdS.reset (sr * 4, UDsp::dbToGain (value (threshold)));
            postS.reset (sr, UDsp::dbToGain (value (post)));
        }

        void process (const Block& b) override
        {
            float inPeak = 0.0f;
            for (int i = 0; i < b.n; ++i)
                inPeak = juce::jmax (inPeak, std::abs (b.l[i]), std::abs (b.r[i]));
            thresholdS.target (UDsp::dbToGain (value (threshold)));
            oversampler.process (b.l, b.r, b.n, [this] (float& l, float& r)
            {
                const float t = thresholdS.next();
                l = clip (l, t);
                r = clip (r, t);
            });
            postS.target (UDsp::dbToGain (value (post)));
            float outPeak = 0.0f;
            for (int i = 0; i < b.n; ++i)
            {
                const float g = postS.next();
                b.l[i] *= g;
                b.r[i] *= g;
                outPeak = juce::jmax (outPeak, std::abs (b.l[i]), std::abs (b.r[i]));
            }
            meterMax (0, outPeak);
            meterMax (1, inPeak);
        }

    private:
        Oversampler oversampler;
        Smooth thresholdS, postS;
    };

    //==============================================================================
    inline void balanceGains (float pan, float& gl, float& gr) noexcept
    {
        const float half = juce::MathConstants<float>::halfPi;
        gl = pan > 0.0f ? std::cos (pan * half) : 1.0f;
        gr = pan < 0.0f ? std::cos (-pan * half) : 1.0f;
        if (pan <= -0.9999f) gr = 0.0f;
        if (pan >= 0.9999f)  gl = 0.0f;
    }

    // Volume + balance (covers the role of FL's Balance). meters: 0 L peak, 1 R peak.
    class BalancePlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_balance";
        static const char* getPluginName() { return "Untitled Balance"; }
        enum { volume, pan };

        BalancePlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), {
                  knob ("volume", "Volume", -60.0f, 12.0f, 0.0f, "dB", -6.0f),
                  knob ("pan", "Pan", -1.0f, 1.0f, 0.0f, "pan") }) {}

    protected:
        void prepare (double sr) override
        {
            float gl, gr;
            targets (gl, gr);
            left.reset (sr, gl);
            right.reset (sr, gr);
        }

        void process (const Block& b) override
        {
            float gl, gr;
            targets (gl, gr);
            left.target (gl);
            right.target (gr);
            float pl = 0.0f, pr = 0.0f;
            for (int i = 0; i < b.n; ++i)
            {
                b.l[i] *= left.next();
                b.r[i] *= right.next();
                pl = juce::jmax (pl, std::abs (b.l[i]));
                pr = juce::jmax (pr, std::abs (b.r[i]));
            }
            meterMax (0, pl);
            meterMax (1, pr);
        }

    private:
        void targets (float& gl, float& gr) const
        {
            const float v = value (volume), g = v <= -60.0f ? 0.0f : UDsp::dbToGain (v);
            balanceGains (value (pan), gl, gr);
            gl *= g;
            gr *= g;
        }

        Smooth left, right;
    };

    //==============================================================================
    // Width, pan, inter-channel delay and polarity (covers the roles of FL's Stereo Enhancer / Stereo Shaper).
    class StereoPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_stereo";
        static const char* getPluginName() { return "Untitled Stereo Enhancer"; }
        enum { width, pan, offset, invert };

        StereoPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), {
                  knob ("width", "Width", 0.0f, 2.0f, 1.0f, "%"),
                  knob ("pan", "Pan", -1.0f, 1.0f, 0.0f, "pan"),
                  knob ("offset", "Delay L/R", -20.0f, 20.0f, 0.0f, "ms"),
                  choice ("invert", "Polarity", { "Normal", "Invert L", "Invert R", "Invert both" }) }) {}

        StereoTap tap; // for the vectorscope

    protected:
        void prepare (double sr) override
        {
            for (auto* d : { &delayL, &delayR }) d->prepare ((int) (sr * 0.025) + 4);
            widthS.reset (sr, value (width));
            offsetS.reset (sr, value (offset) * 0.001f * (float) sr, 0.1);
            float gl, gr;
            balanceGains (value (pan), gl, gr);
            panL.reset (sr, gl);
            panR.reset (sr, gr);
        }

        void process (const Block& b) override
        {
            widthS.target (value (width));
            offsetS.target (value (offset) * 0.001f * (float) rate);
            float gl, gr;
            balanceGains (value (pan), gl, gr);
            panL.target (gl);
            panR.target (gr);
            const int inv = choiceValue (invert);
            const float signL = (inv == 1 || inv == 3) ? -1.0f : 1.0f, signR = (inv == 2 || inv == 3) ? -1.0f : 1.0f;

            for (int i = 0; i < b.n; ++i)
            {
                delayL.push (b.l[i] * signL);
                delayR.push (b.r[i] * signR);
                const float off = offsetS.next();
                const float l = readLinear (delayL, juce::jmax (0.0f, -off));
                const float r = readLinear (delayR, juce::jmax (0.0f, off));
                const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * widthS.next();
                b.l[i] = (mid + side) * panL.next();
                b.r[i] = (mid - side) * panR.next();
            }
            tap.write (b.l, b.r, b.n);
        }

    private:
        static float readLinear (const UDsp::DelayLine& d, float delay) noexcept
        {
            const int i = (int) delay;
            const float t = delay - (float) i;
            return d.at (i) + (d.at (i + 1) - d.at (i)) * t;
        }

        UDsp::DelayLine delayL, delayR;
        Smooth widthS, offsetS, panL, panR;
    };

    //==============================================================================
    // ITU-R BS.1770 loudness meter, pass-through (LUFS + true peak, the metering part of FL's Wave Candy).
    // meters: 0 momentary, 1 short-term, 2 integrated (LUFS), 3 true peak (linear, held).
    class LoudnessPlugin : public Plugin
    {
    public:
        static inline const char* xmlTypeName = "us_loudness";
        static const char* getPluginName() { return "Untitled Loudness Meter"; }
        enum { target };

        LoudnessPlugin (te::PluginCreationInfo info)
            : Plugin (info, xmlTypeName, getPluginName(), { knob ("target", "Target", -30.0f, -6.0f, -14.0f, "LUFS") }) {}

        void requestReset() { resetRequested = true; }

    protected:
        void prepare (double sr) override
        {
            meter.prepare (sr);
            peakHold = 0.0f;
            for (int k = 0; k < 3; ++k) meters[(size_t) k] = -100.0f;
            meters[3] = 0.0f;
        }

        void process (const Block& b) override
        {
            if (resetRequested.exchange (false))
            {
                meter.reset();
                peakHold = 0.0f;
            }
            meter.process (b.l, b.r, b.n);
            for (int i = 0; i < b.n; ++i)
                peakHold = juce::jmax (peakHold, truePeak.process (b.l[i], b.r[i]));
            meters[0] = meter.momentary;
            meters[1] = meter.shortTerm;
            meters[2] = meter.integrated;
            meters[3] = peakHold;
        }

    private:
        UDsp::Loudness meter;
        UDsp::TruePeak truePeak;
        float peakHold = 0.0f;
        std::atomic<bool> resetRequested { false };
    };
}
