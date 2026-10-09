#pragma once

#include "PluginEditor.h"
#include "Studio.h"
#include "Analyzer.h"
#include "AirwindowsList.h"

// The built-in effects you can put in a mixer slot.
// Our own names; engines are Tracktion's built-ins, bundled Airwindows (MIT) and our Analyzer.
namespace Effects
{
    struct Entry { juce::String name, xmlType, category, hint; };

    // Call once per Engine, before any project loads.
    inline void registerAll (te::Engine& engine)
    {
        auto& pm = engine.getPluginManager();
       #define US_REGISTER_AIRWINDOWS(cls, name) pm.createBuiltInType<te::cls>();
        AIRWINDOWS_LIST (US_REGISTER_AIRWINDOWS)
       #undef US_REGISTER_AIRWINDOWS
        pm.createBuiltInType<AnalyzerPlugin>();
        pm.createBuiltInType<MultiSamplerPlugin>();
        pm.createBuiltInType<FMSynthPlugin>();
        pm.createBuiltInType<te::ImpulseResponsePlugin>();
    }

    inline const std::vector<Entry>& builtIn()
    {
        static const std::vector<Entry> list {
            { "Parametric EQ",   te::EqualiserPlugin::xmlTypeName,          "Mixing",     "4-band EQ with a draggable curve" },
            { "Compressor",      te::CompressorPlugin::xmlTypeName,         "Mixing",     "Evens out levels; supports sidechain" },
            { "Bus Glue",        te::AirWindowsButterComp2::xmlTypeName,    "Mixing",     "Gentle bus compressor" },
            { "Vari-Mu",         te::AirWindowsVariMu::xmlTypeName,         "Mixing",     "Tube-style mastering compressor" },
            { "Limiter",         te::AirWindowsADClip7::xmlTypeName,        "Mixing",     "Loudness maximizer / clipper for the master" },
            { "Gate",            te::AirWindowsSoftGate::xmlTypeName,       "Mixing",     "Silences quiet parts" },
            { "De-esser",        te::AirWindowsDeEss::xmlTypeName,          "Mixing",     "Tames harsh S sounds" },
            { "Transient Shaper",te::AirWindowsPoint::xmlTypeName,          "Mixing",     "More or less punch on hits" },
            { "Stereo Widener",  te::AirWindowsWider::xmlTypeName,          "Mixing",     "Makes the stereo image wider or narrower" },
            { "Mid/Side",        te::AirWindowsMidSide::xmlTypeName,        "Mixing",     "Balance centre vs sides" },
            { "Gain",            te::AirWindowsPurestGain::xmlTypeName,     "Mixing",     "Clean volume trim" },

            { "Reverb",          te::ReverbPlugin::xmlTypeName,             "Space",      "Room / hall reverb" },
            { "Small Rooms",     te::AirWindowsPocketVerbs::xmlTypeName,    "Space",      "Many compact reverb types" },
            { "Convolver",       te::ImpulseResponsePlugin::xmlTypeName,    "Space",      "Convolution reverb (generated room)" },
            { "Delay",           te::DelayPlugin::xmlTypeName,              "Space",      "Echo delay" },
            { "Tape Delay",      te::AirWindowsTapeDelay::xmlTypeName,      "Space",      "Warm tape echo" },
            { "Echo",            te::AirWindowsPurestEcho::xmlTypeName,     "Space",      "Multi-tap echo" },

            { "Distortion",      te::AirWindowsDrive::xmlTypeName,          "Color",      "Overdrive" },
            { "Saturation",      te::AirWindowsDensity::xmlTypeName,        "Color",      "Thickens and warms" },
            { "Bitcrusher",      te::AirWindowsDeRez2::xmlTypeName,         "Color",      "Lo-fi sample rate / bit reduction" },
            { "Tape",            te::AirWindowsToTape5::xmlTypeName,        "Color",      "Tape machine saturation" },
            { "Tube Desk",       te::AirWindowsTubeDesk::xmlTypeName,       "Color",      "Tube console colour" },
            { "Vinyl",           te::AirWindowsToVinyl4::xmlTypeName,       "Color",      "Vinyl mastering curve" },
            { "Enhancer",        te::AirWindowsEnergy::xmlTypeName,         "Color",      "Adds energy and presence" },
            { "Air",             te::AirWindowsAir::xmlTypeName,            "Color",      "Top-end sparkle" },
            { "Sub Bass",        te::AirWindowsDubSub::xmlTypeName,         "Color",      "Deepens the low end" },

            { "Chorus",          te::ChorusPlugin::xmlTypeName,             "Modulation", "Classic chorus" },
            { "Ensemble",        te::AirWindowsChorusEnsemble::xmlTypeName, "Modulation", "Rich multi-voice chorus" },
            { "Phaser",          te::PhaserPlugin::xmlTypeName,             "Modulation", "Sweeping phaser" },
            { "Tremolo",         te::AirWindowsTremolo::xmlTypeName,        "Modulation", "Volume wobble" },
            { "Vibrato",         te::AirWindowsVibrato::xmlTypeName,        "Modulation", "Pitch wobble" },

            { "Filter",          te::LowPassPlugin::xmlTypeName,            "Filter",     "Low-pass / high-pass filter" },
            { "Lowpass",         te::AirWindowsLowpass2::xmlTypeName,       "Filter",     "Smooth low-pass" },
            { "Highpass",        te::AirWindowsHighpass2::xmlTypeName,      "Filter",     "Smooth high-pass" },

            { "Pitch Shift",     te::PitchShiftPlugin::xmlTypeName,         "Pitch",      "Shift pitch in semitones" },

            { "Analyzer",        AnalyzerPlugin::xmlTypeName,               "Utility",    "Spectrum analyzer + oscilloscope" },
        };
        return list;
    }

