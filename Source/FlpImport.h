#pragma once

#include "FlpReader.h"
#include "Studio.h"
#include "UntitledEffects2.h"

// FL Studio project (.flp) -> our project. One-shot and read-only: builds a fresh project from the
// parsed file (see FlpReader.h), substituting our own instruments / effects for FL's stock plugins.
namespace FlpImport
{
    struct Report
    {
        juce::File source;
        juce::String version, title;
        double tempo = 0.0;
        int channels = 0, patterns = 0, notes = 0, clips = 0, inserts = 0, effects = 0, automation = 0;
        juce::StringArray substitutions, missingSamples, missingPlugins, skipped;

        juce::String oneLine() const
        {
            auto s = "Imported " + source.getFileName() + ": " + juce::String (channels) + " channels, " + juce::String (patterns) + " patterns, "
                     + juce::String (clips) + " playlist clips";
            if (! missingSamples.isEmpty() || ! missingPlugins.isEmpty())
                s << " (" << (missingSamples.size() + missingPlugins.size()) << " missing files/plugins, see the report)";
            return s;
        }

        juce::String toText() const
        {
            juce::String t;
            t << "FL Studio project: " << source.getFullPathName() << "\n";
            if (title.isNotEmpty()) t << "Title: " << title << "\n";
            t << "Saved with FL Studio " << version << ", " << juce::String (tempo, 3) << " BPM\n\n";
            t << "Imported:\n";
            t << "  " << channels << " channels\n  " << patterns << " patterns with " << notes << " notes\n  " << clips << " playlist clips\n"
              << "  " << inserts << " mixer inserts used, " << effects << " effects\n  " << automation << " automation clips\n";
            auto section = [&t] (const char* heading, const juce::StringArray& lines)
            {
                if (lines.isEmpty())
                    return;
                t << "\n" << heading << " (" << lines.size() << "):\n";
                for (auto& l : lines)
                    t << "  " << l << "\n";
            };
            section ("Substitutions", substitutions);
            section ("Missing samples", missingSamples);
            section ("Missing plugins", missingPlugins);
            section ("Not imported", skipped);
            return t;
        }
    };

    //==========================================================================
    // FL's volume curve: 100% = 0 dB, 125% = +5.6 dB (gain 1.9) -> gain = ratio^2.876.
    // ponytail: fitted to two documented points; refine if FL users report level differences.
    inline float flGainDb (double ratio) { return ratio <= 0.0 ? -100.0f : (float) (20.0 * 2.876 * std::log10 (ratio)); }

    inline juce::String presetForName (const juce::String& channelName, bool fm)
    {
        const auto n = channelName.toLowerCase();
        struct Hint { const char* word; const char* osc; const char* fmPreset; };
        static const Hint hints[] = {
            { "808", "808 Sub", "FM Bass" }, { "sub", "Sub Bass", "FM Bass" }, { "acid", "Acid Bass", "FM Bass" }, { "bass", "Saw Bass", "FM Bass" },
            { "lead", "Supersaw Lead", "Digital Lead" }, { "pad", "Soft Pad", "Glass Pad" }, { "string", "String Pad", "Glass Pad" },
            { "pluck", "Pluck", "FM Pluck" }, { "bell", "Pluck", "FM Bell" }, { "piano", "Pluck", "E-Piano" }, { "key", "Pluck", "E-Piano" },
            { "organ", "Organ", "FM Organ" }, { "brass", "Brass", "Brass Stab" }, { "arp", "Arp Sequence", "Arp Blip" }, { "fx", "Riser FX", "Metal Hit" },
        };
        for (auto& h : hints)
            if (n.contains (h.word))
                return fm ? h.fmPreset : h.osc;
        return fm ? "E-Piano" : "Pluck";
    }

    // Sample paths stored inside a plugin's data (FL samplers like Slicex / FPC keep them as text).
    inline juce::StringArray findSamplePaths (const juce::MemoryBlock& data)
    {
        juce::StringArray found;
        const Flp::Bytes b { (const juce::uint8*) data.getData(), data.getSize() };
        auto scan = [&found] (const juce::String& text)
        {
            for (auto token : juce::StringArray::fromTokens (text, "\n", {}))
            {
                token = token.trim();
                for (auto ext : { ".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg" })
                    if (token.endsWithIgnoreCase (ext))
                    {
                        if (const int i = token.indexOf ("%"); i >= 0)          token = token.substring (i);
                        else if (const int d = token.indexOf (":\\"); d > 0)   token = token.substring (d - 1);
                        found.addIfNotAlreadyThere (token);
                    }
            }
        };
        juce::String latin, utf16;
        for (size_t i = 0; i < b.size; ++i)
            latin += b.data[i] >= 32 && b.data[i] < 127 ? (juce::juce_wchar) b.data[i] : (juce::juce_wchar) '\n';
        for (size_t i = 0; i + 1 < b.size; i += 2)
        {
            const auto c = b.u16 (i);
            utf16 += c >= 32 && c != 0xffff ? (juce::juce_wchar) c : (juce::juce_wchar) '\n';
        }
        scan (latin);
        scan (utf16);
        return found;
    }

