#pragma once

#include "DrumKit.h"
#include "MultiSampler.h"
#include "SynthPresets.h"
#include <regex>

namespace te = tracktion;

// FL-style project model, stored as a ValueTree inside the Tracktion Edit so it
// saves/loads/undoes together with the engine state.
//
//   STUDIO (currentPattern, songMode, swing)
//     CHANNELS / CHANNEL (id, name, type, track, file, colour)
//     PATTERNS / PATTERN (id, name, colour) / NOTE (channel, start, length, key, velocity)
//     INSERTS / INSERT  (index, name, track)  -- mixer inserts; index 0 = master
//     PLAYLIST / PCLIP   (id, type, pattern | channel, lane, start, length, offset, muted)  -- beats
//       type "automation": (plugin, param, name) / POINT (pos beats in clip, value 0..1, tension)
//     MAPPINGS / MAP (cc, channel, plugin, param)  -- MIDI controller links
//     TEMPOS / TEMPOCHANGE (pos beats, bpm)  -- tempo changes after the start (the start tempo is the engine's tempo 0)
//   PCLIP audio: (stretch = follow tempo, origBpm, pitch semitones)   CHANNEL: (arp, arpRate steps, arpOctaves, arpGate %)
//
// Each channel = one Tracktion AudioTrack holding its instrument + one "derived" MidiClip.
// The derived clip's notes are regenerated from the model (patterns + playlist) whenever the
// model changes. Derived changes use no UndoManager: undo restores the model, regen follows.
namespace IDs
{
   #define STUDIO_ID(name) inline const juce::Identifier name (#name);
    STUDIO_ID (STUDIO) STUDIO_ID (CHANNELS) STUDIO_ID (CHANNEL) STUDIO_ID (PATTERNS) STUDIO_ID (PATTERN)
    STUDIO_ID (NOTE) STUDIO_ID (PLAYLIST) STUDIO_ID (PCLIP)
    STUDIO_ID (id) STUDIO_ID (name) STUDIO_ID (type) STUDIO_ID (track) STUDIO_ID (file) STUDIO_ID (colour)
    STUDIO_ID (channel) STUDIO_ID (start) STUDIO_ID (length) STUDIO_ID (key) STUDIO_ID (velocity)
    STUDIO_ID (pattern) STUDIO_ID (lane) STUDIO_ID (currentPattern) STUDIO_ID (songMode) STUDIO_ID (swing)
    STUDIO_ID (derived) STUDIO_ID (nextId) STUDIO_ID (offset) STUDIO_ID (muted) STUDIO_ID (loopStart) STUDIO_ID (loopEnd)
    STUDIO_ID (clipId) STUDIO_ID (INSERTS) STUDIO_ID (INSERT) STUDIO_ID (insert) STUDIO_ID (index)
    STUDIO_ID (POINT) STUDIO_ID (pos) STUDIO_ID (value) STUDIO_ID (tension) STUDIO_ID (plugin) STUDIO_ID (param)
    STUDIO_ID (MAPPINGS) STUDIO_ID (MAP) STUDIO_ID (cc)
    STUDIO_ID (TEMPOS) STUDIO_ID (TEMPOCHANGE) STUDIO_ID (bpm) STUDIO_ID (stretch) STUDIO_ID (pitch) STUDIO_ID (origBpm)
    STUDIO_ID (arp) STUDIO_ID (arpRate) STUDIO_ID (arpOctaves) STUDIO_ID (arpGate)
   #undef STUDIO_ID
}

namespace ChannelType
{
    inline const juce::String sampler = "sampler";
    inline const juce::String synth   = "synth";
    inline const juce::String audio   = "audio";   // FL "audio clip" channel: plays playlist audio clips
    inline const juce::String plugin  = "plugin";  // third-party instrument
    inline const juce::String automation = "automation"; // playlist automation clip type
    inline const juce::String instrument = "instrument"; // MultiSampler: SFZ / SoundFont / WAV / slicer
    inline const juce::String drumpad = "drumpad";       // MultiSampler with 16 pads
    inline const juce::String fm = "fm";                 // our FM Synth
}

// FL-style arpeggiator. Applied when a channel's derived MIDI clip is generated, so it's
// deterministic and renders/exports like any other notes.
namespace Arp
{
    enum Mode { off = 0, up, down, upDown, random };
    inline juce::StringArray modeNames() { return { "Off", "Up", "Down", "Up-down", "Random" }; }

    struct Note { double start, length; int key, velocity; };

    // Every `rate` beats, plays the next of the currently held keys (spread over `octaves`).
    inline std::vector<Note> apply (const std::vector<Note>& held, int mode, double rate, int octaves, double gate, int seed)
    {
        if (mode == off || held.empty() || rate <= 0.0)
            return held;

        double first = 1.0e12, last = 0.0;
        for (auto& n : held)
        {
            first = juce::jmin (first, n.start);
            last = juce::jmax (last, n.start + n.length);
        }

        std::vector<Note> out;
        std::vector<int> previous;
        juce::Random rng (seed);
        int index = 0;
        for (auto step = std::ceil (first / rate - 1.0e-6); step * rate < last - 1.0e-6; step += 1.0)
        {
            const double t = step * rate;
            std::vector<int> keys;
            int velocity = 1;
            double end = t;
            for (auto& n : held)
                if (n.start <= t + 1.0e-6 && n.start + n.length > t + 1.0e-6)
                {
                    keys.push_back (n.key);
                    velocity = juce::jmax (velocity, n.velocity);
                    end = juce::jmax (end, n.start + n.length);
                }

            std::sort (keys.begin(), keys.end());
            keys.erase (std::unique (keys.begin(), keys.end()), keys.end());
            if (keys != previous)
                index = 0; // new chord: start the pattern again
            previous = keys;
            if (keys.empty())
                continue;

            std::vector<int> sequence;
            for (int octave = 0; octave < juce::jlimit (1, 4, octaves); ++octave)
                for (int k : keys)
                    if (k + 12 * octave <= 127)
                        sequence.push_back (k + 12 * octave);
            if (mode == down)
                std::reverse (sequence.begin(), sequence.end());
            else if (mode == upDown)
                for (int i = (int) sequence.size() - 2; i >= 1; --i)
                    sequence.push_back (sequence[(size_t) i]);

            const int key = mode == random ? sequence[(size_t) rng.nextInt ((int) sequence.size())]
                                           : sequence[(size_t) (index % (int) sequence.size())];
            ++index;
            out.push_back ({ t, juce::jmin (rate * juce::jlimit (0.05, 1.0, gate), end - t), key, velocity });
        }
        return out;
    }
}