    // Every bundled Airwindows effect, for the "More" submenu.
    inline const std::vector<Entry>& airwindows()
    {
        static const std::vector<Entry> list = []
        {
            std::vector<Entry> v;
           #define US_AIRWINDOWS_ENTRY(cls, name) v.push_back ({ name, te::cls::xmlTypeName, "Airwindows", {} });
            AIRWINDOWS_LIST (US_AIRWINDOWS_ENTRY)
           #undef US_AIRWINDOWS_ENTRY
            return v;
        }();
        return list;
    }

    // Menu of effects grouped by category; calls onPick with the xml type.
    inline void showAddMenu (juce::Component& target, std::function<void (const juce::String&)> onPick,
                             const juce::Array<juce::PluginDescription>& externalEffects = {})
    {
        juce::PopupMenu m;
        juce::StringArray order { "Mixing", "Space", "Color", "Modulation", "Filter", "Pitch", "Utility" };
        std::map<juce::String, juce::PopupMenu> categories;
        for (auto& e : builtIn())
            categories[e.category].addItem (e.name, [onPick, type = e.xmlType] { onPick (type); });
        for (auto& name : order)
            m.addSubMenu (name, categories[name]);

        juce::PopupMenu more, letterMenu;
        juce::String letter;
        for (auto& e : airwindows())
        {
            const auto first = e.name.substring (0, 1).toUpperCase();
            if (first != letter && letter.isNotEmpty())
            {
                more.addSubMenu (letter, letterMenu);
                letterMenu = {};
            }
            letter = first;
            letterMenu.addItem (e.name, [onPick, type = e.xmlType] { onPick (type); });
        }
        more.addSubMenu (letter, letterMenu);
        m.addSeparator();
        m.addSubMenu ("More (Airwindows)", more);

        juce::PopupMenu plugins;
        for (auto& d : externalEffects)
            plugins.addItem (d.name + "  (" + d.manufacturerName + ")", [onPick, id = d.createIdentifierString()] { onPick ("ext:" + id); });
        m.addSubMenu ("Plugins (VST3 / AU)", plugins, ! externalEffects.isEmpty());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
    }

    inline juce::String displayName (te::Plugin& p)
    {
        for (auto& e : builtIn())
            if (e.xmlType == p.getPluginType())
                return e.name;
        return p.getName();
    }
}