    //==========================================================================
    class Importer
    {
    public:
        Importer (Studio& s, const Flp::Project& p, const juce::File& flp, Report& r) : studio (s), project (p), flpFile (flp), report (r) {}

        void run()
        {
            report.source = flpFile;
            report.version = project.version;
            report.title = project.title;
            report.tempo = project.tempo;

            studio.newProjectFromTemplate ("Empty");
            studio.setBpm (project.tempo);
            studio.setTimeSigNumerator (juce::jlimit (1, 16, project.timeSigNum));
            if (project.timeSigBeat != 4)
                report.skipped.add ("Time signature " + juce::String (project.timeSigNum) + "/" + juce::String (project.timeSigBeat)
                                    + " imported as " + juce::String (project.timeSigNum) + "/4");
            studio.setSwing (project.swing / 128.0);

            setupMixer();
            for (auto& ch : project.channels)
                addChannel (ch);
            addPatterns();
            addPlaylist();
            addAutomationClips();
            reportMarkers();

            studio.setSongMode (studio.getPlaylist().getNumChildren() > 0);
            studio.finishImport();
        }

    private:
        Studio& studio;
        const Flp::Project& project;
        juce::File flpFile;
        Report& report;
        std::map<int, int> channelIds;            // FL channel iid -> our channel id
        std::map<int, std::vector<int>> layers;   // FL layer iid -> child iids
        std::map<int, int> patternIds;
        std::map<juce::String, juce::File> sampleIndex;
        bool sampleIndexBuilt = false;

        double beats (double ticks) const { return ticks / project.ppq; }

        int mapInsert (int fl)
        {
            if (fl <= 0)
                return 0;
            if (fl > Studio::numInserts)
            {
                report.skipped.addIfNotAlreadyThere ("Mixer insert " + juce::String (fl) + " folded into insert " + juce::String (Studio::numInserts)
                                                     + " (we have " + juce::String (Studio::numInserts) + ")");
                return Studio::numInserts;
            }
            return fl;
        }

        //======================================================================
        // Sample paths: absolute, FL's %FLStudio...% folders, next to the .flp, or anywhere below it by name.
        juce::File resolveSample (const juce::String& flPath)
        {
            if (flPath.isEmpty())
                return {};
            const auto path = flPath.replaceCharacter ('\\', '/');
            const auto name = path.fromLastOccurrenceOf ("/", false, false);
            const auto folder = flpFile.getParentDirectory();

            juce::Array<juce::File> candidates;
            if (juce::File::isAbsolutePath (path))
                candidates.add (juce::File (path));
            candidates.add (folder.getChildFile (path.trimCharactersAtStart ("/")));
            candidates.add (folder.getChildFile (name));

            if (path.startsWith ("%"))
            {
                const auto var = path.substring (1).upToFirstOccurrenceOf ("%", false, false);
                const auto rest = path.substring (1).fromFirstOccurrenceOf ("%", false, false).trimCharactersAtStart ("/");
                juce::Array<juce::File> roots;
                if (var.equalsIgnoreCase ("FLStudioUserData"))
                    roots.add (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Image-Line/FL Studio"));
                for (auto apps : { juce::File ("/Applications"), juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory).getChildFile ("Image-Line") })
                    for (auto& app : apps.findChildFiles (juce::File::findDirectories, false, "FL Studio*"))
                        roots.addArray ({ app.getChildFile ("Contents/Resources/FL"), app });
                for (auto& root : roots)
                    candidates.add (root.getChildFile (rest));
            }

            for (auto& c : candidates)
                if (c.existsAsFile())
                    return c;

            if (! sampleIndexBuilt)
            {
                sampleIndexBuilt = true;
                // ponytail: capped scan of the .flp's folder; a project kept in a huge folder may miss files.
                int seen = 0;
                if (folder != juce::File::getSpecialLocation (juce::File::userHomeDirectory) && folder.getParentDirectory() != folder)
                    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
                    {
                        sampleIndex.emplace (entry.getFile().getFileName().toLowerCase(), entry.getFile());
                        if (++seen > 20000) break;
                    }
            }
            if (auto it = sampleIndex.find (name.toLowerCase()); it != sampleIndex.end())
                return it->second;

            report.missingSamples.addIfNotAlreadyThere (flPath);
            return {};
        }

