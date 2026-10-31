#pragma once

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lithium
{

/** The embedded fonts, loaded once and shared by everything that needs them.

    Held through juce::SharedResourcePointer: the first holder creates it and
    the last one to go away deletes it. That keeps the typefaces alive exactly
    as long as some UI exists, and gone before JUCE shuts down (a plain static
    would outlive JUCE's font system when the plugin is unloaded). */
struct EmbeddedFonts
{
    EmbeddedFonts();

    juce::Typeface::Ptr barlowRegular, barlowMedium, barlowSemiBold;
    juce::Typeface::Ptr condensedMedium, condensedSemiBold, condensedBold;
    juce::Typeface::Ptr monoRegular, monoMedium;
};

class LithiumLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LithiumLookAndFeel();

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isMouseOverButton, bool isButtonDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override        { return 6; }

    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;

private:
    juce::SharedResourcePointer<EmbeddedFonts> fonts;
};

} // namespace lithium
