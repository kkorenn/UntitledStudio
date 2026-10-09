#pragma once

#include "DrumKit.h"

namespace te = tracktion;

// FL-style project model, stored as a ValueTree inside the Tracktion Edit so it
// saves/loads/undoes together with the engine state.
//
//   STUDIO (currentPattern, songMode, swing)
//     CHANNELS / CHANNEL (id, name, type, track, file, colour)
//     PATTERNS / PATTERN (id, name, colour) / NOTE (channel, start, length, key, velocity)
//     INSERTS / INSERT  (index, name, track)  -- mixer inserts; index 0 = master
//     PLAYLIST / PCLIP   (id, type, pattern | channel, lane, start, length, offset, muted)  -- beats
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
   #undef STUDIO_ID
}

namespace ChannelType
{
    inline const juce::String sampler = "sampler";
    inline const juce::String synth   = "synth";
    inline const juce::String audio   = "audio";   // FL "audio clip" channel: plays playlist audio clips
    inline const juce::String plugin  = "plugin";  // third-party instrument
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
                   .getChildFile ("untitledstudio").getChildFile ("Projects");
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

    void newProject()
    {
        setEdit (te::createEmptyEdit (engine, engine.getTemporaryFileManager().getTempDirectory().getChildFile ("Untitled.tracktionedit")));

        for (auto t : te::getAudioTracks (*edit))
            edit->deleteTrack (t);

        studio.setProperty (IDs::swing, 0.0, nullptr);
        ensureInserts();
        const int pattern = addPattern ("Pattern 1");
        setCurrentPattern (pattern);

        // FL's default template: Kick, Clap, Hat, Snare.
        for (auto name : { "Kick", "Clap", "Hat", "Snare" })
            addSamplerChannel (DrumKit::getDrumsFolder().getChildFile (juce::String (name) + ".wav"));

        projectFile = juce::File();
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
        auto loaded = te::loadEditFromFile (engine, file);
        if (loaded == nullptr || ! loaded->state.getChildWithName (IDs::STUDIO).isValid())
            return false;

        setEdit (std::move (loaded));
        projectFile = asUntitled ? juce::File() : file;
        finishLoading();
        dirty = asUntitled;
        return true;
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

    // Third-party instrument (VST3 / AU) as a channel.
    int addPluginChannel (const juce::PluginDescription& desc)
    {
        undo().beginNewTransaction ("Add plugin channel");
        auto track = createChannelTrack (desc.name);
        if (auto plugin = edit->getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, desc))
            track->pluginList.insertPlugin (plugin, 0, nullptr);
        return addChannelState (*track, ChannelType::plugin, desc.name, {});
    }

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

