#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "Synth.h"

static const juce::Colour backgroundColour { 0xff2b3237 };

// FL Studio "typing keyboard to piano" layout.
// Z-row = lower octave, Q-row = upper octave (continues to P).
static constexpr int lowerRowRoot = 48; // Z = C3
static constexpr std::array<char, 12> lowerRowKeys { 'z', 's', 'x', 'd', 'c', 'v', 'g', 'b', 'h', 'n', 'j', 'm' };
static constexpr std::array<char, 17> upperRowKeys { 'q', '2', 'w', '3', 'e', 'r', '5', 't', '6', 'y', '7', 'u', 'i', '9', 'o', '0', 'p' };

class MainComponent : public juce::AudioAppComponent,
                      private juce::MidiInputCallback,
                      private juce::MidiKeyboardState::Listener
{
public:
    MainComponent()
    {
        for (int i = 0; i < 16; ++i)
            synth.addVoice (new SynthVoice (params));
        synth.addSound (new SynthSound());

        waveformBox.addItemList ({ "Sine", "Saw", "Square", "Triangle" }, 1);
        waveformBox.setSelectedItemIndex (params.waveform.load(), juce::dontSendNotification);
        waveformBox.onChange = [this] { params.waveform.store (waveformBox.getSelectedItemIndex()); };
        addControl (waveformBox);

        setupKnob (attackKnob,    "Attack",  0.001, 2.0,     0.2,    params.attack);
        setupKnob (decayKnob,     "Decay",   0.001, 2.0,     0.3,    params.decay);
        setupKnob (sustainKnob,   "Sustain", 0.0,   1.0,     0.5,    params.sustain);
        setupKnob (releaseKnob,   "Release", 0.001, 3.0,     0.3,    params.release);
        setupKnob (cutoffKnob,    "Cutoff",  20.0,  20000.0, 1000.0, cutoff);
        setupKnob (resonanceKnob, "Reso",    0.0,   1.0,     0.5,    resonance);
        setupKnob (volumeKnob,    "Volume",  0.0,   1.0,     0.5,    volume);

        settingsButton.onClick = [this] { showAudioSettings(); };
        addControl (settingsButton);

        keyboard.clearKeyMappings(); // typing keys handled below, FL layout
        keyboard.setAvailableRange (24, 108);
        keyboard.setLowestVisibleKey (36);
        keyboard.setVelocity (100.0f / 127.0f, true);
        keyboardState.addListener (this);
        addControl (keyboard);

        setWantsKeyboardFocus (true);
        setSize (1280, 800);
        setAudioChannels (0, 2);

        // Like FL: every connected MIDI keyboard just works.
        for (const auto& device : juce::MidiInput::getAvailableDevices())
            deviceManager.setMidiInputDeviceEnabled (device.identifier, true);
        deviceManager.addMidiInputDeviceCallback ({}, this);
    }

    ~MainComponent() override
    {
        deviceManager.removeMidiInputDeviceCallback ({}, this);
        keyboardState.removeListener (this);
        shutdownAudio();
    }

    // ---- Audio thread: no allocation, no locks, no file I/O, no logging ----

    void prepareToPlay (int blockSize, double sampleRate) override
    {
        synth.setCurrentPlaybackSampleRate (sampleRate);
        midiBuffer.ensureSize (4096);

        filter.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });
        filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);

        masterGain.reset (sampleRate, 0.02);
        cutoffSmoothed.reset (sampleRate, 0.02);
        resonanceSmoothed.reset (sampleRate, 0.02);

        masterGain.setCurrentAndTargetValue (volume.load());
        cutoffSmoothed.setCurrentAndTargetValue (cutoff.load());
        resonanceSmoothed.setCurrentAndTargetValue (resonanceToQ (resonance.load()));
        filter.setCutoffFrequency (cutoffSmoothed.getCurrentValue());
        filter.setResonance (resonanceSmoothed.getCurrentValue());
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();

        midiBuffer.clear();
        keyboardFifo.drainInto (midiBuffer);
        midiInFifo.drainInto (midiBuffer);
        synth.renderNextBlock (*info.buffer, midiBuffer, info.startSample, info.numSamples);

        masterGain.setTargetValue (volume.load());
        cutoffSmoothed.setTargetValue (cutoff.load());
        resonanceSmoothed.setTargetValue (resonanceToQ (resonance.load()));

        auto& buffer = *info.buffer;
        const int numChannels = juce::jmin (buffer.getNumChannels(), 2);

        for (int i = info.startSample; i < info.startSample + info.numSamples; ++i)
        {
            if (cutoffSmoothed.isSmoothing())    filter.setCutoffFrequency (cutoffSmoothed.getNextValue());
            if (resonanceSmoothed.isSmoothing()) filter.setResonance (resonanceSmoothed.getNextValue());
            const float gain = masterGain.getNextValue();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float out = filter.processSample (ch, buffer.getSample (ch, i)) * gain;
                buffer.setSample (ch, i, juce::jlimit (-1.0f, 1.0f, out)); // hard safety ceiling
            }
        }
    }

    void releaseResources() override {}

    // ---- MIDI device thread ----

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
    {
        midiInFifo.push (m);

        if (m.isNoteOnOrOff())
        {
            // Light up the on-screen keyboard (UI thread). Sound already went via midiInFifo.
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this), m]
            {
                if (safe == nullptr)
                    return;
                safe->showingDeviceNote = true;
                safe->keyboardState.processNextMidiEvent (m);
                safe->showingDeviceNote = false;
            });
        }
    }

    // ---- On-screen keyboard (UI thread): mouse clicks + typing keys arrive here ----

    void handleNoteOn (juce::MidiKeyboardState*, int channel, int note, float velocity) override
    {
        if (! showingDeviceNote)
            keyboardFifo.push (juce::MidiMessage::noteOn (channel, note, velocity));
    }

    void handleNoteOff (juce::MidiKeyboardState*, int channel, int note, float) override
    {
        if (! showingDeviceNote)
            keyboardFifo.push (juce::MidiMessage::noteOff (channel, note));
    }

    // ---- UI thread ----

    void paint (juce::Graphics& g) override
    {
        g.fillAll (backgroundColour);
        g.setColour (juce::Colour (0xff3a434a));
        g.fillRect (getLocalBounds().removeFromTop (toolbarHeight));

        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (14.0f);
        g.drawText ("Play with your computer keyboard: Z-row = lower octave, Q-row = upper octave. MIDI keyboards work automatically.",
                    getLocalBounds().withTrimmedTop (toolbarHeight + knobRowHeight + 20).reduced (16, 0).removeFromTop (20),
                    juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto bar = getLocalBounds().removeFromTop (toolbarHeight).reduced (8, 6);
        settingsButton.setBounds (bar.removeFromRight (120));

        auto row = getLocalBounds().withTrimmedTop (toolbarHeight + 8).removeFromTop (knobRowHeight).reduced (16, 0);
        waveformBox.setBounds (row.removeFromLeft (110).withSizeKeepingCentre (110, 26));
        row.removeFromLeft (16);

        for (auto* knob : { &attackKnob, &decayKnob, &sustainKnob, &releaseKnob, &cutoffKnob, &resonanceKnob, &volumeKnob })
            knob->slider.setBounds (row.removeFromLeft (72).withTrimmedTop (20));

        keyboard.setBounds (getLocalBounds().removeFromBottom (keyboardHeight));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::F10Key) { showAudioSettings(); return true; }
        return noteForKey (key.getKeyCode()) >= 0; // swallow note keys
    }

    bool keyStateChanged (bool) override
    {
        if (juce::ModifierKeys::currentModifiers.isCommandDown() || juce::ModifierKeys::currentModifiers.isCtrlDown())
            return false;

        bool handled = false;
        for (auto& [keyCode, note] : typingKeys())
        {
            const bool down = juce::KeyPress::isKeyCurrentlyDown (keyCode);
            if (down == heldNotes[(size_t) note])
                continue;

            heldNotes[(size_t) note] = down;
            if (down) keyboardState.noteOn (1, note, 100.0f / 127.0f);
            else      keyboardState.noteOff (1, note, 0.0f);
            handled = true;
        }
        return handled;
    }

    // No stuck notes when the window loses focus (e.g. opening a dialog).
    void focusLost (FocusChangeType) override { releaseTypingKeys(); }

    void visibilityChanged() override
    {
        if (isShowing())
            grabKeyboardFocus();
    }

