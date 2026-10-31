#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

// Small controls shared by the Edit and Mixer pages. Each one binds itself to
// an APVTS parameter through attach(), and can be re-attached to a different
// parameter later: the Mixer's inspector shows whichever channel is selected
// by re-attaching the same knobs, rather than building new ones.

namespace lithium
{

/** A rotary knob with its name above and its value (as the parameter
    formats it) below. Double-click puts it back to the parameter's default. */
class Knob final : public juce::Component
{
public:
    enum class Style
    {
        labelled,   // name above, value below
        compact     // just the knob, with the value in a tooltip (mixer strips)
    };

    explicit Knob (juce::String name = {}, Style = Style::labelled);

    void attach (juce::AudioProcessorValueTreeState&, const juce::String& parameterId);

    /** Draws the arc from the centre, for gains and pan. */
    void setBipolar (bool);
    void setArcColour (juce::Colour);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    const juce::String name;
    const Style style;
    juce::Slider slider;

    // Destroyed before the slider (members go in reverse order), which it must be.
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

/** A toggle button bound to a bool parameter. */
class ParamToggle final : public juce::TextButton
{
public:
    explicit ParamToggle (const juce::String& buttonText, const juce::String& tooltip = {});
    void attach (juce::AudioProcessorValueTreeState&, const juce::String& parameterId);

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
};

/** A drop-down bound to a choice (or int) parameter. The items are the
    parameter's own choices, so they can't get out of step with it. */
class ParamCombo final : public juce::ComboBox
{
public:
    ParamCombo() = default;
    void attach (juce::AudioProcessorValueTreeState&, const juce::String& parameterId);

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
};

/** A vertical peak meter (one or two channels), -60 to +6 dB. It's fed
    peaks at the UI's refresh rate and does its own ballistics: a fast rise,
    a slower fall, and a peak-hold line. */
class LevelMeter final : public juce::Component
{
public:
    explicit LevelMeter (int numChannels = 1) : channels (numChannels) {}

    /** Linear peaks since the last call. */
    void push (float left, float right = 0.0f);

    void paint (juce::Graphics&) override;

    static constexpr float minDb = -60.0f, maxDb = 6.0f;

private:
    const int channels;
    std::array<float, 2> levelDb { minDb, minDb }, holdDb { minDb, minDb };
    std::array<double, 2> holdUntil {};
};

/** A compressor's gain reduction, as a bar growing down from the top (0 to 24 dB). */
class ReductionMeter final : public juce::Component
{
public:
    void push (float reductionDb);
    void paint (juce::Graphics&) override;

    static constexpr float rangeDb = 24.0f;

private:
    float shownDb = 0.0f;
};

} // namespace lithium
