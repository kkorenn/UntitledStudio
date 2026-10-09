#pragma once

#include "Studio.h"
#include "Widgets.h"

// Rendering the project to audio / stems / MIDI (offline, faster than real time).
namespace Exporter
{
    enum class Format { wav, flac, ogg, mp3 };

    struct Settings
    {
        Format format = Format::wav;
        double sampleRate = 44100.0;
        int bitDepth = 24;
        bool song = true;          // false = current pattern
        bool normalise = false;
        bool dither = true;
        double tailSeconds = 2.0;  // let reverbs/delays ring out
    };

    inline juce::String extensionFor (Format f)
    {
        switch (f)
        {
            case Format::flac: return ".flac";
            case Format::ogg:  return ".ogg";
            case Format::mp3:  return ".mp3";
            case Format::wav:  break;
        }
        return ".wav";
    }


    inline juce::File findLame()
    {
        for (auto path : { "/opt/homebrew/bin/lame", "/usr/local/bin/lame", "/usr/bin/lame" })
            if (juce::File (path).existsAsFile())
                return juce::File (path);
        return {};
    }

    inline std::unique_ptr<juce::AudioFormat> createFormat (Format f)
    {
        switch (f)
        {
            case Format::flac: return std::make_unique<juce::FlacAudioFormat>();
            case Format::ogg:  return std::make_unique<juce::OggVorbisAudioFormat>();
            case Format::mp3:
                if (auto lame = findLame(); lame.existsAsFile())
                    return std::make_unique<juce::LAMEEncoderAudioFormat> (lame);
                return {};
            case Format::wav:  break;
        }
        return std::make_unique<juce::WavAudioFormat>();
    }

    // Renders the chosen tracks (empty = everything) to `file`. Returns true on success.
    inline bool render (Studio& studio, const Settings& s, const juce::File& file, const juce::Array<te::Track*>& tracks, bool masterPlugins)
    {
        auto format = createFormat (s.format);
        if (format == nullptr)
            return false;

        auto& edit = studio.getEdit();
        if (studio.isPlaying())
            studio.togglePlay();

        const bool wasSong = studio.isSongMode();
        studio.setSongMode (s.song);
        studio.regenerateNow();

        const double endBeats = s.song ? juce::jmax (studio.getSongLengthBeats(), studio.getBarBeats())
                                       : studio.getPatternLengthBeats (studio.getCurrentPatternState());
        auto loop = s.song && studio.getSongLoop().getLength() > 0.0 ? studio.getSongLoop() : juce::Range<double> (0.0, endBeats);

        // Samplers load their sounds in the background; make sure they're ready.
        for (auto plugin : te::getAllPlugins (edit, false))
            if (auto sampler = dynamic_cast<te::SamplerPlugin*> (plugin))
                sampler->flushPendingUpdates();
            else if (auto ms = dynamic_cast<MultiSamplerPlugin*> (plugin))
                ms->flushPendingUpdates();

        te::Renderer::Parameters p (edit);
        p.destFile = file;
        p.audioFormat = format.get();
        p.bitDepth = s.format == Format::wav || s.format == Format::flac ? s.bitDepth : 16;
        p.sampleRateForAudio = s.sampleRate;
        p.blockSizeForAudio = 512;
        p.time = { edit.tempoSequence.toTime (te::BeatPosition::fromBeats (loop.getStart())),
                   edit.tempoSequence.toTime (te::BeatPosition::fromBeats (loop.getEnd())) };
        p.endAllowance = te::TimeDuration::fromSeconds (s.tailSeconds);
        p.shouldNormalise = s.normalise;
        p.normaliseToLevelDb = -0.3f;
        p.ditheringEnabled = s.dither && p.bitDepth < 32;
        p.useMasterPlugins = masterPlugins;
        p.usePlugins = true;
        p.checkNodesForAudio = false;
        p.quality = s.format == Format::mp3 ? 0 : -1;
        p.tracksToDo = te::toBitSet (tracks.isEmpty() ? te::getAllTracks (edit) : tracks);

        file.getParentDirectory().createDirectory();
        file.deleteFile();
        const auto result = te::Renderer::renderToFile ("Export", p);

        studio.setSongMode (wasSong);
        studio.transport().ensureContextAllocated(); // rendering frees the live playback graph
        return result.existsAsFile();
    }

