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
#include "Browser.h"
#include "PianoRoll.h"
#include "Playlist.h"
#include "Mixer.h"

//==============================================================================
// FL "typing keyboard to piano": Z-row = lower octave, Q-row = upper octave.
class TypingKeyboard
{
public:
    std::function<void (const juce::MidiMessage&)> onMidi;
    std::function<bool (int keyCode)> isReserved; // e.g. piano roll tool letters

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
            const bool down = juce::KeyPress::isKeyCurrentlyDown (code) && ! (isReserved && isReserved (code));
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
                      public juce::DragAndDropContainer,
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

        patternButton.setTooltip ("Pattern selector: pick, add, clone, rename patterns");
        patternButton.onClick = [this] { showPatternMenu(); };
        addAndMakeVisible (patternButton);

        playlistButton.setTooltip ("Playlist (F5)");
        playlistButton.onClick = [this] { playlistPanel->toggle(); };
        addAndMakeVisible (playlistButton);

        mixerButton.setTooltip ("Mixer (F9)");
        mixerButton.onClick = [this] { mixerPanel->toggle(); };
        addAndMakeVisible (mixerButton);

        pianoRollButton.setTooltip ("Piano roll (F7)");
        pianoRollButton.onClick = [this] { pianoRollPanel->toggle(); };
        addAndMakeVisible (pianoRollButton);

        channelRackButton.setTooltip ("Channel rack (F6)");
        channelRackButton.onClick = [this] { channelRackPanel->toggle(); };
        addAndMakeVisible (channelRackButton);

        browserButton.setTooltip ("Browser (Alt+F8)");
        browserButton.onClick = [this] { toggleBrowser(); };
        addAndMakeVisible (browserButton);

        browser.onOpenProject = [this] (const juce::File& f) { maybeSave ([this, f] { openFile (f); }); };
        addAndMakeVisible (browser);
        addAndMakeVisible (workspace);

        auto list = std::make_unique<Playlist> (studio);
        playlist = list.get();
        list->onShowChannelRack = [this] { channelRackPanel->show(); };
        playlistPanel = std::make_unique<Panel> ("Playlist - Arrangement", std::move (list));
        workspace.addAndMakeVisible (*playlistPanel);
        playlistPanel->setBounds (10, 10, 1100, 700);

        auto rack = std::make_unique<ChannelRack> (studio);
        rack->onOpenChannel = [this] (int id) { toggleChannelWindow (id); };
        rack->onOpenPianoRoll = [this] (int id) { studio.selectChannel (id); pianoRollPanel->show(); };
        channelRackPanel = std::make_unique<Panel> ("Channel rack", std::move (rack));
        workspace.addAndMakeVisible (*channelRackPanel);
        channelRackPanel->setBounds (40, 40, 620, 260);

        auto mix = std::make_unique<Mixer> (studio);
        mix->onOpenEffect = [this] (te::Plugin& p) { openEffectWindow (p); };
        mixerPanel = std::make_unique<Panel> ("Mixer", std::move (mix));
        workspace.addChildComponent (*mixerPanel);
        mixerPanel->setBounds (60, 260, 1040, 420);

        // Windows holding plugin pointers close before a project is replaced.
        studio.editAboutToChange.push_back ([this] { channelPanels.clear(); effectPanels.clear(); });

        auto roll = std::make_unique<PianoRoll> (studio);
        pianoRoll = roll.get();
        pianoRollPanel = std::make_unique<Panel> (roll->getTitle(), std::move (roll));
        workspace.addChildComponent (*pianoRollPanel);
        pianoRollPanel->setBounds (120, 150, 900, 520);

        typing.onMidi = [this] (const juce::MidiMessage& m) { studio.playLiveNote (m); };
        typing.isReserved = [this] (int code)
        {
            return (isFrontPanel (*pianoRollPanel) && juce::String ("pbdec").containsChar ((juce::juce_wchar) code))
                || (isFrontPanel (*playlistPanel) && Playlist::reservedKeys().containsChar ((juce::juce_wchar) code));
        };

