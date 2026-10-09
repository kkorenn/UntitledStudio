#pragma once

#include "PluginEditor.h"
#include "Studio.h"

// FL note names: MIDI 60 = C5.
inline juce::String noteName (int note) { return juce::MidiMessage::getMidiNoteName (note, true, true, 5); }

// Sampler controls: waveform with draggable start/end markers, root note, replace sample.
class SamplerView : public juce::Component,
                    public juce::DragAndDropTarget,
                    public juce::FileDragAndDropTarget,
                    private juce::ChangeListener,
                    private juce::Timer
{
public:
    SamplerView (Studio& s, int channel, te::SamplerPlugin& p) : studio (s), channelId (channel), sampler (&p)
    {
        thumbnail.addChangeListener (this);

        root.format = [] (double v) { return noteName ((int) v); };
        root.setTooltip ("Root note: the key that plays the sample at its original pitch");
        root.onChange = [this] (double v)
        {
            studio.undo().beginNewTransaction ("Root note");
            sampler->setSoundParams (0, (int) v, 0, 127);
        };
        addAndMakeVisible (root);

        replaceButton.setTooltip ("Load a different sample (or drag one here from the browser)");
        replaceButton.onClick = [this] { browseForSample(); };
        addAndMakeVisible (replaceButton);

        loadThumbnail();
        startTimerHz (10);
    }

    ~SamplerView() override { thumbnail.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panelDark);
        const auto channel = studio.getChannel (channelId);
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (24);
        g.setColour (Theme::text);
        g.setFont (Theme::font (14.0f, true));
        g.drawText (channel[IDs::name].toString(), top.withTrimmedRight (260), juce::Justification::centredLeft, true);
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (12.0f));
        g.drawText ("ROOT", rootLabelArea, juce::Justification::centredRight);

        const auto wave = getWaveArea();
        g.setColour (Theme::lcd);
        g.fillRect (wave);

        const double total = thumbnail.getTotalLength();
        if (total > 0.0)
        {
            g.setColour (Theme::lcdText.withAlpha (0.85f));
            thumbnail.drawChannels (g, wave.reduced (0, 4), 0.0, total, 1.0f);

            // Dim what's outside start..end.
            const int sx = timeToX (getStart()), ex = timeToX (getEnd());
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRect (wave.withRight (sx));
            g.fillRect (wave.withLeft (ex));
            g.setColour (Theme::accent);
            g.fillRect (sx - 1, wave.getY(), 2, wave.getHeight());
            g.fillRect (ex - 1, wave.getY(), 2, wave.getHeight());
        }
        else
        {
            g.setColour (Theme::textDim);
            g.drawText ("Drop a sample here", wave, juce::Justification::centred);
        }

        if (dragOver)
        {
            g.setColour (Theme::accent);
            g.drawRect (getLocalBounds(), 2);
        }
    }

    void resized() override
    {
        auto top = getLocalBounds().reduced (10).removeFromTop (24);
        replaceButton.setBounds (top.removeFromRight (130));
        top.removeFromRight (10);
        root.setBounds (top.removeFromRight (56));
        rootLabelArea = top.removeFromRight (40);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        draggingMarker = 0;
        if (thumbnail.getTotalLength() <= 0.0 || ! getWaveArea().contains (e.getPosition()))
            return;

        const int sx = timeToX (getStart()), ex = timeToX (getEnd());
        draggingMarker = std::abs (e.x - sx) <= std::abs (e.x - ex) ? 1 : 2;
        studio.undo().beginNewTransaction ("Sample start/end");
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingMarker == 0)
            return;

        const double t = xToTime (e.x);
        double start = getStart(), end = getEnd();
        if (draggingMarker == 1) start = juce::jlimit (0.0, end - 0.005, t);
        else                     end = juce::jlimit (start + 0.005, thumbnail.getTotalLength(), t);

        const bool wholeEnd = std::abs (end - thumbnail.getTotalLength()) < 1.0e-4;
        sampler->setSoundExcerpt (0, start, wholeEnd && start <= 0.0 ? 0.0 : end - start);
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        studio.undo().beginNewTransaction ("Reset sample start/end");
        sampler->setSoundExcerpt (0, 0.0, 0.0);
        repaint();
    }

    // Drops from the browser or Finder replace the sample.
    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString().startsWith ("file:"); }
    void itemDragEnter (const SourceDetails&) override             { dragOver = true; repaint(); }
    void itemDragExit (const SourceDetails&) override              { dragOver = false; repaint(); }
    void itemDropped (const SourceDetails& d) override
    {
        dragOver = false;
        replace (juce::File (d.description.toString().fromFirstOccurrenceOf ("file:", false, false)));
    }

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override { replace (juce::File (files[0])); }