class Studio : private juce::ValueTree::Listener,
               private juce::AsyncUpdater,
               private juce::ChangeListener
{
public:
    static constexpr double stepBeats = 0.25;   // one step = 1/16 note
    static constexpr int defaultRootNote = 60;  // FL's C5

    explicit Studio (te::Engine& e) : engine (e)
    {
        DrumKit::ensureDefaultKit();
        DrumKit::ensureLibrary();
        MultiSampler::getInstrumentsFolder().createDirectory();
        setupMidiInputs();
        newProject();
    }

    ~Studio() override
    {
        cancelPendingUpdate();
        if (edit != nullptr)
        {
            studio.removeListener (this);
            edit->getUndoManager().removeChangeListener (this);
        }
    }

    static inline const juce::String fileExtension = ".usp";

    static juce::File getProjectsFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("Untitled Studio").getChildFile ("Projects");
    }

    te::Engine& engine;
    te::Edit& getEdit() const                  { return *edit; }
    te::TransportControl& transport() const    { return edit->getTransport(); }
    juce::UndoManager& undo() const            { return edit->getUndoManager(); }
    juce::ValueTree getState() const           { return studio; }

    // Bumped on every model change. UI timers compare against it to know when to repaint.
    int getChangeCount() const noexcept        { return changeCount; }

    //==========================================================================
    // Project

    // FL's default template: Kick, Clap, Hat, Snare.
    void newProject() { newProjectFromTemplate ("Basic beat"); }

    static juce::StringArray getTemplateNames() { return { "Empty", "Basic beat", "Trap", "House", "Lo-fi" }; }

    static juce::File getTemplatesFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("Untitled Studio").getChildFile ("Templates");
    }

    // Built-in templates are written in code; the result is a clean, untitled project.
    void newProjectFromTemplate (const juce::String& name)
    {
        setEdit (te::createEmptyEdit (engine, engine.getTemporaryFileManager().getTempDirectory().getChildFile ("Untitled.tracktionedit")));

        for (auto t : te::getAudioTracks (*edit))
            edit->deleteTrack (t);

        studio.setProperty (IDs::swing, 0.0, nullptr);
        ensureInserts();
        const int patternId = addPattern ("Pattern 1");
        setCurrentPattern (patternId);
        auto pattern = getPattern (patternId);

        auto sample = [this] (const char* file) { return addSamplerChannel (DrumKit::getDrumsFolder().getChildFile (juce::String (file) + ".wav")); };
        auto steps = [this, &pattern] (int channel, std::initializer_list<int> list, int key = defaultRootNote, int velocity = 100)
        {
            for (int step : list)
                addNote (pattern, channel, step * stepBeats, stepBeats, key, velocity);
        };

        if (name == "Trap")
        {
            setBpm (140.0);
            const int kick = sample ("Kick"), sub = addPresetChannel ("808 Sub"), snare = sample ("Snare"), hat = sample ("Hat");
            steps (kick, { 0, 10, 16, 19, 26 });
            steps (snare, { 8, 24 });
            for (int s = 0; s < 32; s += 2)
                steps (hat, { s }, defaultRootNote, s % 4 == 0 ? 110 : 80);
            steps (hat, { 13, 15, 29, 31 }, defaultRootNote, 70); // rolls
            for (auto [start, length, key] : { std::tuple { 0, 10, 36 }, { 10, 6, 36 }, { 16, 3, 39 }, { 19, 7, 34 }, { 26, 6, 36 } })
                addNote (pattern, sub, start * stepBeats, length * stepBeats, key, 110);
        }
        else if (name == "House")
        {
            setBpm (124.0);
            const int kick = sample ("Kick"), clap = sample ("Clap"), hat = sample ("Hat"), open = sample ("Open Hat"), bass = addPresetChannel ("Saw Bass");
            steps (kick, { 0, 4, 8, 12, 16, 20, 24, 28 });
            steps (clap, { 4, 12, 20, 28 });
            steps (open, { 2, 6, 10, 14, 18, 22, 26, 30 });
            for (int s = 0; s < 32; ++s)
                if (s % 4 != 2)
                    steps (hat, { s }, defaultRootNote, s % 2 == 0 ? 90 : 60);
            for (auto [step, key] : { std::pair { 2, 36 }, { 6, 36 }, { 10, 39 }, { 14, 36 }, { 18, 41 }, { 22, 41 }, { 26, 39 }, { 30, 34 } })
                addNote (pattern, bass, step * stepBeats, 2 * stepBeats, key, 100);
        }
        else if (name == "Lo-fi")
        {
            setBpm (85.0);
            studio.setProperty (IDs::swing, 0.5, nullptr);
            const int kick = sample ("Kick"), snare = sample ("Snare"), hat = sample ("Hat"), keys = addPresetChannel ("E-Piano");
            steps (kick, { 0, 7, 10, 16, 23, 26 });
            steps (snare, { 4, 12, 20, 28 });
            for (int s = 0; s < 32; s += 2)
                steps (hat, { s }, defaultRootNote, s % 4 == 0 ? 90 : 65);
            for (auto [bar, chord] : { std::pair { 0, std::array { 50, 53, 57, 60 } }, { 1, std::array { 48, 52, 55, 59 } } })
                for (int key : chord)
                    addNote (pattern, keys, bar * 4.0, 3.75, key, 80);
        }
        else if (name != "Empty")
        {
            for (auto file : { "Kick", "Clap", "Hat", "Snare" })
                sample (file);
        }

        projectFile = juce::File();
        startsDirty = false;
        finishLoading();
    }

    // Writes the whole project (engine state + FL model) to one file.
    bool saveTo (const juce::File& file)
    {
        regenerateNow();
        edit->flushState();
        file.getParentDirectory().createDirectory();

        te::EditFileOperations ops (*edit);
        if (! ops.writeToFile (file, false))
            return false;

        projectFile = file;
        dirty = false;
        return true;
    }

    // Saves without changing the project's file or dirty state (autosave).
    bool saveCopyTo (const juce::File& file)
    {
        regenerateNow();
        edit->flushState();
        file.getParentDirectory().createDirectory();
        te::EditFileOperations ops (*edit);
        return ops.writeToFile (file, false);
    }

    // asUntitled: used for crash recovery, so saving asks for a new location.
    bool load (const juce::File& file, bool asUntitled = false)
    {
        // Only hand Tracktion something that is one of our projects.
        if (auto xml = juce::parseXML (file); xml == nullptr || ! xml->hasTagName ("EDIT") || xml->getChildByName (IDs::STUDIO.toString()) == nullptr)
            return false;

        auto loaded = te::loadEditFromFile (engine, file);
        if (loaded == nullptr || ! loaded->state.getChildWithName (IDs::STUDIO).isValid())
            return false;

        sanitise (loaded->state.getChildWithName (IDs::STUDIO));
        setEdit (std::move (loaded));
        projectFile = asUntitled ? juce::File() : file;
        startsDirty = asUntitled; // recovered autosave: unsaved until saved somewhere
        finishLoading();
        findMissingFiles();
        return true;
    }

    // After building a project from an import (on top of newProjectFromTemplate ("Empty")): one
    // clean starting point, untitled and unsaved.
    void finishImport()
    {
        selectChannel (getChannels().getChild (0)[IDs::id]);
        regenerateNow();
        undo().clearUndoHistory();
        projectFile = juce::File();
        startsDirty = dirty = true;
    }

    static bool isFlpFile (const juce::String& path) { return path.endsWithIgnoreCase (".flp"); }

    // Samples / plugins the last loaded project refers to that aren't there any more.
    const juce::StringArray& getMissingFiles() const { return missingFiles; }

    // Clamps a loaded (maybe hand-edited or corrupted) model into ranges the app can handle.
    static void sanitise (juce::ValueTree studioState)
    {
        constexpr double maxBeats = 4.0 * 9999.0;
        int maxId = 0;
        std::function<void (juce::ValueTree)> walk = [&] (juce::ValueTree v)
        {
            auto clamp = [&v] (const juce::Identifier& id, double lo, double hi)
            {
                if (! v.hasProperty (id))
                    return;
                const double x = v[id];
                const double fixed = std::isfinite (x) ? juce::jlimit (lo, hi, x) : lo;
                if (! juce::exactlyEqual (fixed, x))
                    v.setProperty (id, fixed, nullptr);
            };

            if (v.hasType (IDs::NOTE))
            {
                clamp (IDs::start, 0.0, maxBeats);       clamp (IDs::length, 1.0 / 64.0, maxBeats);
                clamp (IDs::key, 0.0, 127.0);            clamp (IDs::velocity, 1.0, 127.0);
            }
            else if (v.hasType (IDs::PCLIP))
            {
                clamp (IDs::start, 0.0, maxBeats);       clamp (IDs::length, 1.0 / 64.0, maxBeats);
                clamp (IDs::offset, 0.0, maxBeats);      clamp (IDs::lane, 0.0, 499.0);
                clamp (IDs::pitch, -24.0, 24.0);         clamp (IDs::origBpm, 0.0, 522.0);
            }
            else if (v.hasType (IDs::POINT))
            {
                clamp (IDs::pos, 0.0, maxBeats);         clamp (IDs::value, 0.0, 1.0);    clamp (IDs::tension, -1.0, 1.0);
            }
            else if (v.hasType (IDs::CHANNEL))
            {
                clamp (IDs::arp, 0.0, 4.0);              clamp (IDs::arpRate, 0.25, 64.0);
                clamp (IDs::arpOctaves, 1.0, 4.0);       clamp (IDs::arpGate, 5.0, 100.0);
                clamp (IDs::insert, 0.0, numInserts);
            }
            else if (v.hasType (IDs::TEMPOCHANGE))
            {
                clamp (IDs::pos, stepBeats, maxBeats);   clamp (IDs::bpm, 10.0, 522.0);
            }
            else if (v.hasType (IDs::STUDIO))
            {
                clamp (IDs::swing, 0.0, 1.0);            clamp (IDs::loopStart, 0.0, maxBeats);   clamp (IDs::loopEnd, 0.0, maxBeats);
            }

            if (v.hasProperty (IDs::id))
                maxId = juce::jmax (maxId, (int) v[IDs::id]);
            for (auto child : v)
                walk (child);
        };
        walk (studioState);

        if ((int) studioState[IDs::nextId] <= maxId)
            studioState.setProperty (IDs::nextId, maxId + 1, nullptr);
    }

    juce::File getProjectFile() const   { return projectFile; }
    bool isDirty() const                { return dirty; }
    int getEditGeneration() const       { return editGeneration; }

    //==========================================================================
    // Channels

    juce::ValueTree getChannels() const          { return studio.getChildWithName (IDs::CHANNELS); }

    juce::ValueTree getChannel (int channelId) const
    {
        return getChannels().getChildWithProperty (IDs::id, channelId);
    }

    te::AudioTrack* getTrack (const juce::ValueTree& channel) const
    {
        return te::findAudioTrackForID (*edit, te::EditItemID::fromVar (channel[IDs::track]));
    }

    te::AudioTrack* getTrack (int channelId) const { return getTrack (getChannel (channelId)); }

    int addSamplerChannel (const juce::File& file)
    {
        undo().beginNewTransaction ("Add channel");
        auto track = createChannelTrack (file.getFileNameWithoutExtension());

        if (auto sampler = dynamic_cast<te::SamplerPlugin*> (edit->getPluginCache().createNewPlugin (te::SamplerPlugin::xmlTypeName, {}).get()))
        {
            track->pluginList.insertPlugin (*sampler, 0, nullptr);
            setSamplerFile (*sampler, file);
        }

        return addChannelState (*track, ChannelType::sampler, file.getFileNameWithoutExtension(), file);
    }

    int addSynthChannel()
    {
        undo().beginNewTransaction ("Add channel");
        auto track = createChannelTrack ("Synth");
        auto plugin = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});
        track->pluginList.insertPlugin (plugin, 0, nullptr);
        return addChannelState (*track, ChannelType::synth, "Synth", {});
    }

    int addFmSynthChannel()
    {
        undo().beginNewTransaction ("Add channel");
        auto track = createChannelTrack ("FM Synth");
        track->pluginList.insertPlugin (edit->getPluginCache().createNewPlugin (FMSynthPlugin::xmlTypeName, {}), 0, nullptr);
        return addChannelState (*track, ChannelType::fm, "FM Synth", {});
    }

    // FL users pick a sound, not an engine: makes the right synth channel with a factory preset.
    int addPresetChannel (const juce::String& presetName)
    {
        const auto sound = Presets::findSynthSound (presetName);
        if (! sound)
            return 0;

        const int id = sound->pluginType == FMSynthPlugin::xmlTypeName ? addFmSynthChannel() : addSynthChannel();
        if (auto p = getInstrument (id))
            Presets::apply (*p, *sound->preset);
        renameChannel (id, presetName);
        return id;
    }

    // Third-party instrument (VST3 / AU) as a channel.
    int addPluginChannel (const juce::PluginDescription& desc)
    {
        undo().beginNewTransaction ("Add plugin channel");
        auto track = createChannelTrack (desc.name);
        if (auto plugin = edit->getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, desc))
            track->pluginList.insertPlugin (plugin, 0, nullptr);
        return addChannelState (*track, ChannelType::plugin, desc.name, {});
    }

    // SFZ / SoundFont / audio file in our multi-sampler.
    int addInstrumentChannel (const juce::File& file)
    {
        undo().beginNewTransaction ("Add instrument");
        const auto name = file.getFileNameWithoutExtension();
        auto track = createChannelTrack (name);
        if (auto ms = createMultiSampler (*track))
            ms->loadFile (file);
        return addChannelState (*track, ChannelType::instrument, name, file);
    }

    // FL "FPC"-style: 16 pads on notes 36..51, loaded with the default kit.
    int addDrumPadChannel()
    {
        undo().beginNewTransaction ("Add drum pad");
        auto track = createChannelTrack ("Drum pad");
        if (auto ms = createMultiSampler (*track))
            ms->setRegions (MultiSampler::defaultDrumPads());
        return addChannelState (*track, ChannelType::drumpad, "Drum pad", {});
    }

    // FL "Slicex"-style: chops a loop at its hits onto keys from C5 and writes a pattern replaying it.
    int addSlicerChannel (const juce::File& file)
    {
        MultiSampler::SampleCache cache (engine.getAudioFileFormatManager().readFormatManager);
        const auto audio = cache.get (file);
        if (audio.data == nullptr || audio.data->getNumSamples() == 0)
            return 0;

        const auto length = (juce::int64) audio.data->getNumSamples();
        const auto starts = MultiSampler::detectSlices (*audio.data, audio.sampleRate);
        const auto regions = MultiSampler::sliceRegions (file, starts, length);

        undo().beginNewTransaction ("Add slicer");
        const auto name = file.getFileNameWithoutExtension();
        auto track = createChannelTrack (name);
        if (auto ms = createMultiSampler (*track))
            ms->setRegions (regions);
        const int id = addChannelState (*track, ChannelType::instrument, name, file);

        const double beatsPerSample = getBpm() / 60.0 / audio.sampleRate;
        auto pattern = getCurrentPatternState();
        for (int i = 0; i < regions.size(); ++i)
        {
            const auto end = i + 1 < starts.size() ? starts[i + 1] : length;
            addNote (pattern, id, (double) starts[i] * beatsPerSample, (double) (end - starts[i]) * beatsPerSample, 60 + i, 100);
        }
        return id;
    }

    MultiSamplerPlugin* getMultiSampler (int channelId) const { return dynamic_cast<MultiSamplerPlugin*> (getInstrument (channelId)); }

    juce::Array<juce::PluginDescription> getKnownPlugins (bool instruments) const
    {
        juce::Array<juce::PluginDescription> result;
        for (auto& d : engine.getPluginManager().knownPluginList.getTypes())
            if (d.isInstrument == instruments)
                result.add (d);
        std::sort (result.begin(), result.end(), [] (auto& a, auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
        return result;
    }

    std::optional<juce::PluginDescription> findKnownPlugin (const juce::String& identifier) const
    {
        if (auto d = engine.getPluginManager().knownPluginList.getTypeForIdentifierString (identifier))
            return *d;
        return std::nullopt;
    }

    // First plugin on a channel's track (its instrument).
    te::Plugin* getInstrument (int channelId) const
    {
        if (auto t = getTrack (channelId))
            return t->pluginList.getPlugins().getFirst().get();
        return nullptr;
    }

    void replaceSample (int channelId, const juce::File& file)
    {
        auto channel = getChannel (channelId);
        if (auto track = getTrack (channel))
        {
            if (auto sampler = track->pluginList.findFirstPluginOfType<te::SamplerPlugin>())
            {
                undo().beginNewTransaction ("Replace sample");
                setSamplerFile (*sampler, file);
                channel.setProperty (IDs::file, file.getFullPathName(), &undo());
                renameChannel (channelId, file.getFileNameWithoutExtension());
            }
        }
    }

    void renameChannel (int channelId, const juce::String& newName)
    {
        auto channel = getChannel (channelId);
        channel.setProperty (IDs::name, newName, &undo());
        if (auto track = getTrack (channel))
            track->setName (newName);
    }

    void deleteChannel (int channelId)
    {
        auto channel = getChannel (channelId);
        if (! channel.isValid() || getChannels().getNumChildren() <= 1)
            return;

        undo().beginNewTransaction ("Delete channel");

        for (auto pattern : getPatterns())
            for (int i = pattern.getNumChildren(); --i >= 0;)
                if ((int) pattern.getChild (i)[IDs::channel] == channelId)
                    pattern.removeChild (i, &undo());

        if (auto track = getTrack (channel))
            edit->deleteTrack (track);

        getChannels().removeChild (channel, &undo());

        if (selectedChannel == channelId)
            selectChannel (getChannels().getChild (0)[IDs::id]);
    }

    int getSelectedChannel() const { return selectedChannel; }

    // Called just before a project is replaced (New / Open).
    std::vector<std::function<void()>> editAboutToChange;
    int newId()                    { return takeNextId(); }

    void selectChannel (int channelId)
    {
        selectedChannel = channelId;
        updateMidiInputTarget();
        ++changeCount;
    }

    // Plays a note on the selected channel (typing keyboard / on-screen keys).
    void playLiveNote (const juce::MidiMessage& m)
    {
        if (auto track = getTrack (selectedChannel))
            track->injectLiveMidiMessage (m, liveSourceId);
        captureNote (m);
    }

    //==========================================================================
    // Recording (FL: arm the record button, then play).

    struct RecordOptions
    {
        bool audio = true, notes = true, automation = true;
        bool countIn = true, punch = false, monitorInput = false;
    };

    RecordOptions recordOptions;

    static juce::File getRecordingsFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("Untitled Studio").getChildFile ("Recordings");
    }

    bool isRecording() const { return recording; }
    bool recordArmed = false;

    void startRecording()
    {
        if (recording)
            return;

        regenerateNow();
        recording = true;
        setRecordingAutomation (recordOptions.automation);
        pendingNotes.clear();
        recordStartBeats = isSongMode() ? std::floor (getPositionBeats() / getBarBeats()) * getBarBeats() : 0.0;
        recordPattern = {};

        edit->setCountInMode (recordOptions.countIn ? te::Edit::CountIn::oneBar : te::Edit::CountIn::none);
        edit->recordingPunchInOut = recordOptions.punch && getSongLoop().getLength() > 0.0;

        undo().beginNewTransaction ("Record");
        bool audioArmed = false;
        if (recordOptions.audio)
        {
            auto rec = getRecorderTrack (true);
            for (auto instance : edit->getAllInputDevices())
            {
                if (instance->getInputDevice().getDeviceType() != te::InputDevice::waveDevice)
                    continue;
                instance->getInputDevice().setMonitorMode (recordOptions.monitorInput ? te::InputDevice::MonitorMode::on
                                                                                        : te::InputDevice::MonitorMode::off);
                if (instance->setTarget (rec->itemID, true, nullptr, 0))
                {
                    instance->setRecordingEnabled (rec->itemID, true);
                    audioArmed = true;
                }
            }
        }

        if (audioArmed)
            transport().record (false);
        else
            transport().play (false);
    }

    void stopRecording()
    {
        if (! recording)
            return;

        recording = false;
        transport().stop (false, false);
        setRecordingAutomation (false);

        // Notes still held when stopping end now.
        for (auto& [key, held] : pendingNotes)
            addRecordedNote (key, held.first, getPositionBeats() - held.first, held.second);
        pendingNotes.clear();

        // Tracktion finishes writing the audio asynchronously; collect it shortly after.
        juce::Timer::callAfterDelay (400, [this] { harvestRecordedAudio(); });
    }

    // Turns clips recorded onto the hidden recorder track into playlist audio clips.
    int harvestRecordedAudio()
    {
        auto rec = getRecorderTrack (false);
        if (rec == nullptr)
            return 0;

        int harvested = 0;
        juce::Array<juce::ValueTree> done;
        int lane = -1;
        for (auto clip : rec->getClips())
        {
            auto wave = dynamic_cast<te::WaveAudioClip*> (clip);
            if (wave == nullptr)
                continue;

            const auto source = wave->getOriginalFile();
            if (! source.existsAsFile())
                continue;

            // Keep recordings with the user's music, not in a temp folder.
            getRecordingsFolder().createDirectory();
            auto dest = getRecordingsFolder().getNonexistentChildFile ("Recording " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S"), ".wav");
            source.copyFileTo (dest);

            auto& tempo = edit->tempoSequence;
            const auto pos = wave->getPosition();
            const double startBeats = tempo.toBeats (pos.getStart()).inBeats();
            const double lengthBeats = tempo.toBeats (pos.getEnd()).inBeats() - startBeats;
            if (lane < 0)
                lane = findFreeLane (startBeats, lengthBeats);

            if (auto pc = addAudioClip (dest, lane, startBeats); pc.isValid())
            {
                pc.setProperty (IDs::length, lengthBeats, nullptr);
                pc.setProperty (IDs::offset, pos.getOffset().inSeconds() * getBpm() / 60.0, nullptr);
                ++lane; // loop takes stack on the lanes below
                ++harvested;
            }
            done.add (clip->state);
        }

        for (auto& state : done)
            state.getParent().removeChild (state, nullptr);
        if (harvested > 0)
            setSongMode (true); // FL: recorded audio lands in the playlist
        return harvested;
    }

    // Notes from the typing keyboard, on-screen keys and MIDI keyboards while recording.
    void captureNote (const juce::MidiMessage& m)
    {
        if (! recording || ! recordOptions.notes || ! transport().isPlaying() || ! (m.isNoteOn() || m.isNoteOff()))
            return;

        double pos = getPositionBeats();
        if (! isSongMode())
            pos = std::fmod (pos, juce::jmax (1.0e-3, loopEndBeats));

        const int key = m.getNoteNumber();
        if (m.isNoteOn())
        {
            pendingNotes[key] = { pos, (int) m.getVelocity() };
        }
        else if (auto it = pendingNotes.find (key); it != pendingNotes.end())
        {
            double length = pos - it->second.first;
            if (length <= 0.0)
                length += loopEndBeats; // wrapped around the pattern loop
            addRecordedNote (key, it->second.first, length, it->second.second);
            pendingNotes.erase (it);
        }
    }

    void addRecordedNote (int key, double start, double length, int velocity)
    {
        auto pattern = getCurrentPatternState();
        double offset = 0.0;

        if (isSongMode())
        {
            // Song mode: notes go into a fresh pattern placed where recording started.
            if (! recordPattern.isValid())
            {
                const int id = addPattern ("Recording " + juce::String (getPatterns().getNumChildren() + 1));
                recordPattern = addPatternClip (id, findFreeLane (recordStartBeats, getBarBeats()), recordStartBeats);
            }
            pattern = getPattern (recordPattern[IDs::pattern]);
            offset = recordStartBeats;
        }

        if (start - offset < 0.0)
            return;

        addNote (pattern, selectedChannel, start - offset, juce::jmax (1.0 / 64.0, length), key, juce::jlimit (1, 127, velocity));
        if (recordPattern.isValid())
            recordPattern.setProperty (IDs::length, getPatternLengthBeats (pattern), nullptr);
    }

    //==========================================================================
    // Patterns & notes

    juce::ValueTree getPatterns() const          { return studio.getChildWithName (IDs::PATTERNS); }
    juce::ValueTree getPattern (int patternId) const { return getPatterns().getChildWithProperty (IDs::id, patternId); }
    int getCurrentPattern() const                { return studio[IDs::currentPattern]; }
    juce::ValueTree getCurrentPatternState() const { return getPattern (getCurrentPattern()); }

    int addPattern (const juce::String& name)
    {
        const int id = takeNextId();
        juce::ValueTree p (IDs::PATTERN);
        p.setProperty (IDs::id, id, nullptr);
        p.setProperty (IDs::name, name, nullptr);
        getPatterns().appendChild (p, &undo());
        return id;
    }

    void setCurrentPattern (int patternId)       { studio.setProperty (IDs::currentPattern, patternId, nullptr); }

    int newPattern()
    {
        undo().beginNewTransaction ("New pattern");
        const int id = addPattern ("Pattern " + juce::String (getPatterns().getNumChildren() + 1));
        setCurrentPattern (id);
        return id;
    }

    int clonePattern (int patternId)
    {
        const auto source = getPattern (patternId);
        if (! source.isValid())
            return 0;

        undo().beginNewTransaction ("Clone pattern");
        auto copy = source.createCopy();
        const int id = takeNextId();
        copy.setProperty (IDs::id, id, nullptr);
        copy.setProperty (IDs::name, source[IDs::name].toString() + " clone", nullptr);
        getPatterns().appendChild (copy, &undo());
        setCurrentPattern (id);
        return id;
    }

    void deletePattern (int patternId)
    {
        if (getPatterns().getNumChildren() <= 1)
            return;

        undo().beginNewTransaction ("Delete pattern");
        auto playlist = getPlaylist();
        for (int i = playlist.getNumChildren(); --i >= 0;)
            if ((int) playlist.getChild (i)[IDs::pattern] == patternId && playlist.getChild (i)[IDs::type].toString() != ChannelType::audio)
                playlist.removeChild (i, &undo());

        getPatterns().removeChild (getPattern (patternId), &undo());
        if (getCurrentPattern() == patternId)
            setCurrentPattern (getPatterns().getChild (0)[IDs::id]);
    }

    juce::Colour getPatternColour (const juce::ValueTree& pattern) const
    {
        if (pattern.hasProperty (IDs::colour))
            return juce::Colour::fromString (pattern[IDs::colour].toString());
        static const juce::uint32 palette[] = { 0xff5f8fd8, 0xff63b56d, 0xffd88a4c, 0xffb46ed0, 0xffd45d6c, 0xff4fb3b0, 0xffc9b64c, 0xff8a8fd9 };
        return juce::Colour (palette[(int) pattern[IDs::id] % 8]);
    }

    //==========================================================================
    // Mixer: inserts 1..numInserts are AudioTracks; insert 0 is the master.

    static constexpr int numInserts = 20;

    juce::ValueTree getInserts() const { return studio.getChildWithName (IDs::INSERTS); }

    te::AudioTrack* getInsertTrack (int index) const
    {
        const auto insert = getInserts().getChildWithProperty (IDs::index, index);
        return insert.isValid() ? te::findAudioTrackForID (*edit, te::EditItemID::fromVar (insert[IDs::track])) : nullptr;
    }

    juce::String getInsertName (int index) const
    {
        if (index == 0) return "Master";
        return getInserts().getChildWithProperty (IDs::index, index)[IDs::name].toString();
    }

    void renameInsert (int index, const juce::String& name)
    {
        auto insert = getInserts().getChildWithProperty (IDs::index, index);
        insert.setProperty (IDs::name, name, &undo());
        if (auto t = getInsertTrack (index))
            t->setName (name);
    }

    te::PluginList& getInsertPlugins (int index) const
    {
        if (auto t = getInsertTrack (index))
            return t->pluginList;
        return edit->getMasterPluginList();
    }

    te::VolumeAndPanPlugin* getInsertVolume (int index) const
    {
        if (auto t = getInsertTrack (index))
            return t->getVolumePlugin();
        return edit->getMasterVolumePlugin().get();
    }

    te::LevelMeterPlugin* getInsertMeter (int index) const
    {
        if (auto t = getInsertTrack (index))
            return t->getLevelMeterPlugin();
        return edit->getMasterPluginList().findFirstPluginOfType<te::LevelMeterPlugin>();
    }

    static bool isMixerUtility (te::Plugin* p)
    {
        return dynamic_cast<te::VolumeAndPanPlugin*> (p) != nullptr || dynamic_cast<te::LevelMeterPlugin*> (p) != nullptr
            || dynamic_cast<te::AuxReturnPlugin*> (p) != nullptr || dynamic_cast<te::AuxSendPlugin*> (p) != nullptr;
    }

    // The insert's effect slots, in order.
    juce::Array<te::Plugin*> getInsertEffects (int index) const
    {
        juce::Array<te::Plugin*> fx;
        for (auto p : getInsertPlugins (index).getPlugins())
            if (! isMixerUtility (p))
                fx.add (p);
        return fx;
    }

    juce::Array<te::AuxSendPlugin*> getInsertSends (int index) const
    {
        juce::Array<te::AuxSendPlugin*> sends;
        if (index > 0)
            for (auto p : getInsertPlugins (index).getPlugins())
                if (auto s = dynamic_cast<te::AuxSendPlugin*> (p))
                    sends.add (s);
        return sends;
    }

    static constexpr int maxEffectSlots = 10;

    te::Plugin::Ptr addEffect (int index, const juce::String& xmlType)
    {
        if (getInsertEffects (index).size() >= maxEffectSlots)
            return {};

        undo().beginNewTransaction ("Add effect");
        te::Plugin::Ptr plugin;
        if (xmlType.startsWith ("ext:"))   // third-party effect, by identifier
        {
            if (auto desc = findKnownPlugin (xmlType.substring (4)))
                plugin = edit->getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, *desc);
        }
        else
        {
            plugin = edit->getPluginCache().createNewPlugin (xmlType, {});
        }
        if (plugin == nullptr)
            return {};

        // Effects go before the fader / meter (pre-fader chain, like FL's insert slots).
        auto& list = getInsertPlugins (index);
        int position = 0;
        for (auto p : list.getPlugins())
        {
            if (dynamic_cast<te::VolumeAndPanPlugin*> (p) != nullptr || dynamic_cast<te::LevelMeterPlugin*> (p) != nullptr
                || dynamic_cast<te::AuxSendPlugin*> (p) != nullptr)
                break;
            ++position;
        }
        list.insertPlugin (plugin, position, nullptr);
        return plugin;
    }

    void removePlugin (te::Plugin& p)
    {
        undo().beginNewTransaction ("Remove plugin");
        p.deleteFromParent();
    }

    // FL "route to this track": an aux send on `from` feeding an aux return on `to`.
    void addSend (int from, int to)
    {
        auto source = getInsertTrack (from);
        auto dest = getInsertTrack (to);
        if (source == nullptr || dest == nullptr || from == to)
            return;

        undo().beginNewTransaction ("Add send");
        if (dest->pluginList.findFirstPluginOfType<te::AuxReturnPlugin>() == nullptr)
        {
            auto ret = edit->getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, {});
            ret->state.setProperty (te::IDs::busNum, to - 1, nullptr);
            dest->pluginList.insertPlugin (ret, 0, nullptr);
        }

        auto send = edit->getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, {});
        send->state.setProperty (te::IDs::busNum, to - 1, nullptr);
        int position = 0;
        for (auto p : source->pluginList.getPlugins())
        {
            if (dynamic_cast<te::VolumeAndPanPlugin*> (p) != nullptr || dynamic_cast<te::LevelMeterPlugin*> (p) != nullptr)
                break;
            ++position;
        }
        source->pluginList.insertPlugin (send, position, nullptr);
        edit->setAuxBusName (to - 1, getInsertName (to));
    }

    int getChannelInsert (int channelId) const { return getChannel (channelId)[IDs::insert]; }

    // Routes a channel to a mixer insert (0 = master), like FL's channel "FX" number.
    void setChannelInsert (int channelId, int index)
    {
        auto channel = getChannel (channelId);
        index = juce::jlimit (0, numInserts, index);
        channel.setProperty (IDs::insert, index, &undo());
        if (auto t = getTrack (channel))
        {
            if (auto dest = getInsertTrack (index))
                t->getOutput().setOutputToTrack (dest);
            else
                t->getOutput().setOutputToDefaultDevice (false);
        }
    }

    //==========================================================================
    // Playlist

    juce::ValueTree getPlaylist() const { return studio.getChildWithName (IDs::PLAYLIST); }

    juce::ValueTree addPatternClip (int patternId, int lane, double start)
    {
        juce::ValueTree c (IDs::PCLIP);
        c.setProperty (IDs::id, takeNextId(), nullptr);
        c.setProperty (IDs::pattern, patternId, nullptr);
        c.setProperty (IDs::lane, lane, nullptr);
        c.setProperty (IDs::start, juce::jmax (0.0, start), nullptr);
        c.setProperty (IDs::length, getPatternLengthBeats (getPattern (patternId)), nullptr);
        getPlaylist().appendChild (c, &undo());
        return c;
    }

    // FL: dropping a sound on the playlist makes an audio clip (and an audio clip channel for it).
    juce::ValueTree addAudioClip (const juce::File& file, int lane, double start)
    {
        te::AudioFile audioFile (engine, file);
        if (! audioFile.isValid())
            return {};

        int channelId = 0;
        for (auto ch : getChannels())
            if (ch[IDs::type].toString() == ChannelType::audio && ch[IDs::file].toString() == file.getFullPathName())
                channelId = ch[IDs::id];

        if (channelId == 0)
        {
            auto track = createChannelTrack (file.getFileNameWithoutExtension());
            channelId = addChannelState (*track, ChannelType::audio, file.getFileNameWithoutExtension(), file);
        }

        // FL: loops with a tempo in their name ("... 120 BPM") follow the project tempo.
        const double origBpm = bpmFromFileName (file.getFileNameWithoutExtension());
        const double beats = audioFile.getLength() * (origBpm > 0.0 ? origBpm : getBpm()) / 60.0;
        juce::ValueTree c (IDs::PCLIP);
        if (origBpm > 0.0)
        {
            c.setProperty (IDs::origBpm, origBpm, nullptr);
            c.setProperty (IDs::stretch, true, nullptr);
        }
        c.setProperty (IDs::id, takeNextId(), nullptr);
        c.setProperty (IDs::type, ChannelType::audio, nullptr);
        c.setProperty (IDs::channel, channelId, nullptr);
        c.setProperty (IDs::lane, lane, nullptr);
        c.setProperty (IDs::start, juce::jmax (0.0, start), nullptr);
        c.setProperty (IDs::length, beats, nullptr);
        getPlaylist().appendChild (c, &undo());
        return c;
    }

    // "Drum Loop 120 BPM", "loop_92bpm" -> 120, 92. 0 when the name has no tempo.
    static double bpmFromFileName (const juce::String& name)
    {
        static const std::regex re ("(\\d{2,3}(\\.\\d+)?)[ _-]*bpm", std::regex::icase);
        std::smatch m;
        const auto text = name.toStdString();
        if (std::regex_search (text, m, re))
            if (const double bpm = std::stod (m[1].str()); bpm >= 40.0 && bpm <= 300.0)
                return bpm;
        return 0.0;
    }

    // Audio clip time-stretch (follow the project tempo) and pitch. Changing the speed keeps
    // the same audio inside the clip, so its length in beats scales.
    void setClipStretch (juce::ValueTree clip, bool on)
    {
        if (on == (bool) clip[IDs::stretch])
            return;
        undo().beginNewTransaction ("Stretch to tempo");
        double orig = clip[IDs::origBpm];
        if (orig <= 0.0)
            clip.setProperty (IDs::origBpm, orig = getBpm(), &undo());
        clip.setProperty (IDs::stretch, on, &undo());
        scaleClip (clip, on ? orig / getBpm() : getBpm() / orig);
    }

    void setClipOriginalBpm (juce::ValueTree clip, double bpm)
    {
        bpm = juce::jlimit (20.0, 400.0, bpm);
        undo().beginNewTransaction ("Original tempo");
        const double old = clip[IDs::origBpm];
        clip.setProperty (IDs::origBpm, bpm, &undo());
        if ((bool) clip[IDs::stretch] && old > 0.0)
            scaleClip (clip, bpm / old);
    }

    void setClipPitch (juce::ValueTree clip, double semitones)
    {
        undo().beginNewTransaction ("Clip pitch");
        clip.setProperty (IDs::pitch, juce::jlimit (-24.0, 24.0, semitones), &undo());
    }

    // Audio editor "Apply": points a channel at an edited copy of its audio (one undo step).
    bool replaceAudioFile (int channelId, const juce::File& file)
    {
        auto channel = getChannel (channelId);
        auto track = getTrack (channel);
        if (track == nullptr || ! file.existsAsFile())
            return false;

        undo().beginNewTransaction ("Edit audio");
        channel.setProperty (IDs::file, file.getFullPathName(), &undo());
        if (auto sampler = track->pluginList.findFirstPluginOfType<te::SamplerPlugin>())
        {
            const int root = sampler->getNumSounds() > 0 ? sampler->getKeyNote (0) : defaultRootNote;
            setSamplerFile (*sampler, file);
            sampler->setSoundParams (0, root, 0, 127);
        }
        else if (auto ms = getMultiSampler (channelId); ms != nullptr && ms->usesRegionState() && ms->getRegionStates().size() == 1)
        {
            ms->getRegionStates().getFirst().setProperty ("sample", file.getFullPathName(), &undo());
        }
        return true;
    }

    static bool isAudioClip (const juce::ValueTree& clip)      { return clip[IDs::type].toString() == ChannelType::audio; }
    static bool isAutomationClip (const juce::ValueTree& clip) { return clip[IDs::type].toString() == ChannelType::automation; }
    static bool isPatternClip (const juce::ValueTree& clip)    { return ! isAudioClip (clip) && ! isAutomationClip (clip); }

    //==========================================================================
    // Automation clips (FL: right-click a knob -> "Create automation clip")

    std::function<void()> onAutomationClipCreated; // UI shows the playlist

    te::AutomatableParameter* findParam (const juce::var& pluginId, const juce::String& paramId) const
    {
        if (auto p = te::findPluginForID (*edit, te::EditItemID::fromVar (pluginId)))
            for (auto* param : p->getAutomatableParameters())
                if (param->paramID == paramId)
                    return param;
        return nullptr;
    }

    juce::String describeParam (te::AutomatableParameter& param) const
    {
        juce::String owner;
        if (auto t = param.getTrack())
            owner = t->getName();
        else if (param.getPlugin() != nullptr)
            owner = "Master";
        return owner + " - " + param.getParameterName();
    }

    int findFreeLane (double start, double length) const
    {
        for (int lane = 0; lane < 500; ++lane)
        {
            bool busy = false;
            for (auto c : getPlaylist())
                if ((int) c[IDs::lane] == lane && (double) c[IDs::start] < start + length && (double) c[IDs::start] + (double) c[IDs::length] > start)
                    busy = true;
            if (! busy)
                return lane;
        }
        return 0;
    }

    juce::ValueTree createAutomationClip (te::AutomatableParameter& param, double start = 0.0, double length = 0.0)
    {
        auto plugin = param.getPlugin();
        if (plugin == nullptr)
            return {};

        if (length <= 0.0)
            length = juce::jmax (getSongLengthBeats() - start, getBarBeats() * 4.0);

        undo().beginNewTransaction ("Create automation clip");
        juce::ValueTree c (IDs::PCLIP);
        c.setProperty (IDs::id, takeNextId(), nullptr);
        c.setProperty (IDs::type, ChannelType::automation, nullptr);
        c.setProperty (IDs::plugin, plugin->itemID.toVar(), nullptr);
        c.setProperty (IDs::param, param.paramID, nullptr);
        c.setProperty (IDs::name, describeParam (param), nullptr);
        c.setProperty (IDs::lane, findFreeLane (start, length), nullptr);
        c.setProperty (IDs::start, start, nullptr);
        c.setProperty (IDs::length, length, nullptr);

        const float v = param.getCurrentNormalisedValue();
        for (double pos : { 0.0, length })
        {
            juce::ValueTree point (IDs::POINT);
            point.setProperty (IDs::pos, pos, nullptr);
            point.setProperty (IDs::value, v, nullptr);
            point.setProperty (IDs::tension, 0.0, nullptr);
            c.appendChild (point, nullptr);
        }
        getPlaylist().appendChild (c, &undo());
        if (onAutomationClipCreated) onAutomationClipCreated();
        return c;
    }

    // Keeps a clip's points sorted by position (the engine curve needs ascending times).
    static void addAutomationPoint (juce::ValueTree clip, double pos, float value, juce::UndoManager* um)
    {
        juce::ValueTree point (IDs::POINT);
        point.setProperty (IDs::pos, juce::jlimit (0.0, (double) clip[IDs::length], pos), nullptr);
        point.setProperty (IDs::value, juce::jlimit (0.0f, 1.0f, value), nullptr);
        point.setProperty (IDs::tension, 0.0, nullptr);
        int index = 0;
        while (index < clip.getNumChildren() && (double) clip.getChild (index)[IDs::pos] <= pos)
            ++index;
        clip.addChild (point, index, um);
    }

    // Automation recording: while playing in song mode, knob moves are written into a clip.
    bool isRecordingAutomation() const      { return recordAutomation; }
    void setRecordingAutomation (bool on)   { recordAutomation = on; }

    void userChangedParam (te::AutomatableParameter& param)
    {
        if (! recordAutomation || ! isPlaying() || ! isSongMode() || param.getPlugin() == nullptr)
            return;

        const double now = getPositionBeats();
        const auto pluginId = param.getPlugin()->itemID.toVar();
        juce::ValueTree clip;
        for (auto c : getPlaylist())
            if (isAutomationClip (c) && c[IDs::plugin] == pluginId && c[IDs::param].toString() == param.paramID
                && now >= (double) c[IDs::start] && now <= (double) c[IDs::start] + (double) c[IDs::length])
                clip = c;

        if (! clip.isValid())
        {
            const double start = std::floor (now / getBarBeats()) * getBarBeats();
            const auto saved = onAutomationClipCreated;
            onAutomationClipCreated = nullptr;
            clip = createAutomationClip (param, start, getBarBeats() * 4.0);
            onAutomationClipCreated = saved;
            clip.removeAllChildren (nullptr);
        }

        const double pos = now - (double) clip[IDs::start];
        // Drop points we're overwriting (within one step ahead), then add the new one.
        for (int i = clip.getNumChildren(); --i >= 0;)
        {
            const double p = clip.getChild (i)[IDs::pos];
            if (p > pos - 1.0e-6 && p < pos + stepBeats)
                clip.removeChild (i, &undo());
        }
        addAutomationPoint (clip, pos, param.getCurrentNormalisedValue(), &undo());
    }

    //==========================================================================
    // MIDI controller links (FL "Link to controller")

    void startLearning (te::AutomatableParameter& param) { learning = &param; }
    bool isLearning (te::AutomatableParameter& param) const { return learning == &param; }

    void unlinkController (te::AutomatableParameter& param)
    {
        auto maps = studio.getChildWithName (IDs::MAPPINGS);
        for (int i = maps.getNumChildren(); --i >= 0;)
            if (findParam (maps.getChild (i)[IDs::plugin], maps.getChild (i)[IDs::param]) == &param)
                maps.removeChild (i, &undo());
    }

    juce::String getControllerLink (te::AutomatableParameter& param) const
    {
        for (auto m : studio.getChildWithName (IDs::MAPPINGS))
            if (findParam (m[IDs::plugin], m[IDs::param]) == &param)
                return "CC " + m[IDs::cc].toString();
        return {};
    }

    // Message thread: a CC arrived from any MIDI input.
    void handleController (int channel, int cc, int value)
    {
        auto maps = studio.getChildWithName (IDs::MAPPINGS);
        if (learning != nullptr && learning->getPlugin() != nullptr)
        {
            undo().beginNewTransaction ("Link to controller");
            unlinkController (*learning);
            juce::ValueTree m (IDs::MAP);
            m.setProperty (IDs::cc, cc, nullptr);
            m.setProperty (IDs::channel, channel, nullptr);
            m.setProperty (IDs::plugin, learning->getPlugin()->itemID.toVar(), nullptr);
            m.setProperty (IDs::param, learning->paramID, nullptr);
            maps.appendChild (m, &undo());
            learning = nullptr;
        }

        for (auto m : maps)
            if ((int) m[IDs::cc] == cc && (int) m[IDs::channel] == channel)
                if (auto param = findParam (m[IDs::plugin], m[IDs::param]))
                {
                    param->setNormalisedParameter ((float) value / 127.0f, juce::sendNotificationSync);
                    userChangedParam (*param);
                }
    }

    double getSongLengthBeats() const
    {
        double end = 0.0;
        for (auto c : getPlaylist())
            end = juce::jmax (end, (double) c[IDs::start] + (double) c[IDs::length]);
        return end;
    }

    // Song-mode loop region (ruler drag in the playlist). Empty = loop whole song.
    juce::Range<double> getSongLoop() const { return { (double) studio[IDs::loopStart], (double) studio[IDs::loopEnd] }; }
    void setSongLoop (juce::Range<double> r)
    {
        studio.setProperty (IDs::loopStart, r.getStart(), nullptr);
        studio.setProperty (IDs::loopEnd, r.getEnd(), nullptr);
    }

    void setPositionBeats (double beats)
    {
        transport().setPosition (edit->tempoSequence.toTime (te::BeatPosition::fromBeats (juce::jmax (0.0, beats))));
    }

    double getBarBeats() const
    {
        auto& sig = *edit->tempoSequence.getTimeSig (0);
        return juce::jlimit (1, 16, sig.numerator.get()) * 4.0 / juce::jlimit (1, 16, sig.denominator.get());
    }

    // FL-style auto length: at least one bar, rounded up to whole bars.
    double getPatternLengthBeats (const juce::ValueTree& pattern) const
    {
        double end = 0.0;
        for (auto note : pattern)
            end = juce::jmax (end, (double) note[IDs::start] + (double) note[IDs::length]);

        const double bar = getBarBeats();
        return juce::jmax (bar, std::ceil (end / bar - 1.0e-6) * bar);
    }

    juce::ValueTree findStepNote (const juce::ValueTree& pattern, int channelId, int step) const
    {
        const double start = step * stepBeats;
        for (auto note : pattern)
            if ((int) note[IDs::channel] == channelId && std::abs ((double) note[IDs::start] - start) < 1.0e-4)
                return note;
        return {};
    }

    void setStep (int channelId, int step, bool on)
    {
        auto pattern = getCurrentPatternState();
        auto existing = findStepNote (pattern, channelId, step);

        if (on && ! existing.isValid())
            addNote (pattern, channelId, step * stepBeats, stepBeats,
                     getChannel (channelId)[IDs::type].toString() == ChannelType::drumpad ? MultiSampler::firstPadNote : defaultRootNote, 100);
        else if (! on && existing.isValid())
        {
            pattern.removeChild (existing, &undo());
        }
    }

    juce::ValueTree addNote (juce::ValueTree pattern, int channelId, double start, double length, int key, int velocity)
    {
        juce::ValueTree note (IDs::NOTE);
        note.setProperty (IDs::channel, channelId, nullptr);
        note.setProperty (IDs::start, juce::jmax (0.0, start), nullptr);
        note.setProperty (IDs::length, juce::jmax (1.0 / 64.0, length), nullptr);
        note.setProperty (IDs::key, juce::jlimit (0, 127, key), nullptr);
        note.setProperty (IDs::velocity, juce::jlimit (1, 127, velocity), nullptr);
        pattern.appendChild (note, &undo());
        return note;
    }

    // Short audition when placing/clicking notes (FL plays the note you draw).
    void previewNote (int key, int velocity = 100)
    {
        playLiveNote (juce::MidiMessage::noteOn (1, key, (juce::uint8) juce::jlimit (1, 127, velocity)));
        juce::Timer::callAfterDelay (220, [this, key] { playLiveNote (juce::MidiMessage::noteOff (1, key)); });
    }

    double getSwing() const              { return studio[IDs::swing]; }
    void setSwing (double s)             { studio.setProperty (IDs::swing, juce::jlimit (0.0, 1.0, s), &undo()); }

    bool isSongMode() const              { return studio[IDs::songMode]; }
    void setSongMode (bool song)         { studio.setProperty (IDs::songMode, song, nullptr); }

    //==========================================================================
    // Transport

    double getBpm() const                { return edit->tempoSequence.getTempo (0)->getBpm(); }
    void setBpm (double bpm)             { edit->tempoSequence.getTempo (0)->setBpm (juce::jlimit (10.0, 522.0, bpm)); }

    bool isPlaying() const               { return transport().isPlaying(); }

    // Tempo changes over the song (ruler markers). Positions are beats, so clips never move
    // in bars; only their time changes.
    juce::ValueTree getTempoChanges() const { return studio.getChildWithName (IDs::TEMPOS); }

    juce::ValueTree addTempoChange (double beat, double bpm)
    {
        undo().beginNewTransaction ("Tempo change");
        bpm = juce::jlimit (10.0, 522.0, bpm);
        for (auto t : getTempoChanges())
            if (std::abs ((double) t[IDs::pos] - beat) < 1.0e-6)
            {
                t.setProperty (IDs::bpm, bpm, &undo());
                return t;
            }
        juce::ValueTree t (IDs::TEMPOCHANGE);
        t.setProperty (IDs::pos, juce::jmax (stepBeats, beat), nullptr);
        t.setProperty (IDs::bpm, bpm, nullptr);
        getTempoChanges().appendChild (t, &undo());
        return t;
    }

    void removeTempoChange (const juce::ValueTree& t)
    {
        undo().beginNewTransaction ("Remove tempo change");
        getTempoChanges().removeChild (t, &undo());
    }

    double getBpmAtBeat (double beat) const { return edit->tempoSequence.getTempoAt (te::BeatPosition::fromBeats (beat)).getBpm(); }

    // Project time signature: beats per bar over quarter notes (model beats are quarter notes;
    // Tracktion would make x/8 beats eighth notes, so only /4 is offered).
    int getTimeSigNumerator() const   { return edit->tempoSequence.getTimeSig (0)->numerator; }

    void setTimeSigNumerator (int numerator)
    {
        undo().beginNewTransaction ("Time signature");
        auto& sig = *edit->tempoSequence.getTimeSig (0);
        sig.numerator = juce::jlimit (1, 16, numerator);
        sig.denominator = 4;
        markChanged();
    }

    // FL: Space = play/stop, stop returns to the start.
    void togglePlay()
    {
        auto& t = transport();
        if (t.isPlaying() || recording)
        {
            if (recording) stopRecording();
            else           t.stop (false, false);
            t.setPosition (0s);
        }
        else if (recordArmed)
        {
            startRecording();
        }
        else
        {
            regenerateNow();
            t.play (false);
        }
    }

    double getPositionBeats() const
    {
        return edit->tempoSequence.toBeats (transport().getPosition()).inBeats();
    }

    bool isMetronomeOn() const           { return edit->clickTrackEnabled.get(); }
    void setMetronome (bool on)          { edit->clickTrackEnabled = on; }

    // Voices sounding in our own instruments (Instrument, drum pad, FM Synth).
    int getActiveVoices() const
    {
        int n = 0;
        for (auto p : te::getAllPlugins (*edit, false))
            if (auto ms = dynamic_cast<MultiSamplerPlugin*> (p))  n += ms->voicesPlaying.load (std::memory_order_relaxed);
            else if (auto fm = dynamic_cast<FMSynthPlugin*> (p))  n += fm->voicesPlaying.load (std::memory_order_relaxed);
        return n;
    }

    // Push model changes into the engine immediately (normally done async).
    void regenerateNow()
    {
        cancelPendingUpdate();
        handleAsyncUpdate();
    }

