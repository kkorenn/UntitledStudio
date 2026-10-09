#pragma once

#include "FlpImport.h"
#include "Exporter.h"

// FL Studio import self-tests (part of --selftest). A synthetic .flp written here always runs;
// the real-file tests use testdata/flp (scripts/fetch-flp-testdata.sh) and skip without it.
namespace FlpTests
{
    using Check = std::function<bool (bool, const juce::String&)>;

    inline juce::File fixtures() { return juce::File (__FILE__).getParentDirectory().getSiblingFile ("testdata").getChildFile ("flp"); }

    // Writes FLP events the way FL lays them out.
    struct Writer
    {
        juce::MemoryOutputStream out;

        void byte (int id, int v)            { out.writeByte ((char) id); out.writeByte ((char) v); }
        void word (int id, int v)            { out.writeByte ((char) id); out.writeShort ((short) v); }
        void dword (int id, juce::uint32 v)  { out.writeByte ((char) id); out.writeInt ((int) v); }
        void data (int id, const juce::MemoryBlock& d)
        {
            out.writeByte ((char) id);
            for (auto n = d.getSize();; n >>= 7)
            {
                out.writeByte ((char) ((n & 0x7f) | (n > 0x7f ? 0x80 : 0)));
                if (n <= 0x7f) break;
            }
            out << d;
        }
        void text (int id, const juce::String& s)
        {
            juce::MemoryOutputStream t;
            for (auto c : s)
                t.writeShort ((short) c);
            t.writeShort (0);
            data (id, t.getMemoryBlock());
        }
        void ascii (int id, const juce::String& s) { data (id, juce::MemoryBlock (s.toRawUTF8(), s.getNumBytesAsUTF8() + 1)); }

        juce::MemoryBlock finish (int ppq, int numChannels)
        {
            juce::MemoryOutputStream f;
            f.write ("FLhd", 4); f.writeInt (6); f.writeShort (0); f.writeShort ((short) numChannels); f.writeShort ((short) ppq);
            f.write ("FLdt", 4); f.writeInt ((int) out.getDataSize());
            f << out.getMemoryBlock();
            return f.getMemoryBlock();
        }
    };

    inline juce::MemoryBlock ints (std::initializer_list<int> values)
    {
        juce::MemoryOutputStream m;
        for (int v : values) m.writeInt (v);
        return m.getMemoryBlock();
    }