        lastGeneration = studio.getEditGeneration();
        recent.setMaxNumberOfItems (10);
        recent.restoreFromString (settings.getValue ("recentFiles"));

        Widgets::noFocus (*this);
        setWantsKeyboardFocus (true);
        setSize (1400, 860);
        startTimerHz (30);

        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safe != nullptr) safe->offerCrashRecovery();
        });
    }

    ~MainComponent() override
    {
        if (cleanExit)
        {
            getAutosaveFile().deleteFile();
            getLockFile().deleteFile();
        }
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
        patternButton.setBounds (bar.removeFromLeft (130));
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
        playlistButton.setBounds (bar.removeFromLeft (80));
        bar.removeFromLeft (4);
        channelRackButton.setBounds (bar.removeFromLeft (110));
        bar.removeFromLeft (4);
        pianoRollButton.setBounds (bar.removeFromLeft (90));
        bar.removeFromLeft (4);
        mixerButton.setBounds (bar.removeFromLeft (70));
        bar.removeFromLeft (4);
        browserButton.setBounds (bar.removeFromLeft (80));

        browser.setVisible (browserVisible);
        if (browserVisible)
            browser.setBounds (r.removeFromLeft (230));
        workspace.setBounds (r);
    }

    //==========================================================================
    // Keyboard: FL shortcuts + typing keyboard

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto mods = key.getModifiers();
        const bool cmd = mods.isCommandDown();
        const int code = key.getKeyCode();
        if (cmd && code == 'N')                       { newProject(); return true; }
        if (cmd && code == 'O')                       { openProject(); return true; }
        if (cmd && code == 'S' && mods.isShiftDown()) { saveAs (nullptr); return true; }
        if (cmd && code == 'S')                       { save (nullptr); return true; }
        if (cmd && (code == 'Y' || (code == 'Z' && mods.isShiftDown()))) { studio.undo().redo(); return true; }
        if (cmd && code == 'Z')                       { studio.undo().undo(); return true; }
        if (isFrontPanel (*pianoRollPanel) && pianoRoll->handleKey (key))
            return true;
        if (isFrontPanel (*playlistPanel) && playlist->handleKey (key))
            return true;
        if (key == juce::KeyPress::F5Key)             { playlistPanel->toggle(); return true; }
        if (key == juce::KeyPress::F9Key)             { mixerPanel->toggle(); return true; }
        if (key == juce::KeyPress::spaceKey)          { studio.togglePlay(); return true; }
        if (key == juce::KeyPress::F7Key)             { pianoRollPanel->toggle(); return true; }
        if (key == juce::KeyPress::F6Key)             { channelRackPanel->toggle(); return true; }
        if (key == juce::KeyPress::F8Key && mods.isAltDown()) { toggleBrowser(); return true; }
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
            m.addItem ("New                     Ctrl+N", [this] { newProject(); });
            m.addItem ("Open...                 Ctrl+O", [this] { openProject(); });

            juce::PopupMenu recentMenu;
            recent.removeNonExistentFiles();
            recent.createPopupMenuItems (recentMenu, recentMenuBase, true, true);
            m.addSubMenu ("Open recent", recentMenu, recent.getNumFiles() > 0);
            m.addSeparator();
            m.addItem ("Save                    Ctrl+S", [this] { save (nullptr); });
            m.addItem ("Save as...        Ctrl+Shift+S", [this] { saveAs (nullptr); });
        }
        else if (index == 1)
        {
            m.addItem ("Undo                    Ctrl+Z", studio.undo().canUndo(), false, [this] { studio.undo().undo(); });
            m.addItem ("Redo                    Ctrl+Y", studio.undo().canRedo(), false, [this] { studio.undo().redo(); });
        }
        else if (index == 2)
        {
            m.addItem ("Audio settings... (F10)", [this] { showAudioSettings(); });
        }
        return m;
    }

    void menuItemSelected (int itemId, int) override
    {
        if (itemId >= recentMenuBase)
        {
            const auto file = recent.getFile (itemId - recentMenuBase);
            maybeSave ([this, file] { openFile (file); });
        }
    }

    //==========================================================================
    // Project files

    void newProject()             { maybeSave ([this] { studio.newProject(); }); }

    void openProject()
    {
        maybeSave ([this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Open project", Studio::getProjectsFolder(), "*" + Studio::fileExtension);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc)
                                  {
                                      if (fc.getResult().existsAsFile())
                                          openFile (fc.getResult());
                                  });
        });
    }

    void openFile (const juce::File& file)
    {
        if (studio.load (file))
            addToRecent (file);
        else
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle ("Couldn't open project").withMessage (file.getFullPathName()).withButton ("OK"),
                                          nullptr);
    }

    void save (std::function<void()> then)
    {
        if (studio.getProjectFile() == juce::File())
            return saveAs (std::move (then));

        writeProject (studio.getProjectFile());
        if (then) then();
    }

    void saveAs (std::function<void()> then)
    {
        Studio::getProjectsFolder().createDirectory();
        const auto start = studio.getProjectFile() != juce::File() ? studio.getProjectFile()
                                                                     : Studio::getProjectsFolder().getChildFile ("Untitled" + Studio::fileExtension);
        chooser = std::make_unique<juce::FileChooser> ("Save project as", start, "*" + Studio::fileExtension);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, then] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult() == juce::File())
                                      return;
                                  writeProject (fc.getResult().withFileExtension (Studio::fileExtension));
                                  if (then) then();
                              });
    }

    void writeProject (const juce::File& file)
    {
        if (studio.saveTo (file))
            addToRecent (file);
        else
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle ("Couldn't save project").withMessage (file.getFullPathName()).withButton ("OK"),
                                          nullptr);
    }

    // Asks "Save changes?" if needed, then runs `then` (unless cancelled).
    void maybeSave (std::function<void()> then)
    {
        if (! studio.isDirty())
            return then();

        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Save changes?")
                                          .withMessage ("Save changes to " + getProjectName() + "?")
                                          .withButton ("Save").withButton ("Don't save").withButton ("Cancel"),
                                      [this, then] (int result)
                                      {
                                          if (result == 1)      save (then);
                                          else if (result == 2) then();
                                      });
    }

    void quitRequested()
    {
        maybeSave ([this]
        {
            cleanExit = true;
            juce::JUCEApplication::getInstance()->quit();
        });
    }

    juce::String getProjectName() const
    {
        const auto f = studio.getProjectFile();
        return f == juce::File() ? juce::String ("Untitled") : f.getFileNameWithoutExtension();
    }

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

    void openEffectWindow (te::Plugin& plugin)
    {
        for (auto* p : effectPanels)
            if (auto* w = dynamic_cast<EffectWindow*> (&p->getContent()); w != nullptr && w->getPlugin() == &plugin)
                return p->show();

        auto content = std::make_unique<EffectWindow> (studio, plugin);
        const auto size = content->getBounds();
        auto* panel = effectPanels.add (new Panel (Effects::displayName (plugin), std::move (content)));
        workspace.addAndMakeVisible (panel);
        panel->setBounds (380 + 24 * (effectPanels.size() % 6), 100 + 24 * (effectPanels.size() % 6),
                          size.getWidth() + 2, size.getHeight() + Panel::titleHeight + 1);
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

    bool isFrontPanel (const Panel& p) const
    {
        if (! p.isVisible())
            return false;
        for (int i = workspace.getNumChildComponents(); --i >= 0;)
            if (auto* c = workspace.getChildComponent (i); c->isVisible())
                return c == &p;
        return false;
    }

    void toggleBrowser()
    {
        browserVisible = ! browserVisible;
        resized();
    }

    // FL pattern picker.
    void showPatternMenu()
    {
        juce::PopupMenu m;
        const int current = studio.getCurrentPattern();
        for (auto p : studio.getPatterns())
        {
            const int id = p[IDs::id];
            m.addColouredItem (id, p[IDs::name].toString(), studio.getPatternColour (p), true, id == current);
        }
        m.addSeparator();
        m.addItem (-1, "New pattern");
        m.addItem (-2, "Clone");
        m.addItem (-3, "Rename...");
        m.addItem (-4, "Delete", studio.getPatterns().getNumChildren() > 1);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (patternButton), [this, current] (int result)
        {
            if (result > 0)        studio.setCurrentPattern (result);
            else if (result == -1) studio.newPattern();
            else if (result == -2) studio.clonePattern (current);
            else if (result == -4) studio.deletePattern (current);
            else if (result == -3)
            {
                auto* w = new juce::AlertWindow ("Rename pattern", {}, juce::MessageBoxIconType::NoIcon);
                w->addTextEditor ("name", studio.getPattern (current)[IDs::name].toString());
                w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, current] (int r)
                {
                    if (r == 1)
                    {
                        studio.undo().beginNewTransaction ("Rename pattern");
                        studio.getPattern (current).setProperty (IDs::name, w->getTextEditorContents ("name"), &studio.undo());
                    }
                }), true);
            }
        });
    }

    void updatePatSong()
    {
        const auto patternName = studio.getCurrentPatternState()[IDs::name].toString();
        if (patternButton.getButtonText() != patternName)
            patternButton.setButtonText (patternName);

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

        if (studio.getEditGeneration() != lastGeneration)
        {
            lastGeneration = studio.getEditGeneration();
            channelPanels.clear();
        }

        // Autosave every 2 minutes while there are unsaved changes.
        if (++ticks % (30 * 120) == 0 && studio.isDirty())
            studio.saveCopyTo (getAutosaveFile());

        if (auto* window = getTopLevelComponent())
        {
            const auto title = getProjectName() + (studio.isDirty() ? " *" : "") + " - untitledstudio";
            if (window->getName() != title)
                window->setName (title);
        }

        tempo.setValue (studio.getBpm());
        playButton.setToggleState (studio.isPlaying(), juce::dontSendNotification);
        metronome.setToggleState (studio.isMetronomeOn(), juce::dontSendNotification);
        updatePatSong();

        // Close windows of removed effects (e.g. after undo).
        for (int i = effectPanels.size(); --i >= 0;)
            if (auto* w = dynamic_cast<EffectWindow*> (&effectPanels[i]->getContent()))
                if (w->getPlugin()->getOwnerList() == nullptr)
                    effectPanels.remove (i);

        // Close windows of deleted channels.
        for (int i = channelPanels.size(); --i >= 0;)
            if (auto* cs = dynamic_cast<ChannelSettings*> (&channelPanels[i]->getContent()))
                if (! studio.getChannel (cs->getChannelId()).isValid())
                    channelPanels.remove (i);
    }

    //==========================================================================
    // Autosave + crash recovery. A lock file exists while the app runs; if it's still
    // there at startup, the last session crashed and the autosave can be recovered.

    static juce::File getAppDataFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("untitledstudio");
    }

    static juce::File getAutosaveFile() { return getAppDataFolder().getChildFile ("Autosave").getChildFile ("autosave" + Studio::fileExtension); }
    static juce::File getLockFile()     { return getAppDataFolder().getChildFile ("running.lock"); }

    void offerCrashRecovery()
    {
        const bool crashed = getLockFile().existsAsFile() && getAutosaveFile().existsAsFile();
        getLockFile().create();

        if (! crashed)
            return;

        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Recover project?")
                                          .withMessage ("untitledstudio didn't close properly last time.\nRecover your last autosave?")
                                          .withButton ("Recover").withButton ("Discard"),
                                      [this] (int result)
                                      {
                                          if (result == 1)
                                              studio.load (getAutosaveFile(), true);
                                          getAutosaveFile().deleteFile();
                                      });
    }

    void addToRecent (const juce::File& file)
    {
        recent.addFile (file);
        settings.setValue ("recentFiles", recent.toString());
        settings.saveIfNeeded();
    }

    static juce::PropertiesFile::Options settingsOptions()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "untitledstudio";
        o.filenameSuffix = ".settings";
        o.folderName = "untitledstudio";
        o.osxLibrarySubFolder = "Application Support";
        return o;
    }

    static constexpr int menuHeight = 26, toolbarHeight = 44, recentMenuBase = 1000;

    // Order matters: members below are constructed top to bottom.
    Theme::LookAndFeel lookAndFeel;
    Studio studio;
    juce::PropertiesFile settings { settingsOptions() };
    Browser browser { studio.engine, settings };
    bool browserVisible = true;
    juce::RecentlyOpenedFilesList recent;
    std::unique_ptr<juce::FileChooser> chooser;
    int lastGeneration = 0, ticks = 0;
    bool cleanExit = false;

