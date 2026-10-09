#pragma once

#include "DrumKit.h"

namespace te = tracktion;

// FL-style project model, stored as a ValueTree inside the Tracktion Edit so it
// saves/loads/undoes together with the engine state.
//
//   STUDIO (currentPattern, songMode, swing)
//     CHANNELS / CHANNEL (id, name, type, track, file, colour)
//     PATTERNS / PATTERN (id, name, colour) / NOTE (channel, start, length, key, velocity)
//     PLAYLIST / PCLIP   (pattern, lane, start, length)
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
    STUDIO_ID (derived) STUDIO_ID (nextId)
   #undef STUDIO_ID
}

namespace ChannelType
{
    inline const juce::String sampler = "sampler";
    inline const juce::String synth   = "synth";
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
        selectedChannel = getChannels().getChild (0)[IDs::id];
        updateMidiInputTarget();
        regenerateNow();
        undo().clearUndoHistory();
        undo().dispatchPendingMessages();
        dirty = false;
    }

    void setEdit (std::unique_ptr<te::Edit> newEdit)
    {
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
        for (auto type : { IDs::CHANNELS, IDs::PATTERNS, IDs::PLAYLIST })
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
            for (auto pc : studio.getChildWithName (IDs::PLAYLIST))
                endBeats = juce::jmax (endBeats, (double) pc[IDs::start] + (double) pc[IDs::length]);

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

            auto addNotes = [&] (const juce::ValueTree& p, double offset, double maxLength)
            {
                for (auto note : p)
                {
                    if ((int) note[IDs::channel] != channelId)
                        continue;

                    double start = note[IDs::start];
                    if (start >= maxLength)
                        continue;

                    const double stepIndex = start / stepBeats;
                    if (std::abs (stepIndex - std::round (stepIndex)) < 1.0e-4 && ((int) std::round (stepIndex)) % 2 == 1)
                        start += swingShift;

                    const double length = juce::jmin ((double) note[IDs::length], maxLength - (double) note[IDs::start]);
                    seq.addNote (note[IDs::key], te::BeatPosition::fromBeats (offset + start),
                                 te::BeatDuration::fromBeats (length), note[IDs::velocity], 0, nullptr);
                }
            };

            if (song)
            {
                for (auto pc : studio.getChildWithName (IDs::PLAYLIST))
                    addNotes (getPattern (pc[IDs::pattern]), pc[IDs::start], pc[IDs::length]);
            }
            else
            {
                addNotes (pattern, 0.0, endBeats);
            }

            const auto endTime = edit->tempoSequence.toTime (te::BeatPosition::fromBeats (endBeats));
            clip->state.setProperty (te::IDs::start, 0.0, nullptr);
            clip->state.setProperty (te::IDs::length, endTime.inSeconds(), nullptr);
        }

        transport().setLoopRange ({ 0s, edit->tempoSequence.toTime (te::BeatPosition::fromBeats (endBeats)) });
        loopEndBeats = endBeats;
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