//==============================================================================
// Effect factory presets (synth presets are in SynthPresets.h).
// User presets are XML files in ~/Music/untitledstudio/Presets/<plugin type>/.
namespace Presets
{
    inline const std::vector<Preset>& factory (const juce::String& type)
    {
        if (isSynthType (type))
            return synthFactory (type);

        static const std::map<juce::String, std::vector<Preset>> all {
            { te::EqualiserPlugin::xmlTypeName, {
                { "Flat",         Values { { "Low-pass gain", 0 }, { "Mid gain 1", 0 }, { "Mid gain 2", 0 }, { "High-pass gain", 0 } } },
                { "Bass boost",   Values { { "Low-pass freq", 90 }, { "Low-pass gain", 6 } } },
                { "Bright vocal", Values { { "Low-pass freq", 120 }, { "Low-pass gain", -4 }, { "Mid freq 2", 5000 }, { "Mid gain 2", 3 }, { "High-pass gain", 4 } } },
                { "Kick punch",   Values { { "Low-pass freq", 60 }, { "Low-pass gain", 4 }, { "Mid freq 1", 300 }, { "Mid gain 1", -4 }, { "Mid freq 2", 4000 }, { "Mid gain 2", 3 } } },
                { "Telephone",    Values { { "Low-pass freq", 400 }, { "Low-pass gain", -20 }, { "Mid freq 1", 1000 }, { "Mid gain 1", 6 }, { "High-pass freq", 3000 }, { "High-pass gain", -20 } } },
            } },
            { te::CompressorPlugin::xmlTypeName, {
                { "Gentle",         Values { { "threshold", 0.5f }, { "ratio", 0.5f }, { "attack", 30 }, { "release", 150 }, { "output gain", 1 } } },
                { "Vocal",          Values { { "threshold", 0.3f }, { "ratio", 0.7f }, { "attack", 10 }, { "release", 120 }, { "output gain", 4 } } },
                { "Drum smash",     Values { { "threshold", 0.15f }, { "ratio", 0.85f }, { "attack", 1 }, { "release", 60 }, { "output gain", 8 } } },
                { "Sidechain pump", Values { { "threshold", 0.1f }, { "ratio", 0.9f }, { "attack", 0.5f }, { "release", 180 } } },
            } },
            { te::ReverbPlugin::xmlTypeName, {
                { "Small room",  Values { { "room size", 0.2f }, { "damping", 0.6f }, { "wet level", 0.2f }, { "dry level", 0.8f } } },
                { "Vocal plate", Values { { "room size", 0.5f }, { "damping", 0.7f }, { "wet level", 0.25f }, { "dry level", 0.8f } } },
                { "Hall",        Values { { "room size", 0.8f }, { "damping", 0.4f }, { "wet level", 0.35f }, { "dry level", 0.7f } } },
                { "Huge",        Values { { "room size", 0.98f }, { "damping", 0.2f }, { "wet level", 0.5f }, { "dry level", 0.6f } } },
            } },
            { te::DelayPlugin::xmlTypeName, {
                { "Slapback", Values { { "feedback", -20 }, { "mix proportion", 0.25f } } },
                { "Echo",     Values { { "feedback", -6 }, { "mix proportion", 0.35f } } },
                { "Wash",     Values { { "feedback", -2 }, { "mix proportion", 0.5f } } },
            } },
            { te::AirWindowsADClip7::xmlTypeName, {
                { "Master +3 dB", Values { { "boost", 0.25f }, { "soften", 0.5f } } },
                { "Loud",         Values { { "boost", 0.5f }, { "soften", 0.6f } } },
            } },
            { te::AirWindowsDrive::xmlTypeName, {
                { "Light", Values { { "drive", 0.2f } } }, { "Crunch", Values { { "drive", 0.5f } } }, { "Heavy", Values { { "drive", 0.9f }, { "outlevel", 0.7f } } },
            } },
            { te::AirWindowsDensity::xmlTypeName, {
                { "Thicken", Values { { "density", 0.4f } } }, { "Crush", Values { { "density", 0.9f }, { "outlevel", 0.7f } } },
            } },
            { te::AirWindowsDeRez2::xmlTypeName, {
                { "8-bit", Values { { "rate", 0.5f }, { "rez", 0.3f } } }, { "Lo-fi", Values { { "rate", 0.3f }, { "rez", 0.6f } } },
            } },
            { te::AirWindowsToTape5::xmlTypeName, {
                { "Warm", Values { { "louder", 0.3f }, { "fatter", 0.3f } } }, { "Wobbly", Values { { "louder", 0.25f }, { "flutter", 0.5f } } },
            } },
        };
        static const std::vector<Preset> none;
        auto it = all.find (type);
        return it != all.end() ? it->second : none;
    }

