#pragma once

#include "DrumKit.h"
#include <regex>

#if JUCE_CLANG
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#endif
#define TSF_IMPLEMENTATION
#include "../libs/TinySoundFont/tsf.h"
#if JUCE_CLANG
 #pragma clang diagnostic pop
#endif

// Our multi-sample instrument: key/velocity zones, round robins, loops, ADSR.
// Loads SFZ files, SoundFonts (.sf2, parsed by TinySoundFont into regions), single WAVs,
// drum pads and slicer chops. Everything ends up as a list of regions played by one engine.
namespace MultiSampler
{
    inline juce::File getInstrumentsFolder() { return DrumKit::getPacksFolder().getChildFile ("Instruments"); }

    inline bool isInstrumentFile (const juce::File& f) { return f.hasFileExtension ("sfz;sf2"); }

    inline const juce::Identifier REGION ("REGION"), source ("source"), program ("program");

    enum class LoopMode { none, oneShot, continuous, sustain };

    struct Region
    {
        std::shared_ptr<const juce::AudioBuffer<float>> data;
        double sampleRate = 44100.0;
        int lokey = 0, hikey = 127, lovel = 1, hivel = 127, root = 60;
        int seqLength = 1, seqPosition = 1, group = 0, offBy = 0;
        float tune = 0.0f, transpose = 0.0f, keytrack = 100.0f, volume = 0.0f, pan = 0.0f; // cents, semis, %, dB, -1..1
        juce::int64 offset = 0, end = 0, loopStart = 0, loopEnd = 0;  // sample indices, ends exclusive
        LoopMode loopMode = LoopMode::none;
        float attack = 0.0f, decay = 0.0f, sustain = 1.0f, release = 0.001f;
    };

    // Immutable once handed to the audio thread (except the round-robin counters it owns).
    struct SoundSet
    {
        std::vector<Region> regions;
        std::vector<int> seqCounters;
    };

    // Decoded files, shared between rebuilds so editing a region doesn't re-read the disk.
    struct SampleCache
    {
        explicit SampleCache (juce::AudioFormatManager& f) : formats (f) {}

        struct Entry { std::shared_ptr<const juce::AudioBuffer<float>> data; double sampleRate = 44100.0; };