        //======================================================================
        void setupMixer()
        {
            for (int i = 1; i < (int) project.inserts.size(); ++i)
            {
                auto& ins = project.inserts[(size_t) i];
                if (i > Studio::numInserts)
                {
                    for (auto& slot : ins.slots)
                        addEffect (mapInsert (i), slot);
                    continue;
                }

                const int index = i;
                if (ins.name.isNotEmpty())
                    studio.renameInsert (index, ins.name);
                if (auto vp = studio.getInsertVolume (index))
                {
                    vp->setVolumeDb (flGainDb (ins.volume / 12800.0));
                    vp->setPan (juce::jlimit (-1.0f, 1.0f, (float) ins.pan / 6400.0f));
                }
                if (! ins.enabled())
                    if (auto t = studio.getInsertTrack (index))
                        t->setMute (true);
                for (auto& slot : ins.slots)
                    addEffect (index, slot);
            }

            // Routing: FL sends an insert to master and/or other inserts (a route at zero level is off).
            for (int i = 1; i < (int) project.inserts.size() && i <= Studio::numInserts; ++i)
            {
                auto& ins = project.inserts[(size_t) i];
                bool toMaster = false;
                std::vector<int> targets;
                for (int r : ins.routes)
                    if (auto level = ins.routeLevels.find (r); level != ins.routeLevels.end() && level->second <= 0) continue;
                    else if (r == 0) toMaster = true;
                    else if (r != i) targets.push_back (mapInsert (r));
                if (targets.empty())
                    continue;
                if (! toMaster && targets.size() == 1)
                {
                    if (auto src = studio.getInsertTrack (i))
                        if (auto dest = studio.getInsertTrack (targets[0]))
                            src->getOutput().setOutputToTrack (dest);
                }
                else
                {
                    for (int t : targets)
                        studio.addSend (i, t);
                }
            }

            if (! project.inserts.empty())
                for (auto& slot : project.inserts[0].slots)
                    addEffect (0, slot);
        }

        void addEffect (int index, const Flp::Slot& slot)
        {
            const auto& plug = slot.plugin;
            const auto flName = plug.isWrapper() ? (plug.vst.name.isNotEmpty() ? plug.vst.name : plug.name) : plug.internalName;
            const auto where = index == 0 ? juce::String ("master") : "insert " + juce::String (index);
            te::Plugin::Ptr p;

            if (plug.isWrapper())
            {
                if (auto desc = findInstalled (plug.vst, false))
                {
                    p = studio.addEffect (index, "ext:" + desc->createIdentifierString());
                    if (p != nullptr && ! restoreState (*p, plug.vst))
                        report.skipped.add (flName + " on " + where + ": settings not restored");
                }
                else
                {
                    report.missingPlugins.addIfNotAlreadyThere (flName + " (effect on " + where + ")");
                    return;
                }
            }
            else if (auto type = FlStockMap::untitledTypeFor (plug.internalName))
            {
                p = studio.addEffect (index, type);
                if (p != nullptr)
                {
                    const bool decoded = applyStockSettings (*p, plug);
                    report.substitutions.add (plug.internalName + " -> " + p->getName() + (decoded ? "" : " (default settings)") + " on " + where);
                }
            }
            else
            {
                report.skipped.add (plug.internalName + " on " + where + " (no equivalent)");
                return;
            }

            if (p == nullptr)
                return;
            if (! slot.enabled)
                p->setEnabled (false);
            ++report.effects;
        }