private:
    //==========================================================================
    void finishLoading()
    {
        ensureInserts();
        if (! getCurrentPatternState().isValid())
        {
            if (getPatterns().getNumChildren() == 0)
                addPattern ("Pattern 1");
            setCurrentPattern (getPatterns().getChild (0)[IDs::id]);
        }
        selectedChannel = getChannels().getChild (0)[IDs::id];
        updateMidiInputTarget();
        regenerateNow();
        undo().clearUndoHistory();
        undo().dispatchPendingMessages();
        dirty = startsDirty;

        // Tracktion tidies new tracks/clips asynchronously as undoable steps; once those queued
        // updates have run, clear again so a fresh or loaded project doesn't come up "unsaved".
        juce::MessageManager::callAsync ([safe = juce::WeakReference<Studio> (this), generation = editGeneration]
        {
            if (safe != nullptr && safe->editGeneration == generation)
            {
                safe->undo().clearUndoHistory();
                safe->undo().dispatchPendingMessages();
                safe->dirty = safe->startsDirty;
            }
        });
    }

    void setEdit (std::unique_ptr<te::Edit> newEdit)
    {
        // UI holding plugin/parameter pointers must let go before the old Edit dies.
        for (auto& fn : editAboutToChange)
            fn();
        learning = nullptr;
        automatedTargets.clear();

        if (edit != nullptr)
        {
            studio.removeListener (this);
            edit->getUndoManager().removeChangeListener (this);
            edit->getTransport().stop (false, true);
        }

        edit = std::move (newEdit);
        edit->getUndoManager().addChangeListener (this);
        ++editGeneration;
        edit->getTransport().ensureContextAllocated();
        edit->getTransport().looping = true;

        studio = edit->state.getOrCreateChildWithName (IDs::STUDIO, nullptr);
        for (auto type : { IDs::CHANNELS, IDs::PATTERNS, IDs::PLAYLIST, IDs::INSERTS, IDs::MAPPINGS, IDs::TEMPOS })
            studio.getOrCreateChildWithName (type, nullptr);

        studio.addListener (this);
        ++changeCount;
        triggerAsyncUpdate();
    }

    void findMissingFiles()
    {
        missingFiles.clear();
        std::function<void (const juce::ValueTree&)> walk = [&] (const juce::ValueTree& v)
        {
            for (auto id : { IDs::file, te::IDs::source, juce::Identifier ("sample") })
                if (const auto path = v[id].toString(); juce::File::isAbsolutePath (path) && ! juce::File (path).exists())
                    missingFiles.addIfNotAlreadyThere (path);
            for (auto child : v)
                walk (child);
        };
        walk (edit->state);

        for (auto p : te::getAllPlugins (*edit, false))
            if (auto ext = dynamic_cast<te::ExternalPlugin*> (p); ext != nullptr && ext->getAudioPluginInstance() == nullptr)
                missingFiles.addIfNotAlreadyThere ("plugin: " + ext->getName());
    }

    int takeNextId()
    {
        const int id = (int) studio.getProperty (IDs::nextId, 1);
        studio.setProperty (IDs::nextId, id + 1, nullptr);
        return id;
    }