    inline juce::File folderFor (te::Plugin& p)
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("untitledstudio").getChildFile ("Presets").getChildFile (p.getPluginType());
    }

    inline void saveUser (te::Plugin& p, const juce::String& name)
    {
        juce::XmlElement xml ("PRESET");
        for (auto* param : p.getAutomatableParameters())
        {
            auto* e = xml.createNewChildElement ("PARAM");
            e->setAttribute ("id", param->paramID);
            e->setAttribute ("value", param->getCurrentValue());
        }
        if (p.getPluginType() == te::FourOscPlugin::xmlTypeName)
            for (auto& [id, def] : fourOscStateDefaults())
            {
                auto* e = xml.createNewChildElement ("PARAM");
                e->setAttribute ("id", id);
                e->setAttribute ("value", p.state.getProperty (id, def).toString());
            }
        const auto file = folderFor (p).getChildFile (juce::File::createLegalFileName (name) + ".xml");
        file.getParentDirectory().createDirectory();
        xml.writeTo (file);
    }

    inline void loadUser (te::Plugin& p, const juce::File& file)
    {
        if (auto xml = juce::XmlDocument::parse (file))
        {
            Values values;
            for (auto* e : xml->getChildIterator())
                values.push_back ({ e->getStringAttribute ("id"), (float) e->getDoubleAttribute ("value") });
            apply (p, { file.getFileNameWithoutExtension(), values });
        }
    }

    // Factory presets, the user's saved presets and "Save preset..." (effect and synth windows).
    inline void showMenu (Studio& studio, te::Plugin::Ptr plugin, juce::Component& target)
    {
        juce::PopupMenu m;
        juce::String category;
        for (auto& preset : factory (plugin->getPluginType()))
        {
            if (preset.category != category)
                m.addSectionHeader (category = preset.category);
            m.addItem (preset.name, [&studio, plugin, preset]
            {
                studio.undo().beginNewTransaction ("Preset");
                apply (*plugin, preset);
            });
        }

        auto files = folderFor (*plugin).findChildFiles (juce::File::findFiles, false, "*.xml");
        files.sort();
        if (! files.isEmpty())
        {
            m.addSectionHeader ("User");
            for (auto& f : files)
                m.addItem (f.getFileNameWithoutExtension(), [&studio, plugin, f]
                {
                    studio.undo().beginNewTransaction ("Preset");
                    loadUser (*plugin, f);
                });
        }

        m.addSeparator();
        m.addItem ("Save preset...", [plugin]
        {
            auto* w = new juce::AlertWindow ("Save preset", {}, juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", "My preset");
            w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([plugin, w] (int r)
            {
                if (r == 1 && w->getTextEditorContents ("name").isNotEmpty())
                    saveUser (*plugin, w->getTextEditorContents ("name"));
            }), true);
        });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
    }
}

namespace Effects
{
    // Synthesised room impulse response so the Convolver works out of the box.
    inline void loadGeneratedRoom (te::ImpulseResponsePlugin& ir, double seconds = 1.8)
    {
        const double sr = 44100.0;
        juce::AudioBuffer<float> buffer (2, (int) (seconds * sr));
        juce::Random rng (1234);
        for (int ch = 0; ch < 2; ++ch)
        {
            float lp = 0.0f;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const double t = i / sr;
                const float decay = (float) std::exp (-t / (seconds * 0.22));
                lp += 0.35f * ((rng.nextFloat() * 2.0f - 1.0f) - lp); // darker tail
                const float early = (i % 1301 == 0 || i % 2111 == 0) && t < 0.08 ? 0.6f : 0.0f;
                buffer.setSample (ch, i, (lp + early) * decay * 0.5f);
            }
        }
        ir.loadImpulseResponse (std::move (buffer), sr, 24);
    }

    // Anything a freshly added effect needs before use.
    inline void prepareNew (te::Plugin& p)
    {
        if (auto ir = dynamic_cast<te::ImpulseResponsePlugin*> (&p))
            loadGeneratedRoom (*ir);
    }
}