    inline bool exportMix (Studio& studio, const Settings& s, const juce::File& file)
    {
        return render (studio, s, file, {}, true);
    }

    // One file per mixer insert that has channels routed to it (plus "Master" for unrouted ones).
    inline int exportStems (Studio& studio, const Settings& s, const juce::File& folder, const juce::String& baseName)
    {
        int written = 0;
        for (int insert = 0; insert <= Studio::numInserts; ++insert)
        {
            juce::Array<te::Track*> tracks;
            for (auto ch : studio.getChannels())
                if (studio.getChannelInsert (ch[IDs::id]) == insert)
                    if (auto t = studio.getTrack (ch))
                        tracks.add (t);
            if (tracks.isEmpty())
                continue;
            if (auto t = studio.getInsertTrack (insert))
                tracks.add (t);

            const auto name = juce::File::createLegalFileName (baseName + " - " + studio.getInsertName (insert));
            if (render (studio, s, folder.getChildFile (name + extensionFor (s.format)), tracks, false))
                ++written;
        }
        return written;
    }

    // Standard MIDI file: one track per channel, notes as they play in the song (or pattern).
    inline bool exportMidi (Studio& studio, bool song, const juce::File& file)
    {
        const bool wasSong = studio.isSongMode();
        studio.setSongMode (song);
        studio.regenerateNow();

        constexpr int ppq = 960;
        juce::MidiFile midi;
        midi.setTicksPerQuarterNote (ppq);

        juce::MidiMessageSequence tempoTrack;
        tempoTrack.addEvent (juce::MidiMessage::tempoMetaEvent ((int) (60000000.0 / studio.getBpm())));
        auto& sig = *studio.getEdit().tempoSequence.getTimeSig (0);
        tempoTrack.addEvent (juce::MidiMessage::timeSignatureMetaEvent (sig.numerator.get(), sig.denominator.get()));
        midi.addTrack (tempoTrack);

        for (auto ch : studio.getChannels())
        {
            auto track = studio.getTrack (ch);
            if (track == nullptr || ch[IDs::type].toString() == ChannelType::audio)
                continue;

            juce::MidiMessageSequence seq;
            seq.addEvent (juce::MidiMessage::textMetaEvent (3, ch[IDs::name].toString()));
            for (auto clip : track->getClips())
                if (auto mc = dynamic_cast<te::MidiClip*> (clip))
                    for (auto* n : mc->getSequence().getNotes())
                    {
                        const double start = n->getStartBeat().inBeats() * ppq;
                        const double end = n->getEndBeat().inBeats() * ppq;
                        seq.addEvent (juce::MidiMessage::noteOn (1, n->getNoteNumber(), (juce::uint8) n->getVelocity()), start);
                        seq.addEvent (juce::MidiMessage::noteOff (1, n->getNoteNumber()), end);
                    }
            seq.updateMatchedPairs();
            midi.addTrack (seq);
        }

        studio.setSongMode (wasSong);
        file.deleteFile();
        juce::FileOutputStream out (file);
        return out.openedOk() && midi.writeTo (out);
    }