    // A small project: sampler (sample path only findable by name), BooBass, a missing VST,
    // one pattern, one playlist clip and three mixer inserts.
    inline juce::MemoryBlock synthetic()
    {
        using namespace Flp::Id;
        Writer w;
        w.ascii (version, "21.0.3.3517");
        w.dword (tempo, 128000);
        w.byte (timeSigNum, 4);
        w.byte (timeSigBeat, 4);
        w.byte (rackSwing, 64);
        w.text (title, "Synthetic");

        w.word (chanNew, 0);  w.byte (chanType, Flp::ChanType::sampler);  w.text (pluginInternalName, {});
        w.text (pluginName, "Synth Kick");  w.text (samplePath, "C:\\Users\\someone\\Samples\\synthkick.wav");  w.byte (chanRoutedTo, 1);
        w.word (chanNew, 1);  w.byte (chanType, Flp::ChanType::generator);  w.text (pluginInternalName, "BooBass");  w.byte (chanRoutedTo, 3);
        w.word (chanNew, 2);  w.byte (chanType, Flp::ChanType::generator);  w.text (pluginInternalName, "Fruity Wrapper");
        {
            juce::MemoryOutputStream vst;
            vst.writeInt (10);
            auto record = [&vst] (int recordId, const juce::String& s) { vst.writeInt (recordId); vst.writeInt64 ((juce::int64) s.getNumBytesAsUTF8()); vst.write (s.toRawUTF8(), s.getNumBytesAsUTF8()); };
            record (54, "NotInstalledSynth");
            record (56, "Nobody");
            w.data (pluginData, vst.getMemoryBlock());
        }

        w.word (patNew, 1);
        {
            juce::MemoryOutputStream notes;
            auto note = [&notes] (int pos, int channel, int length, int key)
            {
                notes.writeInt (pos); notes.writeShort (0); notes.writeShort ((short) channel); notes.writeInt (length);
                notes.writeShort ((short) key); notes.writeShort (0);
                for (int b : { 120, 0, 64, 0, 64, 100, 128, 128 }) notes.writeByte ((char) b);
            };
            note (0, 0, 0, 60); note (96, 0, 0, 60); note (192, 0, 0, 60); note (0, 1, 192, 48);
            w.data (patNotes, notes.getMemoryBlock());
        }
        w.word (patNew, 1);
        w.text (patName, "Verse");

        w.word (arrNew, 0);
        w.text (arrName, "Arrangement");
        {
            juce::MemoryOutputStream items;
            items.writeInt (384); items.writeShort (20480); items.writeShort (20481); items.writeInt (768);
            items.writeShort (499 - 2); items.writeShort (0); items.writeShort (120); items.writeShort (64); items.writeInt (0);
            items.writeFloat (-1.0f); items.writeFloat (-1.0f);
            w.data (playlist, items.getMemoryBlock());
        }
        w.word (arrCurrent, 0);

        auto insert = [&w] (const juce::String& name, const juce::String& slotPlugin, const juce::MemoryBlock& slotData)
        {
            w.text (insertName, name);
            w.data (insertFlags, ints ({ 0, 4 | 8, 0 }));
            for (int s = 0; s < 10; ++s)
            {
                if (s == 0 && slotPlugin.isNotEmpty())
                {
                    w.text (pluginInternalName, slotPlugin);
                    w.data (pluginData, slotData);
                }
                w.word (slotIndex, s);
            }
            juce::MemoryBlock routes (4);
            routes[0] = 1;
            w.data (insertRouting, routes);
            w.dword (insertOutput, (juce::uint32) -1);
        };
        insert ("Master", {}, {});
        insert ({}, "Fruity Limiter", ints ({ 1, 2, 3 }));
        insert ({}, {}, {});
        insert ("Bass Bus", "Fruity Balance", ints ({ -64, 256 }));
        return w.finish (96, 3);
    }