        Entry get (const juce::File& file)
        {
            const auto key = file.getFullPathName();
            if (auto it = entries.find (key); it != entries.end())
                return it->second;

            Entry e;
            if (auto reader = std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file)))
            {
                auto buffer = std::make_shared<juce::AudioBuffer<float>> ((int) juce::jmin (2u, reader->numChannels), (int) reader->lengthInSamples);
                reader->read (buffer.get(), 0, buffer->getNumSamples(), 0, true, buffer->getNumChannels() > 1);
                e = { buffer, reader->sampleRate };
            }
            return entries[key] = e;
        }

        juce::AudioFormatManager& formats;
        std::map<juce::String, Entry> entries;
    };

    //==========================================================================
    // SFZ

    // Note names use the SFZ convention: c4 = 60.
    inline int parseKey (const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        if (t.isEmpty())
            return -1;
        if (juce::CharacterFunctions::isDigit (t[0]) || t[0] == '-')
            return t.getIntValue();
        if (t[0] < 'a' || t[0] > 'g')
            return -1;

        static const int classes[] = { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g
        int pc = classes[t[0] - 'a'], i = 1;
        if (t[i] == '#')      { ++pc; ++i; }
        else if (t[i] == 'b') { --pc; ++i; }
        return juce::jlimit (0, 127, (t.substring (i).getIntValue() + 1) * 12 + pc);
    }

    // Returns one REGION tree per playable region, with every opcode inherited from
    // <global>/<master>/<group> and `sample` resolved to an absolute path.
    inline juce::Array<juce::ValueTree> parseSfz (const juce::File& file)
    {
        juce::Array<juce::ValueTree> regions;
        auto text = file.loadFileAsString();
        for (int a; (a = text.indexOf ("/*")) >= 0;)
        {
            const int b = text.indexOf (a + 2, "*/");
            text = text.substring (0, a) + (b < 0 ? juce::String() : text.substring (b + 2));
        }

        enum Level { none, control, global, master, group, region };
        Level level = none;
        juce::NamedValueSet levels[6];
        juce::String defaultPath;
        juce::StringPairArray defines;

        auto flush = [&]
        {
            if (level != region)
                return;
            juce::ValueTree r (REGION);
            for (auto l : { global, master, group, region })
                for (auto& p : levels[l])
                    r.setProperty (p.name, p.value, nullptr);

            const auto sample = r["sample"].toString();
            if (sample.isEmpty() || sample.startsWith ("*"))
                return; // built-in generators aren't supported
            r.setProperty ("sample", file.getParentDirectory().getChildFile ((defaultPath + sample).replace ("\\", "/")).getFullPathName(), nullptr);
            regions.add (r);
        };

        auto setOpcode = [&] (juce::String name, const juce::String& value)
        {
            if (level == control)
            {
                if (name == "default_path")
                    defaultPath = value;
                return;
            }
            if (level == none)
                return;

            if (name == "loopmode") name = "loop_mode";
            if (name == "loopstart") name = "loop_start";
            if (name == "loopend") name = "loop_end";

            auto& set = levels[level];
            if (name == "key")
            {
                for (auto id : { "lokey", "hikey", "pitch_keycenter" })
                    set.set (id, parseKey (value));
            }
            else if (name == "lokey" || name == "hikey" || name == "pitch_keycenter")
            {
                set.set (juce::Identifier (name), parseKey (value));
            }
            else
            {
                set.set (juce::Identifier (name), value);
            }
        };

        static const std::regex token (R"(<(\w+)>|(?:^|\s)(\w+)=)");
        for (auto line : juce::StringArray::fromLines (text))
        {
            line = line.upToFirstOccurrenceOf ("//", false, false);
            if (line.trimStart().startsWith ("#define"))
            {
                auto rest = line.trimStart().substring (7).trim();
                defines.set (rest.upToFirstOccurrenceOf (" ", false, false), rest.fromFirstOccurrenceOf (" ", false, false).trim());
                continue;
            }
            if (line.trimStart().startsWith ("#"))
                continue;
            for (auto& key : defines.getAllKeys())
                line = line.replace (key, defines[key]);

            const auto s = line.toStdString();
            std::vector<std::smatch> matches;
            for (auto it = std::sregex_iterator (s.begin(), s.end(), token); it != std::sregex_iterator(); ++it)
                matches.push_back (*it);

            for (size_t i = 0; i < matches.size(); ++i)
            {
                const auto& m = matches[i];
                if (m[1].matched)
                {
                    flush();
                    const auto header = juce::String (m[1].str());
                    level = header == "region"  ? region
                          : header == "group"   ? group
                          : header == "master"  ? master
                          : header == "global"  ? global
                          : header == "control" ? control : none;
                    if (level == region)  levels[region].clear();
                    if (level == group)   levels[group].clear();
                    if (level == master)  { levels[master].clear(); levels[group].clear(); }
                    if (level == global)  { levels[global].clear(); levels[master].clear(); levels[group].clear(); }
                    continue;
                }
                const auto valueStart = (size_t) (m.position (0) + m.length (0));
                const auto valueEnd = i + 1 < matches.size() ? (size_t) matches[i + 1].position (0) : s.size();
                setOpcode (m[2].str(), juce::String (s.substr (valueStart, valueEnd - valueStart)).trim());
            }
        }
        flush();
        return regions;
    }

    // Turns a REGION tree (SFZ opcodes as properties) into a playable region.
    inline std::optional<Region> loadRegion (const juce::ValueTree& t, SampleCache& cache)
    {
        auto entry = cache.get (juce::File (t["sample"].toString()));
        if (entry.data == nullptr || entry.data->getNumSamples() == 0)
            return std::nullopt;

        auto num = [&t] (const char* id, double def) { const auto& v = t[id]; return v.isVoid() || v.toString().isEmpty() ? def : (double) v; };
        const auto length = (juce::int64) entry.data->getNumSamples();

        Region r;
        r.data = entry.data;
        r.sampleRate = entry.sampleRate;
        r.lokey = (int) num ("lokey", 0);
        r.hikey = (int) num ("hikey", 127);
        r.lovel = (int) num ("lovel", 1);
        r.hivel = (int) num ("hivel", 127);
        r.root = (int) num ("pitch_keycenter", 60);
        r.seqLength = juce::jmax (1, (int) num ("seq_length", 1));
        r.seqPosition = (int) num ("seq_position", 1);
        r.group = (int) num ("group", 0);
        r.offBy = (int) num ("off_by", 0);
        r.tune = (float) num ("tune", 0);
        r.transpose = (float) num ("transpose", 0);
        r.keytrack = (float) num ("pitch_keytrack", 100);
        r.volume = (float) num ("volume", 0);
        r.pan = (float) juce::jlimit (-1.0, 1.0, num ("pan", 0) / 100.0);
        r.offset = juce::jlimit<juce::int64> (0, length - 1, (juce::int64) num ("offset", 0));
        r.end = juce::jlimit<juce::int64> (r.offset + 1, length, (juce::int64) num ("end", (double) length - 1) + 1);
        r.loopStart = juce::jlimit<juce::int64> (0, length - 1, (juce::int64) num ("loop_start", (double) r.offset));
        r.loopEnd = juce::jlimit<juce::int64> (0, length, (juce::int64) num ("loop_end", (double) r.end - 1) + 1);
        r.attack = (float) num ("ampeg_attack", 0);
        r.decay = (float) num ("ampeg_decay", 0);
        r.sustain = (float) juce::jlimit (0.0, 1.0, num ("ampeg_sustain", 100) / 100.0);
        r.release = (float) num ("ampeg_release", 0.001);

        const auto mode = t["loop_mode"].toString();
        r.loopMode = mode == "one_shot" ? LoopMode::oneShot
                   : mode == "loop_continuous" ? LoopMode::continuous
                   : mode == "loop_sustain" ? LoopMode::sustain : LoopMode::none;
        if ((r.loopMode == LoopMode::continuous || r.loopMode == LoopMode::sustain) && r.loopEnd <= r.loopStart)
            r.loopMode = LoopMode::none;
        return r;
    }

    //==========================================================================
    // SoundFont: TinySoundFont parses the file; its regions map 1:1 onto ours.

    struct Sf2Result
    {
        std::vector<Region> regions;
        juce::StringArray presetNames;
    };

    inline Sf2Result loadSf2 (const juce::File& file, int presetIndex)
    {
        Sf2Result result;
        auto* raw = tsf_load_filename (file.getFullPathName().toRawUTF8());
        if (raw == nullptr)
            return result;

        std::shared_ptr<tsf> font (raw, tsf_close);
        unsigned int numSamples = 0;
        for (int p = 0; p < font->presetNum; ++p)
        {
            const auto& preset = font->presets[p];
            result.presetNames.add (juce::String (preset.bank).paddedLeft ('0', 3) + ":" + juce::String (preset.preset).paddedLeft ('0', 3)
                                    + "  " + juce::String (preset.presetName));
            for (int i = 0; i < preset.regionNum; ++i)
                numSamples = juce::jmax (numSamples, preset.regions[i].end, preset.regions[i].loop_end + 1);
        }

        if (font->presetNum == 0 || numSamples == 0)
            return result;

        // One mono buffer that refers to TinySoundFont's decoded samples (kept alive with it).
        float* channels[] = { font->fontSamples };
        std::shared_ptr<const juce::AudioBuffer<float>> data (new juce::AudioBuffer<float> (channels, 1, (int) numSamples),
                                                              [font] (const juce::AudioBuffer<float>* b) { delete b; });

        const auto& preset = font->presets[juce::jlimit (0, font->presetNum - 1, presetIndex)];
        for (int i = 0; i < preset.regionNum; ++i)
        {
            const auto& s = preset.regions[i];
            Region r;
            r.data = data;
            r.sampleRate = s.sample_rate;
            r.lokey = s.lokey; r.hikey = s.hikey; r.lovel = s.lovel; r.hivel = s.hivel;
            r.root = s.pitch_keycenter;
            r.keytrack = (float) s.pitch_keytrack;
            r.transpose = (float) s.transpose;
            r.tune = (float) s.tune;
            r.volume = -s.attenuation;
            r.pan = juce::jlimit (-1.0f, 1.0f, s.pan * 2.0f);
            r.group = r.offBy = (int) s.group; // SF2 exclusive class: same class chokes itself
            r.offset = s.offset;
            r.end = juce::jmax<juce::int64> (s.offset + 1, s.end);
            r.loopStart = s.loop_start;
            r.loopEnd = (juce::int64) s.loop_end + 1;
            r.loopMode = s.loop_mode == TSF_LOOPMODE_CONTINUOUS ? LoopMode::continuous
                       : s.loop_mode == TSF_LOOPMODE_SUSTAIN ? LoopMode::sustain : LoopMode::none;
            if (r.loopEnd <= r.loopStart)
                r.loopMode = LoopMode::none;
            r.attack = s.ampenv.attack;
            r.decay = s.ampenv.decay;
            r.sustain = juce::jlimit (0.0f, 1.0f, s.ampenv.sustain);
            r.release = juce::jmax (0.001f, s.ampenv.release);
            result.regions.push_back (r);
        }
        return result;
    }

    //==========================================================================
    // Drum pad (16 pads on notes 36..51) and slicer.

    static constexpr int firstPadNote = 36, numPads = 16;

    inline juce::ValueTree makeRegion (const juce::File& sample, int lokey, int hikey, int root)
    {
        juce::ValueTree r (REGION);
        r.setProperty ("sample", sample.getFullPathName(), nullptr);
        r.setProperty ("lokey", lokey, nullptr);
        r.setProperty ("hikey", hikey, nullptr);
        r.setProperty ("pitch_keycenter", root, nullptr);
        return r;
    }

    inline juce::Array<juce::ValueTree> defaultDrumPads()
    {
        // name, file, tune (cents), group, off_by. Closed hat (group 1) chokes the open hat.
        struct Pad { const char* name; const char* file; int tune, group, offBy; };
        static const Pad pads[numPads] = {
            { "Kick", "Kick", 0, 0, 0 },       { "Snare", "Snare", 0, 0, 0 },    { "Clap", "Clap", 0, 0, 0 },      { "Rim", "Rim", 0, 0, 0 },
            { "Hat", "Hat", 0, 1, 0 },         { "Open Hat", "Open Hat", 0, 2, 1 }, { "Hat Hi", "Hat", 400, 1, 0 }, { "808", "808", 0, 0, 0 },
            { "Kick Lo", "Kick", -300, 0, 0 }, { "Snare Hi", "Snare", 300, 0, 0 }, { "Clap Lo", "Clap", -200, 0, 0 }, { "Rim Lo", "Rim", -500, 0, 0 },
            { "Tom Lo", "Kick", 700, 0, 0 },   { "Tom Hi", "Kick", 1200, 0, 0 },  { "808 Lo", "808", -500, 0, 0 }, { "808 Hi", "808", 500, 0, 0 },
        };

        juce::Array<juce::ValueTree> regions;
        for (int i = 0; i < numPads; ++i)
        {
            const int note = firstPadNote + i;
            auto r = makeRegion (DrumKit::getDrumsFolder().getChildFile (juce::String (pads[i].file) + ".wav"), note, note, note);
            r.setProperty ("name", pads[i].name, nullptr);
            r.setProperty ("loop_mode", "one_shot", nullptr);
            if (pads[i].tune != 0)  r.setProperty ("tune", pads[i].tune, nullptr);
            if (pads[i].group != 0) r.setProperty ("group", pads[i].group, nullptr);
            if (pads[i].offBy != 0) r.setProperty ("off_by", pads[i].offBy, nullptr);
            regions.add (r);
        }
        return regions;
    }

    // Onset detection on energy flux; falls back to 8 equal slices. Returns slice starts (first is 0).
    inline juce::Array<juce::int64> detectSlices (const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        const int hop = 512, total = audio.getNumSamples();
        const int frames = total / hop;
        std::vector<float> energy ((size_t) juce::jmax (0, frames));
        for (int f = 0; f < frames; ++f)
        {
            double sum = 0.0;
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                for (int i = 0; i < hop; ++i)
                    sum += juce::square (audio.getSample (ch, f * hop + i));
            energy[(size_t) f] = (float) std::sqrt (sum / (hop * juce::jmax (1, audio.getNumChannels())));
        }

        juce::Array<juce::int64> starts { 0 };
        if (frames > 2)
        {
            std::vector<float> flux ((size_t) frames, 0.0f);
            for (size_t f = 1; f < (size_t) frames; ++f)
                flux[f] = juce::jmax (0.0f, energy[f] - energy[f - 1]);

            double mean = 0.0, var = 0.0;
            for (auto x : flux) mean += x;
            mean /= frames;
            for (auto x : flux) var += juce::square (x - mean);
            const float threshold = (float) (mean + 1.5 * std::sqrt (var / frames));
            const float peak = *std::max_element (energy.begin(), energy.end());
            const auto minGap = (juce::int64) (0.06 * sampleRate);

            for (size_t f = 1; f + 1 < (size_t) frames; ++f)
            {
                if (flux[f] <= threshold || flux[f] < flux[f - 1] || flux[f] < flux[f + 1] || energy[f] < peak * 0.05f)
                    continue;
                const auto pos = (juce::int64) (f - 1) * hop;
                if (pos - starts.getLast() >= minGap)
                    starts.add (pos);
            }
        }

        if (starts.size() < 2)
        {
            starts.clear();
            for (int i = 0; i < 8; ++i)
                starts.add ((juce::int64) total * i / 8);
        }
        return starts;
    }

    // Engine-side regions for slices: one per key from C5 (60), one-shot, no repitch.
    inline juce::Array<juce::ValueTree> sliceRegions (const juce::File& file, const juce::Array<juce::int64>& starts, juce::int64 length)
    {
        juce::Array<juce::ValueTree> regions;
        for (int i = 0; i < juce::jmin (starts.size(), 64); ++i)
        {
            const int key = 60 + i;
            auto r = makeRegion (file, key, key, key);
            r.setProperty ("name", "Slice " + juce::String (i + 1), nullptr);
            r.setProperty ("offset", starts[i], nullptr);
            r.setProperty ("end", (i + 1 < starts.size() ? starts[i + 1] : length) - 1, nullptr);
            r.setProperty ("loop_mode", "one_shot", nullptr);
            r.setProperty ("ampeg_release", 0.005, nullptr);
            regions.add (r);
        }
        return regions;
    }

    //==========================================================================
    struct Params { float gain = 1.0f, attack = 0.0f, decay = 0.0f, sustain = 1.0f, release = 0.0f, tune = 0.0f; };

    // The voice engine. Audio thread only (or a test driving it directly): no allocation.
    class Engine
    {
    public:
        static constexpr int maxVoices = 32;

        void prepare (double rate)               { sampleRate = rate; reset(); }
        void setSoundSet (SoundSet* s)           { if (s != set) { set = s; reset(); } }
        SoundSet* getSoundSet() const noexcept   { return set; }

        void reset()
        {
            for (auto& v : voices)
                v.region = nullptr;
            pedal = false;
        }

        int getNumActiveVoices() const
        {
            return (int) std::count_if (voices.begin(), voices.end(), [] (auto& v) { return v.region != nullptr; });
        }

        void handleMidi (const juce::MidiMessage& m, const Params& p)
        {
            if (m.isNoteOn())
                noteOn (m.getNoteNumber(), m.getVelocity(), p);
            else if (m.isNoteOff())
                noteOff (m.getNoteNumber());
            else if (m.isSustainPedalOn())
                pedal = true;
            else if (m.isSustainPedalOff())
            {
                pedal = false;
                for (auto& v : voices)
                    if (v.region != nullptr && v.sustained)
                        v.stage = Voice::release;
            }
            else if (m.isPitchWheel())
                bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f;
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                reset();
        }

        void render (juce::AudioBuffer<float>& out, int start, int num, const Params& p)
        {
            if (num <= 0 || out.getNumChannels() == 0)
                return;

            const double pitch = std::pow (2.0, (p.tune + bend) / 12.0);
            float* left = out.getWritePointer (0, start);
            float* right = out.getNumChannels() > 1 ? out.getWritePointer (1, start) : nullptr;

            for (auto& v : voices)
            {
                if (v.region == nullptr)
                    continue;

                const auto& r = *v.region;
                const auto& d = *r.data;
                const float* srcL = d.getReadPointer (0);
                const float* srcR = d.getReadPointer (juce::jmin (1, d.getNumChannels() - 1));
                const auto last = (juce::int64) d.getNumSamples() - 1;
                const double inc = v.baseIncrement * pitch;
                const float gainL = v.gainL * p.gain, gainR = v.gainR * p.gain;

                for (int i = 0; i < num; ++i)
                {
                    const bool looping = r.loopMode == LoopMode::continuous || (r.loopMode == LoopMode::sustain && v.stage != Voice::release);
                    auto index = (juce::int64) v.position;
                    const float frac = (float) (v.position - (double) index);
                    auto next = index + 1;
                    if (looping && next >= r.loopEnd)
                        next = r.loopStart;
                    index = juce::jmin (index, last);
                    next = juce::jmin (next, last);

                    const float env = v.nextEnvelope();
                    const float l = srcL[index] + frac * (srcL[next] - srcL[index]);
                    const float rr = srcR[index] + frac * (srcR[next] - srcR[index]);
                    if (right != nullptr)
                    {
                        left[i] += l * env * gainL;
                        right[i] += rr * env * gainR;
                    }
                    else
                    {
                        left[i] += (l * gainL + rr * gainR) * 0.5f * env;
                    }

                    v.position += inc;
                    if (looping && v.position >= (double) r.loopEnd)
                        v.position -= (double) (r.loopEnd - r.loopStart);

                    if (v.stage == Voice::done || (! looping && v.position >= (double) r.end))
                    {
                        v.region = nullptr;
                        break;
                    }
                }
            }
        }

    private:
        struct Voice
        {
            enum Stage { attack, decay, sustain, release, done };

            const Region* region = nullptr;
            int note = 0;
            juce::uint32 age = 0;
            bool held = false, sustained = false;
            double position = 0.0, baseIncrement = 1.0;
            float gainL = 1.0f, gainR = 1.0f;
            Stage stage = attack;
            float env = 0.0f, attackStep = 1.0f, decayCoef = 0.0f, sustainLevel = 1.0f, releaseCoef = 0.0f;

            float nextEnvelope()
            {
                switch (stage)
                {
                    case attack:  if ((env += attackStep) >= 1.0f) { env = 1.0f; stage = decay; } break;
                    case decay:   env = sustainLevel + (env - sustainLevel) * decayCoef;
                                  if (env - sustainLevel < 1.0e-4f) { env = sustainLevel; stage = sustainLevel > 0.0f ? sustain : done; }
                                  break;
                    case release: env *= releaseCoef; if (env < 1.0e-4f) stage = done; break;
                    case sustain:
                    case done:    break;
                }
                return env;
            }
        };

        // Reaches -60 dB after `seconds`.
        float coefficient (float seconds) const
        {
            return seconds <= 0.0f ? 0.0f : (float) std::exp (-6.9 / (seconds * sampleRate));
        }

        void noteOn (int note, int velocity, const Params& p)
        {
            if (set == nullptr)
                return;

            for (size_t i = 0; i < set->regions.size(); ++i)
            {
                const auto& r = set->regions[i];
                if (note < r.lokey || note > r.hikey || velocity < r.lovel || velocity > r.hivel)
                    continue;
                if (r.seqLength > 1 && (set->seqCounters[i]++ % r.seqLength) + 1 != r.seqPosition)
                    continue;

                if (r.group != 0)
                    for (auto& v : voices)
                        if (v.region != nullptr && v.region->offBy == r.group)
                        {
                            v.stage = Voice::release;
                            v.releaseCoef = coefficient (0.005f);
                        }

                auto& v = findVoice();
                v.region = &r;
                v.note = note;
                v.age = ++ageCounter;
                v.held = true;
                v.sustained = false;
                v.position = (double) r.offset;
                const double semis = (double) (note - r.root) * r.keytrack / 100.0 + r.transpose + r.tune / 100.0;
                v.baseIncrement = std::pow (2.0, semis / 12.0) * r.sampleRate / sampleRate;

                const float gain = juce::Decibels::decibelsToGain (r.volume) * juce::square ((float) velocity / 127.0f);
                v.gainL = gain * juce::jmin (1.0f, 1.0f - r.pan);
                v.gainR = gain * juce::jmin (1.0f, 1.0f + r.pan);

                const float attackTime = r.attack + p.attack;
                v.stage = attackTime > 0.0f ? Voice::attack : Voice::decay;
                v.env = attackTime > 0.0f ? 0.0f : 1.0f;
                v.attackStep = attackTime > 0.0f ? (float) (1.0 / (attackTime * sampleRate)) : 1.0f;
                v.sustainLevel = juce::jlimit (0.0f, 1.0f, r.sustain * p.sustain);
                v.decayCoef = coefficient (r.decay + p.decay);
                v.releaseCoef = coefficient (juce::jmax (0.001f, r.release + p.release));
            }
        }

        void noteOff (int note)
        {
            for (auto& v : voices)
            {
                if (v.region == nullptr || v.note != note || ! v.held)
                    continue;
                v.held = false;
                if (v.region->loopMode == LoopMode::oneShot)
                    continue;
                if (pedal) v.sustained = true;
                else       v.stage = Voice::release;
            }
        }

        // Free voice, else the quietest releasing one, else the oldest.
        Voice& findVoice()
        {
            Voice* best = nullptr;
            for (auto& v : voices)
                if (v.region == nullptr)
                    return v;
            for (auto& v : voices)
                if (v.stage == Voice::release && (best == nullptr || v.env < best->env))
                    best = &v;
            if (best == nullptr)
                for (auto& v : voices)
                    if (best == nullptr || v.age < best->age)
                        best = &v;
            return *best;
        }

        std::array<Voice, maxVoices> voices;
        SoundSet* set = nullptr;
        double sampleRate = 44100.0;
        float bend = 0.0f;
        bool pedal = false;
        juce::uint32 ageCounter = 0;
    };
}

