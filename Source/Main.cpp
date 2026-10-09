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
#include "PluginWindow.h"
#include "Exporter.h"
#include "AudioEditor.h"
#include "Popups.h"

#if JUCE_MAC
 #include <mach/mach.h>
#elif JUCE_WINDOWS
 #include <psapi.h>
#endif

// Resident memory of this process in bytes (0 where not implemented).
inline juce::int64 getProcessMemory()
{
   #if JUCE_MAC
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info (mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t) &info, &count) == KERN_SUCCESS)
        return (juce::int64) info.resident_size;
   #elif JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS pmc {};
    if (K32GetProcessMemoryInfo (GetCurrentProcess(), &pmc, sizeof (pmc)))
        return (juce::int64) pmc.WorkingSetSize;
   #endif
    return 0;
}

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

    std::function<void()> onRightClick;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && onRightClick) onRightClick();
        else juce::Button::mouseDown (e);
    }

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
                                                       : (shape == record ? (getToggleState() ? juce::Colour (0xffff4a3c) : juce::Colour (0xff8a3a35))
                                                                          : Theme::text));
        g.fillPath (p);
    }

private:
    Shape shape;
};

//==============================================================================
class MainComponent : public juce::Component,
                      public juce::MenuBarModel,
                      private juce::ChangeListener,
                      private juce::MidiInputCallback,
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
        recordButton.setClickingTogglesState (true);
        recordButton.setTooltip ("Record: arm, then press Play. Right-click for what to record (audio, notes, automation, count-in)");
        recordButton.onClick = [this] { studio.recordArmed = recordButton.getToggleState(); };
        recordButton.onRightClick = [this] { showRecordMenu(); };
        for (auto* b : { &playButton, &stopButton, &recordButton })
            addAndMakeVisible (b);

        tempo.format = [] (double v) { return juce::String (v, 3); };
        tempo.setTooltip ("Tempo in BPM. Drag up/down, Ctrl for fine, double-click to type");
        tempo.onChange = [this] (double bpm) { studio.setBpm (bpm); };
        addAndMakeVisible (tempo);

        timeSigButton.setTooltip ("Time signature (click to change)");
        timeSigButton.onClick = [this] { showTimeSigMenu(); };
        addAndMakeVisible (timeSigButton);

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
        browser.onPluginChosen = [this] (const juce::PluginDescription& d) { addPlugin (d); };
        browser.onEditAudio = [this] (const juce::File& f)
        {
            openAudioEditor (f, [this] (const juce::File& edited) { flashHint ("Saved the edit as " + edited.getFullPathName()); });
        };
        browser.onPresetChosen = [this] (const Browser::PresetEntry& p) { applyBrowserPreset (p); };
        studio.engine.getPluginManager().knownPluginList.addChangeListener (this);

        // FL: creating an automation clip shows it in the playlist and switches to song mode.
        studio.onAutomationClipCreated = [this] { studio.setSongMode (true); playlistPanel->show(); };

        // MIDI controller knobs (CC) for "Link to controller".
        auto& dm = studio.engine.getDeviceManager().deviceManager;
        for (auto& d : juce::MidiInput::getAvailableDevices())
            dm.setMidiInputDeviceEnabled (d.identifier, true);
        dm.addMidiInputDeviceCallback ({}, this);
        addAndMakeVisible (browser);
        addAndMakeVisible (workspace);

        auto list = std::make_unique<Playlist> (studio);
        playlist = list.get();
        list->onShowChannelRack = [this] { channelRackPanel->show(); };
        list->onEditAudio = [this] (int channelId) { editChannelAudio (channelId); };
        playlistPanel = std::make_unique<Panel> ("Playlist - Arrangement", std::move (list));
        workspace.addAndMakeVisible (*playlistPanel);
        playlistPanel->setBounds (10, 10, 1100, 700);

        auto rack = std::make_unique<ChannelRack> (studio);
        rack->onOpenChannel = [this] (int id)
        {
            // Third-party instruments open their own window (FL's plugin wrapper).
            if (auto p = studio.getInstrument (id); p != nullptr && dynamic_cast<te::ExternalPlugin*> (p) != nullptr)
                return p->showWindowExplicitly();
            toggleChannelWindow (id);
        };
        rack->onManagePlugins = [this] { showPluginManager(); };
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
        studio.editAboutToChange.push_back ([this] { channelPanels.clear(); effectPanels.clear(); editorPanels.clear(); });

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

        Panel::forwardKey = [this] (const juce::KeyPress& k) { return keyPressed (k); };
        Panel::forwardKeyState = [this] { return keyStateChanged (false); };
        restoreLayout();

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
        saveLayout();
        Panel::forwardKey = nullptr;
        Panel::forwardKeyState = nullptr;
        studio.engine.getPluginManager().knownPluginList.removeChangeListener (this);
        studio.engine.getDeviceManager().deviceManager.removeMidiInputDeviceCallback ({}, this);
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
        g.setColour (juce::Time::getMillisecondCounter() < flashUntil ? Theme::accent : Theme::textDim);
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
        g.drawText (statusText, cpuArea, juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (menuHeight);
        menuBar.setBounds (top.removeFromLeft (260));
        cpuArea = top.removeFromRight (200);
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
        bar.removeFromLeft (4);
        timeSigButton.setBounds (bar.removeFromLeft (40));
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

        if (! workspace.getBounds().isEmpty())
            for (auto* p : std::exchange (maximiseWhenSized, {}))
                p->setMaximised (true);
    }

    //==========================================================================
    // Keyboard: FL shortcuts + typing keyboard

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (juce::Component::getCurrentlyModalComponent() != nullptr) // dialogs, menus, the F8 picker
            return false;

        const auto mods = key.getModifiers();
        const bool cmd = mods.isCommandDown();
        const int code = key.getKeyCode();
        if (cmd && code == 'N')                       { newProject(); return true; }
        if (cmd && code == 'O')                       { openProject(); return true; }
        if (cmd && code == 'S' && mods.isShiftDown()) { saveAs (nullptr); return true; }
        if (cmd && code == 'S')                       { save (nullptr); return true; }
        if (cmd && (code == 'Y' || (code == 'Z' && mods.isShiftDown()))) { studio.undo().redo(); return true; }
        if (cmd && code == 'Z')                       { studio.undo().undo(); return true; }
        if (cmd && code == 'R')                       { showExportDialog(); return true; }

        // FL: numpad +/- (or Ctrl+Up/Down) = next / previous pattern.
        const bool ctrl = (cmd || mods.isCtrlDown()) && ! mods.isShiftDown();
        if (key.isKeyCode (juce::KeyPress::numberPadAdd) || (ctrl && key.isKeyCode (juce::KeyPress::upKey)))        { stepPattern (1); return true; }
        if (key.isKeyCode (juce::KeyPress::numberPadSubtract) || (ctrl && key.isKeyCode (juce::KeyPress::downKey))) { stepPattern (-1); return true; }

        if (isFrontPanel (*pianoRollPanel) && pianoRoll->handleKey (key))
            return true;
        if (isFrontPanel (*playlistPanel) && playlist->handleKey (key))
            return true;
        if (key.isKeyCode (juce::KeyPress::F5Key))    { playlistPanel->toggle(); return true; }
        if (key.isKeyCode (juce::KeyPress::F6Key))    { channelRackPanel->toggle(); return true; }
        if (key.isKeyCode (juce::KeyPress::F7Key))    { pianoRollPanel->toggle(); return true; }
        if (key.isKeyCode (juce::KeyPress::F8Key))    { if (mods.isAltDown()) toggleBrowser(); else showSoundPicker(); return true; }
        if (key.isKeyCode (juce::KeyPress::F9Key))    { mixerPanel->toggle(); return true; }
        if (key.isKeyCode (juce::KeyPress::F10Key))   { showAudioSettings(); return true; }
        if (key.isKeyCode (juce::KeyPress::F12Key))   { closeAllWindows(); return true; }
        if (key.isKeyCode (juce::KeyPress::tabKey))   { cycleWindows(); return true; }
        if (key == juce::KeyPress::spaceKey)          { studio.togglePlay(); return true; }
        if (key.getKeyCode() == 'L' && mods.isAnyModifierKeyDown() == false)
        {
            patSongButton.setToggleState (! patSongButton.getToggleState(), juce::sendNotification);
            return true;
        }
        return TypingKeyboard::noteForKey (key.getKeyCode()) >= 0 && ! mods.isCommandDown() && ! mods.isCtrlDown();
    }

    bool keyStateChanged (bool) override
    {
        // Typing in a text box (browser search, number entry) isn't playing notes.
        if (dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr)
            return false;
        return typing.update();
    }
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

            juce::PopupMenu templates;
            for (auto& name : Studio::getTemplateNames())
                templates.addItem (name, [this, name] { maybeSave ([this, name] { studio.newProjectFromTemplate (name); }); });
            auto userTemplates = Studio::getTemplatesFolder().findChildFiles (juce::File::findFiles, false, "*" + Studio::fileExtension);
            userTemplates.sort();
            if (! userTemplates.isEmpty())
            {
                templates.addSectionHeader ("My templates");
                for (auto& f : userTemplates)
                    templates.addItem (f.getFileNameWithoutExtension(), [this, f] { maybeSave ([this, f] { studio.load (f, true); }); });
            }
            m.addSubMenu ("New from template", templates);
            m.addItem ("Open...                 Ctrl+O", [this] { openProject(); });

            juce::PopupMenu recentMenu;
            recent.removeNonExistentFiles();
            recent.createPopupMenuItems (recentMenu, recentMenuBase, true, true);
            m.addSubMenu ("Open recent", recentMenu, recent.getNumFiles() > 0);
            m.addSeparator();
            m.addItem ("Save                    Ctrl+S", [this] { save (nullptr); });
            m.addItem ("Save as...        Ctrl+Shift+S", [this] { saveAs (nullptr); });
            m.addItem ("Save as template...", [this] { saveAsTemplate(); });
            m.addSeparator();
            m.addItem ("Export...                Ctrl+R", [this] { showExportDialog(); });
        }
        else if (index == 1)
        {
            m.addItem ("Undo                    Ctrl+Z", studio.undo().canUndo(), false, [this] { studio.undo().undo(); });
            m.addItem ("Redo                    Ctrl+Y", studio.undo().canRedo(), false, [this] { studio.undo().redo(); });
        }
        else if (index == 2)
        {
            m.addItem ("Audio settings... (F10)", [this] { showAudioSettings(); });
            m.addItem ("Manage plugins...", [this] { showPluginManager(); });
            m.addItem ("Add a sound... (F8)", [this] { showSoundPicker(); });
            m.addSeparator();
            m.addItem ("Keyboard shortcuts...", [this] { showShortcuts(); });
        }
        return m;
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { browser.refreshPlugins(); } // plugin scan finished

    // MIDI thread: forward controller moves to the message thread.
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
    {
        if (m.isNoteOnOrOff())
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this), m]
            {
                if (safe != nullptr) safe->studio.captureNote (m);
            });

        if (m.isController())
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this),
                                              ch = m.getChannel(), cc = m.getControllerNumber(), v = m.getControllerValue()]
            {
                if (safe != nullptr) safe->studio.handleController (ch, cc, v);
            });
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
        {
            addToRecent (file);
            reportMissingFiles();
        }
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

    void saveAsTemplate()
    {
        auto* w = new juce::AlertWindow ("Save as template", "Name it; it shows up in FILE > New from template.", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", "My template");
        w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int r)
        {
            const auto name = juce::File::createLegalFileName (w->getTextEditorContents ("name").trim());
            if (r == 1 && name.isNotEmpty())
                studio.saveCopyTo (Studio::getTemplatesFolder().getChildFile (name + Studio::fileExtension));
        }), true);
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

    // Scan for VST3 / AU plugins (out of process, crashing plugins get blacklisted).
    void showPluginManager()
    {
        auto& pm = studio.engine.getPluginManager();
        auto* list = new juce::PluginListComponent (pm.pluginFormatManager, pm.knownPluginList,
                                                    studio.engine.getTemporaryFileManager().getTempFile ("PluginScanDeadMansPedal"),
                                                    std::addressof (studio.engine.getPropertyStorage().getPropertiesFile()));
        list->setSize (720, 520);

        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (list);
        o.dialogTitle = "Plugins";
        o.dialogBackgroundColour = Theme::panelDark;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

    void addPlugin (const juce::PluginDescription& d)
    {
        if (d.isInstrument)
        {
            const int id = studio.addPluginChannel (d);
            studio.selectChannel (id);
            if (auto p = studio.getInstrument (id)) p->showWindowExplicitly();
        }
        else if (auto* m = dynamic_cast<Mixer*> (&mixerPanel->getContent()))
        {
            if (auto p = studio.addEffect (m->getSelectedInsert(), "ext:" + d.createIdentifierString()))
                p->showWindowExplicitly();
            mixerPanel->show();
        }
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
        content->onEditSample = [this] (int id) { editChannelAudio (id); };
        const auto size = content->getBounds();
        auto* panel = channelPanels.add (new Panel (studio.getChannel (channelId)[IDs::name].toString(), std::move (content)));
        workspace.addAndMakeVisible (panel);
        panel->setBounds (320 + 24 * (channelPanels.size() % 6), 120 + 24 * (channelPanels.size() % 6),
                          size.getWidth() + 2, size.getHeight() + Panel::titleHeight + 1);
        panel->toFront (false);
    }

    // Audio editor window on a file; `onApply` gets the edited copy.
    void openAudioEditor (const juce::File& file, std::function<void (const juce::File&)> onApply)
    {
        if (! file.existsAsFile())
            return;
        auto content = std::make_unique<AudioEditor> (studio.engine.getAudioFileFormatManager().readFormatManager, file);
        content->onPreview = [this] (const juce::File& f) { browser.previewFile (f); };
        content->onApply = std::move (onApply);
        const auto size = content->getBounds();
        auto* panel = editorPanels.add (new Panel ("Audio editor - " + file.getFileName(), std::move (content)));
        workspace.addAndMakeVisible (panel);
        panel->setBounds (200 + 24 * (editorPanels.size() % 6), 140 + 24 * (editorPanels.size() % 6),
                          size.getWidth() + 2, size.getHeight() + Panel::titleHeight + 1);
        panel->toFront (false);
    }

    // Playlist audio clip, sampler or single-sample instrument: edit, then repoint the channel.
    void editChannelAudio (int channelId)
    {
        const juce::File file (studio.getChannel (channelId)[IDs::file].toString());
        openAudioEditor (file, [this, channelId] (const juce::File& edited)
        {
            if (studio.replaceAudioFile (channelId, edited))
                flashHint ("Using the edited audio: " + edited.getFileName() + " (Ctrl+Z goes back to the original)");
        });
    }

    // Browser preset: load it on the selected channel's instrument or the selected insert's effect.
    void applyBrowserPreset (const Browser::PresetEntry& p)
    {
        auto load = [this, &p] (te::Plugin& plugin, const juce::String& where)
        {
            studio.undo().beginNewTransaction ("Preset");
            if (p.userFile.existsAsFile()) Presets::loadUser (plugin, p.userFile);
            else if (p.factory != nullptr) Presets::apply (plugin, *p.factory);
            flashHint ("Loaded preset '" + p.name + "' on " + where);
        };

        if (auto inst = studio.getInstrument (studio.getSelectedChannel()); inst != nullptr && inst->getPluginType() == p.pluginType)
            return load (*inst, studio.getChannel (studio.getSelectedChannel())[IDs::name].toString());

        if (auto* m = dynamic_cast<Mixer*> (&mixerPanel->getContent()))
            for (auto fx : studio.getInsertEffects (m->getSelectedInsert()))
                if (fx->getPluginType() == p.pluginType)
                    return load (*fx, studio.getInsertName (m->getSelectedInsert()));

        flashHint ("'" + p.name + "' is a " + p.group + " preset: select a channel or mixer insert that uses " + p.group);
    }

    void flashHint (const juce::String& text, int ms = 5000)
    {
        flashText = text;
        flashUntil = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    }

    void reportMissingFiles()
    {
        const auto& missing = studio.getMissingFiles();
        if (missing.isEmpty())
            return;
        int plugins = 0;
        for (auto& m : missing)
            plugins += m.startsWith ("plugin:") ? 1 : 0;
        juce::StringArray parts;
        if (missing.size() > plugins) parts.add (juce::String (missing.size() - plugins) + " sample" + (missing.size() - plugins == 1 ? "" : "s"));
        if (plugins > 0)              parts.add (juce::String (plugins) + " plugin" + (plugins == 1 ? "" : "s"));
        flashHint (parts.joinIntoString (" and ") + " could not be found: " + missing.joinIntoString (", "), 12000);
    }

    //==========================================================================
    // Windows

    juce::Array<Panel*> getAllPanels() const
    {
        juce::Array<Panel*> all { playlistPanel.get(), channelRackPanel.get(), pianoRollPanel.get(), mixerPanel.get() };
        for (auto* list : { &effectPanels, &channelPanels, &editorPanels })
            all.addArray (*list);
        return all;
    }

    void closeAllWindows()
    {
        for (auto* p : getAllPanels())
            p->close();
    }

    // FL Tab: the window at the back comes to the front, so repeated presses cycle through them.
    void cycleWindows()
    {
        for (auto* c : workspace.getChildren())
            if (auto* p = dynamic_cast<Panel*> (c); p != nullptr && p->isVisible())
            {
                p->show();
                workspace.repaint();
                return;
            }
    }

    void stepPattern (int delta)
    {
        auto patterns = studio.getPatterns();
        const int index = patterns.indexOf (studio.getCurrentPatternState()) + delta;
        if (juce::isPositiveAndBelow (index, patterns.getNumChildren()))
            studio.setCurrentPattern (patterns.getChild (index)[IDs::id]);
    }

    void showShortcuts()
    {
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (new Shortcuts::Help());
        o.dialogTitle = "Keyboard shortcuts";
        o.dialogBackgroundColour = Theme::panelDark;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    // FL F8 "plugin picker": synth sounds, our instruments, SFZ/SF2 files and installed plugins.
    void showSoundPicker()
    {
        std::vector<SoundPicker::Item> items;
        auto added = [this] (int id)
        {
            if (id == 0)
                return;
            studio.selectChannel (id);
            channelRackPanel->show();
            flashHint ("Added '" + studio.getChannel (id)[IDs::name].toString() + "' to the channel rack");
        };

        items.push_back ({ "Sampler", "Instrument", [this, added] { added (studio.addSamplerChannel (DrumKit::getDrumsFolder().getChildFile ("Kick.wav"))); } });
        items.push_back ({ "4-OSC Synth", "Instrument", [this, added] { added (studio.addSynthChannel()); } });
        items.push_back ({ "FM Synth", "Instrument", [this, added] { added (studio.addFmSynthChannel()); } });
        items.push_back ({ "Drum pad", "Instrument", [this, added] { added (studio.addDrumPadChannel()); } });
        for (auto& sound : Presets::allSynthSounds())
            items.push_back ({ sound.preset->name, "Synth - " + sound.preset->category,
                               [this, added, name = sound.preset->name] { added (studio.addPresetChannel (name)); } });
        for (auto& f : MultiSampler::getInstrumentsFolder().findChildFiles (juce::File::findFiles, true, "*.sfz;*.sf2"))
            items.push_back ({ f.getFileNameWithoutExtension(), f.getFileExtension().substring (1).toUpperCase(),
                               [this, added, f] { added (studio.addInstrumentChannel (f)); } });
        for (auto& d : studio.getKnownPlugins (true))
            items.push_back ({ d.name, d.pluginFormatName, [this, d] { addPlugin (d); } });

        auto area = workspace.getBounds().withHeight (1).withSizeKeepingCentre (40, 1).translated (0, 4);
        juce::CallOutBox::launchAsynchronously (std::make_unique<SoundPicker> (std::move (items)), area, this);
    }

    //==========================================================================
    // Window layout remembered across launches (not for --snapshot / --selftest runs).

    static bool isDevRun() { return juce::JUCEApplicationBase::getCommandLineParameters().contains ("--"); }

    void saveLayout()
    {
        if (isDevRun())
            return;
        juce::XmlElement xml ("LAYOUT");
        xml.setAttribute ("browser", browserVisible);
        for (auto [name, panel] : namedPanels())
        {
            auto* e = xml.createNewChildElement ("PANEL");
            e->setAttribute ("name", name);
            e->setAttribute ("open", panel->isOpen());
            e->setAttribute ("bounds", panel->getNormalBounds().toString());
            e->setAttribute ("maximised", panel->isMaximised());
        }
        settings.setValue ("layout", &xml);
        settings.saveIfNeeded();
    }

    void restoreLayout()
    {
        auto xml = settings.getXmlValue ("layout");
        if (xml == nullptr || isDevRun())
            return;
        browserVisible = xml->getBoolAttribute ("browser", true);
        for (auto [name, panel] : namedPanels())
            if (auto* e = xml->getChildByAttribute ("name", name))
            {
                const auto r = juce::Rectangle<int>::fromString (e->getStringAttribute ("bounds"));
                if (r.getWidth() >= 200 && r.getHeight() >= 120)
                    panel->setBounds (r.withPosition (juce::jmax (0, r.getX()), juce::jmax (0, r.getY())));
                panel->setVisible (e->getBoolAttribute ("open"));
                if (e->getBoolAttribute ("maximised"))
                    maximiseWhenSized.add (panel);
            }
    }

    std::vector<std::pair<const char*, Panel*>> namedPanels() const
    {
        return { { "playlist", playlistPanel.get() }, { "channelRack", channelRackPanel.get() },
                 { "pianoRoll", pianoRollPanel.get() }, { "mixer", mixerPanel.get() } };
    }

    void showTimeSigMenu()
    {
        juce::PopupMenu m;
        for (int n = 2; n <= 12; ++n)
            m.addItem (juce::String (n) + "/4", true, n == studio.getTimeSigNumerator(), [this, n] { studio.setTimeSigNumerator (n); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (timeSigButton));
    }

    bool isFrontPanel (const Panel& p) const
    {
        if (p.isDetached())
            return p.isDetachedAndActive();
        if (! p.isVisible())
            return false;
        for (int i = workspace.getNumChildComponents(); --i >= 0;)
            if (auto* c = workspace.getChildComponent (i); c->isVisible())
                return c == &p;
        return false;
    }

    void showExportDialog()
    {
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (new Exporter::Dialog (studio, getProjectName()));
        o.dialogTitle = "Export";
        o.dialogBackgroundColour = Theme::panelDark;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void showRecordMenu()
    {
        auto& o = studio.recordOptions;
        juce::PopupMenu m;
        m.addSectionHeader ("What to record");
        m.addItem ("Audio (into the playlist)", true, o.audio, [&o] { o.audio = ! o.audio; });
        m.addItem ("Notes (into the pattern)", true, o.notes, [&o] { o.notes = ! o.notes; });
        m.addItem ("Automation (knob moves)", true, o.automation, [&o] { o.automation = ! o.automation; });
        m.addSeparator();
        m.addItem ("Count-in (1 bar)", true, o.countIn, [&o] { o.countIn = ! o.countIn; });
        m.addItem ("Punch in/out (playlist loop region)", true, o.punch, [&o] { o.punch = ! o.punch; });
        m.addItem ("Monitor input", true, o.monitorInput, [&o] { o.monitorInput = ! o.monitorInput; });
        m.addSeparator();
        m.addItem ("Audio input settings... (F10)", [this] { showAudioSettings(); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (recordButton));
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
        if (juce::Time::getMillisecondCounter() < flashUntil)
            newHint = flashText;

        const double beats = studio.getPositionBeats();
        const double barBeats = studio.getBarBeats();
        const int bar = (int) std::floor (beats / barBeats) + 1;
        const double inBar = std::fmod (beats, barBeats);
        const int step = (int) std::floor (inBar / Studio::stepBeats) + 1;
        const int tick = (int) std::floor (std::fmod (inBar, Studio::stepBeats) / Studio::stepBeats * 24.0);
        const auto newTime = juce::String::formatted ("%d:%02d:%02d", bar, step, tick);
        // CPU, RAM, voices: a few times a second is plenty.
        auto newStatus = statusText;
        if (ticks % 8 == 0)
            newStatus = "CPU " + juce::String (juce::roundToInt (studio.engine.getDeviceManager().getCpuUsage() * 100.0)) + "%   RAM "
                      + juce::String (juce::roundToInt ((double) getProcessMemory() / (1024.0 * 1024.0))) + " MB   Voices "
                      + juce::String (studio.getActiveVoices());

        if (newHint != hint || newTime != timeText || newStatus != statusText)
        {
            hint = newHint;
            timeText = newTime;
            statusText = newStatus;
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

        // With tempo changes, show the tempo at the playhead while playing (FL does too).
        tempo.setValue (studio.isPlaying() && studio.getTempoChanges().getNumChildren() > 0 ? studio.getBpmAtBeat (beats) : studio.getBpm());
        const auto sig = juce::String (studio.getTimeSigNumerator()) + "/4";
        if (timeSigButton.getButtonText() != sig)
            timeSigButton.setButtonText (sig);
        playButton.setToggleState (studio.isPlaying(), juce::dontSendNotification);
        recordButton.setToggleState (studio.recordArmed || studio.isRecording(), juce::dontSendNotification);
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
                                          if (result == 1 && studio.load (getAutosaveFile(), true))
                                              reportMissingFiles();
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
    void debugOpenAudioEditor()
    {
        studio.setSongMode (true);
        auto clip = studio.addAudioClip (DrumKit::getPacksFolder().getChildFile ("Loops/Drum Loop 120 BPM.wav"), 0, 0.0);
        studio.addTempoChange (16.0, 90.0);
        playlistPanel->show();
        channelRackPanel->setVisible (false);
        if (clip.isValid())
            editChannelAudio (clip[IDs::channel]);
        if (! editorPanels.isEmpty())
            editorPanels.getLast()->setTopLeftPosition (100, 330);
    }
    void debugOpenPicker()       { showSoundPicker(); }
    void debugOpenFirstChannel() { toggleChannelWindow (studio.getChannels().getChild (0)[IDs::id]); }
    void debugOpenDrumPad()      { toggleChannelWindow (studio.addDrumPadChannel()); }
    void debugTemplate (const juce::String& name) { studio.newProjectFromTemplate (name); }
    void debugOpenFm()           { toggleChannelWindow (studio.addPresetChannel ("E-Piano")); }
    void debugOpenSynth()        { toggleChannelWindow (studio.addPresetChannel ("Acid Bass")); }
    void debugOpenInstrument()   { toggleChannelWindow (studio.addInstrumentChannel (DrumKit::getDrumsFolder().getChildFile ("808.wav"))); }
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
        mixerPanel->show();
        studio.setChannelInsert (studio.getChannels().getChild (0)[IDs::id], 1);
        if (auto p = studio.addEffect (1, te::EqualiserPlugin::xmlTypeName))
        {
            Presets::applyValues (*p, Presets::factory (p->getPluginType())[3].values);
            openEffectWindow (*p);
        }
        studio.addEffect (1, te::ReverbPlugin::xmlTypeName);
        if (auto p = studio.addEffect (0, AnalyzerPlugin::xmlTypeName)) openEffectWindow (*p);
        studio.addSend (1, 2);
        channelRackPanel->setVisible (false);
        if (effectPanels.size() > 1)
            effectPanels[1]->setTopLeftPosition (700, 20);
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
        if (auto t = studio.getTrack (studio.getChannels().getChild (0)[IDs::id]))
        {
            auto clip = studio.createAutomationClip (*t->getVolumePlugin()->volParam, 0.0, 32.0);
            Studio::addAutomationPoint (clip, 8.0, 0.2f, nullptr);
            Studio::addAutomationPoint (clip, 16.0, 0.9f, nullptr);
            clip.getChild (1).setProperty (IDs::tension, 0.6, nullptr);
        }
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

    juce::OwnedArray<Panel> editorPanels;
    juce::TextButton timeSigButton { "4/4" };

    juce::Rectangle<int> hintArea, timeArea, cpuArea;
    juce::String hint, timeText, flashText, statusText;
    juce::uint32 flashUntil = 0;
    juce::Array<Panel*> maximiseWhenSized;
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
        setResizeLimits (960, 600, 16384, 16384);
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
    inline bool check (bool condition, const juce::String& what)
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

        {
            Studio fresh (engine);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300); // let Tracktion's async updates run
            ok &= check (! fresh.isDirty(), "a new project isn't 'unsaved' once the engine settles");
        }
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
            // Editing one channel only rewrites that channel's engine notes.
            const int clap = s.getChannels().getChild (1)[IDs::id];
            s.setStep (clap, 4, true);
            countNotes (s, kick);
            struct Counter : juce::ValueTree::Listener
            {
                int changes = 0;
                void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override        { ++changes; }
                void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { ++changes; }
            } kickChanges, clapChanges;
            auto kickState = s.getTrack (kick)->state, clapState = s.getTrack (clap)->state;
            kickState.addListener (&kickChanges);
            clapState.addListener (&clapChanges);
            s.setStep (kick, 6, true);
            s.regenerateNow();
            kickState.removeListener (&kickChanges);
            clapState.removeListener (&clapChanges);
            ok &= check (kickChanges.changes > 0 && clapChanges.changes == 0, "regen rewrites only the edited channel");
            s.setStep (kick, 6, false);
            s.setStep (clap, 4, false);
        }

        {
            // Workspace windows: maximize / restore, detach into a native window and back.
            juce::Component workspace;
            workspace.setSize (800, 600);
            auto panel = std::make_unique<Panel> ("Test", std::make_unique<juce::Label> ("content", "hello"));
            auto* content = &panel->getContent();
            workspace.addAndMakeVisible (*panel);
            panel->setBounds (10, 20, 300, 200);
            panel->setMaximised (true);
            const bool maximised = panel->getBounds() == workspace.getLocalBounds();
            workspace.setSize (900, 700);
            const bool follows = panel->getBounds() == workspace.getLocalBounds();
            panel->setMaximised (false);
            ok &= check (maximised && follows && panel->getBounds() == juce::Rectangle<int> (10, 20, 300, 200), "window maximize follows the workspace, restore puts it back");
            panel->setDetached (true);
            const bool detached = panel->isDetached() && panel->isOpen() && ! panel->isVisible() && content->getParentComponent() != panel.get()
                                  && content->isShowing();
            panel->setDetached (false);
            ok &= check (detached && ! panel->isDetached() && content->getParentComponent() == panel.get() && panel->isVisible(),
                         "window detaches into its own window and docks back");
            panel->setDetached (true);
            panel = nullptr; // deleting a detached window must not leave its native window behind
            ok &= check (juce::TopLevelWindow::getNumTopLevelWindows() == 0, "deleting a detached window closes it");
        }

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

        {
            // Every catalog effect loads; presets change parameters.
            int created = 0;
            for (auto& e : Effects::builtIn())
                if (auto p = s.addEffect (6, e.xmlType))
                {
                    Effects::prepareNew (*p);
                    ++created;
                    s.removePlugin (*p);
                }
            ok &= check (created == (int) Effects::builtIn().size(), "all built-in effects can be created");

            auto eq = s.addEffect (6, te::EqualiserPlugin::xmlTypeName);
            Presets::applyValues (*eq, Presets::factory (eq->getPluginType())[1].values); // Bass boost
            float lowGain = 0.0f;
            for (auto* param : eq->getAutomatableParameters())
                if (param->paramID == "Low-pass gain")
                    lowGain = param->getCurrentValue();
            ok &= check (std::abs (lowGain - 6.0f) < 0.01f, "factory preset applies");
            s.removePlugin (*eq);
            ok &= check (Effects::airwindows().size() > 150, "Airwindows library available");

           #if JUCE_MAC
            // Third-party hosting: load one of Apple's built-in Audio Units as an effect.
            juce::AudioUnitPluginFormat au;
            juce::OwnedArray<juce::PluginDescription> found;
            for (auto& id : au.searchPathsForPlugins ({}, false, false))
                if (id.containsIgnoreCase ("dely,appl") || id.containsIgnoreCase ("lpas,appl"))
                {
                    au.findAllTypesForFile (found, id);
                    if (! found.isEmpty()) break;
                }
            ok &= check (! found.isEmpty(), "found Apple's built-in Audio Units");
            if (! found.isEmpty())
            {
                engine.getPluginManager().knownPluginList.addType (*found[0]);
                auto p = s.addEffect (7, "ext:" + found[0]->createIdentifierString());
                auto ext = dynamic_cast<te::ExternalPlugin*> (p.get());
                ok &= check (ext != nullptr && ext->getAudioPluginInstance() != nullptr, "hosts an Audio Unit (" + found[0]->name + ")");
                if (p != nullptr) s.removePlugin (*p);
                engine.getPluginManager().knownPluginList.removeType (*found[0]);
            }
           #endif
        }

        {
            // Automation clip on the kick's volume drives the engine curve in song mode.
            auto vol = s.getTrack (kick)->getVolumePlugin()->volParam;
            s.setSongMode (true);
            auto clip = s.createAutomationClip (*vol, 0.0, 8.0);
            clip.getChild (0).setProperty (IDs::value, 0.0, nullptr);
            clip.getChild (1).setProperty (IDs::value, 1.0, nullptr);
            s.regenerateNow();
            ok &= check (vol->getCurve().getNumPoints() == 2, "automation clip writes engine curve");
            const auto mid = s.getEdit().tempoSequence.toTime (te::BeatPosition::fromBeats (4.0));
            const float midValue = vol->valueRange.convertTo0to1 (vol->getCurve().getValueAt (mid, 0.0f));
            ok &= check (std::abs (midValue - 0.5f) < 0.05f, "automation ramps between points");
            s.setSongMode (false);
            s.regenerateNow();
            ok &= check (vol->getCurve().getNumPoints() == 0, "pattern mode ignores automation clips");
            s.getPlaylist().removeChild (clip, nullptr);

            // MIDI learn: link CC 20 then move it.
            auto pan = s.getTrack (kick)->getVolumePlugin()->panParam;
            s.startLearning (*pan);
            s.handleController (1, 20, 64);
            ok &= check (s.getControllerLink (*pan) == "CC 20", "link to controller learns CC");
            s.handleController (1, 20, 127);
            ok &= check (pan->getCurrentNormalisedValue() > 0.99f, "controller moves the knob");
        }

        {
            // Recording: notes captured into the pattern; recorded audio harvested into the playlist.
            s.setSongMode (false);
            s.selectChannel (kick);
            const int before = s.getCurrentPatternState().getNumChildren();
            s.recordOptions = {};
            s.recordOptions.audio = false;
            s.recordOptions.countIn = false;
            s.recordArmed = true;
            s.togglePlay();
            ok &= check (s.isRecording(), "armed + play starts recording");
            s.captureNote (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 90));
            s.captureNote (juce::MidiMessage::noteOff (1, 64));
            s.togglePlay();
            s.recordArmed = false;
            ok &= check (s.getCurrentPatternState().getNumChildren() == before + 1, "recorded note lands in the pattern");

            auto rec = s.getRecorderTrack (true);
            rec->insertWaveClip ("take", DrumKit::getDrumsFolder().getChildFile ("Snare.wav"),
                                te::ClipPosition { te::TimeRange (te::TimePosition::fromSeconds (1.0), te::TimePosition::fromSeconds (1.3)), te::TimeDuration() }, false);
            const int clipsBefore = s.getPlaylist().getNumChildren();
            ok &= check (s.harvestRecordedAudio() == 1 && s.getPlaylist().getNumChildren() == clipsBefore + 1,
                         "recorded audio becomes a playlist clip");
            ok &= check (rec->getClips().isEmpty(), "recorder track is emptied");
        }

        {
            // Export: offline renders of mix, stems and MIDI.
            const auto dir = juce::File::createTempFile ("export");
            dir.createDirectory();
            Exporter::Settings es;
            es.song = false;
            es.tailSeconds = 0.5;
            const auto wav = dir.getChildFile ("mix.wav");
            ok &= check (Exporter::exportMix (s, es, wav), "export WAV mixdown");
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            if (auto reader = std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (wav)))
            {
                juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
                ok &= check (buffer.getMagnitude (0, buffer.getNumSamples()) > 0.01f, "mixdown contains audio");
                ok &= check (std::abs ((double) reader->lengthInSamples / reader->sampleRate - (s.getPatternLengthBeats (s.getCurrentPatternState()) * 60.0 / s.getBpm() + 0.5)) < 0.1,
                             "mixdown length = pattern + tail");
            }
            es.format = Exporter::Format::flac;
            ok &= check (Exporter::exportMix (s, es, dir.getChildFile ("mix.flac")), "export FLAC");
            es.format = Exporter::Format::ogg;
            ok &= check (Exporter::exportMix (s, es, dir.getChildFile ("mix.ogg")), "export OGG");
            if (Exporter::findLame().existsAsFile())
            {
                es.format = Exporter::Format::mp3;
                ok &= check (Exporter::exportMix (s, es, dir.getChildFile ("mix.mp3")), "export MP3");
            }
            es.format = Exporter::Format::wav;
            ok &= check (Exporter::exportStems (s, es, dir, "test") >= 2, "export stems");
            ok &= check (Exporter::exportMidi (s, false, dir.getChildFile ("song.mid")), "export MIDI");
            dir.deleteRecursively();
        }

        // Multi-sampler instruments (SFZ, single WAV loop, drum pad, slicer).
        const auto instrDir = juce::File::createTempFile ("instr");
        int sfzChannel = 0, padChannel = 0, sliceChannel = 0, sf2Channel = 0;
        {
            using namespace DrumKit::detail;
            const auto tone = instrDir.getChildFile ("samples/tone.wav");
            double phase = 0.0;
            writeWav (tone, render (0.1, [&] (double, Rng&) { phase += juce::MathConstants<double>::twoPi * 440.0 / sampleRate; return (float) std::sin (phase) * 0.5f; }));

            const auto sfz = instrDir.getChildFile ("test.sfz");
            sfz.replaceWithText ("// test instrument\n"
                                 "<control> default_path=samples/\n"
                                 "<global> ampeg_release=0.2 /* inherited */\n"
                                 "<group> lokey=c4 hikey=b4 pitch_keycenter=c4\n"
                                 "<region> sample=tone.wav lovel=1 hivel=63\n"
                                 "<region> sample=tone.wav lovel=64 hivel=127 volume=-3\n"
                                 "<group> key=c#5 seq_length=2 unknown_opcode=7\n"
                                 "<region> sample=tone.wav seq_position=1\n"
                                 "<region> sample=tone.wav seq_position=2 tune=50\n"
                                 "<region> sample=*sine\n");
            const auto regions = MultiSampler::parseSfz (sfz);
            ok &= check (regions.size() == 4, "SFZ: 4 regions parsed (generator skipped)");
            if (regions.size() == 4)
            {
                ok &= check ((int) regions[0]["lokey"] == 60 && (int) regions[0]["hikey"] == 71 && (int) regions[0]["pitch_keycenter"] == 60,
                             "SFZ: group key range with note names (c4 = 60)");
                ok &= check ((int) regions[0]["hivel"] == 63 && (int) regions[1]["lovel"] == 64 && juce::approximatelyEqual ((double) regions[1]["volume"], -3.0),
                             "SFZ: velocity layers");
                ok &= check (juce::approximatelyEqual ((double) regions[3]["ampeg_release"], 0.2) && juce::File (regions[0]["sample"].toString()) == tone,
                             "SFZ: <global> inherited, default_path resolved");
                ok &= check ((int) regions[2]["lokey"] == 73 && (int) regions[2]["hikey"] == 73 && (int) regions[2]["seq_length"] == 2
                             && (int) regions[3]["seq_position"] == 2, "SFZ: key= shortcut and round robin");
            }

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            auto makeSet = [&formats] (const juce::Array<juce::ValueTree>& trees)
            {
                MultiSampler::SampleCache cache (formats);
                auto set = std::make_unique<MultiSampler::SoundSet>();
                for (auto& t : trees)
                    if (auto r = MultiSampler::loadRegion (t, cache))
                        set->regions.push_back (*r);
                set->seqCounters.resize (set->regions.size());
                return set;
            };
            auto run = [] (MultiSampler::Engine& e, double seconds)
            {
                juce::AudioBuffer<float> out (2, (int) (seconds * 44100.0));
                out.clear();
                for (int i = 0; i < out.getNumSamples(); i += 512)
                    e.render (out, i, juce::jmin (512, out.getNumSamples() - i), {});
                return out;
            };

            {
                auto set = makeSet (regions);
                MultiSampler::Engine e;
                e.prepare (44100.0);
                e.setSoundSet (set.get());
                e.handleMidi (juce::MidiMessage::noteOn (1, 73, (juce::uint8) 100), {});
                ok &= check (e.getNumActiveVoices() == 1, "round robin plays one of two alternates");
                e.reset();
                e.handleMidi (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 30), {});
                ok &= check (e.getNumActiveVoices() == 1, "velocity picks one layer");
            }

            for (auto looped : { true, false })
            {
                auto region = MultiSampler::makeRegion (tone, 0, 127, 60);
                region.setProperty ("loop_mode", looped ? "loop_continuous" : "no_loop", nullptr);
                auto set = makeSet ({ region });
                MultiSampler::Engine e;
                e.prepare (44100.0);
                e.setSoundSet (set.get());
                e.handleMidi (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), {});
                const auto out = run (e, 0.5);
                const float tail = out.getMagnitude (0, 22050 - 4410, 4410);
                if (looped) ok &= check (tail > 0.1f, "looping region keeps sounding past the sample end");
                else        ok &= check (tail < 1.0e-4f && e.getNumActiveVoices() == 0, "unlooped region stops at the sample end");
            }

            {
                auto set = makeSet (MultiSampler::defaultDrumPads());
                MultiSampler::Engine e;
                e.prepare (44100.0);
                e.setSoundSet (set.get());
                e.handleMidi (juce::MidiMessage::noteOn (1, MultiSampler::firstPadNote + 5, (juce::uint8) 100), {}); // open hat
                run (e, 0.05);
                e.handleMidi (juce::MidiMessage::noteOn (1, MultiSampler::firstPadNote + 4, (juce::uint8) 100), {}); // closed hat
                run (e, 0.3);
                ok &= check (e.getNumActiveVoices() == 0, "closed hat chokes the open hat");
            }

            // End to end: SFZ channel renders through the engine.
            sfzChannel = s.addInstrumentChannel (sfz);
            auto ms = s.getMultiSampler (sfzChannel);
            ok &= check (ms != nullptr && ms->getInfo().numRegions == 4, "SFZ instrument channel loads 4 regions");
            s.addNote (s.getCurrentPatternState(), sfzChannel, 0.0, 1.0, 60, 100);
            Exporter::Settings es;
            es.song = false;
            es.tailSeconds = 0.2;
            const auto wav = instrDir.getChildFile ("render.wav");
            Exporter::render (s, es, wav, { s.getTrack (sfzChannel) }, false);
            if (auto reader = std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (wav)))
            {
                juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
                ok &= check (buffer.getMagnitude (0, buffer.getNumSamples()) > 0.05f, "instrument channel renders audio offline");
            }
            else ok &= check (false, "instrument channel renders audio offline");

            // SoundFont (only if one is installed, e.g. `brew install fluid-synth`).
            const juce::File sf2 ("/opt/homebrew/share/fluid-synth/sf2/VintageDreamsWaves-v2.sf2");
            if (sf2.existsAsFile())
            {
                sf2Channel = s.addInstrumentChannel (sf2);
                auto font = s.getMultiSampler (sf2Channel);
                ok &= check (font->getInfo().presetNames.size() > 1 && font->getInfo().numRegions > 0, "SoundFont loads presets");
                font->setProgram (2);
                font->flushPendingUpdates();
                ok &= check (font->getProgram() == 2 && font->getInfo().numRegions > 0, "SoundFont preset change");
                s.addNote (s.getCurrentPatternState(), sf2Channel, 0.0, 1.0, 60, 100);
                Exporter::render (s, es, wav, { s.getTrack (sf2Channel) }, false);
                if (auto reader = std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (wav)))
                {
                    juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
                    reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
                    ok &= check (buffer.getMagnitude (0, buffer.getNumSamples()) > 0.01f, "SoundFont channel renders audio offline");
                }
            }

            padChannel = s.addDrumPadChannel();
            ok &= check (s.getMultiSampler (padChannel)->getInfo().numRegions == 16, "drum pad has 16 pads");

            // Slicer: 4 hits in a 2 s loop.
            const auto loopFile = instrDir.getChildFile ("loop.wav");
            writeWav (loopFile, render (2.0, [] (double t, Rng& rng)
            {
                const double local = std::fmod (t, 0.5);
                return local < 0.15 ? rng.next() * (float) std::exp (-local / 0.03) : 0.0f;
            }));
            const int notesBefore = s.getCurrentPatternState().getNumChildren();
            sliceChannel = s.addSlicerChannel (loopFile);
            const int slices = sliceChannel != 0 ? s.getMultiSampler (sliceChannel)->getInfo().numRegions : 0;
            ok &= check (slices == 4, "slicer finds the 4 hits (" + juce::String (slices) + ")");
            ok &= check (s.getCurrentPatternState().getNumChildren() == notesBefore + slices, "slicer writes one pattern note per slice");
        }

        int fmChannel = 0;
        {
            // Phase 14 synths, presets, templates, sound library.
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            const auto tmp = juce::File::createTempFile ("synths");
            tmp.createDirectory();

            auto renderPeak = [&] (Studio& st, int channel)
            {
                Exporter::Settings es;
                es.song = false;
                es.tailSeconds = 0.3;
                const auto wav = tmp.getChildFile ("synth.wav");
                Exporter::render (st, es, wav, { st.getTrack (channel) }, false);
                float peak = 0.0f, rms = 0.0f;
                if (auto reader = std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (wav)))
                {
                    juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
                    reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
                    peak = buffer.getMagnitude (0, buffer.getNumSamples());
                    rms = buffer.getRMSLevel (0, 0, buffer.getNumSamples());
                }
                return std::pair { peak, rms };
            };
            auto paramValue = [] (te::Plugin* p, const juce::String& id)
            {
                for (auto* param : p->getAutomatableParameters())
                    if (param->paramID == id)
                        return param->getCurrentValue();
                return -12345.0f;
            };

            Studio t (engine);
            t.newProjectFromTemplate ("Empty");
            const int fm = t.addFmSynthChannel();
            auto* fmPlugin = dynamic_cast<FMSynthPlugin*> (t.getInstrument (fm));
            ok &= check (fmPlugin != nullptr && t.getChannel (fm)[IDs::type].toString() == ChannelType::fm, "FM Synth channel");
            t.addNote (t.getCurrentPatternState(), fm, 0.0, 1.0, 60, 100);
            t.addNote (t.getCurrentPatternState(), fm, 1.0, 1.0, 67, 100);
            ok &= check (renderPeak (t, fm).first > 0.05f, "FM Synth renders audio offline");

            if (fmPlugin != nullptr)
            {
                for (int op = 0; op < FM::numOps; ++op)
                    fmPlugin->getParam (op * FM::numOpParams + FM::level)->setParameter (0.5f, juce::sendNotificationSync);
                juce::Array<float> levels;
                for (int a = 0; a < FM::numAlgorithms; ++a)
                {
                    fmPlugin->getParam (FM::algorithm)->setParameter ((float) a, juce::sendNotificationSync);
                    levels.add (renderPeak (t, fm).second);
                }
                bool allSound = true, allDiffer = true;
                for (int a = 0; a < levels.size(); ++a)
                {
                    allSound &= levels[a] > 0.005f;
                    if (a > 0) allDiffer &= std::abs (levels[a] - levels[0]) > 1.0e-4f;
                }
                ok &= check (allSound, "every FM algorithm renders audio");
                ok &= check (allDiffer, "FM algorithms sound different");
            }

            // Factory presets: enough of them, and every value hits a real parameter (or 4-OSC state).
            for (auto type : { te::FourOscPlugin::xmlTypeName, FMSynthPlugin::xmlTypeName })
            {
                const int ch = juce::String (type) == FMSynthPlugin::xmlTypeName ? t.addFmSynthChannel() : t.addSynthChannel();
                auto* plugin = t.getInstrument (ch);
                bool valid = true;
                for (auto& preset : Presets::synthFactory (type))
                    for (auto& [id, v] : preset.values)
                    {
                        bool isParam = false;
                        for (auto* param : plugin->getAutomatableParameters())
                            isParam |= param->paramID == id;
                        bool isState = false;
                        for (auto& [stateId, def] : Presets::fourOscStateDefaults())
                            isState |= juce::String (type) == te::FourOscPlugin::xmlTypeName && stateId == id;
                        if (! isParam && ! isState)
                        {
                            std::cout << "        unknown preset value " << preset.name << ": " << id << std::endl;
                            valid = false;
                        }
                    }
                ok &= check (Presets::synthFactory (type).size() >= 12 && valid, juce::String (type) + ": " + juce::String ((int) Presets::synthFactory (type).size())
                                                                                     + " factory presets, all values valid");
            }

            {
                const int synth = t.addSynthChannel();
                auto* osc = t.getInstrument (synth);
                Presets::apply (*osc, *Presets::findSynthSound ("Acid Bass")->preset);
                const bool acid = (int) osc->state["filterType"] == 1 && std::abs (paramValue (osc, "filterResonance") - 78.0f) < 0.01f;
                Presets::apply (*osc, *Presets::findSynthSound ("Soft Pad")->preset);
                ok &= check (acid && std::abs (paramValue (osc, "ampAttack") - 1.2f) < 0.01f && std::abs (paramValue (osc, "filterResonance") - 0.5f) < 0.01f
                                 && (int) osc->state["voices1"] == 4,
                             "4-OSC preset changes parameters + state, resets the rest");
                Presets::apply (*fmPlugin, *Presets::findSynthSound ("FM Bell")->preset);
                const bool bell = std::abs (paramValue (fmPlugin, "op2Ratio") - 3.5f) < 0.01f;
                Presets::apply (*fmPlugin, *Presets::findSynthSound ("E-Piano")->preset);
                ok &= check (bell && std::abs (paramValue (fmPlugin, "op4Ratio") - 14.0f) < 0.01f && std::abs (paramValue (fmPlugin, "op2Ratio") - 1.0f) < 0.01f,
                             "FM preset changes parameters");
            }

            {
                const int before = t.getChannels().getNumChildren();
                const int acid = t.addPresetChannel ("Acid Bass");
                auto* p = t.getInstrument (acid);
                ok &= check (p != nullptr && p->getPluginType() == te::FourOscPlugin::xmlTypeName && t.getChannel (acid)[IDs::name].toString() == "Acid Bass"
                                 && (int) p->state["filterType"] == 1 && std::abs (paramValue (p, "filterFreq") - 48.0f) < 0.01f,
                             "Synth presets: 'Acid Bass' makes a named 4-OSC channel with the preset");
                const int keys = t.addPresetChannel ("E-Piano");
                ok &= check (t.getChannel (keys)[IDs::type].toString() == ChannelType::fm && std::abs (paramValue (t.getInstrument (keys), "op4Ratio") - 14.0f) < 0.01f,
                             "Synth presets: 'E-Piano' makes an FM channel");
                t.undo().undo();
                ok &= check (t.getChannels().getNumChildren() == before + 1, "preset channel is one undo step");
                t.addNote (t.getCurrentPatternState(), acid, 0.0, 0.5, 36, 110);
                ok &= check (renderPeak (t, acid).first > 0.05f, "preset channel renders audio");
            }

            // Templates
            struct Expect { const char* name; double bpm; int channels; bool notes; };
            for (auto e : { Expect { "Empty", 120.0, 0, false }, Expect { "Basic beat", 120.0, 4, false }, Expect { "Trap", 140.0, 4, true },
                            Expect { "House", 124.0, 5, true }, Expect { "Lo-fi", 85.0, 4, true } })
            {
                t.newProjectFromTemplate (e.name);
                int notes = 0;
                for (auto p : t.getPatterns()) notes += p.getNumChildren();
                ok &= check (std::abs (t.getBpm() - e.bpm) < 1.0e-6 && t.getChannels().getNumChildren() == e.channels && (notes > 0) == e.notes
                                 && ! t.isDirty() && ! t.undo().canUndo(),
                             juce::String ("template '") + e.name + "': " + juce::String (t.getBpm()) + " BPM, " + juce::String (t.getChannels().getNumChildren())
                                 + " channels, " + juce::String (notes) + " notes, clean");
            }
            ok &= check (t.getSwing() > 0.0, "Lo-fi template swings");
            {
                Exporter::Settings es;
                es.song = false;
                es.tailSeconds = 0.3;
                t.newProjectFromTemplate ("Trap");
                const auto wav = tmp.getChildFile ("trap.wav");
                float sub = 0.0f;
                for (auto ch : t.getChannels())
                    if (ch[IDs::name].toString() == "808 Sub")
                        sub = renderPeak (t, ch[IDs::id]).first;
                ok &= check (sub > 0.05f && Exporter::exportMix (t, es, wav), "Trap template renders (808 sub synth sounds)");
            }

            // Sound library (generated into a temp folder so we can time it).
            const auto packs = tmp.getChildFile ("Packs");
            const auto start = juce::Time::getMillisecondCounterHiRes();
            DrumKit::ensureLibrary (packs);
            const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
            int valid = 0;
            for (auto& e : DrumKit::Library::entries())
                if (auto reader = std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (packs.getChildFile (e.path))))
                {
                    juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
                    reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
                    valid += buffer.getNumSamples() > 1000 && buffer.getMagnitude (0, buffer.getNumSamples()) > 0.05f ? 1 : 0;
                }
            ok &= check (valid == (int) DrumKit::Library::entries().size(),
                         "sound library: " + juce::String (valid) + " valid files generated in " + juce::String ((int) ms) + " ms");
            const auto modified = packs.getChildFile ("FX/Impact.wav").getLastModificationTime();
            DrumKit::ensureLibrary (packs);
            ok &= check (packs.getChildFile ("FX/Impact.wav").getLastModificationTime() == modified, "sound library doesn't regenerate existing files");
            bool installed = true;
            for (auto& e : DrumKit::Library::entries())
                installed &= DrumKit::getPacksFolder().getChildFile (e.path).existsAsFile();
            ok &= check (installed && DrumKit::getPacksFolder().getChildFile ("Loops/Drum Loop 120 BPM.wav").existsAsFile(), "sound library installed in Packs");
            tmp.deleteRecursively();

            // FM channel for the save/load checks below.
            fmChannel = s.addPresetChannel ("FM Bell");
        }

        {
            // Phase 14f: audio editor, stretch + pitch, time signature, tempo changes, arpeggiator, browser search.
            using AudioEdit::Buffer;
            const auto tmp = juce::File::createTempFile ("pro");
            tmp.createDirectory();
            auto& formats = engine.getAudioFileFormatManager().readFormatManager;

            Buffer ramp (2, 44100);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < ramp.getNumSamples(); ++i)
                    ramp.setSample (ch, i, 0.5f * (float) i / (float) ramp.getNumSamples());
            const AudioEdit::Range all (0, ramp.getNumSamples());

            auto rev = ramp;
            AudioEdit::reverse (rev, all);
            ok &= check (rev.getSample (0, 0) > 0.49f && rev.getSample (1, rev.getNumSamples() - 1) < 1.0e-4f, "audio editor: reverse");
            auto norm = ramp;
            AudioEdit::normalise (norm, all);
            ok &= check (std::abs (juce::Decibels::gainToDecibels (norm.getMagnitude (0, norm.getNumSamples())) + 0.3f) < 0.01f, "audio editor: normalize peaks at -0.3 dB");
            Buffer ones (2, 1000);
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill (ones.getWritePointer (ch), 1.0f, 1000);
            AudioEdit::fadeIn (ones, { 0, 1000 });
            ok &= check (std::abs (ones.getSample (0, 0)) < 1.0e-6f && ones.getSample (1, 999) > 0.99f, "audio editor: fade in starts at 0");
            auto cut = ramp;
            const auto clipboard = AudioEdit::copy (cut, { 1000, 11000 });
            AudioEdit::remove (cut, { 1000, 11000 });
            ok &= check (cut.getNumSamples() == 34100 && juce::approximatelyEqual (cut.getSample (0, 1000), ramp.getSample (0, 11000)), "audio editor: cut shortens the sound");
            AudioEdit::insert (cut, 1000, clipboard);
            ok &= check (cut.getNumSamples() == 44100 && juce::approximatelyEqual (cut.getSample (1, 5000), ramp.getSample (1, 5000)), "audio editor: paste puts it back");
            auto trimmed = ramp;
            AudioEdit::trim (trimmed, { 100, 600 });
            ok &= check (trimmed.getNumSamples() == 500 && juce::approximatelyEqual (trimmed.getSample (0, 0), ramp.getSample (0, 100)), "audio editor: trim to selection");
            auto loud = ramp;
            AudioEdit::gain (loud, all, 6.0206f);
            ok &= check (std::abs (loud.getMagnitude (0, loud.getNumSamples()) - 2.0f * ramp.getMagnitude (0, ramp.getNumSamples())) < 1.0e-3f, "audio editor: gain");

            Studio st (engine);
            st.newProjectFromTemplate ("Empty");
            st.setSongMode (true);
            const auto loop = DrumKit::getPacksFolder().getChildFile ("Loops/Drum Loop 120 BPM.wav");
            st.undo().beginNewTransaction();
            auto loopClip = st.addAudioClip (loop, 0, 0.0);
            const int loopChannel = loopClip[IDs::channel];
            auto waveClip = [&st] (int channel) -> te::WaveAudioClip*
            {
                st.regenerateNow();
                for (auto c : st.getTrack (channel)->getClips())
                    if (auto w = dynamic_cast<te::WaveAudioClip*> (c))
                        return w;
                return nullptr;
            };

            Buffer loopAudio;
            double loopRate = 0.0;
            ok &= check (AudioEdit::read (formats, loop, loopAudio, loopRate), "audio editor: reads the loop");
            AudioEdit::reverse (loopAudio, { 0, loopAudio.getNumSamples() });
            const auto edited = AudioEdit::nextEditFile (loop, tmp);
            ok &= check (AudioEdit::writeWav (loopAudio, loopRate, edited) && edited.getFileName() == "Drum Loop 120 BPM (edit 1).wav"
                             && AudioEdit::nextEditFile (edited, tmp).getFileName() == "Drum Loop 120 BPM (edit 2).wav" && loop.existsAsFile(),
                         "audio editor: Apply writes a new numbered file, original kept");
            st.undo().beginNewTransaction();
            st.replaceAudioFile (loopChannel, edited);
            ok &= check (waveClip (loopChannel) != nullptr && waveClip (loopChannel)->getOriginalFile() == edited, "audio editor: Apply repoints the playlist clip");
            st.undo().undo();
            ok &= check (waveClip (loopChannel)->getOriginalFile() == loop, "audio editor: undo goes back to the original file");

            // Stretch + pitch: a 120 BPM loop (4 s = 8 beats) at 140 BPM.
            ok &= check (juce::approximatelyEqual (Studio::bpmFromFileName ("Drum Loop 120 BPM"), 120.0) && juce::approximatelyEqual (Studio::bpmFromFileName ("loop_92bpm"), 92.0)
                             && juce::approximatelyEqual (Studio::bpmFromFileName ("Kick"), 0.0), "BPM read from file names");
            st.setBpm (140.0);
            st.setClipPitch (loopClip, 3.0);
            if (auto w = waveClip (loopChannel))
            {
                ok &= check ((bool) loopClip[IDs::stretch] && std::abs ((double) loopClip[IDs::length] - 8.0) < 1.0e-3, "dropped loop stretches to tempo, 8 beats long");
                ok &= check (w->getAutoTempo() && std::abs (w->getLoopInfo().getBpm (w->getWaveInfo()) - 120.0) < 0.01
                                 && std::abs (w->getPosition().getLength().inSeconds() - 8.0 * 60.0 / 140.0) < 1.0e-3,
                             "engine clip follows tempo (auto-tempo, 120 BPM source, " + juce::String (w->getPosition().getLength().inSeconds(), 3) + " s at 140)");
                ok &= check (std::abs (w->getPitchChange() - 3.0f) < 1.0e-4f && w->getTimeStretchMode() == te::TimeStretcher::soundtouchBetter, "clip pitch +3 st applied");
            }
            {
                Exporter::Settings es;
                es.tailSeconds = 0.0;
                const auto wav = tmp.getChildFile ("stretch.wav");
                Exporter::exportMix (st, es, wav);
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (wav));
                juce::AudioBuffer<float> mix (2, reader != nullptr ? (int) reader->lengthInSamples : 0);
                if (reader != nullptr)
                    reader->read (&mix, 0, mix.getNumSamples(), 0, true, true);
                ok &= check (mix.getNumSamples() > 0 && mix.getMagnitude (0, mix.getNumSamples()) > 0.05f
                                 && std::abs (mix.getNumSamples() / reader->sampleRate - 8.0 * 60.0 / 140.0) < 0.05,
                             "stretched + pitched loop renders offline at the project tempo");
            }
            st.undo().beginNewTransaction();
            st.setClipStretch (loopClip, false);
            ok &= check (! waveClip (loopChannel)->getAutoTempo() && std::abs ((double) loopClip[IDs::length] - 8.0 * 140.0 / 120.0) < 1.0e-3,
                         "stretch off: plays at its own speed, clip length follows");
            st.undo().undo();

            // Time signature
            const int synth = st.addSynthChannel();
            st.setTimeSigNumerator (3);
            auto pattern = st.getCurrentPatternState();
            st.addNote (pattern, synth, 3.5, 0.5, 60, 100);
            ok &= check (juce::approximatelyEqual (st.getBarBeats(), 3.0) && std::abs (st.getPatternLengthBeats (pattern) - 6.0) < 1.0e-6, "3/4: 3-beat bars, pattern rounds to 6 beats");
            st.undo().undo();
            ok &= check (juce::approximatelyEqual (st.getBarBeats(), 4.0), "undo restores 4/4");
            pattern.removeAllChildren (nullptr);

            // Tempo change at bar 5: 8 bars of pattern at 120, then 60 from beat 16.
            st.setBpm (120.0);
            st.getPlaylist().removeAllChildren (nullptr);
            st.addNote (pattern, synth, 0.0, 1.0, 60, 100);
            for (int bar = 0; bar < 8; ++bar)
                st.addPatternClip (pattern[IDs::id], 1, bar * 4.0);
            auto hit = st.addAudioClip (DrumKit::getDrumsFolder().getChildFile ("Kick.wav"), 2, 20.0);
            auto mixLength = [&]
            {
                Exporter::Settings es;
                es.tailSeconds = 0.0;
                const auto wav = tmp.getChildFile ("tempo.wav");
                Exporter::exportMix (st, es, wav);
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (wav));
                return reader != nullptr ? (double) reader->lengthInSamples / reader->sampleRate : -1.0;
            };
            const double before = mixLength();
            const double hitBefore = waveClip (hit[IDs::channel])->getPosition().getStart().inSeconds();
            st.addTempoChange (16.0, 60.0);
            const double after = mixLength();
            const double hitAfter = waveClip (hit[IDs::channel])->getPosition().getStart().inSeconds();
            ok &= check (std::abs (before - 16.0) < 0.05 && std::abs (after - 24.0) < 0.05,
                         "tempo change at bar 5 (120 -> 60): mixdown " + juce::String (before, 2) + " s -> " + juce::String (after, 2) + " s");
            ok &= check (juce::approximatelyEqual ((double) hit[IDs::start], 20.0) && std::abs (hitBefore - 10.0) < 1.0e-3 && std::abs (hitAfter - 12.0) < 1.0e-3,
                         "clip stays at beat 20, its time moves 10 s -> 12 s");
            st.undo().beginNewTransaction();
            st.removeTempoChange (st.getTempoChanges().getChild (0));
            st.regenerateNow();
            ok &= check (st.getEdit().tempoSequence.getNumTempos() == 1 && std::abs (waveClip (hit[IDs::channel])->getPosition().getStart().inSeconds() - 10.0) < 1.0e-3,
                         "removing the tempo change restores timing");

            // Arpeggiator: a held 1-bar C-E-G chord.
            st.setSongMode (false);
            st.getPlaylist().removeAllChildren (nullptr);
            pattern.removeAllChildren (nullptr);
            for (int key : { 60, 64, 67 })
                st.addNote (pattern, synth, 0.0, 4.0, key, 100);
            auto channel = st.getChannel (synth);
            auto arpNotes = [&]
            {
                st.regenerateNow();
                juce::Array<int> keys;
                for (auto c : st.getTrack (synth)->getClips())
                    if (auto mc = dynamic_cast<te::MidiClip*> (c))
                        for (auto n : mc->getSequence().getNotes())
                            keys.add (n->getNoteNumber());
                return keys;
            };
            ok &= check (arpNotes().size() == 3, "arp off: the chord plays as 3 notes");
            channel.setProperty (IDs::arp, Arp::up, nullptr);
            channel.setProperty (IDs::arpOctaves, 2, nullptr);
            auto up = arpNotes();
            ok &= check (up.size() == 16 && up[0] == 60 && up[2] == 67 && up[3] == 72 && up[5] == 79 && up[6] == 60, "arp up, 1/16, 2 octaves: 16 notes 60 64 67 72 76 79 60...");
            channel.setProperty (IDs::arp, Arp::upDown, nullptr);
            channel.setProperty (IDs::arpOctaves, 1, nullptr);
            channel.setProperty (IDs::arpRate, 2.0, nullptr);
            auto upDown = arpNotes();
            ok &= check (upDown.size() == 8 && upDown[2] == 67 && upDown[3] == 64 && upDown[4] == 60, "arp up-down, 1/8: 8 notes 60 64 67 64 60...");
            channel.setProperty (IDs::arp, Arp::down, nullptr);
            ok &= check (arpNotes()[0] == 67, "arp down starts on the top note");

            // Browser search (the filtering, not the UI).
            const auto sounds = tmp.getChildFile ("sounds");
            for (auto name : { "Kicks/Big Kick.wav", "Snare Tight.wav", "Kick notes.txt" })
            {
                sounds.getChildFile (name).getParentDirectory().createDirectory();
                sounds.getChildFile (name).replaceWithText ("x");
            }
            const auto found = Browser::searchFiles ({ sounds }, "kick", "*.wav;*.aif");
            ok &= check (found.size() == 1 && found[0].getFileName() == "Big Kick.wav", "browser search finds sounds in subfolders (not other files)");
            ok &= check (Browser::matchesSearch ("Drum Loop 120 BPM", "loop 120") && ! Browser::matchesSearch ("Drum Loop 120 BPM", "loop 90"), "browser search matches every word");
            int bassPresets = 0;
            bool acid = false, eq = false;
            for (auto& p : Browser::allPresets())
                if (Browser::matchesSearch (p.name, "bass"))
                {
                    ++bassPresets;
                    acid |= p.name == "Acid Bass" && p.pluginType == te::FourOscPlugin::xmlTypeName && p.factory != nullptr;
                    eq |= p.name == "Bass boost" && p.pluginType == te::EqualiserPlugin::xmlTypeName;
                }
            ok &= check (acid && eq, "browser presets: 'bass' finds synth and effect presets (" + juce::String (bassPresets) + ")");
            tmp.deleteRecursively();
        }

        const auto file = juce::File::createTempFile (Studio::fileExtension);
        s.setSongMode (false);
        const int savedChannels = s.getChannels().getNumChildren();
        const int savedNotes = countNotes (s, kick);
        ok &= check (s.isDirty(), "edits mark project dirty");
        ok &= check (s.saveTo (file), "save project");
        ok &= check (! s.isDirty(), "clean after save");
        {
            Studio loaded (engine);
            ok &= check (loaded.load (file), "load project");
            ok &= check (loaded.getChannels().getNumChildren() == savedChannels, "channels survive save/load");
            ok &= check (countNotes (loaded, kick) == savedNotes, "notes survive save/load");
            ok &= check (loaded.getInsertEffects (3).size() == 1 && loaded.getChannelInsert (kick) == 3, "mixer survives save/load");
            for (int id : { sfzChannel, padChannel, sliceChannel, sf2Channel })
                if (id != 0)
                    ok &= check (loaded.getMultiSampler (id) != nullptr && s.getMultiSampler (id) != nullptr
                                 && loaded.getMultiSampler (id)->getInfo().numRegions == s.getMultiSampler (id)->getInfo().numRegions
                                 && loaded.getMultiSampler (id)->getProgram() == s.getMultiSampler (id)->getProgram(),
                                 "instrument channel survives save/load (" + loaded.getChannel (id)[IDs::name].toString() + ")");
            ok &= check (dynamic_cast<FMSynthPlugin*> (loaded.getInstrument (fmChannel)) != nullptr, "FM Synth plugin restored from the project");
            if (auto* a = dynamic_cast<FMSynthPlugin*> (s.getInstrument (fmChannel)))
                if (auto* b = dynamic_cast<FMSynthPlugin*> (loaded.getInstrument (fmChannel)))
                {
                    bool same = true;
                    for (int i = 0; i < FM::numParams; ++i)
                        same &= std::abs (a->getParam (i)->getCurrentValue() - b->getParam (i)->getCurrentValue()) < 1.0e-5f;
                    ok &= check (same && std::abs (b->getParam (FM::numOpParams + FM::ratio)->getCurrentValue() - 3.5f) < 1.0e-5f
                                     && loaded.getChannel (fmChannel)[IDs::type].toString() == ChannelType::fm,
                                 "FM Synth channel survives save/load with its parameters");
                }
            loaded.setStep (kick, 2, true);
            ok &= check (countNotes (loaded, kick) == savedNotes + 1, "loaded project still drives the engine");
        }
        {
            // Fuzz: broken, foreign, old and hostile project files load gracefully or not at all.
            const auto dir = juce::File::createTempFile ("fuzz");
            dir.createDirectory();
            const auto valid = file.loadFileAsString();
            Studio f (engine);
            double slowest = 0.0;
            auto tryLoad = [&] (const juce::String& text)
            {
                const auto x = dir.getChildFile ("fuzz" + Studio::fileExtension);
                x.replaceWithText (text);
                const auto t = juce::Time::getMillisecondCounterHiRes();
                const bool loaded = f.load (x);
                if (loaded)
                    f.regenerateNow();
                slowest = juce::jmax (slowest, juce::Time::getMillisecondCounterHiRes() - t);
                return loaded;
            };
            auto withStudio = [&] (std::function<void (juce::XmlElement&)> change)
            {
                auto xml = juce::parseXML (valid);
                change (*xml->getChildByName ("STUDIO"));
                return xml->toString();
            };

            juce::Random rng (1234);
            juce::MemoryBlock garbage (4000);
            rng.fillBitsRandomly (garbage.getData(), garbage.getSize());
            const bool rejected = ! tryLoad ("") && ! tryLoad (valid.substring (0, valid.length() / 2)) && ! tryLoad (garbage.toString())
                                  && ! tryLoad ("<FOO><STUDIO/></FOO>") && ! tryLoad (valid.replace ("<STUDIO", "<NOTSTUDIO").replace ("</STUDIO>", "</NOTSTUDIO>"))
                                  && ! f.load (dir.getChildFile ("missing" + Studio::fileExtension));
            ok &= check (rejected && f.getChannels().getNumChildren() == 4,
                         "fuzz: empty, truncated, garbage, foreign XML, no STUDIO, missing file -> rejected, open project kept");

            const bool old = tryLoad (withStudio ([] (juce::XmlElement& st)
            {
                for (auto name : { "INSERTS", "MAPPINGS", "TEMPOS" })
                    st.removeChildElement (st.getChildByName (name), true);
            }));
            ok &= check (old && f.getChannels().getNumChildren() == savedChannels && f.getInserts().getNumChildren() == Studio::numInserts,
                         "fuzz: older project shape (no INSERTS/MAPPINGS/TEMPOS) loads");
            ok &= check (tryLoad ("<EDIT><STUDIO currentPattern=\"7\"><CHANNELS><CHANNEL id=\"3\" track=\"999\"/></CHANNELS></STUDIO></EDIT>")
                             && f.getCurrentPatternState().isValid() && f.getInserts().getNumChildren() == Studio::numInserts,
                         "fuzz: minimal hand-written project loads (pattern + inserts created)");

            const bool huge = tryLoad (withStudio ([] (juce::XmlElement& st)
            {
                st.setAttribute ("swing", "inf");
                st.setAttribute ("nextId", -5);
                st.setAttribute ("currentPattern", 123456);
                for (auto* ch : st.getChildByName ("CHANNELS")->getChildIterator())
                {
                    ch->setAttribute ("arp", 1);
                    ch->setAttribute ("arpRate", "1e-300");
                    ch->setAttribute ("insert", 999);
                }
                for (auto* p : st.getChildByName ("PATTERNS")->getChildIterator())
                    for (auto* n : p->getChildIterator())
                    {
                        n->setAttribute ("start", "1e308");
                        n->setAttribute ("length", "nan");
                        n->setAttribute ("key", 99999);
                        n->setAttribute ("velocity", -3);
                    }
                auto* tempo = st.getChildByName ("TEMPOS")->createNewChildElement ("TEMPOCHANGE");
                tempo->setAttribute ("pos", "1e300");
                tempo->setAttribute ("bpm", 0);
            }).replace ("numerator=\"4\"", "numerator=\"0\""));
            bool clamped = huge;
            for (auto p : f.getPatterns())
                for (auto n : p)
                    clamped &= (double) n[IDs::start] <= 4.0 * 9999.0 && (int) n[IDs::key] <= 127 && (int) n[IDs::velocity] >= 1 && std::isfinite ((double) n[IDs::length]);
            ok &= check (clamped && f.getCurrentPatternState().isValid() && f.getSwing() <= 1.0 && slowest < 5000.0,
                         "fuzz: huge / NaN / negative numbers are clamped (slowest load " + juce::String ((int) slowest) + " ms)");

            const juce::String ghost = "/nonexistent/untitledstudio/Ghost Kick.wav";
            auto withGhosts = juce::parseXML (valid);
            withGhosts->getChildByName ("STUDIO")->getChildByName ("CHANNELS")->getChildElement (0)->setAttribute ("file", ghost);
            auto* plugin = withGhosts->getChildByName ("TRACK")->createNewChildElement ("PLUGIN");
            plugin->setAttribute ("type", te::ExternalPlugin::xmlTypeName);
            plugin->setAttribute ("name", "Ghost Synth");
            plugin->setAttribute ("filename", "/nonexistent/Ghost.vst3");
            const bool missing = tryLoad (withGhosts->toString());
            juce::String plugins;
            for (auto& m : f.getMissingFiles())
                if (m.startsWith ("plugin:")) plugins << m;
            ok &= check (missing && f.getMissingFiles().contains (ghost) && plugins.isNotEmpty(),
                         "fuzz: missing samples + plugins load and are reported (" + juce::String (f.getMissingFiles().size()) + " missing)");

            // Random mutations: corrupt bytes, and values / elements inside the model.
            const juce::StringArray nasty { "", "-1", "0", "1e308", "-1e308", "nan", "inf", "abc", "2147483647", "-99999", "0.000001" };
            int loads = 0, total = 0;
            for (int i = 0; i < 8; ++i, ++total)
            {
                auto bytes = valid.toStdString();
                for (int k = 0; k < 20; ++k)
                    bytes[(size_t) rng.nextInt ((int) bytes.size())] = (char) (32 + rng.nextInt (95));
                loads += tryLoad (bytes) ? 1 : 0;
            }
            for (int i = 0; i < 16; ++i, ++total)
            {
                loads += tryLoad (withStudio ([&] (juce::XmlElement& st)
                {
                    juce::Array<juce::XmlElement*> all;
                    std::function<void (juce::XmlElement&)> collect = [&] (juce::XmlElement& e)
                    {
                        all.add (&e);
                        for (auto* c : e.getChildIterator()) collect (*c);
                    };
                    collect (st);
                    for (int k = 0; k < 6; ++k)
                    {
                        auto* e = all[rng.nextInt (all.size())];
                        if (e->getNumAttributes() > 0)
                            e->setAttribute (e->getAttributeName (rng.nextInt (e->getNumAttributes())), nasty[rng.nextInt (nasty.size())]);
                    }
                    if (auto* victim = all[rng.nextInt (all.size())]; victim != &st && rng.nextBool())
                        for (auto* parent : all)
                            if (parent->containsChildElement (victim))
                            {
                                parent->removeChildElement (victim, true);
                                break;
                            }
                })) ? 1 : 0;
            }
            ok &= check (slowest < 5000.0, "fuzz: " + juce::String (total) + " random mutations, no crash or hang (" + juce::String (loads)
                                               + " loaded, the rest rejected; slowest " + juce::String ((int) slowest) + " ms)");
            dir.deleteRecursively();
        }

        file.deleteFile();
        instrDir.deleteRecursively();

        std::cout << (ok ? "PASS" : "FAILED") << std::endl;
        return ok;
    }

    //==========================================================================
    // `untitledstudio --stress`: 100 channels, 50 effects, 16 bars. Prints a load report.
    inline void stress (te::Engine& engine)
    {
        using Clock = std::chrono::steady_clock;
        auto msSince = [] (Clock::time_point t) { return std::chrono::duration<double, std::milli> (Clock::now() - t).count(); };

        Studio s (engine);
        s.newProjectFromTemplate ("Empty");
        auto start = Clock::now();

        const auto drums = DrumKit::getDrumsFolder();
        const juce::StringArray samples { "Kick", "Clap", "Hat", "Snare", "Open Hat", "808", "Cowbell", "Crash" };
        juce::StringArray oscSounds, fmSounds;
        for (auto& sound : Presets::allSynthSounds())
            (sound.pluginType == FMSynthPlugin::xmlTypeName ? fmSounds : oscSounds).add (sound.preset->name);

        auto pattern = s.getCurrentPatternState();
        for (int i = 0; i < 100; ++i)
        {
            const int kind = i % 4;
            const int id = kind == 0 ? s.addSamplerChannel (drums.getChildFile (samples[i / 4 % samples.size()] + ".wav"))
                         : kind == 1 ? s.addPresetChannel (oscSounds[i / 4 % oscSounds.size()])
                         : kind == 2 ? s.addPresetChannel (fmSounds[i / 4 % fmSounds.size()])
                                     : s.addInstrumentChannel (drums.getChildFile (samples[(i / 4 + 3) % samples.size()] + ".wav"));
            s.setChannelInsert (id, 1 + i % Studio::numInserts);
            if (kind == 0 || kind == 3)
                for (int beat = 0; beat < 16; ++beat)
                    s.addNote (pattern, id, beat + (i % 3) * Studio::stepBeats, Studio::stepBeats, 60, 100);
            else
                for (int n = 0; n < 8; ++n)
                    s.addNote (pattern, id, n * 2.0, 1.5, 48 + (i * 5 + n * 3) % 24, 90);
        }

        const char* fxTypes[] = { te::EqualiserPlugin::xmlTypeName, te::CompressorPlugin::xmlTypeName, te::ReverbPlugin::xmlTypeName,
                                  te::DelayPlugin::xmlTypeName, te::AirWindowsButterComp2::xmlTypeName };
        int effects = 0;
        for (int i = 0; i < 50; ++i)
            if (auto p = s.addEffect (1 + i % Studio::numInserts, fxTypes[i % 5]))
            {
                Effects::prepareNew (*p);
                ++effects;
            }

        for (int bar = 0; bar < 16; bar += 4)
            s.addPatternClip (pattern[IDs::id], 0, bar * 4.0);
        s.setSongMode (true);
        const double buildMs = msSince (start);

        int notes = 0;
        for (auto n : s.getCurrentPatternState()) notes += n.hasType (IDs::NOTE) ? 1 : 0;

        start = Clock::now();
        s.regenerateNow();
        const double regenMs = msSince (start);
        start = Clock::now();
        s.getCurrentPatternState().getChild (0).setProperty (IDs::velocity, 99, nullptr); // one-note edit
        s.regenerateNow();
        const double editRegenMs = msSince (start);

        std::cout << "Stress project: " << s.getChannels().getNumChildren() << " channels, " << effects << " effects, "
                  << notes << " notes per pattern, " << s.getSongLengthBeats() / 4.0 << " bars at " << s.getBpm() << " BPM" << std::endl;
        std::cout << "  build " << juce::roundToInt (buildMs) << " ms, full regen " << juce::String (regenMs, 1)
                  << " ms, regen after a one-note edit " << juce::String (editRegenMs, 1) << " ms" << std::endl;

        // (a) Offline render through the Exporter.
        const double songSeconds = s.getSongLengthBeats() * 60.0 / s.getBpm();
        const auto wav = juce::File::createTempFile (".wav");
        Exporter::Settings es;
        es.tailSeconds = 0.0;
        start = Clock::now();
        const bool rendered = Exporter::exportMix (s, es, wav);
        const double renderMs = msSince (start);
        wav.deleteFile();
        std::cout << "  offline render (512 blocks): " << (rendered ? "" : "FAILED ") << juce::String (songSeconds, 1) << " s of audio in "
                  << juce::String (renderMs / 1000.0, 2) << " s = " << juce::String (songSeconds * 1000.0 / renderMs, 2) << "x realtime" << std::endl;

        // (b) The live playback graph, driven block by block like an audio device (256 @ 44.1 kHz).
        constexpr int blockSize = 256;
        constexpr double sampleRate = 44100.0;
        auto& hosted = engine.getDeviceManager().getHostedAudioDeviceInterface();
        te::HostedAudioDeviceInterface::Parameters hp;
        hp.sampleRate = sampleRate;
        hp.blockSize = blockSize;
        hp.inputChannels = 0;
        engine.getDeviceManager().setCpuLimitBeforeMuting (1000.0); // blocks are pumped back to back, don't let it mute
        hosted.initialise (hp);
        hosted.prepareToPlay (sampleRate, blockSize);
        engine.getDeviceManager().dispatchPendingUpdates();
        s.transport().freePlaybackContext();
        s.transport().ensureContextAllocated();
        s.regenerateNow();
        s.transport().setPosition (0s);
        s.transport().play (false);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        for (int i = 0; i < 200; ++i) // warm up: graph build, sample loading
            hosted.processBlock (buffer, midi);

        const double budgetMs = blockSize * 1000.0 / sampleRate;
        const int numBlocks = (int) (songSeconds * sampleRate / blockSize);
        std::vector<double> times;
        times.reserve ((size_t) numBlocks);
        float peak = 0.0f;
        for (int i = 0; i < numBlocks; ++i)
        {
            buffer.clear();
            start = Clock::now();
            hosted.processBlock (buffer, midi);
            times.push_back (msSince (start));
            peak = juce::jmax (peak, buffer.getMagnitude (0, blockSize));
        }
        s.transport().stop (false, false);

        auto sorted = times;
        std::sort (sorted.begin(), sorted.end());
        const double avg = std::accumulate (times.begin(), times.end(), 0.0) / (double) times.size();
        const auto pct = [budgetMs] (double ms) { return juce::String (ms / budgetMs * 100.0, 1) + "%"; };
        const auto over = std::count_if (times.begin(), times.end(), [budgetMs] (double t) { return t > budgetMs; });
        std::cout << "  realtime graph, " << blockSize << " samples @ 44.1 kHz (budget " << juce::String (budgetMs, 2) << " ms, "
                  << engine.getEngineBehaviour().getNumberOfCPUsToUseForAudio() << " audio threads), " << numBlocks << " blocks, peak "
                  << juce::String (peak, 2) << ":" << std::endl;
        std::cout << "    average " << juce::String (avg, 3) << " ms (" << pct (avg) << "), median " << pct (sorted[sorted.size() / 2])
                  << ", 99th percentile " << pct (sorted[sorted.size() * 99 / 100]) << ", worst " << juce::String (sorted.back(), 3)
                  << " ms (" << pct (sorted.back()) << ")" << std::endl;
        std::cout << "    blocks over budget (would drop out): " << over << std::endl;
    }
}

class UntitledStudioApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    void initialise (const juce::String& commandLine) override
    {
        // When launched as the plugin-scanner child process, just scan and exit.
        if (te::PluginManager::startChildProcessPluginScan (commandLine))
            return;

        engine = std::make_unique<te::Engine> ("untitledstudio", std::make_unique<StudioUIBehaviour>(),
                                               std::make_unique<StudioEngineBehaviour>());
        Effects::registerAll (*engine);

        // Dev aid: list every effect's parameters (used to write factory presets).
        if (commandLine.contains ("--dump-params"))
        {
            auto edit = te::Edit::createSingleTrackEdit (*engine);
            auto all = Effects::builtIn();
            all.push_back ({ "4-OSC Synth", te::FourOscPlugin::xmlTypeName, "Synth", {} });
            all.push_back ({ "FM Synth", FMSynthPlugin::xmlTypeName, "Synth", {} });
            for (auto& e : all)
                if (auto p = edit->getPluginCache().createNewPlugin (e.xmlType, {}))
                {
                    std::cout << e.name << " [" << e.xmlType << "]" << std::endl;
                    for (auto* param : p->getAutomatableParameters())
                        std::cout << "    " << param->paramID << " | " << param->getParameterName() << " | " << param->valueRange.start
                                  << ".." << param->valueRange.end << " = " << param->getCurrentValue() << std::endl;
                }
            edit = nullptr;
            quit();
            return;
        }

        if (commandLine.contains ("--selftest"))
        {
            const bool ok = SelfTest::run (*engine);
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--stress"))
        {
            SelfTest::stress (*engine);
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

            if (commandLine.contains ("--open-drumpad"))
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->debugOpenDrumPad();

            if (commandLine.contains ("--template "))
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->debugTemplate (commandLine.fromFirstOccurrenceOf ("--template ", false, false).upToFirstOccurrenceOf (" --", false, false).unquoted());

            if (commandLine.contains ("--open-fm"))
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->debugOpenFm();

            if (commandLine.contains ("--open-synth"))
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->debugOpenSynth();

            if (commandLine.contains ("--open-instrument"))
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->debugOpenInstrument();

            if (commandLine.contains ("--open-audio-editor"))
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->debugOpenAudioEditor();

            if (commandLine.contains ("--open-channel"))
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                    mc->debugOpenFirstChannel();

            // --snapshot-scale 2 renders like a Retina display.
            const float scale = commandLine.contains ("--snapshot-scale ")
                                    ? juce::jlimit (0.5f, 4.0f, commandLine.fromFirstOccurrenceOf ("--snapshot-scale ", false, false).getFloatValue()) : 1.0f;
            const bool picker = commandLine.contains ("--open-picker");
            juce::Timer::callAfterDelay (2500, [this, file, scale, picker]
            {
                if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()); mc != nullptr && picker)
                    mc->debugOpenPicker(); // opened last: a callout closes itself when the app isn't in front
                if (auto* c = mainWindow->getContentComponent())
                {
                    juce::PNGImageFormat png;
                    file.deleteFile();
                    if (auto out = file.createOutputStream())
                        png.writeImageToStream (c->createComponentSnapshot (c->getLocalBounds(), true, scale), *out);
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