        // Stock effect settings whose layout is known.
        static bool applyStockSettings (te::Plugin& p, const Flp::Plugin& plug)
        {
            const Flp::Bytes d { (const juce::uint8*) plug.data.getData(), plug.data.getSize() };
            const auto name = plug.internalName;
            auto db = [] (double ratio, float lo, float hi) { return juce::jlimit (lo, hi, ratio <= 0.0 ? lo : (float) (20.0 * std::log10 (ratio))); };

            if (name == "Fruity Balance" && d.size >= 8)
            {
                Presets::applyValues (p, { { "pan", juce::jlimit (-1.0f, 1.0f, d.i32 (0) / 128.0f) },
                                           { "volume", juce::jlimit (-60.0f, 12.0f, flGainDb (d.u32 (4) / 256.0)) } });
                return true;
            }
            if (name == "Fruity Soft Clipper" && d.size >= 8)
            {
                Presets::applyValues (p, { { "threshold", db (d.u32 (0) / 128.0, -24.0f, 0.0f) }, { "post", db (d.u32 (4) / 128.0, -24.0f, 12.0f) } });
                return true;
            }
            if (name == "Fruity Stereo Enhancer" && d.size >= 24)
            {
                const int inv = (int) d.u32 (20);
                Presets::applyValues (p, { { "pan", juce::jlimit (-1.0f, 1.0f, d.i32 (0) / 128.0f) },
                                           { "width", juce::jlimit (0.0f, 2.0f, 1.0f - d.i32 (8) / 96.0f) },
                                           { "offset", juce::jlimit (-20.0f, 20.0f, d.i32 (12) * 500.0f / 512.0f) },
                                           { "invert", (float) (inv == 1 ? 1 : inv == 2 ? 2 : 0) } });
                return true;
            }
            if (name == "Fruity Fast Dist" && d.size >= 20)
            {
                Presets::applyValues (p, { { "drive", juce::jlimit (0.0f, 48.0f, d.u32 (0) / 128.0f * 24.0f) }, { "curve", d.u32 (8) == 0 ? 1.0f : 0.0f },
                                           { "mix", juce::jlimit (0.0f, 1.0f, d.u32 (12) / 128.0f) }, { "output", db (d.u32 (16) / 128.0, -24.0f, 6.0f) } });
                return true;
            }
            if (name == "Fruity Blood Overdrive" && d.size >= 32)
            {
                const size_t o = d.size >= 36 ? 4 : 0;
                Presets::applyValues (p, { { "curve", 2.0f }, { "drive", juce::jlimit (0.0f, 48.0f, d.u32 (o + 8) / 10000.0f * 24.0f + (d.u32 (o + 12) != 0 ? 20.0f : 0.0f)) },
                                           { "output", db (d.u32 (o + 20) / 10000.0, -24.0f, 6.0f) } });
                return true;
            }
            if (name == "Soundgoodizer" && d.size >= 8)
            {
                const size_t o = d.size >= 12 ? 4 : 0;
                Presets::applyValues (p, { { "mode", (float) juce::jlimit (0, 3, (int) d.u32 (o)) }, { "amount", juce::jlimit (0.0f, 1.0f, d.u32 (o + 4) / 1000.0f) } });
                return true;
            }
            return false;
        }

        //======================================================================
        // Third-party plugins: by name (and vendor) among the scanned plugins.
        std::optional<juce::PluginDescription> findInstalled (const Flp::Vst& vst, bool instrument) const
        {
            const auto name = vst.name.trim();
            std::optional<juce::PluginDescription> best;
            int bestScore = 0;
            for (auto& d : studio.engine.getPluginManager().knownPluginList.getTypes())
            {
                if (! d.name.equalsIgnoreCase (name) && ! d.descriptiveName.equalsIgnoreCase (name))
                    continue;
                const int score = 10 + (d.manufacturerName.equalsIgnoreCase (vst.vendor) ? 4 : 0) + (d.isInstrument == instrument ? 2 : 0)
                                + (d.pluginFormatName == "VST3" ? 1 : 0);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = d;
                }
            }
            return best;
        }