    inline bool run (te::Engine& engine, const Check& check)
    {
        bool ok = true;
        Studio s (engine);
        const auto dir = juce::File::createTempFile ("flp-test");
        dir.createDirectory();

        {
            // Synthetic project: always runs, tests the converter end to end.
            const auto sample = dir.getChildFile ("samples/synthkick.wav");
            sample.getParentDirectory().createDirectory();
            DrumKit::getDrumsFolder().getChildFile ("Kick.wav").copyFileTo (sample);
            const auto flp = dir.getChildFile ("Synthetic.flp");
            flp.replaceWithData (synthetic().getData(), synthetic().getSize());

            FlpImport::Report report;
            const bool imported = FlpImport::importFile (s, flp, report).wasOk();
            auto channels = s.getChannels();
            auto kick = channels.getChild (0), bass = channels.getChild (1), ghost = channels.getChild (2);
            auto pattern = s.getPatterns().getChild (0);
            int kickNotes = 0, bassNotes = 0;
            for (auto n : pattern)
            {
                kickNotes += (int) n[IDs::channel] == (int) kick[IDs::id] && std::abs ((double) n[IDs::length] - 0.25) < 1.0e-9 ? 1 : 0;
                bassNotes += (int) n[IDs::channel] == (int) bass[IDs::id] && (int) n[IDs::key] == 48 && std::abs ((double) n[IDs::length] - 2.0) < 1.0e-9 ? 1 : 0;
            }
            auto clip = s.getPlaylist().getChild (0);
            ok &= check (imported && channels.getNumChildren() == 3 && kick[IDs::name] == juce::var ("Synth Kick")
                             && kick[IDs::file].toString() == sample.getFullPathName() && bass[IDs::name] == juce::var ("BooBass")
                             && ghost[IDs::name] == juce::var ("NotInstalledSynth") && report.missingPlugins.contains ("NotInstalledSynth"),
                         "flp: synthetic project -> sampler (sample found by name next to the .flp), BooBass substitute, missing VST placeholder");
            ok &= check (s.getPatterns().getNumChildren() == 1 && pattern[IDs::name] == juce::var ("Verse") && pattern.getNumChildren() == 4
                             && kickNotes == 3 && bassNotes == 1,
                         "flp: synthetic pattern + notes (step notes 1 step long, ticks -> beats)");
            ok &= check (s.getPlaylist().getNumChildren() == 1 && (int) clip[IDs::pattern] == (int) pattern[IDs::id] && (int) clip[IDs::lane] == 2
                             && std::abs ((double) clip[IDs::start] - 4.0) < 1.0e-9 && std::abs ((double) clip[IDs::length] - 8.0) < 1.0e-9 && s.isSongMode(),
                         "flp: synthetic playlist clip (track -> lane, position, length)");
            auto limiter = s.getInsertEffects (1).getFirst();
            auto balance = s.getInsertEffects (3).getFirst();
            ok &= check (std::abs (s.getBpm() - 128.0) < 1.0e-6 && std::abs (s.getSwing() - 0.5) < 1.0e-6 && (int) kick[IDs::insert] == 1
                             && (int) bass[IDs::insert] == 3 && s.getInsertName (3) == "Bass Bus"
                             && limiter != nullptr && limiter->getPluginType() == UFX::LimiterPlugin::xmlTypeName
                             && balance != nullptr && balance->getPluginType() == UFX::BalancePlugin::xmlTypeName
                             && std::abs (s.findParam (balance->itemID.toVar(), "pan")->getCurrentValue() + 0.5f) < 1.0e-4,
                         "flp: synthetic tempo, swing, routing, insert names, stock effects -> Untitled effects (+ Balance settings)");
            ok &= check (s.isDirty() && s.getProjectFile() == juce::File() && ! s.undo().canUndo(),
                         "flp: import is one untitled, unsaved project with no undo history");
        }

        const auto root = fixtures();
        if (! root.isDirectory())
        {
            std::cout << "  skip flp fixtures (testdata/flp missing; run scripts/fetch-flp-testdata.sh)" << std::endl;
            dir.deleteRecursively();
            return ok;
        }

        const auto fl20 = root.getChildFile ("FL 20.8.4.flp");
        Flp::Project p;
        const bool parsed = Flp::readFile (fl20, p).wasOk();
        {
            // Ground truth from PyFLP 2.2 on the same file.
            int notes = 0, sumKeys = 0, sumPos = 0, sumLen = 0;
            std::map<int, int> perChannel;
            for (auto& pat : p.patterns)
                for (auto& n : pat.notes)
                {
                    ++notes; sumKeys += n.key; sumPos += n.position; sumLen += n.length;
                    ++perChannel[n.channel];
                }
            bool fourEach = perChannel.size() == 12;
            for (auto& [ch, count] : perChannel) fourEach &= count == 4 && ch >= 1 && ch <= 12;
            const auto* midi = p.patterns.size() == 5 ? &p.patterns[0] : nullptr;
            ok &= check (parsed && p.version == "20.8.4.2576" && p.ppq == 96 && std::abs (p.tempo - 69.42) < 1.0e-9 && p.title == "PyFLP Test FLP"
                             && p.artists == "demberto" && p.channels.size() == 19 && p.channelCount == 19 && p.timeSigNum == 4 && p.timeSigBeat == 4,
                         "flp: FL 20.8.4 header, version, PPQ, tempo, title, 19 channels");
            ok &= check (p.patterns.size() == 5 && midi != nullptr && midi->iid == 3 && midi->name == "MIDI" && midi->notes.size() == 48 && notes == 48
                             && sumKeys == 2880 && sumPos == 8640 && sumLen == 96 && fourEach && p.patterns[3].name == "Timemarkers" && p.patterns[3].length == 1824,
                         "flp: FL 20.8.4 patterns: 5, 48 notes in 'MIDI' (4 on each of channels 1-12), sums match PyFLP");

            auto chan = [&p] (int iid) { return p.findChannel (iid); };
            ok &= check (chan (0)->plugin.internalName == "BooBass" && chan (2)->type == Flp::ChanType::layer && chan (5)->type == Flp::ChanType::automation
                             && chan (6)->plugin.vst.name == "Sylenth1" && chan (6)->plugin.vst.vendor == "LennarDigital" && chan (6)->plugin.vst.state.getSize() == 19564
                             && chan (7)->type == Flp::ChanType::audioClip && chan (11)->samplePath == "%FLStudioFactoryData%\\Data\\Patches\\Packs\\Drums\\Kicks\\22in Kick.wav"
                             && chan (12)->volume == 0 && chan (13)->volume == 12800 && chan (14)->pan == 0 && chan (15)->pan == 12800 && ! chan (16)->enabled
                             && chan (1)->insert == 2 && chan (4)->hasColour && chan (4)->colour == juce::Colour (20, 20, 255),
                         "flp: FL 20.8.4 channels: types, plugin names, VST info + state, sample path, volume, pan, enabled, routing, colour");
            const auto& points = chan (5)->points;
            ok &= check (points.size() == 4 && points[1].beats == 8.0 && points[2].value == 0.0 && std::abs (points[3].beats - 16.0) < 1.0e-5,
                         "flp: FL 20.8.4 automation clip points (positions in beats, values 0..1)");

            const auto* arr = p.getCurrentArrangement();
            int patternItems = 0, audioItems = 0;
            if (arr != nullptr)
                for (auto& it : arr->items) (it.pattern >= 0 ? patternItems : audioItems) += 1;
            ok &= check (p.arrangements.size() == 2 && arr != nullptr && arr->name == "Just tracks" && arr->items.size() == 22 && patternItems == 5
                             && audioItems == 17 && arr->trackNames.size() == 500 && arr->trackNames[7] == "Audio track"
                             && p.arrangements[1].markers.size() == 11 && p.arrangements[1].markers[2].name == "4/4" && p.arrangements[1].markers[2].signature,
                         "flp: FL 20.8.4 arrangements: current one's 22 playlist items, 500 tracks, 11 time markers in the other");
            ok &= check (p.inserts.size() == 127 && p.inserts[0].name == "Master" && p.inserts[1].name == "Audio track" && p.inserts[4].volume == 0
                             && p.inserts[6].pan == -6400 && ! p.inserts[10].enabled() && p.inserts[1].routes == std::vector<int> { 0, 100, 101, 102, 103 }
                             && p.inserts[1].routeLevels[100] == 0 && p.inserts[19].slots.size() == 7 && p.inserts[19].slots[0].plugin.internalName == "Fruity Balance"
                             && p.inserts[19].slots[1].plugin.internalName == "Fruity Fast Dist" && p.inserts[19].slots[6].plugin.vst.name == "OTT",
                         "flp: FL 20.8.4 mixer: 127 inserts, names, volume, pan, enabled, routing, effect slots");
        }
        {
            Flp::Project mc;
            const bool read = Flp::readFile (root.getChildFile ("patterns/multi-channel.flp"), mc).wasOk();
            const bool match = read && mc.patterns.size() == 1 && mc.patterns[0].notes.size() == 2
                               && mc.patterns[0].notes[0].channel == 1 && mc.patterns[0].notes[1].channel == 0
                               && mc.patterns[0].notes[0].length == 384 && mc.patterns[0].notes[1].key == 60;
            ok &= check (match, "flp: multi-channel.flp pattern notes match PyFLP");
        }
        {
            auto corrupted = root.getChildFile ("corrupted").findChildFiles (juce::File::findFiles, false, "*.flp");
            int rejected = 0;
            for (auto& f : corrupted)
            {
                Flp::Project x;
                rejected += Flp::readFile (f, x).failed() ? 1 : 0;
            }
            ok &= check (corrupted.size() >= 6 && rejected == corrupted.size(), "flp: all " + juce::String (corrupted.size()) + " corrupted fixtures rejected");
        }
        {
            // Seeded mutations of the real file: byte flips and truncation. Read every one, convert the ones that read.
            juce::MemoryBlock valid;
            fl20.loadFileAsData (valid);
            juce::Random rng (4242);
            int readable = 0, converted = 0;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < 50; ++i)
            {
                auto bytes = valid;
                if (i % 10 == 9)
                    bytes.setSize ((size_t) rng.nextInt ((int) bytes.getSize()));
                for (int k = 0, n = 1 + rng.nextInt (40); k < n && bytes.getSize() > 0; ++k)
                    bytes[(size_t) rng.nextInt ((int) bytes.getSize())] = (char) rng.nextInt (256);
                const auto f = dir.getChildFile ("mutant.flp");
                f.replaceWithData (bytes.getData(), bytes.getSize());
                Flp::Project x;
                if (Flp::readFile (f, x).wasOk())
                {
                    ++readable;
                    if (converted < 8)
                    {
                        FlpImport::Report r;
                        converted += FlpImport::importFile (s, f, r).wasOk() ? 1 : 0;
                    }
                }
            }
            ok &= check (true, "flp: 50 seeded mutations, no crash or hang (" + juce::String (readable) + " readable, " + juce::String (converted)
                                   + " converted, " + juce::String ((int) (juce::Time::getMillisecondCounterHiRes() - t0)) + " ms)");
        }
        {
            // Full conversion of the real file, then save / load / render.
            FlpImport::Report report;
            const bool imported = FlpImport::importFile (s, fl20, report).wasOk();
            int balance = 0, softclip = 0, stereo = 0, enhancer = 0, distortion = 0;
            for (auto* fx : s.getInsertEffects (19))
            {
                balance += fx->getPluginType() == UFX::BalancePlugin::xmlTypeName ? 1 : 0;
                softclip += fx->getPluginType() == UFX::SoftClipperPlugin::xmlTypeName ? 1 : 0;
                stereo += fx->getPluginType() == UFX::StereoPlugin::xmlTypeName ? 1 : 0;
                enhancer += fx->getPluginType() == UFX::EnhancerPlugin::xmlTypeName ? 1 : 0;
                distortion += fx->getPluginType() == UFX::DistortionPlugin::xmlTypeName ? 1 : 0;
            }
            ok &= check (imported && balance == 1 && softclip == 1 && stereo == 1 && enhancer == 1 && distortion == 1 && report.missingPlugins.size() >= 2
                             && s.getInsertName (1) == "Audio track",
                         "flp: FL 20.8.4 insert 19 stock effects -> Untitled Balance / Distortion / Soft Clipper / Stereo / Enhancer, missing VSTs reported");

            const int channels = s.getChannels().getNumChildren();
            int notes = 0;
            for (auto pat : s.getPatterns()) notes += pat.getNumChildren();
            const auto saved = dir.getChildFile ("fl20" + Studio::fileExtension);
            const bool roundTrip = s.saveTo (saved) && s.load (saved) && s.getChannels().getNumChildren() == channels;
            Exporter::Settings es;
            es.tailSeconds = 0.0;
            const auto wav = dir.getChildFile ("fl20.wav");
            const bool rendered = Exporter::exportMix (s, es, wav) && wav.getSize() > 1000;
            ok &= check (roundTrip && rendered && channels == report.channels && channels == 13 && notes == 72 && report.notes == 72 && report.clips == 5,
                         "flp: FL 20.8.4 converts (" + juce::String (channels) + " channels, " + juce::String (notes) + " notes, "
                             + juce::String (report.clips) + " clips, " + juce::String (report.effects) + " effects), saves, loads and renders");
        }

        dir.deleteRecursively();
        return ok;
    }
}