//==============================================================================
// State: `source` (.sfz / .sf2 / .wav path), `program` (SF2 preset index), REGION children
// (used when the source isn't an SFZ/SF2: single WAV, drum pad, slicer), plus the params.
class MultiSamplerPlugin : public te::Plugin, private juce::AsyncUpdater
{
public:
    MultiSamplerPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        auto um = getUndoManager();
        volumeValue.referTo (state, "msVolume", um, 0.0f);
        attackValue.referTo (state, "msAttack", um, 0.0f);
        decayValue.referTo (state, "msDecay", um, 0.0f);
        sustainValue.referTo (state, "msSustain", um, 1.0f);
        releaseValue.referTo (state, "msRelease", um, 0.05f);
        tuneValue.referTo (state, "msTune", um, 0.0f);

        volume  = addParam ("volume",  "Volume",  { -48.0f, 12.0f, 0.0f, 2.0f });
        attack  = addParam ("attack",  "Attack",  { 0.0f, 10.0f, 0.0f, 0.3f });
        decay   = addParam ("decay",   "Decay",   { 0.0f, 10.0f, 0.0f, 0.3f });
        sustain = addParam ("sustain", "Sustain", { 0.0f, 1.0f });
        release = addParam ("release", "Release", { 0.0f, 10.0f, 0.0f, 0.3f });
        tune    = addParam ("tune",    "Tune",    { -24.0f, 24.0f });