        // FL's wrapper state -> the plugin. VST3: FL keeps the component / controller states as
        // { u32 id, u64 size, data } records (id 3 / 4). VST2: { i32 marker, u32 flags, u8, u32 size,
        // chunk } - restored into a VST3 build of the plugin through the VST2-compatible "VstW" bank
        // that VST3 versions of VST2 plugins accept. AU targets: not restored.
        // ponytail: layouts inferred from a few files; unverified against installed plugins.
        static bool restoreState (te::Plugin& p, const Flp::Vst& vst)
        {
            auto* ext = dynamic_cast<te::ExternalPlugin*> (&p);
            auto* instance = ext != nullptr ? ext->getAudioPluginInstance() : nullptr;
            if (instance == nullptr || vst.state.isEmpty() || instance->getPluginDescription().pluginFormatName != "VST3")
                return false;

            const Flp::Bytes s { (const juce::uint8*) vst.state.getData(), vst.state.getSize() };
            juce::MemoryBlock component, controller;
            if (vst.isVst3())
            {
                for (size_t pos = 4; s.has (pos, 12);)
                {
                    const auto id = s.u32 (pos);
                    const auto size = s.u64 (pos + 4);
                    if (size > s.size - pos - 12)
                        break;
                    if (id == 3) component.append (s.data + pos + 12, (size_t) size);
                    if (id == 4) controller.append (s.data + pos + 12, (size_t) size);
                    pos += 12 + (size_t) size;
                }
            }
            else
            {
                const auto chunkSize = (size_t) s.u32 (9);
                if (chunkSize == 0 || ! s.has (13, chunkSize))
                    return false;
                juce::MemoryOutputStream out (component, false);
                auto be = [&out] (juce::uint32 v) { out.writeIntBigEndian ((int) v); };
                out.write ("VstW", 4); be (8); be (1); be (0);
                out.write ("CcnK", 4); be ((juce::uint32) (152 + chunkSize)); out.write ("FBCh", 4); be (2);
                juce::uint32 fxId = 0;
                if (vst.fourcc.length() == 4)
                    for (int i = 3; i >= 0; --i) fxId = (fxId << 8) | (juce::uint8) vst.fourcc[i];
                be (fxId); be (1); be (0);
                for (int i = 0; i < 32; ++i) be (0);
                be ((juce::uint32) chunkSize);
                out.write (s.data + 13, chunkSize);
            }
            if (component.isEmpty())
                return false;

            juce::XmlElement xml ("VST3PluginState");
            xml.createNewChildElement ("IComponent")->addTextElement (component.toBase64Encoding());
            if (! controller.isEmpty())
                xml.createNewChildElement ("IEditController")->addTextElement (controller.toBase64Encoding());
            juce::MemoryBlock blob;
            juce::AudioProcessor::copyXmlToBinary (xml, blob);
            instance->setStateInformation (blob.getData(), (int) blob.getSize());
            return true;
        }

        //======================================================================
        void addChannel (const Flp::Channel& ch)
        {
            const auto name = ch.displayName();
            int id = 0;

            switch (ch.type)
            {
                case Flp::ChanType::automation:
                    return; // becomes an automation clip
                case Flp::ChanType::layer:
                    layers[ch.iid] = ch.children;
                    report.substitutions.add ("Layer '" + name + "' -> its notes are copied to the layered channels");
                    return;
                case Flp::ChanType::audioClip:
                    id = addAudioChannel (ch);
                    break;
                case Flp::ChanType::sampler:
                    id = addSampler (ch);
                    break;
                default:
                    id = ch.plugin.internalName.isEmpty() && ch.samplePath.isNotEmpty() ? addSampler (ch) : addGenerator (ch, name);
                    break;
            }
            if (id == 0)
                return;

            channelIds[ch.iid] = id;
            ++report.channels;
            if (name.isNotEmpty())
                studio.renameChannel (id, name);
            if (ch.hasColour && ch.colour != juce::Colour (0xff5c656a))
                studio.getChannel (id).setProperty (IDs::colour, ch.colour.toString(), nullptr);

            studio.setChannelInsert (id, mapInsert (ch.insert));

            if (auto t = studio.getTrack (id))
            {
                if (auto vp = t->getVolumePlugin())
                {
                    // Relative to FL's default channel level (78%), which is our 0 dB.
                    vp->setVolumeDb (juce::jlimit (-100.0f, 6.0f, flGainDb (ch.volume / 12800.0) - flGainDb (10000.0 / 12800.0)));
                    vp->setPan (juce::jlimit (-1.0f, 1.0f, (float) (ch.pan - 6400) / 6400.0f));
                }
                if (! ch.enabled)
                    t->setMute (true);
            }
        }

        int addSampler (const Flp::Channel& ch)
        {
            const auto file = resolveSample (ch.samplePath);
            const int id = studio.addSamplerChannel (file);
            if (ch.rootNote != Studio::defaultRootNote && file != juce::File())
                if (auto t = studio.getTrack (id))
                    if (auto sampler = t->pluginList.findFirstPluginOfType<te::SamplerPlugin>(); sampler != nullptr && sampler->getNumSounds() > 0)
                        sampler->setSoundParams (0, juce::jlimit (0, 127, ch.rootNote), 0, 127);
            return id;
        }

        // FL audio clip channel: our audio channel is created by its first playlist clip.
        int addAudioChannel (const Flp::Channel& ch)
        {
            const auto file = resolveSample (ch.samplePath);
            if (! file.existsAsFile())
            {
                report.skipped.add ("Audio clip channel '" + ch.displayName() + "' and its clips (no sample)");
                return 0;
            }
            int id = 0;
            if (auto arr = project.getCurrentArrangement())
                for (auto& item : arr->items)
                    if (item.channel == ch.iid)
                        if (auto clip = studio.addAudioClip (file, juce::jmin (item.track, maxLane), beats (item.position)); clip.isValid())
                        {
                            clip.setProperty (IDs::length, juce::jmax (1.0 / 64.0, beats (item.length)), nullptr);
                            // ponytail: channel-item offsets read as beats (-1 = none); unverified for audio clips.
                            if (item.startOffset > 0.0f && std::isfinite (item.startOffset))
                                clip.setProperty (IDs::offset, (double) item.startOffset, nullptr);
                            if (item.muted)
                                clip.setProperty (IDs::muted, true, nullptr);
                            ++report.clips;
                            id = clip[IDs::channel];
                        }
            return id != 0 ? id : studio.addSamplerChannel (file); // no clips: still playable from the rack
        }