public:
    te::AudioTrack* getRecorderTrack (bool create)
    {
        for (auto t : te::getAudioTracks (*edit))
            if (t->state[IDs::type].toString() == "recorder")
                return t;
        if (! create)
            return nullptr;
        auto t = edit->insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (*edit), nullptr);
        t->setName ("Recorder");
        t->state.setProperty (IDs::type, "recorder", nullptr);
        t->setMute (true); // monitoring comes from the input device, not this track's playback
        return t.get();
    }

private:
    void ensureInserts()
    {
        auto inserts = getInserts();
        for (int i = inserts.getNumChildren() + 1; i <= numInserts; ++i)
        {
            auto track = edit->insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (*edit), nullptr);
            track->setName ("Insert " + juce::String (i));
            juce::ValueTree insert (IDs::INSERT);
            insert.setProperty (IDs::index, i, nullptr);
            insert.setProperty (IDs::name, "Insert " + juce::String (i), nullptr);
            insert.setProperty (IDs::track, track->itemID.toVar(), nullptr);
            inserts.appendChild (insert, nullptr);
        }

        auto& master = edit->getMasterPluginList();
        if (master.findFirstPluginOfType<te::LevelMeterPlugin>() == nullptr)
            master.insertPlugin (edit->getPluginCache().createNewPlugin (te::LevelMeterPlugin::xmlTypeName, {}), -1, nullptr);
    }

    te::AudioTrack* createChannelTrack (const juce::String& name)
    {
        auto track = edit->insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (*edit), nullptr);
        track->setName (name);

        auto clip = track->insertMIDIClip ("Notes", { 0s, edit->tempoSequence.toTime (te::BeatPosition::fromBeats (getBarBeats())) }, nullptr);
        clip->state.setProperty (IDs::derived, true, nullptr);
        return track.get();
    }

    MultiSamplerPlugin* createMultiSampler (te::AudioTrack& track)
    {
        auto plugin = edit->getPluginCache().createNewPlugin (MultiSamplerPlugin::xmlTypeName, {});
        track.pluginList.insertPlugin (plugin, 0, nullptr);
        return dynamic_cast<MultiSamplerPlugin*> (plugin.get());
    }

    int addChannelState (te::AudioTrack& track, const juce::String& type, const juce::String& name, const juce::File& file)
    {
        const int id = takeNextId();
        juce::ValueTree c (IDs::CHANNEL);
        c.setProperty (IDs::id, id, nullptr);
        c.setProperty (IDs::name, name, nullptr);
        c.setProperty (IDs::type, type, nullptr);
        c.setProperty (IDs::track, track.itemID.toVar(), nullptr);
        if (file != juce::File())
            c.setProperty (IDs::file, file.getFullPathName(), nullptr);
        getChannels().appendChild (c, &undo());
        return id;
    }

    static void setSamplerFile (te::SamplerPlugin& sampler, const juce::File& file)
    {
        while (sampler.getNumSounds() > 0)
            sampler.removeSound (0);

        sampler.addSound (file.getFullPathName(), file.getFileNameWithoutExtension(), 0.0, 0.0, 0.0f);
        sampler.setSoundParams (0, defaultRootNote, 0, 127);
        sampler.setSoundOpenEnded (0, true); // one-shot, like FL's sampler default
    }

    void scaleClip (juce::ValueTree& clip, double ratio)
    {
        clip.setProperty (IDs::length, (double) clip[IDs::length] * ratio, &undo());
        clip.setProperty (IDs::offset, (double) clip[IDs::offset] * ratio, &undo());
    }

    // Model tempo changes -> engine tempo sequence (derived, no UndoManager).
    void syncTempoChanges()
    {
        auto tempoState = edit->state.getChildWithName (te::IDs::TEMPOSEQUENCE);
        juce::Array<juce::ValueTree> engineTempos;
        for (auto t : tempoState)
            if (t.hasType (te::IDs::TEMPO))
                engineTempos.add (t);
        if (engineTempos.isEmpty())
            return;

        engineTempos.getReference (0).setProperty (te::IDs::curve, 1.0, nullptr); // jumps, not ramps

        std::vector<std::pair<double, double>> wanted, current;
        for (auto t : getTempoChanges())
            wanted.push_back ({ t[IDs::pos], t[IDs::bpm] });
        std::sort (wanted.begin(), wanted.end());
        for (int i = 1; i < engineTempos.size(); ++i)
            current.push_back ({ engineTempos[i][te::IDs::startBeat], engineTempos[i][te::IDs::bpm] });
        if (wanted == current)
            return;

        for (int i = 1; i < engineTempos.size(); ++i)
            tempoState.removeChild (engineTempos[i], nullptr);
        int index = tempoState.indexOf (engineTempos[0]);
        for (auto& [beat, bpm] : wanted)
            tempoState.addChild (te::TempoSetting::create (te::BeatPosition::fromBeats (beat), bpm, 1.0f), ++index, nullptr);
    }

    // Writes playlist automation clips into the engine's parameter curves (song mode only;
    // FL automation clips belong to the playlist). Curves are derived, so no UndoManager.
    void syncAutomation (bool song)
    {
        std::map<juce::String, juce::Array<juce::ValueTree>> byTarget;
        for (auto c : getPlaylist())
            if (isAutomationClip (c))
                byTarget[c[IDs::plugin].toString() + "|" + c[IDs::param].toString()].add (c);

        std::set<juce::String> stillAutomated;
        for (auto& [key, clips] : byTarget)
        {
            auto param = findParam (clips.getFirst()[IDs::plugin], clips.getFirst()[IDs::param]);
            if (param == nullptr)
                continue;

            stillAutomated.insert (key);
            auto& curve = param->getCurve();
            curve.clear (nullptr);
            if (! song)
                continue;

            std::sort (clips.begin(), clips.end(), [] (auto& a, auto& b) { return (double) a[IDs::start] < (double) b[IDs::start]; });
            for (auto& c : clips)
            {
                if ((bool) c[IDs::muted])
                    continue;
                for (auto point : c)
                {
                    const double beat = (double) c[IDs::start] + juce::jmin ((double) point[IDs::pos], (double) c[IDs::length]);
                    curve.addPoint (edit->tempoSequence.toTime (te::BeatPosition::fromBeats (beat)),
                                    param->valueRange.convertFrom0to1 ((float) point[IDs::value]),
                                    (float) point[IDs::tension], nullptr);
                }
            }
        }

        // Params whose clips were all deleted: drop their curves.
        for (auto& key : automatedTargets)
            if (! stillAutomated.count (key))
                if (auto param = findParam (key.upToFirstOccurrenceOf ("|", false, false), key.fromFirstOccurrenceOf ("|", false, false)))
                    param->getCurve().clear (nullptr);
        automatedTargets = stillAutomated;
    }

    // Keeps the track's wave clips in step with the playlist's audio clips (song mode only).
    // Built straight from ValueTrees with no UndoManager: they're derived, like the MIDI clips.
    void syncAudioClips (te::AudioTrack& track, int channelId, bool song)
    {
        std::map<int, juce::ValueTree> wanted;
        if (song)
            for (auto pc : getPlaylist())
                if (isAudioClip (pc) && (int) pc[IDs::channel] == channelId && ! (bool) pc[IDs::muted])
                    wanted[(int) pc[IDs::id]] = pc;

        const juce::File file (getChannel (channelId)[IDs::file].toString());
        auto& tempo = edit->tempoSequence;

        juce::Array<juce::ValueTree> stale;
        for (auto clip : track.getClips())
            if (dynamic_cast<te::WaveAudioClip*> (clip) != nullptr && ! wanted.count ((int) clip->state[IDs::clipId]))
                stale.add (clip->state);
        for (auto& state : stale)
            state.getParent().removeChild (state, nullptr);

        for (auto& [id, pc] : wanted)
        {
            const double start = pc[IDs::start], length = pc[IDs::length], offset = pc[IDs::offset];
            const auto startTime = tempo.toTime (te::BeatPosition::fromBeats (start));
            const auto endTime = tempo.toTime (te::BeatPosition::fromBeats (start + length));

            // Stretched clips are beat based (Tracktion auto-tempo, stretched in real time);
            // others play at their own speed, so their offset is source seconds.
            const bool stretch = (bool) pc[IDs::stretch] && (double) pc[IDs::origBpm] > 0.0;
            const double pitch = pc[IDs::pitch];
            const bool stretching = stretch || std::abs (pitch) > 1.0e-6;
            const double offsetSeconds = stretch ? offset / tempo.getBeatsPerSecondAt (startTime) : offset * 60.0 / getBpm();

            juce::ValueTree existing;
            for (auto clip : track.getClips())
                if ((int) clip->state[IDs::clipId] == id)
                    existing = clip->state;

            const bool isNew = ! existing.isValid();
            if (isNew)
            {
                existing = juce::ValueTree (te::IDs::AUDIOCLIP);
                existing.setProperty (IDs::clipId, id, nullptr);
                edit->createNewItemID().writeID (existing, nullptr);
            }

            existing.setProperty (te::IDs::name, file.getFileNameWithoutExtension(), nullptr);
            existing.setProperty (te::IDs::source, file.getFullPathName(), nullptr);
            existing.setProperty (te::IDs::start, startTime.inSeconds(), nullptr);
            existing.setProperty (te::IDs::length, (endTime - startTime).inSeconds(), nullptr);
            existing.setProperty (te::IDs::offset, offsetSeconds, nullptr);
            if (stretch)
            {
                auto loopInfo = existing.getOrCreateChildWithName (te::IDs::LOOPINFO, nullptr);
                loopInfo.setProperty (te::IDs::numBeats, te::AudioFile (engine, file).getLength() * (double) pc[IDs::origBpm] / 60.0, nullptr);
                loopInfo.setProperty (te::IDs::numerator, 4, nullptr);
                loopInfo.setProperty (te::IDs::denominator, 4, nullptr);
            }
            existing.setProperty (te::IDs::autoTempo, stretch, nullptr);
            existing.setProperty (te::IDs::pitchChange, pitch, nullptr);
            existing.setProperty (te::IDs::elastiqueMode, stretching ? (int) te::TimeStretcher::soundtouchBetter : (int) te::TimeStretcher::disabled, nullptr);
            existing.setProperty (te::IDs::proxyAllowed, ! stretching, nullptr);

            if (isNew)
                track.state.addChild (existing, -1, nullptr);
        }
    }

    // Rewriting a sequence makes the engine rebuild its playback graph, so a channel's notes are
    // only rewritten when they actually changed (editing one channel leaves the others alone).
    static void writeNotes (te::MidiList& seq, std::vector<Arp::Note>& notes)
    {
        auto order = [] (const Arp::Note& a, const Arp::Note& b) { return std::tie (a.start, a.key, a.length, a.velocity) < std::tie (b.start, b.key, b.length, b.velocity); };
        auto same = [] (const Arp::Note& a, const Arp::Note& b)
        {
            return std::abs (a.start - b.start) < 1.0e-9 && std::abs (a.length - b.length) < 1.0e-9 && a.key == b.key && a.velocity == b.velocity;
        };

        std::vector<Arp::Note> current;
        current.reserve ((size_t) seq.getNotes().size());
        for (auto* n : seq.getNotes())
            current.push_back ({ n->getStartBeat().inBeats(), n->getLengthBeats().inBeats(), n->getNoteNumber(), n->getVelocity() });

        std::sort (notes.begin(), notes.end(), order);
        std::sort (current.begin(), current.end(), order);
        if (std::equal (notes.begin(), notes.end(), current.begin(), current.end(), same))
            return;

        seq.clear (nullptr);
        for (auto& n : notes)
            seq.addNote (n.key, te::BeatPosition::fromBeats (n.start), te::BeatDuration::fromBeats (n.length), n.velocity, 0, nullptr);
    }

    te::MidiClip* getDerivedClip (te::AudioTrack& track) const
    {
        for (auto clip : track.getClips())
            if (auto mc = dynamic_cast<te::MidiClip*> (clip))
                if (mc->state[IDs::derived])
                    return mc;
        return nullptr;
    }

    //==========================================================================
    // Model -> engine

    void handleAsyncUpdate() override
    {
        syncTempoChanges();
        regeneratedBarBeats = getBarBeats();
        const bool song = isSongMode();
        const double swingShift = getSwing() * stepBeats * (2.0 / 3.0);
        const auto pattern = getCurrentPatternState();
        double endBeats = song ? 0.0 : getPatternLengthBeats (pattern);

        if (song)
            endBeats = getSongLengthBeats();

        endBeats = juce::jmax (endBeats, getBarBeats());

        // One pass over the notes, bucketed by channel (not one pass per channel).
        std::map<int, int> arpModes;
        for (auto channel : getChannels())
            arpModes[(int) channel[IDs::id]] = channel[IDs::arp];
        std::map<int, std::vector<Arp::Note>> byChannel;

        // Notes of pattern `p` that fall inside [offset, offset + clipLength), placed at clipStart.
        // Swing shifts odd steps (the arp swings its own output instead).
        auto addNotes = [&] (const juce::ValueTree& p, double clipStart, double clipLength, double offset)
        {
            for (auto note : p)
            {
                const auto arpMode = arpModes.find ((int) note[IDs::channel]);
                if (arpMode == arpModes.end())
                    continue;

                const double rel = (double) note[IDs::start] - offset;
                if (rel < -1.0e-6 || rel >= clipLength - 1.0e-6)
                    continue;

                double start = note[IDs::start];
                const double stepIndex = start / stepBeats;
                if (arpMode->second == Arp::off && std::abs (stepIndex - std::round (stepIndex)) < 1.0e-4 && ((int) std::round (stepIndex)) % 2 == 1)
                    start += swingShift;

                byChannel[arpMode->first].push_back ({ clipStart + start - offset, juce::jmin ((double) note[IDs::length], clipLength - rel),
                                                       (int) note[IDs::key], (int) note[IDs::velocity] });
            }
        };

        if (song)
        {
            for (auto pc : getPlaylist())
                if (isPatternClip (pc) && ! (bool) pc[IDs::muted])
                    addNotes (getPattern (pc[IDs::pattern]), pc[IDs::start], pc[IDs::length], pc[IDs::offset]);
        }
        else
        {
            addNotes (pattern, 0.0, endBeats, 0.0);
        }

        for (auto channel : getChannels())
        {
            auto track = getTrack (channel);
            if (track == nullptr)
                continue;

            auto clip = getDerivedClip (*track);
            if (clip == nullptr)
                continue;

            const int channelId = channel[IDs::id];
            const int arpMode = channel[IDs::arp];
            auto& notes = byChannel[channelId];

            if (arpMode != Arp::off)
            {
                notes = Arp::apply (notes, arpMode, (double) channel.getProperty (IDs::arpRate, 1.0) * stepBeats,
                                    channel.getProperty (IDs::arpOctaves, 1), (double) channel.getProperty (IDs::arpGate, 90.0) / 100.0, channelId);
                for (auto& n : notes)
                {
                    const double stepIndex = n.start / stepBeats;
                    if (std::abs (stepIndex - std::round (stepIndex)) < 1.0e-4 && ((juce::int64) std::round (stepIndex)) % 2 == 1)
                        n.start += swingShift;
                }
            }

            writeNotes (clip->getSequence(), notes);

            if (channel[IDs::type].toString() == ChannelType::audio)
                syncAudioClips (*track, channelId, song);

            const auto endTime = edit->tempoSequence.toTime (te::BeatPosition::fromBeats (endBeats));
            clip->state.setProperty (te::IDs::start, 0.0, nullptr);
            clip->state.setProperty (te::IDs::length, endTime.inSeconds(), nullptr);
        }

        syncAutomation (song);

        auto loop = juce::Range<double> (0.0, endBeats);
        if (song && getSongLoop().getLength() > 1.0e-6)
            loop = getSongLoop();

        transport().setLoopRange ({ edit->tempoSequence.toTime (te::BeatPosition::fromBeats (loop.getStart())),
                                    edit->tempoSequence.toTime (te::BeatPosition::fromBeats (loop.getEnd())) });
        loopEndBeats = loop.getEnd();
    }

