#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <map>

// Reader for FL Studio project files (.flp), read-only. The format is undocumented; event IDs
// and struct layouts follow the community's reverse-engineering notes (format facts only).
//
// File: "FLhd" chunk (format, channel count, PPQ) then "FLdt" chunk holding a flat event stream.
// Each event = id byte + value: ids 0-63 one byte, 64-127 two, 128-191 four, 192-255 a varint
// size then that many bytes. Events are positional: a "new channel" event starts a channel and
// the following channel events belong to it, and so on for patterns, arrangements and inserts.
namespace Flp
{
    namespace Id
    {
        enum : int
        {
            // byte
            chanEnabled = 0, chanVolByte = 2, chanPanByte = 3, rackSwing = 11, timeSigNum = 17, timeSigBeat = 18,
            chanType = 21, chanRoutedTo = 22, markerNum = 33, markerDen = 34,
            // word
            chanNew = 64, patNew = 65, tempoCoarse = 66, chanVolWord = 72, chanPanWord = 73, tempoFine = 93,
            chanChild = 94, slotIndex = 98, arrNew = 99, arrCurrent = 100,
            // dword
            pluginColour = 128, chanRootNote = 135, insertOutput = 147, markerPos = 148, insertColour = 149,
            patColour = 150, insertInput = 154, tempo = 156, patLength = 164,
            // text
            chanNameOld = 192, patName = 193, title = 194, comments = 195, samplePath = 196, version = 199,
            pluginInternalName = 201, pluginName = 203, insertName = 204, markerName = 205, genre = 206, artists = 207,
            groupName = 231, trackName = 239, arrName = 241,
            // data
            pluginWrapper = 212, pluginData = 213, chanLevels = 219, patNotes = 224, mixerParams = 225,
            remoteCtrl = 227, playlist = 233, automation = 234, insertRouting = 235, insertFlags = 236, trackData = 238
        };
    }

    namespace ChanType { enum : int { sampler = 0, generator = 2, layer = 3, audioClip = 4, automation = 5 }; }

    struct Event { int id; size_t offset, size; juce::uint32 value; };

    // Bounds-checked little-endian reads (out of range reads give 0).
    struct Bytes
    {
        const juce::uint8* data = nullptr;
        size_t size = 0;

        bool has (size_t off, size_t n) const { return off <= size && n <= size - off; }
        juce::uint32 u8 (size_t o) const  { return has (o, 1) ? data[o] : 0u; }
        juce::uint32 u16 (size_t o) const { return has (o, 2) ? (juce::uint32) (data[o] | (data[o + 1] << 8)) : 0u; }
        juce::uint32 u32 (size_t o) const { return has (o, 4) ? (juce::uint32) data[o] | ((juce::uint32) data[o + 1] << 8) | ((juce::uint32) data[o + 2] << 16) | ((juce::uint32) data[o + 3] << 24) : 0u; }
        juce::uint64 u64 (size_t o) const { return has (o, 8) ? (juce::uint64) u32 (o) | ((juce::uint64) u32 (o + 4) << 32) : 0u; }
        int i32 (size_t o) const          { return (int) u32 (o); }
        float f32 (size_t o) const        { const auto v = u32 (o); float f; std::memcpy (&f, &v, 4); return f; }
        double f64 (size_t o) const       { const auto v = u64 (o); double d; std::memcpy (&d, &v, 8); return d; }
        Bytes sub (size_t o, size_t n) const { return has (o, n) ? Bytes { data + o, n } : Bytes {}; }
        juce::MemoryBlock copy() const    { return size > 0 ? juce::MemoryBlock (data, size) : juce::MemoryBlock(); }
    };

    // Third-party plugin hosted by FL's wrapper ("Fruity Wrapper" plugin data).
    struct Vst
    {
        juce::String name, vendor, path, fourcc;
        juce::MemoryBlock guid, state;
        bool isVst3() const { return path.endsWithIgnoreCase (".vst3") || (fourcc.isEmpty() && guid.getSize() == 16); }
    };