        static constexpr int maxLane = 63;

        int addPreset (const juce::String& preset, const juce::String& flName, const juce::String& note)
        {
            const int id = studio.addPresetChannel (preset);
            report.substitutions.add (flName + " -> " + (Presets::findSynthSound (preset)->pluginType == FMSynthPlugin::xmlTypeName ? "FM Synth" : "4-OSC")
                                      + " '" + preset + "'" + note);
            return id;
        }

        int addGenerator (const Flp::Channel& ch, const juce::String& name)
        {
            const auto& plug = ch.plugin;
            const auto flName = plug.internalName;
            const auto lower = flName.toLowerCase();
            auto is = [&lower] (std::initializer_list<const char*> names)
            {
                for (auto n : names) if (lower == n) return true;
                return false;
            };

            if (plug.isWrapper())
            {
                const auto vstName = plug.vst.name.isNotEmpty() ? plug.vst.name : name;
                if (auto desc = findInstalled (plug.vst, true))
                {
                    const int id = studio.addPluginChannel (*desc);
                    if (auto p = studio.getInstrument (id); p == nullptr || ! restoreState (*p, plug.vst))
                        report.skipped.add (vstName + ": settings not restored");
                    return id;
                }
                report.missingPlugins.addIfNotAlreadyThere (vstName);
                return addPreset (presetForName (name, false), vstName, " (placeholder, plugin not installed)");
            }

            if (is ({ "midi out", "fruity dance", "control surface", "fruity voltage controller", "dashboard" }))
            {
                report.skipped.add ("Channel '" + name + "' (" + flName + ", makes no sound)");
                return 0;
            }
            if (is ({ "fruit kick", "drumaxx", "drumpad" }))
            {
                report.substitutions.add (flName + " -> Sampler (Kick)");
                return studio.addSamplerChannel (DrumKit::getDrumsFolder().getChildFile ("Kick.wav"));
            }

            const auto samples = findSamplePaths (plug.data);
            auto firstSample = [this, &samples]
            {
                for (auto& s : samples)
                    if (auto f = resolveSample (s); f.existsAsFile())
                        return f;
                return juce::File();
            };

            if (is ({ "fpc" }))
            {
                const int id = studio.addDrumPadChannel();
                int mapped = 0;
                if (auto ms = studio.getMultiSampler (id))
                {
                    auto pads = MultiSampler::defaultDrumPads();
                    for (int i = 0; i < samples.size() && i < pads.size(); ++i)
                        if (auto f = resolveSample (samples[i]); f.existsAsFile())
                        {
                            pads.getReference (i).setProperty ("sample", f.getFullPathName(), nullptr);
                            pads.getReference (i).setProperty ("name", f.getFileNameWithoutExtension(), nullptr);
                            ++mapped;
                        }
                    ms->setRegions (pads);
                }
                report.substitutions.add ("FPC -> Drum pad (" + juce::String (mapped) + " pads from the project's samples, the rest default kit)");
                return id;
            }
            if (is ({ "slicex", "fruity slicer", "fruity granulizer" }))
                if (auto f = firstSample(); f.existsAsFile())
                    if (const int id = studio.addSlicerChannel (f))
                    {
                        report.substitutions.add (flName + " -> Slicer (" + f.getFileName() + ", slices re-detected)");
                        return id;
                    }
            if (is ({ "flex", "directwave", "fruity slicer", "slicex", "fruity granulizer" }))
            {
                if (auto f = firstSample(); f.existsAsFile())
                {
                    report.substitutions.add (flName + " -> Instrument (" + f.getFileName() + ")");
                    return studio.addInstrumentChannel (f);
                }
                return addPreset (presetForName (name, false), flName, " (placeholder, samples not found)");
            }

            if (is ({ "3x osc" }))
            {
                report.substitutions.add ("3x Osc -> 4-OSC (default sound, settings not decoded)");
                return studio.addSynthChannel();
            }
            if (is ({ "boobass" }))           return addPreset ("Sub Bass", flName, "");
            if (is ({ "transistor bass" }))   return addPreset ("Acid Bass", flName, "");
            if (is ({ "plucked!" }))          return addPreset ("Pluck", flName, "");
            if (is ({ "sytrus", "gms", "toxic biohazard", "ogun", "morphine", "fruity dx10", "fl keys" }))
                return addPreset (presetForName (name, true), flName, " (approximation)");
            if (is ({ "harmor", "harmless", "poizone", "sawer", "simsynth", "sakura", "autogun", "flowstone" }))
                return addPreset (presetForName (name, false), flName, " (approximation)");

            return addPreset (presetForName (name, false), flName.isNotEmpty() ? flName : name, " (placeholder, no equivalent)");
        }

