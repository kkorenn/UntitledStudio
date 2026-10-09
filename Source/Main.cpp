#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

static const juce::Colour backgroundColour { 0xff2b3237 };

class MainComponent : public juce::AudioAppComponent
{
public:
    MainComponent()
    {
        playButton.setClickingTogglesState (true);
        playButton.onClick = [this]
        {
            const bool on = playButton.getToggleState();
            playing.store (on);
            playButton.setButtonText (on ? "Stop" : "Play");
        };

        volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volumeSlider.setRange (0.0, 1.0);
        volumeSlider.setValue (volume.load(), juce::dontSendNotification);
        volumeSlider.setDoubleClickReturnValue (true, 0.5);
        volumeSlider.onValueChange = [this] { volume.store ((float) volumeSlider.getValue()); };

        settingsButton.onClick = [this] { showAudioSettings(); };

        // Keep keyboard focus on this component so Space / F10 always work, like FL.
        for (juce::Component* c : { (juce::Component*) &playButton, (juce::Component*) &volumeSlider, (juce::Component*) &settingsButton })
        {
            c->setWantsKeyboardFocus (false);
            c->setMouseClickGrabsKeyboardFocus (false);
            addAndMakeVisible (c);
        }
        setWantsKeyboardFocus (true);

        setSize (1280, 800);
        setAudioChannels (0, 2);
    }

    ~MainComponent() override { shutdownAudio(); }

    // ---- Audio thread: no allocation, no locks, no file I/O, no logging ----

    void prepareToPlay (int, double sampleRate) override
    {
        currentSampleRate = sampleRate;
        gain.reset (sampleRate, 0.02);
        gain.setCurrentAndTargetValue (0.0f);
        phase = 0.0;
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        auto& buffer = *info.buffer;
        gain.setTargetValue (playing.load() ? volume.load() * maxLevel : 0.0f);

        const double twoPi = juce::MathConstants<double>::twoPi;
        const double delta = twoPi * frequency / currentSampleRate;

        for (int i = 0; i < info.numSamples; ++i)
        {
            const float sample = (float) std::sin (phase) * gain.getNextValue();
            phase += delta;
            if (phase >= twoPi)
                phase -= twoPi;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, info.startSample + i, sample);
        }
    }

    void releaseResources() override {}

    // ---- UI thread ----

    void paint (juce::Graphics& g) override
    {
        g.fillAll (backgroundColour);
        g.setColour (juce::Colour (0xff3a434a));
        g.fillRect (getLocalBounds().removeFromTop (toolbarHeight));
    }

    void resized() override
    {
        auto bar = getLocalBounds().removeFromTop (toolbarHeight).reduced (8, 6);
        playButton.setBounds (bar.removeFromLeft (70));
        bar.removeFromLeft (12);
        volumeSlider.setBounds (bar.removeFromLeft (160));
        settingsButton.setBounds (bar.removeFromRight (120));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::spaceKey) { playButton.triggerClick(); return true; }
        if (key == juce::KeyPress::F10Key)   { showAudioSettings();       return true; }
        return false;
    }

    void visibilityChanged() override
    {
        if (isShowing())
            grabKeyboardFocus();
    }

private:
    void showAudioSettings()
    {
        auto* selector = new juce::AudioDeviceSelectorComponent (deviceManager, 0, 0, 2, 2,
                                                                 false, false, true, false);
        selector->setSize (520, 420);

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
    static constexpr float maxLevel = 0.25f; // -12 dBFS ceiling: protects ears and speakers
    static constexpr double frequency = 440.0;

    // Shared between UI and audio threads.
    std::atomic<bool> playing { false };
    std::atomic<float> volume { 0.5f };

    // Audio thread only.
    double currentSampleRate = 44100.0;
    double phase = 0.0;
    juce::SmoothedValue<float> gain;

    juce::TextButton playButton { "Play" };
    juce::Slider volumeSlider;
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
