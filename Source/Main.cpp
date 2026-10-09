#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;
using namespace std::literals;

#include "Theme.h"
#include "Widgets.h"
#include "Panel.h"
#include "Studio.h"
#include "PluginEditor.h"
#include "ChannelSettings.h"
#include "ChannelRack.h"

//==============================================================================
// FL "typing keyboard to piano": Z-row = lower octave, Q-row = upper octave.
class TypingKeyboard
{
public:
    std::function<void (const juce::MidiMessage&)> onMidi;

    static int noteForKey (int keyCode)
    {
        for (auto& [code, note] : keys())
            if (code == juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) keyCode))
                return note;
        return -1;
    }

    bool update()
    {
        if (juce::ModifierKeys::currentModifiers.isCommandDown() || juce::ModifierKeys::currentModifiers.isCtrlDown())
            return false;

        bool handled = false;
        for (auto& [code, note] : keys())
        {
            const bool down = juce::KeyPress::isKeyCurrentlyDown (code);
            if (down != held[(size_t) note])
            {
                held[(size_t) note] = down;
                onMidi (down ? juce::MidiMessage::noteOn (1, note, (juce::uint8) 100) : juce::MidiMessage::noteOff (1, note));
                handled = true;
            }
        }
        return handled;
    }

    void releaseAll()
    {
        for (int note = 0; note < 128; ++note)
            if (std::exchange (held[(size_t) note], false))
                onMidi (juce::MidiMessage::noteOff (1, note));
    }

private:
    static const std::vector<std::pair<int, int>>& keys()
    {
        static const auto k = []
        {
            const int root = 48; // Z = C (one octave below FL's C5 = 60)
            const std::string lower = "zsxdcvgbhnjm", upper = "q2w3er5t6y7ui9o0p";
            std::vector<std::pair<int, int>> v;
            for (size_t i = 0; i < lower.size(); ++i) v.emplace_back (lower[i], root + (int) i);
            for (size_t i = 0; i < upper.size(); ++i) v.emplace_back (upper[i], root + 12 + (int) i);
            return v;
        }();
        return k;
    }

    std::array<bool, 128> held {};
};

//==============================================================================
class TransportButton : public juce::Button
{
public:
    enum Shape { play, stop, record };

    TransportButton (Shape s, const juce::String& hint) : juce::Button (hint), shape (s) { setTooltip (hint); }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (down ? Theme::panelDark : (over ? Theme::titleActive : Theme::panel));
        g.fillRoundedRectangle (r, 3.0f);

        const auto icon = r.withSizeKeepingCentre (r.getHeight() * 0.42f, r.getHeight() * 0.42f);
        juce::Path p;
        if (shape == play)        p.addTriangle (icon.getTopLeft(), icon.getBottomLeft(), { icon.getRight(), icon.getCentreY() });
        else if (shape == stop)   p.addRectangle (icon);
        else                      p.addEllipse (icon);

        g.setColour (shape == play && getToggleState() ? Theme::led
                                                       : (shape == record ? juce::Colour (0xffd2463c) : Theme::text));
        g.fillPath (p);
    }

private:
    Shape shape;
};