        //======================================================================
        void addPatterns()
        {
            int dropped = 0;
            for (auto& fp : project.patterns)
            {
                const int id = studio.addPattern (fp.name.isNotEmpty() ? fp.name : "Pattern " + juce::String (fp.iid));
                patternIds[fp.iid] = id;
                auto pattern = studio.getPattern (id);
                if (fp.hasColour && fp.colour != juce::Colour (0xff485156))
                    pattern.setProperty (IDs::colour, fp.colour.toString(), nullptr);

                for (auto& n : fp.notes)
                {
                    std::vector<int> targets;
                    if (auto it = layers.find (n.channel); it != layers.end())
                        targets = it->second;
                    else
                        targets.push_back (n.channel);

                    // FL fine pitch (+-1200 cents in 10 cent steps) rounds to the nearest key.
                    const int key = juce::jlimit (0, 127, n.key + juce::roundToInt ((n.finePitch - 120) / 10.0));
                    const double length = n.length > 0 ? beats (n.length) : Studio::stepBeats;
                    const int velocity = juce::jlimit (1, 127, juce::roundToInt (n.velocity * 127.0 / 128.0));
                    bool placed = false;
                    for (int t : targets)
                        if (auto it = channelIds.find (t); it != channelIds.end())
                        {
                            studio.addNote (pattern, it->second, beats (n.position), length, key, velocity);
                            ++report.notes;
                            placed = true;
                        }
                    dropped += placed ? 0 : 1;
                }
                ++report.patterns;
            }
            if (dropped > 0)
                report.skipped.add (juce::String (dropped) + " notes on channels that weren't imported");

            // The empty template's pattern goes when the project brings its own.
            if (! project.patterns.empty())
            {
                const int templatePattern = studio.getPatterns().getChild (0)[IDs::id];
                studio.setCurrentPattern (patternIds.begin()->second);
                studio.deletePattern (templatePattern);
            }
        }

        void addPlaylist()
        {
            auto arr = project.getCurrentArrangement();
            if (arr == nullptr)
                return;
            if (project.arrangements.size() > 1)
                report.skipped.add (juce::String ((int) project.arrangements.size() - 1) + " other arrangement(s) (imported '" + arr->name + "')");

            int folded = 0;
            for (auto& item : arr->items)
            {
                if (item.pattern < 0)
                    continue;
                auto it = patternIds.find (item.pattern);
                if (it == patternIds.end())
                    continue;
                folded += item.track > maxLane ? 1 : 0;
                auto clip = studio.addPatternClip (it->second, juce::jmin (item.track, maxLane), beats (item.position));
                clip.setProperty (IDs::length, juce::jmax (1.0 / 64.0, beats (item.length)), nullptr);
                if (item.startTicks > 0)
                    clip.setProperty (IDs::offset, beats (item.startTicks), nullptr);
                if (item.muted)
                    clip.setProperty (IDs::muted, true, nullptr);
                ++report.clips;
            }
            if (folded > 0)
                report.skipped.add (juce::String (folded) + " clips on playlist tracks above " + juce::String (maxLane + 1) + " moved to the last lane");

            int named = 0;
            for (auto& n : arr->trackNames)
                named += n.isNotEmpty() ? 1 : 0;
            if (named > 0)
                report.skipped.add (juce::String (named) + " playlist track names (our playlist lanes have no names)");
        }

