#pragma once

#include "Studio.h"
#include "Widgets.h"

// A slider bound to an engine parameter, with FL's right-click menu:
// Create automation clip / Link to controller / Reset. Records automation when enabled.
class ParamKnob : public juce::Slider
{
public:
    ParamKnob() { Widgets::setupKnob (*this, {}); }

    void bind (Studio& s, te::AutomatableParameter::Ptr p)
    {
        studio = &s;
        param = p;
        const auto range = param->valueRange;
        setNormalisableRange ({ range.start, range.end, range.interval, range.skew });
        setValue (param->getCurrentValue(), juce::dontSendNotification);
        if (auto def = param->getDefaultValue())
            setDoubleClickReturnValue (true, *def);
        getProperties().set ("bipolar", range.start < 0.0f && range.end > 0.0f);

        onDragStart = [this]
        {
            studio->undo().beginNewTransaction (param->getParameterName());
            param->parameterChangeGestureBegin();
        };
        onDragEnd = [this] { param->parameterChangeGestureEnd(); };
        onValueChange = [this]
        {
            param->setParameter ((float) getValue(), juce::sendNotificationSync);
            studio->userChangedParam (*param);
            updateHint();
        };
        updateHint();
    }

    te::AutomatableParameter* getParam() const { return param.get(); }

    // Follow changes from automation, undo, MIDI controllers.
    void sync()
    {
        if (param != nullptr && ! isMouseButtonDown() && std::abs (getValue() - param->getCurrentValue()) > 1.0e-6)
        {
            setValue (param->getCurrentValue(), juce::dontSendNotification);
            updateHint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && param != nullptr)
            return showMenu();
        juce::Slider::mouseDown (e);
    }

private:
    void updateHint()
    {
        auto hint = studio->describeParam (*param) + ": " + param->getCurrentValueAsStringWithLabel();
        if (auto link = studio->getControllerLink (*param); link.isNotEmpty())
            hint << "  [" << link << "]";
        setTooltip (hint);
    }

    void showMenu()
    {
        juce::PopupMenu m;
        m.addItem ("Create automation clip", [this] { studio->createAutomationClip (*param); });
        m.addItem ("Link to controller...", [this]
        {
            studio->startLearning (*param);
            setTooltip ("Move a knob on your MIDI controller to link it to " + param->getParameterName());
        });
        const bool linked = studio->getControllerLink (*param).isNotEmpty();
        m.addItem ("Unlink controller", linked, false, [this]
        {
            studio->undo().beginNewTransaction ("Unlink controller");
            studio->unlinkController (*param);
            updateHint();
        });
        m.addSeparator();
        m.addItem ("Reset", param->getDefaultValue().has_value(), false, [this]
        {
            studio->undo().beginNewTransaction ("Reset");
            param->setParameter (*param->getDefaultValue(), juce::sendNotificationSync);
            sync();
        });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
    }

    Studio* studio = nullptr;
    te::AutomatableParameter::Ptr param;
};