        const double beats = audioFile.getLength() * getBpm() / 60.0;
        juce::ValueTree c (IDs::PCLIP);
        c.setProperty (IDs::id, takeNextId(), nullptr);
        c.setProperty (IDs::type, ChannelType::audio, nullptr);
        c.setProperty (IDs::channel, channelId, nullptr);
        c.setProperty (IDs::lane, lane, nullptr);
        c.setProperty (IDs::start, juce::jmax (0.0, start), nullptr);
        c.setProperty (IDs::length, beats, nullptr);
        getPlaylist().appendChild (c, &undo());
        return c;
    }

    static bool isAudioClip (const juce::ValueTree& clip) { return clip[IDs::type].toString() == ChannelType::audio; }

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
        return sig.numerator.get() * 4.0 / sig.denominator.get();
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
            addNote (pattern, channelId, step * stepBeats, stepBeats, defaultRootNote, 100);
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

    // FL: Space = play/stop, stop returns to the start.
    void togglePlay()
    {
        auto& t = transport();
        if (t.isPlaying())
        {
            t.stop (false, false);
            t.setPosition (0s);
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
        selectedChannel = getChannels().getChild (0)[IDs::id];
        updateMidiInputTarget();
        regenerateNow();
        undo().clearUndoHistory();
        undo().dispatchPendingMessages();
        dirty = false;
    }

    void setEdit (std::unique_ptr<te::Edit> newEdit)
    {
        // UI holding plugin/parameter pointers must let go before the old Edit dies.
        for (auto& fn : editAboutToChange)
            fn();

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
        for (auto type : { IDs::CHANNELS, IDs::PATTERNS, IDs::PLAYLIST, IDs::INSERTS })
            studio.getOrCreateChildWithName (type, nullptr);

        studio.addListener (this);
        ++changeCount;
        triggerAsyncUpdate();
    }

    int takeNextId()
    {
        const int id = (int) studio.getProperty (IDs::nextId, 1);
        studio.setProperty (IDs::nextId, id + 1, nullptr);
        return id;
    }

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
            const double offsetSeconds = offset * 60.0 / getBpm();

            juce::ValueTree existing;
            for (auto clip : track.getClips())
                if ((int) clip->state[IDs::clipId] == id)
                    existing = clip->state;

            if (! existing.isValid())
            {
                existing = juce::ValueTree (te::IDs::AUDIOCLIP);
                existing.setProperty (te::IDs::name, file.getFileNameWithoutExtension(), nullptr);
                existing.setProperty (te::IDs::source, file.getFullPathName(), nullptr);
                existing.setProperty (IDs::clipId, id, nullptr);
                edit->createNewItemID().writeID (existing, nullptr);
                existing.setProperty (te::IDs::start, startTime.inSeconds(), nullptr);
                existing.setProperty (te::IDs::length, (endTime - startTime).inSeconds(), nullptr);
                existing.setProperty (te::IDs::offset, offsetSeconds, nullptr);
                track.state.addChild (existing, -1, nullptr);
            }
            else
            {
                existing.setProperty (te::IDs::start, startTime.inSeconds(), nullptr);
                existing.setProperty (te::IDs::length, (endTime - startTime).inSeconds(), nullptr);
                existing.setProperty (te::IDs::offset, offsetSeconds, nullptr);
            }
        }
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
        // ponytail: regenerates every channel on any change. Fine for hundreds of notes;
        // switch to per-channel dirty flags if big projects get slow.
        const bool song = isSongMode();
        const double swingShift = getSwing() * stepBeats * (2.0 / 3.0);
        const auto pattern = getCurrentPatternState();
        double endBeats = song ? 0.0 : getPatternLengthBeats (pattern);

        if (song)
            endBeats = getSongLengthBeats();

        endBeats = juce::jmax (endBeats, getBarBeats());

        for (auto channel : getChannels())
        {
            auto track = getTrack (channel);
            if (track == nullptr)
                continue;

            auto clip = getDerivedClip (*track);
            if (clip == nullptr)
                continue;

            const int channelId = channel[IDs::id];
            auto& seq = clip->getSequence();
            seq.clear (nullptr);

            // Notes of pattern `p` that fall inside [offset, offset + clipLength), placed at clipStart.
            auto addNotes = [&] (const juce::ValueTree& p, double clipStart, double clipLength, double offset)
            {
                for (auto note : p)
                {
                    if ((int) note[IDs::channel] != channelId)
                        continue;

                    const double rel = (double) note[IDs::start] - offset;
                    if (rel < -1.0e-6 || rel >= clipLength - 1.0e-6)
                        continue;

                    double start = note[IDs::start];
                    const double stepIndex = start / stepBeats;
                    if (std::abs (stepIndex - std::round (stepIndex)) < 1.0e-4 && ((int) std::round (stepIndex)) % 2 == 1)
                        start += swingShift;

                    const double length = juce::jmin ((double) note[IDs::length], clipLength - rel);
                    seq.addNote (note[IDs::key], te::BeatPosition::fromBeats (clipStart + start - offset),
                                 te::BeatDuration::fromBeats (length), note[IDs::velocity], 0, nullptr);
                }
            };

            if (song)
            {
                for (auto pc : getPlaylist())
                    if (! isAudioClip (pc) && ! (bool) pc[IDs::muted])
                        addNotes (getPattern (pc[IDs::pattern]), pc[IDs::start], pc[IDs::length], pc[IDs::offset]);
            }
            else
            {
                addNotes (pattern, 0.0, endBeats, 0.0);
            }

            if (channel[IDs::type].toString() == ChannelType::audio)
                syncAudioClips (*track, channelId, song);

            const auto endTime = edit->tempoSequence.toTime (te::BeatPosition::fromBeats (endBeats));
            clip->state.setProperty (te::IDs::start, 0.0, nullptr);
            clip->state.setProperty (te::IDs::length, endTime.inSeconds(), nullptr);
        }

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
    void changeListenerCallback (juce::ChangeBroadcaster*) override { dirty = true; }

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
    int selectedChannel = 0;
    int changeCount = 0, editGeneration = 0;
    bool dirty = false;
    juce::File projectFile;
    double loopEndBeats = 4.0;
    const te::MPESourceID liveSourceId = te::createUniqueMPESourceID();
};