public:
    void requestQuit() { quitRequested(); }
    void debugOpenFirstChannel() { toggleChannelWindow (studio.getChannels().getChild (0)[IDs::id]); }
    void debugOpenPianoRoll()
    {
        auto pattern = studio.getCurrentPatternState();
        const int ch = studio.getSelectedChannel();
        for (auto [start, key] : { std::pair { 0.0, 72 }, { 0.5, 75 }, { 1.0, 79 }, { 2.0, 77 }, { 3.0, 72 } })
            studio.addNote (pattern, ch, start, 0.5, key, 70 + (int) (start * 15));
        pianoRollPanel->show();
    }

    void debugOpenMixer()
    {
        studio.setChannelInsert (studio.getChannels().getChild (0)[IDs::id], 1);
        if (auto p = studio.addEffect (1, te::CompressorPlugin::xmlTypeName)) openEffectWindow (*p);
        studio.addEffect (1, te::ReverbPlugin::xmlTypeName);
        studio.addSend (1, 2);
        channelRackPanel->setVisible (false);
        mixerPanel->show();
        if (auto* m = dynamic_cast<Mixer*> (&mixerPanel->getContent()))
            juce::ignoreUnused (m);
    }

    void debugDemoSong()
    {
        const int p1 = studio.getCurrentPattern();
        for (int step : { 0, 4, 8, 12 })
            studio.setStep (studio.getChannels().getChild (0)[IDs::id], step, true);
        const int p2 = studio.newPattern();
        for (int step : { 4, 12 })
            studio.setStep (studio.getChannels().getChild (1)[IDs::id], step, true);
        for (int bar = 0; bar < 8; ++bar)
            studio.addPatternClip (p1, 0, bar * 4.0);
        for (int bar = 2; bar < 8; ++bar)
            studio.addPatternClip (p2, 1, bar * 4.0);
        studio.addAudioClip (DrumKit::getDrumsFolder().getChildFile ("808.wav"), 2, 4.0);
        studio.setSongMode (true);
        channelRackPanel->setVisible (false);
    }