    //==========================================================================
    // FL-style export window.
    class Dialog : public juce::Component
    {
    public:
        explicit Dialog (Studio& s, juce::String projectName) : studio (s), baseName (std::move (projectName))
        {
            formatBox.addItemList ({ "WAV", "FLAC", "OGG", findLame().existsAsFile() ? "MP3" : "MP3 (install LAME: brew install lame)" }, 1);
            formatBox.setSelectedItemIndex (0);
            rateBox.addItemList ({ "44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz" }, 1);
            rateBox.setSelectedItemIndex (0);
            bitsBox.addItemList ({ "16-bit", "24-bit", "32-bit float" }, 1);
            bitsBox.setSelectedItemIndex (1);
            modeBox.addItemList ({ "Full song", "Current pattern" }, 1);
            modeBox.setSelectedItemIndex (studio.isSongMode() ? 0 : 1);
            whatBox.addItemList ({ "Mixdown (one file)", "Stems (one file per mixer insert)", "MIDI file" }, 1);
            whatBox.setSelectedItemIndex (0);
            whatBox.onChange = [this] { updateEnabled(); };

            normalise.setButtonText ("Normalize");
            dither.setButtonText ("Dither");
            dither.setToggleState (true, juce::dontSendNotification);

            exportButton.onClick = [this] { chooseAndExport(); };

            for (juce::Component* c : std::initializer_list<juce::Component*> { &formatBox, &rateBox, &bitsBox, &modeBox, &whatBox,
                                                                                &normalise, &dither, &exportButton, &status })
                addAndMakeVisible (c);
            status.setColour (juce::Label::textColourId, Theme::textDim);
            updateEnabled();
            setSize (420, 330);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::panelDark);
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (13.0f));
            const char* labels[] = { "Export", "Range", "Format", "Sample rate", "Depth" };
            for (int i = 0; i < 5; ++i)
                g.drawText (labels[i], 16, 16 + i * 36, 100, 26, juce::Justification::centredLeft);
        }

        void resized() override
        {
            juce::Component* rows[] = { &whatBox, &modeBox, &formatBox, &rateBox, &bitsBox };
            for (int i = 0; i < 5; ++i)
                rows[i]->setBounds (120, 16 + i * 36, getWidth() - 136, 26);
            normalise.setBounds (120, 200, 120, 24);
            dither.setBounds (250, 200, 120, 24);
            exportButton.setBounds (getWidth() - 136, 240, 120, 30);
            status.setBounds (16, 280, getWidth() - 32, 40);
        }

    private:
        void updateEnabled()
        {
            const bool audio = whatBox.getSelectedItemIndex() != 2;
            for (auto* c : std::initializer_list<juce::Component*> { &formatBox, &rateBox, &bitsBox, &normalise, &dither })
                c->setEnabled (audio);
        }

        Settings getSettings() const
        {
            Settings s;
            s.format = (Format) formatBox.getSelectedItemIndex();
            s.sampleRate = std::array<double, 4> { 44100.0, 48000.0, 88200.0, 96000.0 }[(size_t) rateBox.getSelectedItemIndex()];
            s.bitDepth = std::array<int, 3> { 16, 24, 32 }[(size_t) bitsBox.getSelectedItemIndex()];
            s.song = modeBox.getSelectedItemIndex() == 0;
            s.normalise = normalise.getToggleState();
            s.dither = dither.getToggleState();
            return s;
        }

        void chooseAndExport()
        {
            const int what = whatBox.getSelectedItemIndex();
            const auto settings = getSettings();
            const auto folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Untitled Studio").getChildFile ("Exports");
            folder.createDirectory();

            if (what == 1)
            {
                chooser = std::make_unique<juce::FileChooser> ("Choose a folder for the stems", folder);
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                      [this, settings] (const juce::FileChooser& fc)
                                      {
                                          if (! fc.getResult().isDirectory()) return;
                                          status.setText ("Rendering stems...", juce::dontSendNotification);
                                          const int n = exportStems (studio, settings, fc.getResult(), baseName);
                                          status.setText ("Wrote " + juce::String (n) + " stems to " + fc.getResult().getFullPathName(), juce::dontSendNotification);
                                      });
                return;
            }

            const auto ext = what == 2 ? juce::String (".mid") : extensionFor (settings.format);
            chooser = std::make_unique<juce::FileChooser> ("Export as", folder.getChildFile (baseName + ext), "*" + ext);
            chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this, settings, what, ext] (const juce::FileChooser& fc)
                                  {
                                      if (fc.getResult() == juce::File()) return;
                                      const auto file = fc.getResult().withFileExtension (ext);
                                      status.setText ("Rendering...", juce::dontSendNotification);
                                      const bool ok = what == 2 ? exportMidi (studio, settings.song, file) : exportMix (studio, settings, file);
                                      status.setText (ok ? "Saved " + file.getFullPathName() : juce::String ("Export failed"), juce::dontSendNotification);
                                      if (ok) file.revealToUser();
                                  });
        }

        Studio& studio;
        juce::String baseName;
        juce::ComboBox formatBox, rateBox, bitsBox, modeBox, whatBox;
        juce::ToggleButton normalise, dither;
        juce::TextButton exportButton { "Export..." };
        juce::Label status;
        std::unique_ptr<juce::FileChooser> chooser;
    };
}
