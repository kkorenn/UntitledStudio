#pragma once

#include "BinaryData.h"
#include <deque>
#include <mutex>

// Release plumbing: version, icon, splash, about box, local crash reports, update check.
namespace Release
{
    // Single source of truth: project(VERSION) in CMakeLists.txt.
    inline juce::String version()   { return JUCE_APPLICATION_VERSION_STRING; }
    inline juce::String buildDate() { return __DATE__; }
    inline const char* sourceUrl = "https://github.com/kkorenn/UntitledStudio";

    inline juce::Image icon()
    {
        return juce::ImageCache::getFromMemory (BinaryData::icon_1024_png, BinaryData::icon_1024_pngSize);
    }

    inline juce::File appDataFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Untitled Studio");
    }

    //==========================================================================
    // Keeps the last log lines in memory so a crash report can include them.
    class LogRing : public juce::Logger
    {
    public:
        void logMessage (const juce::String& message) override
        {
            juce::Logger::outputDebugString (message);
            const std::scoped_lock sl (lock);
            lines.push_back (juce::Time::getCurrentTime().toString (false, true, true, true) + "  " + message);
            if (lines.size() > 200)
                lines.pop_front();
        }

        juce::String recent()
        {
            // try_lock: a crash while another thread holds the lock mustn't deadlock the handler.
            std::unique_lock sl (lock, std::try_to_lock);
            if (! sl.owns_lock())
                return "(log busy)";
            juce::StringArray out;
            for (auto& l : lines)
                out.add (l);
            return out.joinIntoString ("\n");
        }

    private:
        std::mutex lock;
        std::deque<juce::String> lines;
    };

    inline LogRing& log() { static LogRing ring; return ring; }

    //==========================================================================
    // Crash reports are written to ~/Library/Untitled Studio/CrashReports (nothing is sent anywhere).
    // Reports in the folder are "pending" until the user dismisses them; then they move to Seen/.
    namespace Crash
    {
        inline juce::File folder() { return appDataFolder().getChildFile ("CrashReports"); }

        inline juce::String makeReport (const juce::String& reason)
        {
            juce::String r;
            r << "Untitled Studio crash report\n"
              << "Time:     " << juce::Time::getCurrentTime().toISO8601 (true) << "\n"
              << "Version:  " << version() << " (built " << buildDate() << ")\n"
              << "OS:       " << juce::SystemStats::getOperatingSystemName() << " / " << juce::SystemStats::getCpuModel() << "\n"
              << "Reason:   " << reason << "\n\n"
              << "Backtrace:\n" << juce::SystemStats::getStackBacktrace() << "\n"
              << "Recent log:\n" << log().recent() << "\n";
            return r;
        }

        inline juce::File write (const juce::File& dir, const juce::String& text)
        {
            dir.createDirectory();
            auto file = dir.getNonexistentChildFile ("crash-" + juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S"), ".txt", false);
            file.replaceWithText (text);
            return file;
        }

        inline juce::Array<juce::File> pending (const juce::File& dir)
        {
            auto files = dir.findChildFiles (juce::File::findFiles, false, "crash-*.txt");
            files.sort();
            return files;
        }

        inline void markSeen (const juce::File& dir)
        {
            const auto seen = dir.getChildFile ("Seen");
            seen.createDirectory();
            for (auto& f : pending (dir))
                f.moveFileTo (seen.getChildFile (f.getFileName()));
        }

        // HOOK: uploading reports (with the user's consent) would go here once a crash server exists.

        // ponytail: allocates inside a signal handler (as JUCE's own examples do). Good enough to get a
        // report out in practice; a pre-allocated, async-signal-safe writer if reports ever come out truncated.
        inline void handler (void* info)
        {
           #if JUCE_WINDOWS
            const juce::String reason = "unhandled exception";
            juce::ignoreUnused (info);
           #else
            const juce::String reason = "signal " + juce::String ((int) (juce::pointer_sized_int) info);
           #endif
            write (folder(), makeReport (reason));
        }
    }

    //==========================================================================
    // Update check against a small JSON feed: { "version": "x.y.z", "url": "...", "notes": "..." }.
    namespace Update
    {
        struct Info { juce::String version, url, notes; };

        inline std::optional<Info> parse (const juce::String& json)
        {
            const auto v = juce::JSON::parse (json);
            Info i { v["version"].toString().trim(), v["url"].toString().trim(), v["notes"].toString().trim() };
            const bool versionOk = i.version.trimCharactersAtStart ("vV").substring (0, 1).containsOnly ("0123456789") && i.version.isNotEmpty();
            // Only web links: the URL is opened in the browser, so nothing else from the feed is trusted.
            const bool urlOk = i.url.startsWithIgnoreCase ("https://") || i.url.startsWithIgnoreCase ("http://");
            if (! versionOk || ! urlOk)
                return {};
            return i;
        }

        // Semantic version compare: <0, 0, >0. "v" prefix and "+build" ignored; 1.0.0-beta < 1.0.0.
        inline int compare (const juce::String& a, const juce::String& b)
        {
            auto split = [] (const juce::String& s)
            {
                const auto core = s.trim().trimCharactersAtStart ("vV").upToFirstOccurrenceOf ("+", false, false);
                return std::pair { juce::StringArray::fromTokens (core.upToFirstOccurrenceOf ("-", false, false), ".", {}),
                                   core.fromFirstOccurrenceOf ("-", false, false) };
            };
            const auto [na, pa] = split (a);
            const auto [nb, pb] = split (b);
            for (int i = 0; i < juce::jmax (na.size(), nb.size()); ++i)
                if (const int x = na[i].getIntValue(), y = nb[i].getIntValue(); x != y)
                    return x < y ? -1 : 1;
            if (pa.isEmpty() != pb.isEmpty())
                return pa.isEmpty() ? 1 : -1;
            return juce::jlimit (-1, 1, pa.compareNatural (pb));
        }

        inline bool isNewer (const juce::String& remote, const juce::String& current) { return compare (remote, current) > 0; }

        // Blocking; call from a background thread. On macOS juce::URL uses NSURLSession (no curl needed).
        inline std::optional<Info> fetch (const juce::String& url)
        {
            auto stream = juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                                 .withConnectionTimeoutMs (10000));
            if (stream == nullptr)
                return {};
            return parse (stream->readEntireStreamAsString());
        }
    }

    //==========================================================================
    // Shown while the engine starts (normal launches only).
    class Splash : public juce::SplashScreen
    {
    public:
        Splash() : juce::SplashScreen ("Untitled Studio", 420, 260, true) {}

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (Theme::panelDark);
            g.fillRoundedRectangle (r, 14.0f);
            g.setColour (Theme::outline);
            g.drawRoundedRectangle (r.reduced (0.5f), 14.0f, 1.0f);
            g.drawImage (icon(), r.withSizeKeepingCentre (136, 136).withY (18), juce::RectanglePlacement::centred);
            g.setColour (Theme::text);
            g.setFont (Theme::font (26.0f, true));
            g.drawText ("Untitled Studio", r.withTop (160).withHeight (34), juce::Justification::centred);
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (14.0f));
            g.drawText ("Version " + version() + "  -  starting the audio engine...", r.withTop (198).withHeight (22), juce::Justification::centred);
        }
    };

    //==========================================================================
    class AboutBox : public juce::Component
    {
    public:
        AboutBox()
        {
            link.setButtonText (sourceUrl);
            link.setURL (juce::URL (sourceUrl));
            link.setFont (Theme::font (14.0f), false, juce::Justification::centredLeft);
            link.setColour (juce::HyperlinkButton::textColourId, Theme::accent);
            addAndMakeVisible (link);

            credits.setMultiLine (true);
            credits.setReadOnly (true);
            credits.setScrollbarsShown (true);
            credits.setFont (Theme::font (13.0f));
            credits.setColour (juce::TextEditor::backgroundColourId, Theme::lcd);
            credits.setColour (juce::TextEditor::textColourId, Theme::text);
            credits.setColour (juce::TextEditor::outlineColourId, Theme::outline);
            credits.setText (juce::String::fromUTF8 (BinaryData::THIRD_PARTY_LICENSES_md, BinaryData::THIRD_PARTY_LICENSES_mdSize), false);
            addAndMakeVisible (credits);
            setSize (680, 620);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::panelDark);
            g.drawImage (icon(), juce::Rectangle<float> (16, 16, 112, 112), juce::RectanglePlacement::centred);
            auto text = juce::Rectangle<int> (140, 22, getWidth() - 156, 24);
            g.setColour (Theme::text);
            g.setFont (Theme::font (24.0f, true));
            g.drawText ("Untitled Studio", text, juce::Justification::centredLeft);
            g.setFont (Theme::font (14.0f));
            g.setColour (Theme::textDim);
            g.drawText ("Version " + version() + "  (built " + buildDate() + ")", text.translated (0, 30).withHeight (18), juce::Justification::centredLeft);
            g.setColour (Theme::text);
            g.drawFittedText ("Free software under the GNU General Public License v3. It comes with ABSOLUTELY NO WARRANTY. "
                              "You may share and change it under the GPL; the full source code is available at:",
                              text.translated (0, 52).withHeight (36), juce::Justification::topLeft, 2);
        }

        void resized() override
        {
            link.setBounds (140, 112, getWidth() - 156, 20);
            credits.setBounds (getLocalBounds().reduced (16).withTrimmedTop (128));
        }

    private:
        juce::HyperlinkButton link;
        juce::TextEditor credits;
    };
}
