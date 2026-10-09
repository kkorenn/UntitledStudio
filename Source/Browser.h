#pragma once

#include "Studio.h"
#include "Widgets.h"

// Plays audio files for preview, alongside the engine's output.
// Streams from disk on a background thread (never reads files on the audio thread).
class SamplePreview
{
public:
    explicit SamplePreview (te::Engine& e) : engine (e)
    {
        readAhead.startThread();
        player.setSource (&transport);
        engine.getDeviceManager().deviceManager.addAudioCallback (&player);
    }

    ~SamplePreview()
    {
        engine.getDeviceManager().deviceManager.removeAudioCallback (&player);
        transport.setSource (nullptr);
        player.setSource (nullptr);
    }

    void play (const juce::File& file)
    {
        transport.stop();
        transport.setSource (nullptr);
        reader.reset();

        if (auto* r = engine.getAudioFileFormatManager().readFormatManager.createReaderFor (file))
        {
            reader = std::make_unique<juce::AudioFormatReaderSource> (r, true);
            transport.setSource (reader.get(), 32768, &readAhead, r->sampleRate);
            transport.setPosition (0.0);
            transport.start();
        }
    }

    void stop() { transport.stop(); }

private:
    te::Engine& engine;
    juce::TimeSliceThread readAhead { "Preview read-ahead" };
    std::unique_ptr<juce::AudioFormatReaderSource> reader;
    juce::AudioTransportSource transport;
    juce::AudioSourcePlayer player;
};

// FL-style browser docked on the left: folders + sounds, click to preview, drag to use.
class Browser : public juce::Component
{
public:
    static inline const juce::String dragPrefix = "file:";
    std::function<void (const juce::File&)> onOpenProject;
    std::function<void (const juce::PluginDescription&)> onPluginChosen; // double-click a plugin

    void refreshPlugins() { rebuild(); }

    Browser (te::Engine& e, juce::PropertiesFile& s) : engine (e), settings (s), preview (e)
    {
        tree.setRootItemVisible (false);
        tree.setDefaultOpenness (false);
        tree.setColour (juce::TreeView::backgroundColourId, Theme::panelDark);
        tree.setIndentSize (14);
        addAndMakeVisible (tree);

        addFolderButton.setTooltip ("Add a folder of your own sounds to the browser");
        addFolderButton.onClick = [this] { chooseFolder(); };
        addAndMakeVisible (addFolderButton);

        rebuild();
        Widgets::noFocus (*this);
    }

    ~Browser() override { tree.setRootItem (nullptr); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panelDark);
        g.setColour (Theme::panel);
        g.fillRect (getLocalBounds().removeFromTop (headerHeight));
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (12.0f, true));
        g.drawText ("BROWSER", getLocalBounds().removeFromTop (headerHeight).reduced (8, 0), juce::Justification::centredLeft);
        g.setColour (Theme::outline);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedRight (1);
        auto header = r.removeFromTop (headerHeight);
        addFolderButton.setBounds (header.removeFromRight (headerHeight).reduced (3));
        tree.setBounds (r);
    }

    void previewFile (const juce::File& f) { preview.play (f); }