private:
    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
    };

    static const std::vector<std::pair<int, int>>& typingKeys()
    {
        static const auto keys = []
        {
            std::vector<std::pair<int, int>> k;
            for (size_t i = 0; i < lowerRowKeys.size(); ++i) k.emplace_back (lowerRowKeys[i], lowerRowRoot + (int) i);
            for (size_t i = 0; i < upperRowKeys.size(); ++i) k.emplace_back (upperRowKeys[i], lowerRowRoot + 12 + (int) i);
            return k;
        }();
        return keys;
    }

    static int noteForKey (int keyCode)
    {
        for (auto& [code, note] : typingKeys())
            if (code == juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) keyCode))
                return note;
        return -1;
    }

    void releaseTypingKeys()
    {
        for (int note = 0; note < 128; ++note)
        {
            if (heldNotes[(size_t) note])
            {
                heldNotes[(size_t) note] = false;
                keyboardState.noteOff (1, note, 0.0f);
            }
        }
    }

    // Maps "Reso" knob 0..1 to filter Q (0.707 = flat, higher = more peak).
    static float resonanceToQ (float r) { return 0.707f + r * r * 8.0f; }

    void addControl (juce::Component& c)
    {
        // Controls never steal keyboard focus, so typing-keyboard notes always work.
        c.setWantsKeyboardFocus (false);
        c.setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (c);
    }

    void setupKnob (Knob& knob, const juce::String& name, double min, double max, double mid, std::atomic<float>& target)
    {
        knob.slider.setRange (min, max);
        knob.slider.setSkewFactorFromMidPoint (mid);
        knob.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, true, 64, 16);
        knob.slider.setNumDecimalPlacesToDisplay (2);
        knob.slider.setValue (target.load(), juce::dontSendNotification);
        knob.slider.setDoubleClickReturnValue (true, target.load());
        knob.slider.onValueChange = [&knob, &target] { target.store ((float) knob.slider.getValue()); };
        addControl (knob.slider);

        knob.label.setText (name, juce::dontSendNotification);
        knob.label.setJustificationType (juce::Justification::centred);
        knob.label.attachToComponent (&knob.slider, false);
    }

    void showAudioSettings()
    {
        auto* selector = new juce::AudioDeviceSelectorComponent (deviceManager, 0, 0, 2, 2,
                                                                 true, false, true, false);
        selector->setSize (520, 480);

        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (selector);
        options.dialogTitle = "Audio Settings";
        options.dialogBackgroundColour = backgroundColour;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    }

    static constexpr int toolbarHeight = 40;
    static constexpr int knobRowHeight = 110;
    static constexpr int keyboardHeight = 100;

    // Shared between UI and audio threads.
    SynthParams params;
    std::atomic<float> cutoff { 20000.0f }, resonance { 0.0f }, volume { 0.7f };
    MidiFifo keyboardFifo, midiInFifo;

    // Audio thread only.
    juce::Synthesiser synth;
    juce::MidiBuffer midiBuffer;
    juce::dsp::StateVariableTPTFilter<float> filter;
    juce::SmoothedValue<float> masterGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoffSmoothed;
    juce::SmoothedValue<float> resonanceSmoothed;

    // UI thread only.
    std::array<bool, 128> heldNotes {};
    bool showingDeviceNote = false;
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard { keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard };
    juce::ComboBox waveformBox;
    Knob attackKnob, decayKnob, sustainKnob, releaseKnob, cutoffKnob, resonanceKnob, volumeKnob;
    juce::TextButton settingsButton { "Audio Settings" };
};

class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (const juce::String& name)
        : DocumentWindow (name, backgroundColour, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent(), true);
        setResizable (true, true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class UntitledStudioApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    void initialise (const juce::String&) override { mainWindow = std::make_unique<MainWindow> (getApplicationName()); }
    void shutdown() override                       { mainWindow = nullptr; }
    void systemRequestedQuit() override            { quit(); }

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (UntitledStudioApp)