//==============================================================================
class MainComponent : public juce::Component,
                      public juce::MenuBarModel,
                      private juce::Timer
{
public:
    MainComponent (te::Engine& e) : studio (e)
    {
        setLookAndFeel (&lookAndFeel);
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        menuBar.setModel (this);
        addAndMakeVisible (menuBar);

        patSongButton.setClickingTogglesState (true);
        patSongButton.setTooltip ("Pattern / Song mode (L)");
        patSongButton.onClick = [this] { studio.setSongMode (patSongButton.getToggleState()); updatePatSong(); };
        addAndMakeVisible (patSongButton);

        playButton.onClick = [this] { studio.togglePlay(); };
        stopButton.onClick = [this] { if (studio.isPlaying()) studio.togglePlay(); else studio.transport().setPosition (0s); };
        recordButton.setEnabled (false);
        for (auto* b : { &playButton, &stopButton, &recordButton })
            addAndMakeVisible (b);

        tempo.format = [] (double v) { return juce::String (v, 3); };
        tempo.setTooltip ("Tempo in BPM. Drag up/down, Ctrl for fine, double-click to type");
        tempo.onChange = [this] (double bpm) { studio.setBpm (bpm); };
        addAndMakeVisible (tempo);

        metronome.setTooltip ("Metronome");
        metronome.onClick = [this] { studio.setMetronome (metronome.getToggleState()); };
        addAndMakeVisible (metronome);

        channelRackButton.setTooltip ("Channel rack (F6)");
        channelRackButton.onClick = [this] { channelRackPanel->toggle(); };
        addAndMakeVisible (channelRackButton);

        addAndMakeVisible (workspace);

        auto rack = std::make_unique<ChannelRack> (studio);
        rack->onOpenChannel = [this] (int id) { toggleChannelWindow (id); };
        channelRackPanel = std::make_unique<Panel> ("Channel rack", std::move (rack));
        workspace.addAndMakeVisible (*channelRackPanel);
        channelRackPanel->setBounds (260, 60, 620, 260);

        typing.onMidi = [this] (const juce::MidiMessage& m) { studio.playLiveNote (m); };

        Widgets::noFocus (*this);
        setWantsKeyboardFocus (true);
        setSize (1400, 860);
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        menuBar.setModel (nullptr);
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        setLookAndFeel (nullptr);
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::background);
        g.setColour (Theme::toolbar);
        g.fillRect (getLocalBounds().removeFromTop (menuHeight + toolbarHeight));

        // Hint bar (FL shows help for whatever is under the mouse here).
        g.setColour (Theme::lcd);
        g.fillRoundedRectangle (hintArea.toFloat(), 3.0f);
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (12.0f));
        g.drawText (hint, hintArea.reduced (8, 0), juce::Justification::centredLeft, true);

        // Song position: bar:step:tick
        g.setColour (Theme::lcd);
        g.fillRoundedRectangle (timeArea.toFloat(), 3.0f);
        g.setColour (Theme::lcdText);
        g.setFont (Theme::font ((float) timeArea.getHeight() * 0.62f, true));
        g.drawText (timeText, timeArea, juce::Justification::centred);

        g.setColour (Theme::textDim);
        g.setFont (Theme::font (11.0f));
        g.drawText ("CPU " + juce::String (juce::roundToInt (cpu * 100.0)) + "%", cpuArea, juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (menuHeight);
        menuBar.setBounds (top.removeFromLeft (260));
        cpuArea = top.removeFromRight (70);
        hintArea = top.reduced (4, 3);

        auto bar = r.removeFromTop (toolbarHeight).reduced (6, 6);
        patSongButton.setBounds (bar.removeFromLeft (64));
        bar.removeFromLeft (8);
        playButton.setBounds (bar.removeFromLeft (34));
        stopButton.setBounds (bar.removeFromLeft (34));
        recordButton.setBounds (bar.removeFromLeft (34));
        bar.removeFromLeft (10);
        tempo.setBounds (bar.removeFromLeft (84));
        bar.removeFromLeft (8);
        timeArea = bar.removeFromLeft (100);
        bar.removeFromLeft (8);
        metronome.setBounds (bar.removeFromLeft (22).withSizeKeepingCentre (20, 20));
        bar.removeFromLeft (20);
        channelRackButton.setBounds (bar.removeFromLeft (110));

        workspace.setBounds (r);
    }

    //==========================================================================
    // Keyboard: FL shortcuts + typing keyboard

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto mods = key.getModifiers();
        if (key == juce::KeyPress::spaceKey)          { studio.togglePlay(); return true; }
        if (key == juce::KeyPress::F6Key)             { channelRackPanel->toggle(); return true; }
        if (key == juce::KeyPress::F10Key)            { showAudioSettings(); return true; }
        if (key.getKeyCode() == 'L' && mods.isAnyModifierKeyDown() == false)
        {
            patSongButton.setToggleState (! patSongButton.getToggleState(), juce::sendNotification);
            return true;
        }
        return TypingKeyboard::noteForKey (key.getKeyCode()) >= 0 && ! mods.isCommandDown() && ! mods.isCtrlDown();
    }

    bool keyStateChanged (bool) override { return typing.update(); }
    void focusLost (FocusChangeType) override { typing.releaseAll(); }

    //==========================================================================
    // Menu bar

    juce::StringArray getMenuBarNames() override { return { "FILE", "EDIT", "OPTIONS" }; }

    juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
    {
        juce::PopupMenu m;
        if (index == 0)
        {
            m.addItem ("New", [this] { studio.newProject(); });
        }
        else if (index == 1)
        {
            m.addItem ("Undo", studio.undo().canUndo(), false, [this] { studio.undo().undo(); });
            m.addItem ("Redo", studio.undo().canRedo(), false, [this] { studio.undo().redo(); });
        }
        else if (index == 2)
        {
            m.addItem ("Audio settings... (F10)", [this] { showAudioSettings(); });
        }
        return m;
    }

    void menuItemSelected (int, int) override {}