public:
    double getLoopEndBeats() const { return loopEndBeats; }

private:
    void markChanged()
    {
        ++changeCount;
        dirty = true;
        triggerAsyncUpdate();
    }

    // Any undoable action (knob moves, mutes...) also counts as an unsaved change.
    // A time signature change (or its undo) lives in the engine, so regenerate for it here.
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (undo().canUndo() || undo().canRedo()) // clearing the history isn't an edit
            dirty = true;
        if (! juce::approximatelyEqual (getBarBeats(), regeneratedBarBeats))
            markChanged();
    }

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { markChanged(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override                { markChanged(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override         { markChanged(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override                 { markChanged(); }
    void valueTreeRedirected (juce::ValueTree&) override                                  { markChanged(); }

    //==========================================================================
    // Hardware MIDI keyboards play the selected channel, like FL.

    void setupMidiInputs()
    {
        for (auto& midiIn : engine.getDeviceManager().getMidiInDevices())
        {
            midiIn->setMonitorMode (te::InputDevice::MonitorMode::on);
            midiIn->setEnabled (true);
        }
    }

    void updateMidiInputTarget()
    {
        auto track = getTrack (selectedChannel);
        if (track == nullptr)
            return;

        for (auto instance : edit->getAllInputDevices())
            if (instance->getInputDevice().getDeviceType() == te::InputDevice::physicalMidiDevice)
                [[maybe_unused]] auto result = instance->setTarget (track->itemID, true, nullptr, 0);
    }

    std::unique_ptr<te::Edit> edit;
    juce::ValueTree studio;
    juce::StringArray missingFiles;
    std::set<juce::String> automatedTargets;
    te::AutomatableParameter* learning = nullptr;
    bool recordAutomation = false, recording = false;
    double recordStartBeats = 0.0;
    juce::ValueTree recordPattern;
    std::map<int, std::pair<double, int>> pendingNotes;
    int selectedChannel = 0;
    int changeCount = 0, editGeneration = 0;
    bool dirty = false, startsDirty = false;
    juce::File projectFile;
    double loopEndBeats = 4.0, regeneratedBarBeats = 4.0;
    const te::MPESourceID liveSourceId = te::createUniqueMPESourceID();
    JUCE_DECLARE_WEAK_REFERENCEABLE (Studio)
};
