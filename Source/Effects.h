#pragma once

#include "PluginEditor.h"
#include "Studio.h"

// The built-in effects you can put in a mixer slot.
namespace Effects
{
    struct Entry { juce::String name, xmlType, category; };

    inline const std::vector<Entry>& builtIn()
    {
        static const std::vector<Entry> list {
            { "EQ",          te::EqualiserPlugin::xmlTypeName,  "Mixing" },
            { "Compressor",  te::CompressorPlugin::xmlTypeName, "Mixing" },
            { "Reverb",      te::ReverbPlugin::xmlTypeName,     "Space" },
            { "Delay",       te::DelayPlugin::xmlTypeName,      "Space" },
            { "Chorus",      te::ChorusPlugin::xmlTypeName,     "Modulation" },
            { "Phaser",      te::PhaserPlugin::xmlTypeName,     "Modulation" },
            { "Filter",      te::LowPassPlugin::xmlTypeName,    "Filter" },
            { "Pitch shift", te::PitchShiftPlugin::xmlTypeName, "Pitch" },
        };
        return list;
    }

    // Menu of effects grouped by category; calls onPick with the xml type.
    inline void showAddMenu (juce::Component& target, std::function<void (const juce::String&)> onPick)
    {
        juce::PopupMenu m;
        std::map<juce::String, juce::PopupMenu> categories;
        for (auto& e : builtIn())
            categories[e.category].addItem (e.name, [onPick, type = e.xmlType] { onPick (type); });
        for (auto& [name, sub] : categories)
            m.addSubMenu (name, sub);
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

// Window content for one effect: bypass, sidechain source, and its controls.
class EffectWindow : public juce::Component, private juce::Timer
{
public:
    EffectWindow (Studio& s, te::Plugin& p) : studio (s), plugin (&p), editor (p)
    {
        bypass.setTooltip ("Enable / bypass this effect");
        bypass.setToggleState (p.isEnabled(), juce::dontSendNotification);
        bypass.onClick = [this]
        {
            studio.undo().beginNewTransaction ("Bypass");
            plugin->setEnabled (bypass.getToggleState());
        };
        addAndMakeVisible (bypass);

        if (p.canSidechain())
        {
            sidechain.addItemList (p.getSidechainSourceNames (true), 1);
            sidechain.setText (p.getSidechainSourceName(), juce::dontSendNotification);
            sidechain.setTooltip ("Sidechain input: e.g. duck the bass when the kick hits");
            sidechain.setTextWhenNothingSelected ("None");
            sidechain.onChange = [this]
            {
                studio.undo().beginNewTransaction ("Sidechain");
                plugin->setSidechainSourceByName (sidechain.getText());
                if (auto comp = dynamic_cast<te::CompressorPlugin*> (plugin.get()))
                    comp->useSidechainTrigger = sidechain.getSelectedItemIndex() > 0;
            };
            addAndMakeVisible (sidechain);
        }

        addAndMakeVisible (editor);
        Widgets::noFocus (*this);
        setSize (520, 360);
        startTimerHz (5);
    }

    te::Plugin* getPlugin() const { return plugin.get(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        if (sidechain.isVisible())
        {
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (12.0f));
            g.drawText ("SIDECHAIN", sidechain.getBounds().withX (sidechain.getX() - 76).withWidth (72), juce::Justification::centredRight);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (30).reduced (6, 5);
        bypass.setBounds (top.removeFromLeft (20));
        sidechain.setBounds (top.removeFromRight (180));
        editor.setBounds (r);
    }

private:
    void timerCallback() override { bypass.setToggleState (plugin->isEnabled(), juce::dontSendNotification); }

    Studio& studio;
    te::Plugin::Ptr plugin;
    Widgets::LedButton bypass { "Enable / bypass" };
    juce::ComboBox sidechain;
    GenericPluginEditor editor;
};