private:

    TypingKeyboard typing;

    juce::MenuBarComponent menuBar;
    juce::TextButton patSongButton { "PAT" };
    TransportButton playButton { TransportButton::play, "Play / stop (Space)" };
    TransportButton stopButton { TransportButton::stop, "Stop" };
    TransportButton recordButton { TransportButton::record, "Record (coming in Phase 12)" };
    Widgets::DragNumber tempo { 10.0, 522.0, 0.5 };
    Widgets::LedButton metronome { "Metronome", Theme::accent };
    juce::TextButton channelRackButton { "Channel rack" }, browserButton { "Browser" }, pianoRollButton { "Piano roll" },
                     playlistButton { "Playlist" }, patternButton { "Pattern 1" }, mixerButton { "Mixer" };

    juce::Component workspace;
    std::unique_ptr<Panel> playlistPanel, channelRackPanel, pianoRollPanel, mixerPanel;
    juce::OwnedArray<Panel> effectPanels;
    Playlist* playlist = nullptr;
    PianoRoll* pianoRoll = nullptr;
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

    void requestQuit()
    {
        if (auto* mc = dynamic_cast<MainComponent*> (getContentComponent()))
            mc->requestQuit();
    }
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

        {
            // Song mode: two placements of the 5-note pattern -> 10 engine notes.
            const int p = s.getCurrentPattern();
            s.addPatternClip (p, 0, 0.0);
            s.addPatternClip (p, 0, 8.0);
            s.setSongMode (true);
            ok &= check (countNotes (s, kick) == 10, "song mode plays every playlist placement");
            auto muted = s.getPlaylist().getChild (1);
            muted.setProperty (IDs::muted, true, nullptr);
            ok &= check (countNotes (s, kick) == 5, "muted clip is silent");
            muted.setProperty (IDs::muted, false, nullptr);

            auto audio = s.addAudioClip (DrumKit::getDrumsFolder().getChildFile ("Kick.wav"), 1, 4.0);
            s.regenerateNow();
            int waveClips = 0;
            if (auto t = s.getTrack ((int) audio[IDs::channel]))
                for (auto c : t->getClips())
                    waveClips += dynamic_cast<te::WaveAudioClip*> (c) != nullptr ? 1 : 0;
            ok &= check (waveClips == 1, "audio clip becomes an engine wave clip");
            s.setSongMode (false);
            ok &= check (countNotes (s, kick) == 5, "pattern mode plays only the current pattern");
        }

        {
            // Mixer routing: channel -> insert 3, effect + send survive.
            s.undo().beginNewTransaction();
            s.setChannelInsert (kick, 3);
            ok &= check (s.getTrack (kick)->getOutput().getDestinationTrack() == s.getInsertTrack (3), "channel routes to its insert");
            ok &= check (s.addEffect (3, te::ReverbPlugin::xmlTypeName) != nullptr, "add effect to insert");
            ok &= check (s.getInsertEffects (3).size() == 1, "insert shows one effect");
            s.addSend (3, 5);
            ok &= check (s.getInsertSends (3).size() == 1, "send created");
            s.undo().beginNewTransaction();
            s.setChannelInsert (kick, 0);
            ok &= check (s.getTrack (kick)->getOutput().getDestinationTrack() == nullptr, "insert 0 = master");
            s.setChannelInsert (kick, 3);
        }

        const auto file = juce::File::createTempFile (Studio::fileExtension);
        ok &= check (s.isDirty(), "edits mark project dirty");
        ok &= check (s.saveTo (file), "save project");
        ok &= check (! s.isDirty(), "clean after save");
        {
            Studio loaded (engine);
            ok &= check (loaded.load (file), "load project");
            ok &= check (loaded.getChannels().getNumChildren() == 5, "channels survive save/load");
            ok &= check (countNotes (loaded, kick) == 5, "notes survive save/load");
            ok &= check (loaded.getInsertEffects (3).size() == 1 && loaded.getChannelInsert (kick) == 3, "mixer survives save/load");
            loaded.setStep (kick, 2, true);
            ok &= check (countNotes (loaded, kick) == 6, "loaded project still drives the engine");
        }
        file.deleteFile();

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
            const auto file = juce::File (commandLine.fromFirstOccurrenceOf ("--snapshot", false, false).trim().upToFirstOccurrenceOf (" ", false, false).unquoted());
            if (commandLine.contains ("--open-mixer"))
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->debugOpenMixer();

            if (commandLine.contains ("--demo-song"))
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->debugDemoSong();

            if (commandLine.contains ("--open-pianoroll"))
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->debugOpenPianoRoll();

            if (commandLine.contains ("--open-channel"))
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->debugOpenFirstChannel();

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

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr) mainWindow->requestQuit();
        else quit();
    }

private:
    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (UntitledStudioApp)