//==============================================================================
// EQ with a frequency-response curve; drag the 4 band dots (freq/gain), wheel = Q.
class EqView : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit EqView (te::EqualiserPlugin& p) : eq (&p)
    {
        setTooltip ("Drag a dot to change frequency/gain. Mouse wheel on a dot changes width (Q). Double-click resets.");
        const char* bands[4][3] = { { "Low-pass freq", "Low-pass gain", "Low-pass Q" }, { "Mid freq 1", "Mid gain 1", "Mid Q 1" },
                                    { "Mid freq 2", "Mid gain 2", "Mid Q 2" }, { "High-pass freq", "High-pass gain", "High-pass Q" } };
        for (int b = 0; b < 4; ++b)
            for (int k = 0; k < 3; ++k)
                for (auto* param : p.getAutomatableParameters())
                    if (param->paramID == bands[b][k])
                        params[b][k] = param;
        startTimerHz (20);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (4.0f);
        g.setColour (Theme::lcd);
        g.fillRoundedRectangle (r, 4.0f);

        g.setFont (Theme::font (10.0f));
        for (float f : { 50.0f, 100.0f, 500.0f, 1000.0f, 5000.0f, 10000.0f })
        {
            const float x = freqToX (f, r);
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            g.setColour (Theme::textDim);
            g.drawText (f >= 1000 ? juce::String ((int) f / 1000) + "k" : juce::String ((int) f), (int) x + 2, (int) r.getBottom() - 13, 30, 12, juce::Justification::left);
        }
        for (float db : { -12.0f, 0.0f, 12.0f })
        {
            g.setColour (juce::Colours::white.withAlpha (db == 0.0f ? 0.15f : 0.07f));
            g.drawHorizontalLine ((int) dbToY (db, r), r.getX(), r.getRight());
        }

        juce::Path curve;
        for (int px = 0; px <= (int) r.getWidth(); px += 2)
        {
            const float x = r.getX() + (float) px;
            const float y = dbToY (eq->getDBGainAtFrequency (xToFreq (x, r)), r);
            if (px == 0) curve.startNewSubPath (x, y);
            else         curve.lineTo (x, y);
        }
        g.setColour (Theme::accent);
        g.strokePath (curve, juce::PathStrokeType (2.0f));

        const juce::Colour colours[] = { juce::Colour (0xff5f8fd8), juce::Colour (0xff63b56d), juce::Colour (0xffd88a4c), juce::Colour (0xffb46ed0) };
        for (int b = 0; b < 4; ++b)
        {
            if (params[b][0] == nullptr) continue;
            const auto c = dotPosition (b, r);
            g.setColour (colours[b].withAlpha (dragging == b ? 1.0f : 0.85f));
            g.fillEllipse (c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f);
            g.setColour (juce::Colours::black);
            g.drawEllipse (c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f, 1.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = bandAt (e.position);
        if (dragging >= 0)
            for (auto& p : params[dragging]) if (p != nullptr) p->parameterChangeGestureBegin();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        const auto r = getLocalBounds().toFloat().reduced (4.0f);
        params[dragging][0]->setParameter (juce::jlimit (20.0f, 20000.0f, xToFreq (e.position.x, r)), juce::sendNotificationSync);
        params[dragging][1]->setParameter (juce::jlimit (-20.0f, 20.0f, yToDb (e.position.y, r)), juce::sendNotificationSync);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
            for (auto& p : params[dragging]) if (p != nullptr) p->parameterChangeGestureEnd();
        dragging = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int b = bandAt (e.position);
        if (b >= 0 && params[b][1] != nullptr)
            params[b][1]->setParameter (0.0f, juce::sendNotificationSync);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int b = bandAt (e.position);
        if (b >= 0 && params[b][2] != nullptr)
            params[b][2]->setParameter (juce::jlimit (0.1f, 4.0f, params[b][2]->getCurrentValue() * (w.deltaY > 0 ? 1.1f : 1.0f / 1.1f)),
                                        juce::sendNotificationSync);
    }

private:
    static float freqToX (float f, juce::Rectangle<float> r) { return r.getX() + r.getWidth() * std::log10 (f / 20.0f) / 3.0f; }
    static float xToFreq (float x, juce::Rectangle<float> r) { return 20.0f * std::pow (10.0f, 3.0f * (x - r.getX()) / r.getWidth()); }
    static float dbToY (float db, juce::Rectangle<float> r)  { return r.getCentreY() - db / 24.0f * r.getHeight() * 0.5f; }
    static float yToDb (float y, juce::Rectangle<float> r)   { return (r.getCentreY() - y) / (r.getHeight() * 0.5f) * 24.0f; }

    juce::Point<float> dotPosition (int b, juce::Rectangle<float> r) const
    {
        return { freqToX (params[b][0]->getCurrentValue(), r), dbToY (params[b][1]->getCurrentValue(), r) };
    }

    int bandAt (juce::Point<float> p) const
    {
        const auto r = getLocalBounds().toFloat().reduced (4.0f);
        for (int b = 0; b < 4; ++b)
            if (params[b][0] != nullptr && dotPosition (b, r).getDistanceFrom (p) < 10.0f)
                return b;
        return -1;
    }

    void timerCallback() override { repaint(); }

    juce::ReferenceCountedObjectPtr<te::EqualiserPlugin> eq;
    te::AutomatableParameter::Ptr params[4][3];
    int dragging = -1;
};

//==============================================================================
// Window content for one effect: header (bypass, presets, sidechain) + its controls.
class EffectWindow : public juce::Component, private juce::Timer
{
public:
    EffectWindow (Studio& s, te::Plugin& p) : studio (s), plugin (&p)
    {
        bypass.setTooltip ("Enable / bypass this effect");
        bypass.setToggleState (p.isEnabled(), juce::dontSendNotification);
        bypass.onClick = [this]
        {
            studio.undo().beginNewTransaction ("Bypass");
            plugin->setEnabled (bypass.getToggleState());
        };
        addAndMakeVisible (bypass);

        presetButton.setTooltip ("Factory and user presets");
        presetButton.onClick = [this] { showPresetMenu(); };
        addAndMakeVisible (presetButton);

        if (p.canSidechain())
        {
            sidechain.addItemList (p.getSidechainSourceNames (true), 1);
            sidechain.setText (p.getSidechainSourceName(), juce::dontSendNotification);
            sidechain.setTextWhenNothingSelected ("None");
            sidechain.setTooltip ("Sidechain input: e.g. duck the bass when the kick hits");
            sidechain.onChange = [this]
            {
                studio.undo().beginNewTransaction ("Sidechain");
                plugin->setSidechainSourceByName (sidechain.getText());
                if (auto comp = dynamic_cast<te::CompressorPlugin*> (plugin.get()))
                    comp->useSidechainTrigger = sidechain.getSelectedItemIndex() > 0;
            };
            addAndMakeVisible (sidechain);
        }

        int height = 300;
        if (auto analyzer = dynamic_cast<AnalyzerPlugin*> (&p))
        {
            visual = std::make_unique<AnalyzerView> (*analyzer);
            height = 380;
        }
        else
        {
            if (auto eq = dynamic_cast<te::EqualiserPlugin*> (&p))
            {
                visual = std::make_unique<EqView> (*eq);
                height = 520;
            }
            knobs = std::make_unique<GenericPluginEditor> (s, p);
            addAndMakeVisible (*knobs);
        }
        if (visual != nullptr)
            addAndMakeVisible (*visual);

        description = [&]
        {
            for (auto& e : Effects::builtIn())
                if (e.xmlType == p.getPluginType())
                    return e.hint;
            return juce::String();
        }();

        Widgets::noFocus (*this);
        setSize (560, height);
        startTimerHz (5);
    }

    te::Plugin* getPlugin() const { return plugin.get(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        auto header = getLocalBounds().removeFromTop (headerHeight);
        g.setGradientFill (juce::ColourGradient (Theme::titleActive, 0.0f, 0.0f, Theme::panel, 0.0f, (float) headerHeight, false));
        g.fillRect (header);
        g.setColour (Theme::accent);
        g.setFont (Theme::font (17.0f, true));
        g.drawText (Effects::displayName (*plugin), header.withTrimmedLeft (34).withWidth (200).withTrimmedBottom (14), juce::Justification::centredLeft);
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (11.0f));
        g.drawText (description, header.withTrimmedLeft (34).withTrimmedTop (24).withWidth (300), juce::Justification::centredLeft, true);
        if (sidechain.isVisible())
            g.drawText ("SIDECHAIN", sidechain.getBounds().withY (sidechain.getY() - 14).withHeight (12), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto header = r.removeFromTop (headerHeight).reduced (8, 10);
        bypass.setBounds (header.removeFromLeft (18).withSizeKeepingCentre (16, 16));
        sidechain.setBounds (header.removeFromRight (150).withTrimmedTop (12));
        header.removeFromRight (8);
        presetButton.setBounds (header.removeFromRight (90).withSizeKeepingCentre (90, 24));

        if (visual != nullptr)
            visual->setBounds (knobs != nullptr ? r.removeFromTop (r.getHeight() / 2) : r);
        if (knobs != nullptr)
            knobs->setBounds (r);
    }

private:
    void showPresetMenu() { Presets::showMenu (studio, plugin, presetButton); }

    void timerCallback() override { bypass.setToggleState (plugin->isEnabled(), juce::dontSendNotification); }

    static constexpr int headerHeight = 48;

    Studio& studio;
    te::Plugin::Ptr plugin;
    Widgets::LedButton bypass { "Enable / bypass" };
    juce::TextButton presetButton { "Presets" };
    juce::ComboBox sidechain;
    std::unique_ptr<juce::Component> visual;
    std::unique_ptr<GenericPluginEditor> knobs;
    juce::String description;
};