    struct Plugin
    {
        juce::String internalName, name;   // internal: FL's plugin name ("Fruity Wrapper" for VST/AU)
        juce::MemoryBlock data;
        Vst vst;
        bool isWrapper() const { return internalName == "Fruity Wrapper"; }
        bool isEmpty() const   { return internalName.isEmpty() && name.isEmpty() && data.isEmpty(); }
    };

    struct AutoPoint { double beats, value, tension; };

    struct Channel
    {
        int iid = 0, type = ChanType::sampler, insert = 0, rootNote = 60;
        int volume = 10000, pan = 6400;     // 0..12800 (FL default 10000 = 78%), 0..12800 (6400 centre)
        bool enabled = true;
        juce::String samplePath;
        juce::Colour colour;
        bool hasColour = false;
        Plugin plugin;
        std::vector<int> children;          // layer channels
        std::vector<AutoPoint> points;      // automation channels

        juce::String displayName() const
        {
            if (plugin.name.isNotEmpty()) return plugin.name;
            if (plugin.vst.name.isNotEmpty()) return plugin.vst.name;
            if (samplePath.isNotEmpty())  return samplePath.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false).upToLastOccurrenceOf (".", false, false);
            return plugin.internalName;
        }
    };

    struct Note
    {
        int position = 0, length = 0, channel = 0, key = 60, velocity = 100, pan = 64, finePitch = 120;
    };

    struct Pattern
    {
        int iid = 0, length = 0;            // length in ticks, 0 = auto
        juce::String name;
        juce::Colour colour;
        bool hasColour = false;
        std::vector<Note> notes;
    };

    struct PlaylistItem
    {
        int position = 0, length = 0, track = 0, channel = -1, pattern = -1;  // ticks; channel item (audio / automation) or pattern
        bool muted = false;
        float startOffset = -1.0f, endOffset = -1.0f;   // channel items: beats (-1 = none)
        int startTicks = -1, endTicks = -1;             // pattern items: ticks (-1 = none)
    };

    struct Marker { int position = 0, numerator = 0, denominator = 0; bool signature = false; juce::String name; };

    struct Arrangement
    {
        int iid = 0;
        juce::String name;
        std::vector<PlaylistItem> items;
        std::vector<juce::String> trackNames;
        std::vector<Marker> markers;
    };

    struct Slot { int index = 0; bool enabled = true; Plugin plugin; };

    struct Insert
    {
        juce::String name;
        juce::uint32 flags = 0;
        bool hasFlags = false;
        int volume = 12800, pan = 0;        // 0..16000 (12800 = 0 dB), -6400..6400
        std::vector<Slot> slots;
        std::vector<int> routes;            // target insert indexes (0 = master)
        std::map<int, int> routeLevels;
        bool enabled() const { return ! hasFlags || (flags & (1u << 3)) != 0; }
    };

    // A link from a controller (automation clip) to a parameter. Undocumented beyond the
    // parameter / destination fields, see FlpImport for how it's interpreted.
    struct RemoteController { int ownerChannel = -1, source = -1, param = 0, dest = 0; bool vst = false; };

    struct Project
    {
        int format = 0, channelCount = 0, ppq = 96;
        juce::String version, title, artists, comments, genre;
        int versionMajor = 0, versionMinor = 0;
        double tempo = 140.0;
        int timeSigNum = 4, timeSigBeat = 4, swing = 0;   // swing 0..128
        std::vector<Channel> channels;
        std::vector<Pattern> patterns;
        std::vector<Arrangement> arrangements;
        int currentArrangement = 0;
        std::vector<Insert> inserts;        // [0] = master
        std::vector<RemoteController> controllers;
        int numEvents = 0;

        const Channel* findChannel (int iid) const
        {
            for (auto& c : channels) if (c.iid == iid) return &c;
            return nullptr;
        }

        const Arrangement* getCurrentArrangement() const
        {
            for (auto& a : arrangements) if (a.iid == currentArrangement) return &a;
            return arrangements.empty() ? nullptr : &arrangements.front();
        }
    };

    inline bool isValidPpq (int ppq)
    {
        for (int v : { 24, 48, 72, 96, 120, 144, 168, 192, 384, 768, 960 })
            if (v == ppq) return true;
        return false;
    }

    inline bool isValidFormat (int f)
    {
        for (int v : { -1, 0, 0x10, 24, 0x20, 0x30, 0x31, 0x32, 0x40, 0x50 })
            if (v == f) return true;
        return false;
    }

    inline bool isTextEvent (int id) { return (id >= 192 && id < 208) || id == Id::groupName || id == Id::trackName || id == Id::arrName; }

    // Splits the file into events. Anything malformed rejects the whole file.
    inline juce::Result readEvents (Bytes file, Project& p, std::vector<Event>& events)
    {
        if (file.size < 22 || std::memcmp (file.data, "FLhd", 4) != 0)  return juce::Result::fail ("Not an FL Studio project (header magic missing)");
        if (file.u32 (4) != 6)                                       return juce::Result::fail ("Corrupted header (unexpected header size)");
        p.format = (juce::int16) file.u16 (8);
        p.channelCount = (int) file.u16 (10);
        p.ppq = (int) file.u16 (12);
        if (! isValidFormat (p.format))                              return juce::Result::fail ("Unsupported FL Studio file format " + juce::String (p.format));
        if (! isValidPpq (p.ppq))                                    return juce::Result::fail ("Corrupted header (invalid PPQ " + juce::String (p.ppq) + ")");
        if (std::memcmp (file.data + 14, "FLdt", 4) != 0)            return juce::Result::fail ("Corrupted file (data chunk missing)");
        if ((size_t) file.u32 (18) != file.size - 22)                return juce::Result::fail ("Corrupted file (data chunk size doesn't match the file)");

        size_t pos = 22;
        while (pos < file.size)
        {
            Event e { (int) file.data[pos++], 0, 0, 0 };
            size_t n = e.id < 64 ? 1 : e.id < 128 ? 2 : e.id < 192 ? 4 : 0;
            if (e.id >= 192)
            {
                juce::uint64 size = 0;
                for (int shift = 0;; shift += 7)
                {
                    if (pos >= file.size || shift > 28)
                        return juce::Result::fail ("Corrupted event size at byte " + juce::String ((juce::int64) pos));
                    const auto b = file.data[pos++];
                    size |= (juce::uint64) (b & 0x7f) << shift;
                    if ((b & 0x80) == 0) break;
                }
                n = (size_t) size;
            }
            if (! file.has (pos, n))
                return juce::Result::fail ("Corrupted file (event runs past the end at byte " + juce::String ((juce::int64) pos) + ")");
            e.offset = pos;
            e.size = n;
            e.value = e.id < 64 ? file.u8 (pos) : e.id < 128 ? file.u16 (pos) : e.id < 192 ? file.u32 (pos) : 0u;
            events.push_back (e);
            pos += n;
        }
        if (events.empty())
            return juce::Result::fail ("Empty project");
        return juce::Result::ok();
    }

    inline juce::String decodeText (Bytes b, bool unicode)
    {
        juce::String s;
        if (unicode)
        {
            std::u16string u;
            for (size_t i = 0; i + 1 < b.size; i += 2)
                u.push_back ((char16_t) b.u16 (i));
            while (! u.empty() && u.back() == 0) u.pop_back();
            for (auto& c : u) if (c == 0) c = ' ';
            s = juce::String (juce::CharPointer_UTF16 ((const juce::CharPointer_UTF16::CharType*) u.c_str()));
        }
        else
        {
            for (size_t i = 0; i < b.size && b.data[i] != 0; ++i)
                s += (juce::juce_wchar) b.data[i];   // Latin-1 / ANSI
        }
        return s;
    }

    inline juce::Colour colourFrom (juce::uint32 rgba) { return juce::Colour ((juce::uint8) (rgba & 0xff), (juce::uint8) ((rgba >> 8) & 0xff), (juce::uint8) ((rgba >> 16) & 0xff)); }

    // The wrapper's data is a list of { u32 id, u64 size, bytes } records after a u32 kind.
    inline Vst parseWrapper (Bytes b)
    {
        Vst v;
        size_t pos = 4;
        while (b.has (pos, 12))
        {
            const auto id = b.u32 (pos);
            const auto size = b.u64 (pos + 4);
            pos += 12;
            if (size > b.size - pos)
                break;
            const auto chunk = b.sub (pos, (size_t) size);
            auto text = [&]
            {
                const auto* c = (const char*) chunk.data;
                const auto t = juce::CharPointer_UTF8::isValidString (c, (int) chunk.size) ? juce::String::fromUTF8 (c, (int) chunk.size) : decodeText (chunk, false);
                return t.trimCharactersAtEnd (juce::String::charToString (0));
            };
            switch (id)
            {
                case 51: v.fourcc = text(); break;
                case 52: v.guid = chunk.copy(); break;
                case 53: v.state = chunk.copy(); break;
                case 54: v.name = text(); break;
                case 55: v.path = text(); break;
                case 56: v.vendor = text(); break;
                default: break;
            }
            pos += (size_t) size;
        }
        return v;
    }

    inline juce::Result read (const juce::MemoryBlock& bytes, Project& p)
    {
        const Bytes file { (const juce::uint8*) bytes.getData(), bytes.getSize() };
        std::vector<Event> events;
        if (auto r = readEvents (file, p, events); r.failed())
            return r;
        p.numEvents = (int) events.size();

        bool unicode = true;
        auto data = [&] (const Event& e) { return file.sub (e.offset, e.size); };
        auto text = [&] (const Event& e) { return decodeText (data (e), unicode); };

        Pattern* pattern = nullptr;
        Arrangement* arrangement = nullptr;
        Insert* insert = nullptr;
        bool inMixer = false;
        Plugin slotPlugin;
        std::map<int, size_t> patternIndex;

        auto currentChannel = [&]() -> Channel* { return p.channels.empty() ? nullptr : &p.channels.back(); };

        for (auto& e : events)
        {
            const int id = e.id;
            const auto v = e.value;

            if (id == Id::version)
            {
                p.version = decodeText (data (e), false);
                auto parts = juce::StringArray::fromTokens (p.version, ".", {});
                p.versionMajor = parts[0].getIntValue();
                p.versionMinor = parts[1].getIntValue();
                unicode = p.versionMajor > 11 || (p.versionMajor == 11 && p.versionMinor >= 5);
                continue;
            }

            // Mixer section: inserts end with their output event; slot plugin events precede the slot index.
            const bool insertEvent = id == Id::insertName || id == Id::insertFlags || id == Id::slotIndex || id == Id::insertRouting
                                  || id == Id::insertInput || id == Id::insertOutput || id == Id::insertColour;
            if (insertEvent)
                inMixer = true;
            if (inMixer && (insertEvent || id == Id::pluginInternalName || id == Id::pluginName || id == Id::pluginWrapper || id == Id::pluginData))
            {
                if (insert == nullptr)
                {
                    p.inserts.emplace_back();
                    insert = &p.inserts.back();
                }
                switch (id)
                {
                    case Id::insertName:          insert->name = text (e); break;
                    case Id::insertFlags:         insert->flags = data (e).u32 (4); insert->hasFlags = true; break;
                    case Id::insertRouting:       for (size_t i = 0; i < e.size; ++i) if (file.data[e.offset + i] != 0) insert->routes.push_back ((int) i); break;
                    case Id::insertOutput:        insert = nullptr; break;
                    case Id::pluginInternalName:  slotPlugin.internalName = text (e); break;
                    case Id::pluginName:          slotPlugin.name = text (e); break;
                    case Id::pluginData:          slotPlugin.data = data (e).copy(); break;
                    case Id::slotIndex:
                        if (! slotPlugin.isEmpty())
                        {
                            Slot s;
                            s.index = (int) v;
                            s.plugin = std::move (slotPlugin);
                            if (s.plugin.isWrapper())
                                s.plugin.vst = parseWrapper ({ (const juce::uint8*) s.plugin.data.getData(), s.plugin.data.getSize() });
                            insert->slots.push_back (std::move (s));
                        }
                        slotPlugin = {};
                        break;
                    default: break;
                }
                continue;
            }

            switch (id)
            {
                case Id::title:      p.title = text (e); break;
                case Id::comments:   p.comments = text (e); break;
                case Id::artists:    p.artists = text (e); break;
                case Id::genre:      p.genre = text (e); break;
                case Id::tempo:      p.tempo = v / 1000.0; break;
                case Id::tempoCoarse: p.tempo = v; break;
                case Id::tempoFine:  p.tempo = std::floor (p.tempo) + v / 1000.0; break;
                case Id::timeSigNum: p.timeSigNum = (int) v; break;
                case Id::timeSigBeat: p.timeSigBeat = (int) v; break;
                case Id::rackSwing:  p.swing = (int) v; break;

                case Id::chanNew:
                    p.channels.emplace_back();
                    p.channels.back().iid = (int) v;
                    break;

                case Id::patNew:
                    if (auto it = patternIndex.find ((int) v); it != patternIndex.end())
                        pattern = &p.patterns[it->second];
                    else
                    {
                        patternIndex[(int) v] = p.patterns.size();
                        p.patterns.emplace_back();
                        p.patterns.back().iid = (int) v;
                        pattern = &p.patterns.back();
                    }
                    break;
                case Id::patName:    if (pattern != nullptr) pattern->name = text (e); break;
                case Id::patLength:  if (pattern != nullptr) pattern->length = (int) v; break;
                case Id::patColour:  if (pattern != nullptr) { pattern->colour = colourFrom (v); pattern->hasColour = true; } break;
                case Id::patNotes:
                    if (pattern != nullptr)
                        for (size_t o = 0; o + 24 <= e.size; o += 24)
                        {
                            const auto n = file.sub (e.offset + o, 24);
                            pattern->notes.push_back ({ (int) n.u32 (0), (int) n.u32 (8), (int) n.u16 (6), (int) n.u16 (12),
                                                        (int) n.u8 (21), (int) n.u8 (20), (int) n.u8 (16) });
                        }
                    break;

                case Id::arrNew:
                    p.arrangements.emplace_back();
                    p.arrangements.back().iid = (int) v;
                    arrangement = &p.arrangements.back();
                    break;
                case Id::arrCurrent: p.currentArrangement = (int) v; break;
                case Id::arrName:    if (arrangement != nullptr) arrangement->name = text (e); break;
                case Id::trackData:  if (arrangement != nullptr) arrangement->trackNames.emplace_back(); break;
                case Id::trackName:  if (arrangement != nullptr && ! arrangement->trackNames.empty()) arrangement->trackNames.back() = text (e); break;
                case Id::playlist:
                    if (arrangement != nullptr)
                    {
                        const bool wide = e.size % 60 == 0 && (e.size % 32 != 0 || p.versionMajor >= 21);
                        const size_t stride = wide ? 60 : 32;
                        const int maxTrack = p.versionMajor > 12 || (p.versionMajor == 12 && p.versionMinor >= 9) ? 499 : 198;
                        for (size_t o = 0; o + stride <= e.size; o += stride)
                        {
                            const auto it = file.sub (e.offset + o, stride);
                            PlaylistItem item;
                            item.position = (int) it.u32 (0);
                            item.length = (int) it.u32 (8);
                            item.track = juce::jmax (0, maxTrack - (int) it.u16 (12));
                            const int base = (int) it.u16 (4), index = (int) it.u16 (6);
                            if (index > base) item.pattern = index - base;
                            else              item.channel = index;
                            // ponytail: muted = item flag 0x2000 is unverified (no muted clip in the test files).
                            item.muted = (it.u16 (18) & 0x2000) != 0;
                            item.startOffset = it.f32 (24);
                            item.endOffset = it.f32 (28);
                            item.startTicks = it.i32 (24);
                            item.endTicks = it.i32 (28);
                            arrangement->items.push_back (item);
                        }
                    }
                    break;

                case Id::markerPos:
                {
                    Marker m;
                    m.signature = v >= 134217728u;
                    m.position = (int) (m.signature ? v - 134217728u : v);
                    if (arrangement != nullptr) arrangement->markers.push_back (m);
                    break;
                }
                case Id::markerNum:  if (arrangement != nullptr && ! arrangement->markers.empty()) arrangement->markers.back().numerator = (int) v; break;
                case Id::markerDen:  if (arrangement != nullptr && ! arrangement->markers.empty()) arrangement->markers.back().denominator = (int) v; break;
                case Id::markerName: if (arrangement != nullptr && ! arrangement->markers.empty()) arrangement->markers.back().name = text (e); break;

                case Id::mixerParams:
                    for (size_t o = 0; o + 12 <= e.size; o += 12)
                    {
                        const auto it = file.sub (e.offset + o, 12);
                        const int param = (int) it.u8 (4), where = (int) it.u16 (6), msg = it.i32 (8);
                        const int insertIndex = (where >> 6) & 0x7f, slotIndex = where & 0x3f;
                        if (insertIndex >= (int) p.inserts.size())
                            continue;
                        auto& ins = p.inserts[(size_t) insertIndex];
                        if (param == 0)
                        {
                            for (auto& s : ins.slots) if (s.index == slotIndex) s.enabled = msg != 0;
                        }
                        else if (param >= 64 && param < 192) ins.routeLevels[param - 64] = msg;
                        else if (param == 192)               ins.volume = msg;
                        else if (param == 193)               ins.pan = msg;
                    }
                    break;

                case Id::remoteCtrl:
                {
                    const auto d = data (e);
                    RemoteController rc;
                    rc.ownerChannel = currentChannel() != nullptr ? currentChannel()->iid : -1;
                    rc.source = (int) d.u16 (0);
                    rc.param = (int) (d.u16 (4) & 0x7fff);
                    rc.vst = (d.u16 (4) & 0x8000) != 0;
                    rc.dest = (int) (juce::int16) d.u16 (6);
                    p.controllers.push_back (rc);
                    break;
                }

                default:
                {
                    auto* channel = currentChannel();
                    if (channel == nullptr)
                        break;
                    switch (id)
                    {
                        case Id::chanType:           channel->type = (int) v; break;
                        case Id::chanEnabled:        channel->enabled = v != 0; break;
                        case Id::chanRoutedTo:       channel->insert = (int) (juce::int8) v; break;
                        case Id::chanRootNote:       channel->rootNote = (int) v; break;
                        case Id::chanVolWord:        channel->volume = (int) v; break;
                        case Id::chanPanWord:        channel->pan = (int) v; break;
                        case Id::chanChild:          channel->children.push_back ((int) v); break;
                        case Id::samplePath:         channel->samplePath = text (e); break;
                        case Id::chanNameOld:        if (channel->plugin.name.isEmpty()) channel->plugin.name = text (e); break;
                        case Id::pluginName:         channel->plugin.name = text (e); break;
                        case Id::pluginInternalName: channel->plugin.internalName = text (e); break;
                        case Id::pluginColour:       channel->colour = colourFrom (v); channel->hasColour = true; break;
                        case Id::pluginData:
                            channel->plugin.data = data (e).copy();
                            if (channel->plugin.isWrapper())
                                channel->plugin.vst = parseWrapper (data (e));
                            break;
                        case Id::chanLevels:
                            if (e.size >= 8)
                            {
                                channel->pan = data (e).i32 (0);
                                channel->volume = data (e).i32 (4);
                            }
                            break;
                        case Id::automation:
                        {
                            const auto d = data (e);
                            const auto count = (size_t) d.u32 (17);
                            double pos = 0.0;
                            for (size_t i = 0; i < count && d.has (21 + i * 24, 24); ++i)
                            {
                                const size_t o = 21 + i * 24;
                                pos += d.f64 (o);
                                if (std::isfinite (pos) && std::isfinite (d.f64 (o + 8)))
                                    channel->points.push_back ({ pos, d.f64 (o + 8), d.f32 (o + 16) });
                            }
                            break;
                        }
                        default: break;
                    }
                    break;
                }
            }
        }

        return juce::Result::ok();
    }

    inline juce::Result readFile (const juce::File& f, Project& p)
    {
        if (! f.existsAsFile())
            return juce::Result::fail ("File not found: " + f.getFullPathName());
        if (f.getSize() > (juce::int64) 1024 * 1024 * 1024)
            return juce::Result::fail ("File too large");
        juce::MemoryBlock mb;
        if (! f.loadFileAsData (mb))
            return juce::Result::fail ("Couldn't read " + f.getFullPathName());
        return read (mb, p);
    }
}
