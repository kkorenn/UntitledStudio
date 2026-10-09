#pragma once

#include "PluginEditor.h"
#include "Studio.h"

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
                if (dynamic_cast<te::SamplerPlugin*> (plugin.get()) == nullptr)
                {
                    editor = std::make_unique<GenericPluginEditor> (*plugin);
                    addAndMakeVisible (*editor);
                }
            }
        }

        replaceButton.onClick = [this] { browseForSample(); };
        replaceButton.setTooltip ("Load a different sample into this channel");
        if (editor == nullptr)
            addAndMakeVisible (replaceButton);

        keyboard.setAvailableRange (24, 108);
        keyboard.setLowestVisibleKey (48);
        keyboard.setVelocity (100.0f / 127.0f, true);
        keyboard.clearKeyMappings();
        keyboardState.addListener (this);
        addAndMakeVisible (keyboard);

        Widgets::noFocus (*this);
        setSize (editor != nullptr ? 640 : 460, editor != nullptr ? 460 : 220);
    }

    ~ChannelSettings() override { keyboardState.removeListener (this); }

    int getChannelId() const { return channelId; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panelDark);
        if (editor == nullptr)
        {
            const auto channel = studio.getChannel (channelId);
            g.setColour (Theme::text);
            g.setFont (Theme::font (14.0f, true));
            g.drawText (channel[IDs::name].toString(), getLocalBounds().reduced (12).removeFromTop (20), juce::Justification::centredLeft);
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (12.0f));
            g.drawText (channel[IDs::file].toString(), getLocalBounds().reduced (12).withTrimmedTop (24).removeFromTop (18),
                        juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        keyboard.setBounds (r.removeFromBottom (70));
        if (editor != nullptr)
            editor->setBounds (r);
        else
            replaceButton.setBounds (r.reduced (12).withTrimmedTop (52).removeFromTop (26).removeFromLeft (140));
    }

private:
    void browseForSample()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a sample", DrumKit::getPacksFolder(),
                                                       studio.engine.getAudioFileFormatManager().readFormatManager.getWildcardForAllFormats());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult().existsAsFile())
                                  {
                                      studio.replaceSample (channelId, fc.getResult());
                                      repaint();
                                  }
                              });
    }

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
    std::unique_ptr<GenericPluginEditor> editor;
    juce::TextButton replaceButton { "Replace sample..." };
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard { keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard };
    std::unique_ptr<juce::FileChooser> chooser;
};