        volume->attachToCurrentValue (volumeValue);
        attack->attachToCurrentValue (attackValue);
        decay->attachToCurrentValue (decayValue);
        sustain->attachToCurrentValue (sustainValue);
        release->attachToCurrentValue (releaseValue);
        tune->attachToCurrentValue (tuneValue);

        rebuild(); // synchronous: a loaded project / render is ready to play immediately
    }

    ~MultiSamplerPlugin() override
    {
        notifyListenersOfDeletion();
        cancelPendingUpdate();
        for (auto p : getAutomatableParameters())
            p->detachFromCurrentValue();
    }

    static const char* getPluginName()                  { return "Instrument"; }
    static inline const char* xmlTypeName = "us_multisampler";

    juce::String getName() const override               { return "Instrument"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Inst"; }
    juce::String getSelectableDescription() override    { return "Instrument"; }
    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool needsConstantBufferSize() override             { return false; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }

    void initialise (const te::PluginInitialisationInfo& info) override { voiceEngine.prepare (info.sampleRate); }
    void deinitialise() override {}

    void applyToBuffer (const te::PluginRenderContext& rc) override
    {
        if (rc.destBuffer == nullptr || rc.bufferNumSamples <= 0)
            return;

        rc.destBuffer->clear (rc.bufferStartSample, rc.bufferNumSamples);
        if (auto next = pending.exchange (nullptr))
            voiceEngine.setSoundSet (next);
        inUse.store (voiceEngine.getSoundSet());

        MultiSampler::Params p;
        p.gain = juce::Decibels::decibelsToGain (volume->getCurrentValue(), -47.9f);
        p.attack = attack->getCurrentValue();
        p.decay = decay->getCurrentValue();
        p.sustain = sustain->getCurrentValue();
        p.release = release->getCurrentValue();
        p.tune = tune->getCurrentValue();

        int done = 0;
        if (auto midi = rc.bufferForMidiMessages)
        {
            if (midi->isAllNotesOff)
                voiceEngine.reset();

            for (auto& m : *midi)
            {
                const int at = juce::jlimit (0, rc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * sampleRate));
                voiceEngine.render (*rc.destBuffer, rc.bufferStartSample + done, at - done, p);
                done = juce::jmax (done, at);
                voiceEngine.handleMidi (m, p);
            }
        }
        voiceEngine.render (*rc.destBuffer, rc.bufferStartSample + done, rc.bufferNumSamples - done, p);
        voicesPlaying.store (voiceEngine.getNumActiveVoices(), std::memory_order_relaxed);
    }

    std::atomic<int> voicesPlaying { 0 }; // for the status bar

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        te::copyValueTree (state, v, getUndoManager());
        for (auto p : getAutomatableParameters())
            p->updateFromAttachedValue();
        triggerAsyncUpdate();
    }

    //==========================================================================
    // Message thread API

    te::AutomatableParameter::Ptr volume, attack, decay, sustain, release, tune;

    struct Info
    {
        juce::String name, error;
        int numRegions = 0;
        juce::StringArray presetNames;  // SF2 only
        juce::int64 firstRegionLength = 0;
    };

    const Info& getInfo() const     { return instrumentInfo; }
    juce::File getSource() const    { return juce::File (state[MultiSampler::source].toString()); }
    int getProgram() const          { return state[MultiSampler::program]; }

    void setProgram (int index)     { state.setProperty (MultiSampler::program, index, getUndoManager()); }

    // A loaded SFZ/SF2 is described by its file; anything else keeps its regions in the state.
    bool usesRegionState() const    { return ! MultiSampler::isInstrumentFile (getSource()); }

    juce::Array<juce::ValueTree> getRegionStates() const
    {
        juce::Array<juce::ValueTree> list;
        for (auto c : state)
            if (c.hasType (MultiSampler::REGION))
                list.add (c);
        return list;
    }

    // .sfz / .sf2 load as-is; an audio file becomes one region over the whole keyboard (root C5).
    void loadFile (const juce::File& file)
    {
        juce::Array<juce::ValueTree> regions;
        if (! MultiSampler::isInstrumentFile (file))
            regions.add (MultiSampler::makeRegion (file, 0, 127, 60));
        setRegions (regions, file);
    }

    void setRegions (const juce::Array<juce::ValueTree>& regions, const juce::File& sourceFile = {})
    {
        for (auto r : getRegionStates())
            state.removeChild (r, nullptr);
        for (auto& r : regions)
            state.appendChild (r.createCopy(), nullptr);
        state.setProperty (MultiSampler::source, sourceFile.getFullPathName(), nullptr);
        state.setProperty (MultiSampler::program, 0, nullptr);
        flushPendingUpdates();
    }

    void flushPendingUpdates()
    {
        if (isUpdatePending())
        {
            cancelPendingUpdate();
            rebuild();
        }
    }