        //======================================================================
        // Automation clips: FL's remote-controller link tells what the clip drives.
        // ponytail: the link layout (destination = channel, or 0x2000 | insert << 6 | slot; parameter 0/1 or
        // 192/193 = volume/pan, 0x8000 = plugin parameter index) is inferred and unverified (no linked clip
        // in the test files). Unrecognised targets are listed in the report.
        te::AutomatableParameter* findTarget (const Flp::RemoteController& rc, bool& isVolume)
        {
            isVolume = false;
            te::Plugin* plugin = nullptr;
            te::VolumeAndPanPlugin* fader = nullptr;
            if ((rc.dest & 0x2000) != 0)
            {
                const int insert = mapInsert ((rc.dest >> 6) & 0x7f), slot = rc.dest & 0x3f;
                if (slot == 0x3f)
                    fader = studio.getInsertVolume (insert);
                else
                    plugin = studio.getInsertEffects (insert)[slot];
            }
            else if (auto it = channelIds.find (rc.dest); it != channelIds.end())
            {
                if (rc.vst) plugin = studio.getInstrument (it->second);
                else if (auto t = studio.getTrack (it->second)) fader = t->getVolumePlugin();
            }

            if (fader != nullptr && ! rc.vst)
            {
                isVolume = rc.param == 0 || rc.param == 192;
                if (isVolume)                           return fader->volParam.get();
                if (rc.param == 1 || rc.param == 193)   return fader->panParam.get();
                return nullptr;
            }
            if (plugin != nullptr && dynamic_cast<te::ExternalPlugin*> (plugin) != nullptr)
                return plugin->getAutomatableParameter (rc.param).get();
            return nullptr;
        }

        void addAutomationClips()
        {
            auto arr = project.getCurrentArrangement();
            for (auto& ch : project.channels)
            {
                if (ch.type != Flp::ChanType::automation)
                    continue;
                const auto name = ch.displayName().isNotEmpty() ? ch.displayName() : juce::String ("Automation clip");

                const Flp::RemoteController* link = nullptr;
                for (auto& rc : project.controllers)
                    if (rc.ownerChannel == ch.iid && link == nullptr) link = &rc;
                for (auto& rc : project.controllers)
                    if (rc.source == ch.iid && link == nullptr) link = &rc;

                bool isVolume = false;
                auto* param = link != nullptr ? findTarget (*link, isVolume) : nullptr;
                if (param == nullptr)
                {
                    report.skipped.add ("Automation clip '" + name + "' (target not recognised)");
                    continue;
                }

                bool placed = false;
                if (arr != nullptr)
                    for (auto& item : arr->items)
                        if (item.channel == ch.iid)
                        {
                            auto clip = studio.createAutomationClip (*param, beats (item.position), juce::jmax (Studio::stepBeats, beats (item.length)));
                            clip.setProperty (IDs::lane, juce::jmin (item.track, maxLane), nullptr);
                            clip.setProperty (IDs::name, name, nullptr);
                            if (item.muted) clip.setProperty (IDs::muted, true, nullptr);
                            clip.removeAllChildren (nullptr);
                            for (auto& pt : ch.points)
                            {
                                float v = (float) juce::jlimit (0.0, 1.0, pt.value);
                                if (isVolume) // FL fader position -> our fader position
                                    v = te::decibelsToVolumeFaderPosition (flGainDb (v * 1.25));
                                Studio::addAutomationPoint (clip, pt.beats, v, nullptr);
                            }
                            ++report.automation;
                            ++report.clips;
                            placed = true;
                        }
                if (! placed)
                    report.skipped.add ("Automation clip '" + name + "' (not placed in the playlist)");
            }
        }

        void reportMarkers()
        {
            auto arr = project.getCurrentArrangement();
            if (arr == nullptr || arr->markers.empty())
                return;
            juce::StringArray names;
            for (auto& m : arr->markers)
                names.add ((m.signature ? "time signature " + juce::String (m.numerator) + "/" + juce::String (m.denominator) : juce::String ("marker"))
                           + (m.name.trim().isNotEmpty() ? " '" + m.name.trim() + "'" : juce::String())
                           + " at bar " + juce::String (1 + m.position / (project.ppq * juce::jmax (1, project.timeSigNum))));
            report.skipped.add (juce::String ((int) arr->markers.size()) + " time markers: " + names.joinIntoString (", "));
        }
    };

    //==========================================================================
    // Reads and converts. On a read error the open project is left untouched.
    inline juce::Result importFile (Studio& studio, const juce::File& file, Report& report)
    {
        Flp::Project project;
        if (auto r = Flp::readFile (file, project); r.failed())
            return r;
        if (project.format != 0)
            return juce::Result::fail ("This is an FL Studio preset or score, not a project (.flp)");
        Importer (studio, project, file, report).run();
        report.inserts = 0;
        for (int i = 1; i <= Studio::numInserts; ++i)
        {
            bool used = ! studio.getInsertEffects (i).isEmpty() || ! studio.getInsertSends (i).isEmpty();
            for (auto ch : studio.getChannels())
                used |= (int) ch[IDs::insert] == i;
            report.inserts += used ? 1 : 0;
        }
        return juce::Result::ok();
    }
}