private:
    double getStart() const { return sampler->getNumSounds() > 0 ? sampler->getSoundStartTime (0) : 0.0; }

    double getEnd() const
    {
        const double length = sampler->getNumSounds() > 0 ? sampler->getSoundLength (0) : 0.0;
        return length > 0.0 ? juce::jmin (thumbnail.getTotalLength(), getStart() + length) : thumbnail.getTotalLength();
    }

    juce::Rectangle<int> getWaveArea() const { return getLocalBounds().reduced (10).withTrimmedTop (34); }

    int timeToX (double t) const
    {
        const auto w = getWaveArea();
        return w.getX() + (int) (w.getWidth() * t / juce::jmax (1.0e-6, thumbnail.getTotalLength()));
    }

    double xToTime (int x) const
    {
        const auto w = getWaveArea();
        return juce::jlimit (0.0, thumbnail.getTotalLength(), (x - w.getX()) / (double) w.getWidth() * thumbnail.getTotalLength());
    }

    void loadThumbnail()
    {
        currentFile = juce::File (studio.getChannel (channelId)[IDs::file].toString());
        thumbnail.setSource (currentFile.existsAsFile() ? new juce::FileInputSource (currentFile) : nullptr);
        repaint();
    }

    void replace (const juce::File& f)
    {
        if (f.existsAsFile())
        {
            studio.replaceSample (channelId, f);
            loadThumbnail();
        }
    }

    void browseForSample()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a sample", DrumKit::getPacksFolder(),
                                                       studio.engine.getAudioFileFormatManager().readFormatManager.getWildcardForAllFormats());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { replace (fc.getResult()); });
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    void timerCallback() override
    {
        // Follow undo/redo and external changes.
        if (sampler->getNumSounds() > 0)
            root.setValue (sampler->getKeyNote (0));
        if (juce::File (studio.getChannel (channelId)[IDs::file].toString()) != currentFile)
            loadThumbnail();
    }

    Studio& studio;
    const int channelId;
    juce::ReferenceCountedObjectPtr<te::SamplerPlugin> sampler;
    juce::AudioThumbnailCache cache { 4 };
    juce::AudioThumbnail thumbnail { 256, studio.engine.getAudioFileFormatManager().readFormatManager, cache };
    juce::File currentFile;
    Widgets::DragNumber root { 0.0, 127.0, 0.25 };
    juce::Rectangle<int> rootLabelArea;
    juce::TextButton replaceButton { "Replace sample..." };
    std::unique_ptr<juce::FileChooser> chooser;
    int draggingMarker = 0;
    bool dragOver = false;
};

// FL's channel settings window: the instrument's controls + a playable keyboard at the bottom.
class ChannelSettings : public juce::Component, private juce::MidiKeyboardState::Listener
{
public:
    ChannelSettings (Studio& s, int channel) : studio (s), channelId (channel)
    {
        if (auto track = studio.getTrack (channelId))
        {
            if (auto plugin = track->pluginList.getPlugins().getFirst())
            {
                if (auto sampler = dynamic_cast<te::SamplerPlugin*> (plugin.get()))
                    editor = std::make_unique<SamplerView> (studio, channelId, *sampler);
                else
                    editor = std::make_unique<GenericPluginEditor> (*plugin);
                addAndMakeVisible (*editor);
            }
        }

        keyboard.setAvailableRange (24, 108);
        keyboard.setLowestVisibleKey (48);
        keyboard.setVelocity (100.0f / 127.0f, true);
        keyboard.clearKeyMappings();
        keyboard.setOctaveForMiddleC (5); // FL naming
        keyboardState.addListener (this);
        addAndMakeVisible (keyboard);

        Widgets::noFocus (*this);
        const bool isSampler = dynamic_cast<SamplerView*> (editor.get()) != nullptr;
        setSize (isSampler ? 560 : 640, isSampler ? 300 : 460);
    }

    ~ChannelSettings() override { keyboardState.removeListener (this); }

    int getChannelId() const { return channelId; }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panelDark); }

    void resized() override
    {
        auto r = getLocalBounds();
        keyboard.setBounds (r.removeFromBottom (70));
        if (editor != nullptr)
            editor->setBounds (r);
    }

private:
    void handleNoteOn (juce::MidiKeyboardState*, int ch, int note, float vel) override
    {
        studio.selectChannel (channelId);
        studio.playLiveNote (juce::MidiMessage::noteOn (ch, note, vel));
    }

    void handleNoteOff (juce::MidiKeyboardState*, int ch, int note, float) override
    {
        studio.playLiveNote (juce::MidiMessage::noteOff (ch, note));
    }

    Studio& studio;
    const int channelId;
    std::unique_ptr<juce::Component> editor;
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard { keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard };
};