private:
    void showAudioSettings()
    {
        auto* selector = new juce::AudioDeviceSelectorComponent (studio.engine.getDeviceManager().deviceManager,
                                                                 0, 2, 2, 2, true, false, true, false);
        selector->setSize (520, 520);

        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (selector);
        o.dialogTitle = "Audio Settings";
        o.dialogBackgroundColour = Theme::panelDark;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void toggleChannelWindow (int channelId)
    {
        for (auto* p : channelPanels)
        {
            if (auto* cs = dynamic_cast<ChannelSettings*> (&p->getContent()); cs != nullptr && cs->getChannelId() == channelId)
            {
                p->toggle();
                return;
            }
        }

        auto content = std::make_unique<ChannelSettings> (studio, channelId);
        const auto size = content->getBounds();
        auto* panel = channelPanels.add (new Panel (studio.getChannel (channelId)[IDs::name].toString(), std::move (content)));
        workspace.addAndMakeVisible (panel);
        panel->setBounds (320 + 24 * (channelPanels.size() % 6), 120 + 24 * (channelPanels.size() % 6),
                          size.getWidth() + 2, size.getHeight() + Panel::titleHeight + 1);
        panel->toFront (false);
    }

    void updatePatSong()
    {
        const bool song = studio.isSongMode();
        patSongButton.setToggleState (song, juce::dontSendNotification);
        patSongButton.setButtonText (song ? "SONG" : "PAT");
    }

    void timerCallback() override
    {
        // Keep keyboard focus here so Space / typing keys always work (FL behaviour).
        if (juce::Component::getCurrentlyFocusedComponent() == nullptr && isShowing()
            && getPeer() != nullptr && getPeer()->isFocused())
            grabKeyboardFocus();

        // Hint bar: tooltip of whatever is under the mouse.
        juce::String newHint;
        for (auto* c = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse(); c != nullptr; c = c->getParentComponent())
        {
            if (auto* tc = dynamic_cast<juce::TooltipClient*> (c))
            {
                newHint = tc->getTooltip();
                if (newHint.isNotEmpty())
                    break;
            }
        }

        const double beats = studio.getPositionBeats();
        const double barBeats = studio.getBarBeats();
        const int bar = (int) std::floor (beats / barBeats) + 1;
        const double inBar = std::fmod (beats, barBeats);
        const int step = (int) std::floor (inBar / Studio::stepBeats) + 1;
        const int tick = (int) std::floor (std::fmod (inBar, Studio::stepBeats) / Studio::stepBeats * 24.0);
        const auto newTime = juce::String::formatted ("%d:%02d:%02d", bar, step, tick);
        const double newCpu = studio.engine.getDeviceManager().getCpuUsage();

        if (newHint != hint || newTime != timeText || std::abs (newCpu - cpu) > 0.01)
        {
            hint = newHint;
            timeText = newTime;
            cpu = newCpu;
            repaint (0, 0, getWidth(), menuHeight + toolbarHeight);
        }

        tempo.setValue (studio.getBpm());
        playButton.setToggleState (studio.isPlaying(), juce::dontSendNotification);
        metronome.setToggleState (studio.isMetronomeOn(), juce::dontSendNotification);
        updatePatSong();

        // Close windows of deleted channels.
        for (int i = channelPanels.size(); --i >= 0;)
            if (auto* cs = dynamic_cast<ChannelSettings*> (&channelPanels[i]->getContent()))
                if (! studio.getChannel (cs->getChannelId()).isValid())
                    channelPanels.remove (i);
    }

    static constexpr int menuHeight = 26, toolbarHeight = 44;

    Theme::LookAndFeel lookAndFeel;
    Studio studio;
    TypingKeyboard typing;

    juce::MenuBarComponent menuBar;
    juce::TextButton patSongButton { "PAT" };
    TransportButton playButton { TransportButton::play, "Play / stop (Space)" };
    TransportButton stopButton { TransportButton::stop, "Stop" };
    TransportButton recordButton { TransportButton::record, "Record (coming in Phase 12)" };
    Widgets::DragNumber tempo { 10.0, 522.0, 0.5 };
    Widgets::LedButton metronome { "Metronome", Theme::accent };
    juce::TextButton channelRackButton { "Channel rack" };

    juce::Component workspace;
    std::unique_ptr<Panel> channelRackPanel;
    juce::OwnedArray<Panel> channelPanels;

    juce::Rectangle<int> hintArea, timeArea, cpuArea;
    juce::String hint, timeText;
    double cpu = 0.0;
};

