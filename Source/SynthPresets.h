#pragma once

#include "FMSynth.h"

// Presets: a preset is a set of parameter values (by parameter ID or name). For synths an ID that
// isn't a parameter is a plugin state property (e.g. the 4-OSC's wave shapes and filter type).
// Synth presets live here (Studio's templates use them); effect presets are in Effects.h.
namespace Presets
{
    using Values = std::vector<std::pair<juce::String, float>>;
    struct Preset { juce::String name; Values values; juce::String category = {}; };

    inline bool isSynthType (const juce::String& type)
    {
        return type == te::FourOscPlugin::xmlTypeName || type == FMSynthPlugin::xmlTypeName;
    }

    inline void applyValues (te::Plugin& p, const Values& values)
    {
        for (auto& [id, value] : values)
        {
            bool found = false;
            for (auto* param : p.getAutomatableParameters())
                if (param->paramID.equalsIgnoreCase (id) || param->getParameterName().equalsIgnoreCase (id))
                {
                    param->setParameter (value, juce::sendNotificationSync);
                    found = true;
                }

            if (! found && isSynthType (p.getPluginType()) && juce::Identifier::isValidIdentifier (id))
                p.state.setProperty (id, juce::exactlyEqual (std::floor (value), value) ? juce::var ((int) value) : juce::var (value), p.getUndoManager());
        }
    }

    // 4-OSC settings that are plugin state, not parameters. Every 4-OSC preset starts from these.
    inline const Values& fourOscStateDefaults()
    {
        static const Values v {
            { "waveShape1", 1 }, { "waveShape2", 0 }, { "waveShape3", 0 }, { "waveShape4", 0 },
            { "voices1", 1 }, { "voices2", 1 }, { "voices3", 1 }, { "voices4", 1 },
            { "filterType", 0 }, { "filterSlope", 12 }, { "voiceMode", 2 }, { "delay", 1 },
            { "distortionOn", 0 }, { "reverbOn", 0 }, { "delayOn", 0 }, { "chorusOn", 0 },
        };
        return v;
    }

    namespace detail
    {
        inline Values withBase (const Values& base, const Values& v)
        {
            auto all = base;
            all.insert (all.end(), v.begin(), v.end());
            return all;
        }

        // 4-OSC wave shapes.
        enum { off = 0, sine, triangle, saw, sawDown, square, noise };
    }

