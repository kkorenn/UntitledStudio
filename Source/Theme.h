#pragma once

// FL-inspired dark slate theme. Own colours, no FL artwork.
namespace Theme
{
    inline const juce::Colour background   { 0xff2e353b };
    inline const juce::Colour workspace    { 0xff252b30 };
    inline const juce::Colour toolbar      { 0xff3d464d };
    inline const juce::Colour panel        { 0xff3a4248 };
    inline const juce::Colour panelDark    { 0xff30373c };
    inline const juce::Colour titleBar     { 0xff4a545b };
    inline const juce::Colour titleActive  { 0xff56626b };
    inline const juce::Colour outline      { 0xff1c2125 };
    inline const juce::Colour text         { 0xffd9dee2 };
    inline const juce::Colour textDim      { 0xff8d979e };
    inline const juce::Colour accent       { 0xffff9a3c }; // orange
    inline const juce::Colour led          { 0xff7cd85a }; // green LED
    inline const juce::Colour ledOff       { 0xff3a4a35 };
    inline const juce::Colour stepOffA     { 0xff4c555c };
    inline const juce::Colour stepOffB     { 0xff5c4a4d };
    inline const juce::Colour stepOn       { 0xffe8ecef };
    inline const juce::Colour lcd          { 0xff1d2226 };
    inline const juce::Colour lcdText      { 0xffa8f07a };

    inline juce::Font font (float size, bool bold = false)
    {
        return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    }

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel()
        {
            setColour (juce::ResizableWindow::backgroundColourId, background);
            setColour (juce::TextButton::buttonColourId, panel);
            setColour (juce::TextButton::buttonOnColourId, accent.darker (0.2f));
            setColour (juce::TextButton::textColourOffId, text);
            setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            setColour (juce::ComboBox::backgroundColourId, panelDark);
            setColour (juce::ComboBox::outlineColourId, outline);
            setColour (juce::PopupMenu::backgroundColourId, panelDark);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.8f));
            setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
            setColour (juce::Label::textColourId, text);
            setColour (juce::Slider::rotarySliderFillColourId, accent);
            setColour (juce::Slider::rotarySliderOutlineColourId, outline);
            setColour (juce::Slider::thumbColourId, accent);
            setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::TextEditor::backgroundColourId, lcd);
            setColour (juce::TextEditor::outlineColourId, outline);
            setColour (juce::ScrollBar::thumbColourId, titleBar);
            setColour (juce::TooltipWindow::backgroundColourId, panelDark);
        }

        // Compact FL-style knob: dark disc, orange arc, light pointer.
        void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider& s) override
        {
            auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
            const float size = juce::jmin (r.getWidth(), r.getHeight());
            r = r.withSizeKeepingCentre (size, size);
            const auto centre = r.getCentre();
            const float radius = size * 0.5f;
            const float angle = startAngle + pos * (endAngle - startAngle);

            g.setColour (outline);
            g.fillEllipse (r);
            g.setColour (juce::Colour (0xff5b666e));
            g.fillEllipse (r.reduced (size * 0.16f));

            const bool bipolar = s.getProperties().getWithDefault ("bipolar", false);
            const float arcStart = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, radius * 0.9f, radius * 0.9f, 0.0f,
                               juce::jmin (arcStart, angle), juce::jmax (arcStart, angle), true);
            g.setColour (s.isEnabled() ? accent : textDim);
            g.strokePath (arc, juce::PathStrokeType (juce::jmax (1.5f, size * 0.1f)));

            juce::Path pointer;
            pointer.addRectangle (-size * 0.04f, -radius * 0.78f, size * 0.08f, radius * 0.5f);
            g.setColour (text);
            g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
        }

        juce::Font getTextButtonFont (juce::TextButton&, int h) override { return font (juce::jmin (14.0f, (float) h * 0.6f)); }
        juce::Font getPopupMenuFont() override                         { return font (14.0f); }
        juce::Font getLabelFont (juce::Label&) override                { return font (13.0f); }
    };
}