//==============================================================================
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, te::Engine& engine)
        : DocumentWindow (name, Theme::background, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent (engine), true);
        setResizable (true, true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

//==============================================================================
// Headless checks: `untitledstudio --selftest`. Exit code 0 = pass.
namespace SelfTest
{
    inline bool check (bool condition, const char* what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << std::endl;
        return condition;
    }

    inline int countNotes (Studio& s, int channelId)
    {
        s.regenerateNow();
        for (auto clip : s.getTrack (channelId)->getClips())
            if (auto mc = dynamic_cast<te::MidiClip*> (clip))
                return mc->getSequence().getNumNotes();
        return -1;
    }

    inline bool run (te::Engine& engine)
    {
        Studio s (engine);
        bool ok = true;
        const int kick = s.getChannels().getChild (0)[IDs::id];

        ok &= check (s.getChannels().getNumChildren() == 4, "default project has 4 channels");
        ok &= check (countNotes (s, kick) == 0, "new pattern is empty");

        s.undo().beginNewTransaction();
        for (int step : { 0, 4, 8, 12 })
            s.setStep (kick, step, true);
        ok &= check (countNotes (s, kick) == 4, "4 steps -> 4 engine notes");

        s.undo().beginNewTransaction();
        s.setStep (kick, 4, false);
        ok &= check (countNotes (s, kick) == 3, "step off removes note");

        s.undo().undo();
        ok &= check (countNotes (s, kick) == 4, "undo restores note");

        s.setStep (kick, 31, true);
        ok &= check (std::abs (s.getPatternLengthBeats (s.getCurrentPatternState()) - 8.0) < 1.0e-6, "pattern grows to 2 bars");

        std::cout << (ok ? "PASS" : "FAILED") << std::endl;
        return ok;
    }
}

class UntitledStudioApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    void initialise (const juce::String& commandLine) override
    {
        engine = std::make_unique<te::Engine> ("untitledstudio");

        if (commandLine.contains ("--selftest"))
        {
            const bool ok = SelfTest::run (*engine);
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine);

        // Dev aid: --snapshot <file.png> renders the window to an image, then quits.
        if (commandLine.contains ("--snapshot"))
        {
            const auto file = juce::File (commandLine.fromFirstOccurrenceOf ("--snapshot", false, false).trim().unquoted());
            juce::Timer::callAfterDelay (2500, [this, file]
            {
                if (auto* c = mainWindow->getContentComponent())
                {
                    juce::PNGImageFormat png;
                    file.deleteFile();
                    if (auto out = file.createOutputStream())
                        png.writeImageToStream (c->createComponentSnapshot (c->getLocalBounds(), true, 1.0f), *out);
                }
                quit();
            });
        }
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        engine = nullptr;
    }

    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (UntitledStudioApp)