    inline const std::vector<Preset>& synthFactory (const juce::String& type)
    {
        using namespace detail;
        auto osc = [] (const Values& v) { return withBase (fourOscStateDefaults(), v); };

        static const std::vector<Preset> fourOsc {
            { "Sub Bass", osc ({ { "waveShape1", sine }, { "waveShape2", triangle }, { "tune2", 12 }, { "level2", -14 }, { "voiceMode", 0 },
                                 { "ampAttack", 0.005f }, { "ampDecay", 0.3f }, { "ampSustain", 100 }, { "ampRelease", 0.08f }, { "masterLevel", -2 } }), "Bass" },
            { "808 Sub", osc ({ { "waveShape1", sine }, { "voiceMode", 1 }, { "legato", 70 }, { "distortionOn", 1 }, { "distortion", 0.35f },
                                { "ampAttack", 0.002f }, { "ampDecay", 1.4f }, { "ampSustain", 0 }, { "ampRelease", 0.2f }, { "masterLevel", -4 } }), "Bass" },
            { "Acid Bass", osc ({ { "waveShape1", saw }, { "voiceMode", 1 }, { "legato", 45 }, { "filterType", 1 }, { "filterSlope", 24 },
                                  { "filterFreq", 48 }, { "filterResonance", 78 }, { "filterAmount", 0.55f }, { "filterAttack", 0 }, { "filterDecay", 0.18f },
                                  { "filterSustain", 0 }, { "filterRelease", 0.1f }, { "filterVelocity", 50 }, { "distortionOn", 1 }, { "distortion", 0.3f },
                                  { "ampAttack", 0.002f }, { "ampDecay", 0.3f }, { "ampSustain", 80 }, { "ampRelease", 0.05f }, { "masterLevel", -6 } }), "Bass" },
            { "Saw Bass", osc ({ { "waveShape1", saw }, { "voices1", 3 }, { "detune1", 0.12f }, { "waveShape2", square }, { "tune2", -12 }, { "level2", -6 },
                                 { "voiceMode", 0 }, { "filterType", 1 }, { "filterSlope", 24 }, { "filterFreq", 70 }, { "filterResonance", 20 },
                                 { "filterAmount", 0.3f }, { "filterDecay", 0.25f }, { "filterSustain", 30 },
                                 { "ampAttack", 0.003f }, { "ampDecay", 0.2f }, { "ampSustain", 90 }, { "ampRelease", 0.08f }, { "masterLevel", -8 } }), "Bass" },
            { "Supersaw Lead", osc ({ { "waveShape1", saw }, { "voices1", 8 }, { "detune1", 0.3f }, { "spread1", 80 },
                                      { "waveShape2", saw }, { "voices2", 8 }, { "detune2", 0.25f }, { "spread2", -80 }, { "tune2", 12 }, { "level2", -8 },
                                      { "filterType", 1 }, { "filterFreq", 112 }, { "filterResonance", 10 },
                                      { "ampAttack", 0.01f }, { "ampDecay", 0.2f }, { "ampSustain", 90 }, { "ampRelease", 0.3f },
                                      { "reverbOn", 1 }, { "reverbSize", 0.6f }, { "reverbMix", 0.2f }, { "reverbWidth", 1 },
                                      { "delayOn", 1 }, { "delay", 0.75f }, { "delayMix", 0.15f }, { "delayFeedback", -12 }, { "masterLevel", -12 } }), "Lead" },
            { "Square Lead", osc ({ { "waveShape1", square }, { "pulseWidth1", 0.4f }, { "waveShape2", square }, { "fineTune2", 8 }, { "level2", -6 },
                                    { "voiceMode", 1 }, { "legato", 30 }, { "filterType", 1 }, { "filterFreq", 100 },
                                    { "ampAttack", 0.005f }, { "ampDecay", 0.1f }, { "ampSustain", 85 }, { "ampRelease", 0.15f },
                                    { "delayOn", 1 }, { "delay", 0.5f }, { "delayMix", 0.2f }, { "delayFeedback", -10 }, { "masterLevel", -10 } }), "Lead" },
            { "Soft Pad", osc ({ { "waveShape1", triangle }, { "voices1", 4 }, { "detune1", 0.12f }, { "spread1", 70 },
                                 { "waveShape2", saw }, { "voices2", 4 }, { "detune2", 0.2f }, { "spread2", -70 }, { "tune2", 12 }, { "level2", -16 },
                                 { "filterType", 1 }, { "filterFreq", 80 }, { "filterAmount", 0.2f }, { "filterAttack", 1.5f }, { "filterSustain", 60 },
                                 { "ampAttack", 1.2f }, { "ampDecay", 1.0f }, { "ampSustain", 90 }, { "ampRelease", 2.0f },
                                 { "chorusOn", 1 }, { "chorusMix", 0.4f }, { "reverbOn", 1 }, { "reverbSize", 0.85f }, { "reverbMix", 0.4f }, { "reverbWidth", 1 },
                                 { "masterLevel", -10 } }), "Pad" },
            { "String Pad", osc ({ { "waveShape1", saw }, { "voices1", 6 }, { "detune1", 0.18f }, { "spread1", 90 },
                                   { "filterType", 1 }, { "filterFreq", 92 }, { "filterResonance", 5 },
                                   { "ampAttack", 0.6f }, { "ampDecay", 0.5f }, { "ampSustain", 95 }, { "ampRelease", 1.2f },
                                   { "reverbOn", 1 }, { "reverbSize", 0.75f }, { "reverbMix", 0.3f }, { "reverbWidth", 1 }, { "masterLevel", -10 } }), "Pad" },
            { "Pluck", osc ({ { "waveShape1", saw }, { "waveShape2", square }, { "tune2", 12 }, { "level2", -10 },
                              { "filterType", 1 }, { "filterSlope", 24 }, { "filterFreq", 55 }, { "filterResonance", 30 }, { "filterAmount", 0.6f },
                              { "filterAttack", 0 }, { "filterDecay", 0.25f }, { "filterSustain", 0 },
                              { "ampAttack", 0.001f }, { "ampDecay", 0.4f }, { "ampSustain", 0 }, { "ampRelease", 0.3f },
                              { "reverbOn", 1 }, { "reverbSize", 0.5f }, { "reverbMix", 0.2f }, { "delayOn", 1 }, { "delay", 0.75f }, { "delayMix", 0.2f },
                              { "masterLevel", -6 } }), "Pluck" },
            { "Brass", osc ({ { "waveShape1", saw }, { "voices1", 3 }, { "detune1", 0.1f }, { "spread1", 40 },
                              { "filterType", 1 }, { "filterFreq", 62 }, { "filterResonance", 15 }, { "filterAmount", 0.45f },
                              { "filterAttack", 0.08f }, { "filterDecay", 0.5f }, { "filterSustain", 40 }, { "filterVelocity", 60 },
                              { "ampAttack", 0.05f }, { "ampDecay", 0.3f }, { "ampSustain", 85 }, { "ampRelease", 0.2f },
                              { "reverbOn", 1 }, { "reverbSize", 0.4f }, { "reverbMix", 0.15f }, { "masterLevel", -8 } }), "Brass" },
            { "Organ", osc ({ { "waveShape1", sine }, { "waveShape2", sine }, { "tune2", 12 }, { "level2", -6 },
                              { "waveShape3", sine }, { "tune3", 19 }, { "level3", -10 }, { "waveShape4", sine }, { "tune4", 24 }, { "level4", -12 },
                              { "ampAttack", 0.005f }, { "ampDecay", 0.1f }, { "ampSustain", 100 }, { "ampRelease", 0.06f },
                              { "chorusOn", 1 }, { "chorusMix", 0.3f }, { "masterLevel", -9 } }), "Organ" },
            { "Arp Sequence", osc ({ { "waveShape1", square }, { "pulseWidth1", 0.3f }, { "filterType", 1 }, { "filterSlope", 24 },
                                     { "filterFreq", 72 }, { "filterResonance", 40 }, { "filterAmount", 0.4f }, { "filterDecay", 0.15f }, { "filterSustain", 0 },
                                     { "ampAttack", 0.001f }, { "ampDecay", 0.2f }, { "ampSustain", 0 }, { "ampRelease", 0.1f },
                                     { "delayOn", 1 }, { "delay", 0.75f }, { "delayMix", 0.3f }, { "delayFeedback", -8 }, { "masterLevel", -6 } }), "Arp" },
            { "Riser FX", osc ({ { "waveShape1", noise }, { "waveShape2", saw }, { "voices2", 4 }, { "detune2", 0.4f }, { "level2", -10 },
                                 { "filterType", 1 }, { "filterFreq", 30 }, { "filterResonance", 50 }, { "filterAmount", 1 }, { "filterAttack", 6 },
                                 { "filterSustain", 100 }, { "ampAttack", 4 }, { "ampSustain", 100 }, { "ampRelease", 1.5f },
                                 { "reverbOn", 1 }, { "reverbSize", 0.9f }, { "reverbMix", 0.5f }, { "reverbWidth", 1 }, { "masterLevel", -10 } }), "FX" },
        };

        // FM: algorithms 0 stack, 1 two stacks, 2 three-into-one, 3 additive, 4 branch, 5 one-into-three, 6 stack + carrier, 7 4>1 + 2 + 3.
        auto op = [] (int n, float ratio, float level, float a, float d, float s, float r, float fine = 0.0f)
        {
            const auto id = "op" + juce::String (n);
            return Values { { id + "Ratio", ratio }, { id + "Level", level }, { id + "Attack", a }, { id + "Decay", d },
                            { id + "Sustain", s }, { id + "Release", r }, { id + "Fine", fine } };
        };
        auto fm = [] (int alg, std::initializer_list<Values> ops, const Values& extra)
        {
            Values v { { "algorithm", (float) alg } };
            for (auto& o : ops) v.insert (v.end(), o.begin(), o.end());
            v.insert (v.end(), extra.begin(), extra.end());
            return v;
        };

        static const std::vector<Preset> fmSynth {
            { "E-Piano", fm (1, { op (1, 1, 0.8f, 0.002f, 2.5f, 0.15f, 0.5f), op (2, 1, 0.35f, 0.002f, 1.2f, 0.1f, 0.4f),
                                  op (3, 1, 0.45f, 0.001f, 1.0f, 0.0f, 0.4f, 3), op (4, 14, 0.25f, 0.001f, 0.25f, 0.0f, 0.2f) },
                             { { "velocity", 0.9f }, { "cutoff", 12000 } }), "Keys" },
            { "Clav", fm (0, { op (1, 1, 0.9f, 0.001f, 0.8f, 0.3f, 0.05f), op (2, 3, 0.6f, 0.001f, 0.25f, 0.2f, 0.05f),
                               op (3, 1, 0.0f, 0.001f, 1, 0, 0.1f), op (4, 1, 0.0f, 0.001f, 1, 0, 0.1f) },
                          { { "velocity", 0.8f }, { "cutoff", 9000 }, { "resonance", 0.2f } }), "Keys" },
            { "Marimba", fm (1, { op (1, 1, 0.8f, 0.001f, 0.6f, 0.0f, 0.3f), op (2, 4, 0.3f, 0.001f, 0.05f, 0.0f, 0.05f),
                                  op (3, 3, 0.3f, 0.001f, 0.4f, 0.0f, 0.2f), op (4, 10, 0.2f, 0.001f, 0.03f, 0.0f, 0.03f) },
                             { { "velocity", 0.8f } }), "Keys" },
            { "FM Bell", fm (1, { op (1, 1, 0.7f, 0.001f, 5.0f, 0.0f, 3.0f), op (2, 3.5f, 0.5f, 0.001f, 4.0f, 0.0f, 3.0f),
                                  op (3, 2, 0.5f, 0.001f, 3.0f, 0.0f, 2.5f, 7), op (4, 7, 0.3f, 0.001f, 2.0f, 0.0f, 2.0f) },
                             { { "velocity", 0.6f } }), "Bell" },
            { "FM Bass", fm (0, { op (1, 1, 1.0f, 0.001f, 0.6f, 0.7f, 0.08f), op (2, 1, 0.6f, 0.001f, 0.3f, 0.2f, 0.08f),
                                  op (3, 2, 0.2f, 0.001f, 0.2f, 0.0f, 0.08f), op (4, 1, 0.0f, 0.001f, 1, 0, 0.1f) },
                             { { "feedback", 0.3f }, { "cutoff", 3000 }, { "resonance", 0.2f }, { "velocity", 0.5f } }), "Bass" },
            { "Growl Bass", fm (2, { op (1, 0.5f, 1.0f, 0.002f, 0.5f, 0.8f, 0.1f), op (2, 1, 0.5f, 0.002f, 0.4f, 0.4f, 0.1f),
                                     op (3, 2, 0.3f, 0.002f, 0.3f, 0.3f, 0.1f), op (4, 0.5f, 0.4f, 0.002f, 1.0f, 0.6f, 0.1f) },
                                { { "feedback", 0.6f }, { "cutoff", 1800 }, { "resonance", 0.5f } }), "Bass" },
            { "Brass Stab", fm (0, { op (1, 1, 0.9f, 0.04f, 0.4f, 0.9f, 0.15f), op (2, 1, 0.55f, 0.06f, 0.4f, 0.6f, 0.15f),
                                     op (3, 1, 0.0f, 0.001f, 1, 0, 0.1f), op (4, 1, 0.0f, 0.001f, 1, 0, 0.1f) },
                                { { "cutoff", 6000 }, { "velocity", 0.7f } }), "Brass" },
            { "FM Organ", fm (3, { op (1, 0.5f, 0.7f, 0.005f, 1, 1, 0.05f), op (2, 1, 0.8f, 0.005f, 1, 1, 0.05f),
                                   op (3, 2, 0.5f, 0.005f, 1, 1, 0.05f), op (4, 3, 0.4f, 0.005f, 1, 1, 0.05f) },
                              { { "feedback", 0.15f }, { "velocity", 0.2f }, { "volume", -3 } }), "Organ" },
            { "FM Pluck", fm (0, { op (1, 1, 0.9f, 0.001f, 0.5f, 0.0f, 0.3f), op (2, 2, 0.5f, 0.001f, 0.2f, 0.0f, 0.2f),
                                   op (3, 3, 0.2f, 0.001f, 0.1f, 0.0f, 0.1f), op (4, 1, 0.0f, 0.001f, 1, 0, 0.1f) },
                              { { "cutoff", 8000 }, { "velocity", 0.8f } }), "Pluck" },
            { "Glass Pad", fm (1, { op (1, 1, 0.7f, 1.5f, 2.0f, 0.8f, 2.5f), op (2, 2, 0.25f, 2.0f, 2.0f, 0.7f, 2.0f),
                                    op (3, 1, 0.7f, 1.2f, 2.0f, 0.8f, 2.5f, 8), op (4, 3, 0.2f, 2.5f, 2.0f, 0.6f, 2.0f) },
                               { { "velocity", 0.3f }, { "cutoff", 7000 } }), "Pad" },
            { "Digital Lead", fm (4, { op (1, 1, 0.9f, 0.005f, 0.3f, 0.9f, 0.2f), op (2, 2, 0.3f, 0.005f, 0.3f, 0.8f, 0.2f),
                                       op (3, 1, 0.35f, 0.005f, 0.5f, 0.7f, 0.2f), op (4, 1, 0.3f, 0.005f, 0.5f, 0.7f, 0.2f) },
                                  { { "feedback", 0.5f }, { "cutoff", 9000 } }), "Lead" },
            { "Arp Blip", fm (0, { op (1, 1, 0.9f, 0.001f, 0.15f, 0.0f, 0.1f), op (2, 3, 0.35f, 0.001f, 0.08f, 0.0f, 0.05f),
                                   op (3, 1, 0.0f, 0.001f, 1, 0, 0.1f), op (4, 1, 0.0f, 0.001f, 1, 0, 0.1f) },
                              { { "velocity", 0.6f } }), "Arp" },
            { "Metal Hit", fm (2, { op (1, 1, 0.9f, 0.001f, 1.5f, 0.0f, 1.0f), op (2, 1.5f, 0.6f, 0.001f, 0.8f, 0.0f, 0.5f),
                                    op (3, 3.5f, 0.5f, 0.001f, 0.6f, 0.0f, 0.5f), op (4, 7.5f, 0.5f, 0.001f, 1.0f, 0.0f, 0.8f) },
                               { { "feedback", 0.7f }, { "velocity", 0.5f } }), "FX" },
        };

        static const std::vector<Preset> none;
        if (type == te::FourOscPlugin::xmlTypeName) return fourOsc;
        if (type == FMSynthPlugin::xmlTypeName)     return fmSynth;
        return none;
    }

    // Synth presets replace the whole sound: everything else goes back to its default first.
    inline void apply (te::Plugin& p, const Preset& preset)
    {
        if (isSynthType (p.getPluginType()))
            for (auto* param : p.getAutomatableParameters())
                if (auto def = param->getDefaultValue())
                    param->setParameter (*def, juce::sendNotificationSync);
        applyValues (p, preset.values);
    }

    // Every synth preset, for the "Synth presets" menu: the sound, not the engine.
    struct SynthSound { juce::String pluginType; const Preset* preset; };

    inline std::vector<SynthSound> allSynthSounds()
    {
        std::vector<SynthSound> list;
        for (auto type : { te::FourOscPlugin::xmlTypeName, FMSynthPlugin::xmlTypeName })
            for (auto& p : synthFactory (type))
                list.push_back ({ type, &p });
        return list;
    }

    inline std::optional<SynthSound> findSynthSound (const juce::String& name)
    {
        for (auto& s : allSynthSounds())
            if (s.preset->name == name)
                return s;
        return std::nullopt;
    }

    inline juce::StringArray synthCategories() { return { "Bass", "Lead", "Pad", "Keys", "Pluck", "Bell", "Brass", "Organ", "Arp", "FX" }; }
}