protected:
    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
    {
        te::Plugin::valueTreePropertyChanged (v, id);
        if (v.hasType (MultiSampler::REGION) || (v == state && (id == MultiSampler::source || id == MultiSampler::program)))
            triggerAsyncUpdate();
    }

    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        te::Plugin::valueTreeChildAdded (parent, child);
        if (child.hasType (MultiSampler::REGION))
            triggerAsyncUpdate();
    }

    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int index) override
    {
        te::Plugin::valueTreeChildRemoved (parent, child, index);
        if (child.hasType (MultiSampler::REGION))
            triggerAsyncUpdate();
    }

private:
    void handleAsyncUpdate() override { rebuild(); }

    // ponytail: decodes on the message thread (UI stalls briefly on huge SF2/SFZ libraries);
    // move to a background thread + the same pending-pointer handover if that gets annoying.
    void rebuild()
    {
        auto set = std::make_unique<MultiSampler::SoundSet>();
        Info newInfo;
        const auto file = getSource();
        newInfo.name = file.getFileNameWithoutExtension();

        if (file.hasFileExtension ("sf2"))
        {
            auto sf2 = MultiSampler::loadSf2 (file, getProgram());
            set->regions = std::move (sf2.regions);
            newInfo.presetNames = sf2.presetNames;
            if (sf2.presetNames.isEmpty())
                newInfo.error = "Couldn't read " + file.getFileName();
        }
        else
        {
            MultiSampler::SampleCache fresh (edit.engine.getAudioFileFormatManager().readFormatManager);
            const auto trees = file.hasFileExtension ("sfz") ? MultiSampler::parseSfz (file) : getRegionStates();
            for (auto& t : trees)
            {
                const auto path = t["sample"].toString();
                if (auto it = cache.entries.find (path); it != cache.entries.end())
                    fresh.entries[path] = it->second; // reuse decoded audio
                if (auto r = MultiSampler::loadRegion (t, fresh))
                    set->regions.push_back (*r);
            }
            cache.entries = std::move (fresh.entries);
            if (set->regions.empty() && ! trees.isEmpty())
                newInfo.error = "Samples not found";
        }

        set->seqCounters.resize (set->regions.size(), 0);
        newInfo.numRegions = (int) set->regions.size();
        if (! set->regions.empty())
            newInfo.firstRegionLength = set->regions.front().data->getNumSamples();
        instrumentInfo = newInfo;

        // Hand over to the audio thread; free sets it has moved past.
        sets.push_back (std::move (set));
        pending.store (sets.back().get());
        const auto current = std::find_if (sets.begin(), sets.end(), [u = inUse.load()] (auto& s) { return s.get() == u; });
        if (current != sets.end())
            sets.erase (sets.begin(), current);
    }

    juce::CachedValue<float> volumeValue, attackValue, decayValue, sustainValue, releaseValue, tuneValue;
    MultiSampler::Engine voiceEngine;
    MultiSampler::SampleCache cache { edit.engine.getAudioFileFormatManager().readFormatManager };
    std::vector<std::unique_ptr<MultiSampler::SoundSet>> sets; // oldest first
    std::atomic<MultiSampler::SoundSet*> pending { nullptr }, inUse { nullptr };
    Info instrumentInfo;
};