private:
    struct Item : public juce::TreeViewItem
    {
        Item (Browser& b, const juce::File& f, const juce::String& label = {})
            : browser (b), file (f), name (label.isNotEmpty() ? label : f.getFileName()) {}

        bool mightContainSubItems() override { return file.isDirectory(); }
        int getItemHeight() const override   { return 22; }

        void paintItem (juce::Graphics& g, int w, int h) override
        {
            if (isSelected())
            {
                g.setColour (Theme::accent.withAlpha (0.25f));
                g.fillRect (0, 0, w, h);
            }
            g.setColour (file.isDirectory() ? Theme::text : Theme::textDim.brighter (0.3f));
            g.setFont (Theme::font (13.0f, file.isDirectory() && getParentItem() == browser.tree.getRootItem()));
            g.drawText (name, 4, 0, w - 4, h, juce::Justification::centredLeft, true);
        }

        void itemOpennessChanged (bool isNowOpen) override
        {
            if (isNowOpen && getNumSubItems() == 0)
            {
                auto children = file.findChildFiles (juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles, false);
                children.sort();
                const auto wildcard = browser.engine.getAudioFileFormatManager().readFormatManager.getWildcardForAllFormats();

                for (auto& c : children)
                    if (c.isDirectory())
                        addSubItem (new Item (browser, c));
                for (auto& c : children)
                    if (isAudioFile (c, wildcard) || c.hasFileExtension (Studio::fileExtension) || MultiSampler::isInstrumentFile (c))
                        addSubItem (new Item (browser, c));
            }
        }

        static bool isAudioFile (const juce::File& f, const juce::String& wildcard)
        {
            if (f.isDirectory())
                return false;
            for (auto& pattern : juce::StringArray::fromTokens (wildcard, ";", {}))
                if (f.getFileName().matchesWildcard (pattern.trim(), true))
                    return true;
            return false;
        }

        void itemClicked (const juce::MouseEvent& e) override
        {
            if (file.isDirectory())
                setOpen (! isOpen());
            else if (! file.hasFileExtension (Studio::fileExtension) && ! MultiSampler::isInstrumentFile (file))
                browser.preview.play (file); // FL: click a sound to hear it

            if (e.mods.isPopupMenu() && getParentItem() == browser.tree.getRootItem() && browser.isUserFolder (file))
            {
                juce::PopupMenu m;
                m.addItem ("Remove from browser", [b = &browser, f = file] { b->removeUserFolder (f); });
                m.showMenuAsync ({});
            }
        }

        void itemDoubleClicked (const juce::MouseEvent&) override
        {
            if (file.hasFileExtension (Studio::fileExtension) && browser.onOpenProject)
                browser.onOpenProject (file);
        }

        juce::var getDragSourceDescription() override
        {
            return file.isDirectory() || file.hasFileExtension (Studio::fileExtension) ? juce::var()
                                                                                        : juce::var (dragPrefix + file.getFullPathName());
        }

        juce::String getUniqueName() const override { return file.getFullPathName(); }

        Browser& browser;
        juce::File file;
        juce::String name;
    };

    struct Root : public juce::TreeViewItem
    {
        bool mightContainSubItems() override { return true; }
    };

    // "Plugin database": Favorites / Instruments / Effects. Double-click to use, drag instruments to the rack.
    struct PluginItem : public juce::TreeViewItem
    {
        PluginItem (Browser& b, juce::PluginDescription d) : browser (b), desc (std::move (d)) {}

        bool mightContainSubItems() override { return false; }
        int getItemHeight() const override   { return 22; }

        void paintItem (juce::Graphics& g, int w, int h) override
        {
            if (isSelected())
            {
                g.setColour (Theme::accent.withAlpha (0.25f));
                g.fillRect (0, 0, w, h);
            }
            g.setColour (Theme::textDim.brighter (0.3f));
            g.setFont (Theme::font (13.0f));
            g.drawText ((browser.isFavorite (desc) ? juce::String::fromUTF8 ("\xe2\x98\x85 ") : juce::String()) + desc.name,
                        4, 0, w - 4, h, juce::Justification::centredLeft, true);
        }

        void itemDoubleClicked (const juce::MouseEvent&) override { if (browser.onPluginChosen) browser.onPluginChosen (desc); }

        void itemClicked (const juce::MouseEvent& e) override
        {
            if (! e.mods.isPopupMenu())
                return;
            juce::PopupMenu m;
            const bool fav = browser.isFavorite (desc);
            m.addItem (fav ? "Remove from favorites" : "Add to favorites", [b = &browser, d = desc, fav] { b->setFavorite (d, ! fav); });
            m.showMenuAsync ({});
        }

        juce::var getDragSourceDescription() override { return "plugin:" + desc.createIdentifierString(); }
        juce::String getUniqueName() const override   { return desc.createIdentifierString(); }

        Browser& browser;
        juce::PluginDescription desc;
    };

    struct GroupItem : public juce::TreeViewItem
    {
        explicit GroupItem (juce::String n) : name (std::move (n)) {}
        bool mightContainSubItems() override { return true; }
        int getItemHeight() const override   { return 22; }
        void paintItem (juce::Graphics& g, int w, int h) override
        {
            g.setColour (Theme::text);
            g.setFont (Theme::font (13.0f, getParentItem() != nullptr && getParentItem()->getParentItem() == nullptr));
            g.drawText (name, 4, 0, w - 4, h, juce::Justification::centredLeft, true);
        }
        void itemClicked (const juce::MouseEvent&) override { setOpen (! isOpen()); }
        juce::String getUniqueName() const override { return name; }
        juce::String name;
    };

    bool isFavorite (const juce::PluginDescription& d) const
    {
        return juce::StringArray::fromLines (settings.getValue ("pluginFavorites")).contains (d.createIdentifierString());
    }

    void setFavorite (const juce::PluginDescription& d, bool fav)
    {
        auto list = juce::StringArray::fromLines (settings.getValue ("pluginFavorites"));
        if (fav) list.addIfNotAlreadyThere (d.createIdentifierString());
        else     list.removeString (d.createIdentifierString());
        list.removeEmptyStrings();
        settings.setValue ("pluginFavorites", list.joinIntoString ("\n"));
        rebuild();
    }

    juce::TreeViewItem* createPluginsGroup()
    {
        auto* group = new GroupItem ("Plugin database");
        auto* favorites = new GroupItem ("Favorites");
        auto* instruments = new GroupItem ("Instruments");
        auto* effects = new GroupItem ("Effects");
        auto types = engine.getPluginManager().knownPluginList.getTypes();
        std::sort (types.begin(), types.end(), [] (auto& a, auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
        for (auto& d : types)
        {
            (d.isInstrument ? instruments : effects)->addSubItem (new PluginItem (*this, d));
            if (isFavorite (d))
                favorites->addSubItem (new PluginItem (*this, d));
        }
        for (auto* g : { favorites, instruments, effects })
            group->addSubItem (g);
        return group;
    }



    juce::StringArray getUserFolders() const
    {
        return juce::StringArray::fromLines (settings.getValue ("browserFolders").trim());
    }

    bool isUserFolder (const juce::File& f) const { return getUserFolders().contains (f.getFullPathName()); }

    void removeUserFolder (const juce::File& f)
    {
        auto folders = getUserFolders();
        folders.removeString (f.getFullPathName());
        settings.setValue ("browserFolders", folders.joinIntoString ("\n"));
        rebuild();
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Add folder to browser",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (! fc.getResult().isDirectory())
                                      return;
                                  auto folders = getUserFolders();
                                  folders.addIfNotAlreadyThere (fc.getResult().getFullPathName());
                                  folders.removeEmptyStrings();
                                  settings.setValue ("browserFolders", folders.joinIntoString ("\n"));
                                  rebuild();
                              });
    }

    void rebuild()
    {
        tree.setRootItem (nullptr);
        root = std::make_unique<Root>();
        root->addSubItem (new Item (*this, DrumKit::getPacksFolder(), "Packs"));
        root->addSubItem (new Item (*this, Studio::getProjectsFolder(), "Projects"));
        for (auto& path : getUserFolders())
            if (juce::File (path).isDirectory())
                root->addSubItem (new Item (*this, juce::File (path)));
        root->addSubItem (createPluginsGroup());
        tree.setRootItem (root.get());
        root->setOpen (true);
        if (auto* packs = root->getSubItem (0))
            packs->setOpen (true);
    }

    static constexpr int headerHeight = 26;

    te::Engine& engine;
    juce::PropertiesFile& settings;
    SamplePreview preview;
    juce::TreeView tree;
    std::unique_ptr<Root> root;
    juce::TextButton addFolderButton { "+" };
    std::unique_ptr<juce::FileChooser> chooser;
};
