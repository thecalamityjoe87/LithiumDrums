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

/** The slider every knob and fader in the UI is built on, so they all
    handle the mouse and keyboard the same way:
      - dragging is relative (clicking doesn't jump the value), and holding
        Shift while dragging or scrolling makes it ten times finer;
      - double-clicking puts it back to its default (setDoubleClickReturnValue);
      - the wheel moves it by the same proportion whatever its range;
      - it can be reached with Tab and moved with the arrow keys, and shows
        an outline when it has the keyboard focus. */
class ControlSlider : public juce::Slider
{
public:
    ControlSlider();

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void focusGained (FocusChangeType) override                { repaint(); }
    void focusLost (FocusChangeType) override                  { repaint(); }

    /** Pixels of drag for the whole range (Shift makes it fineFactor times more). */
    static constexpr double pixelsForFullRange = 200.0;
    static constexpr double fineFactor = 10.0;

private:
    juce::Point<float> lastDragPosition;
};

/** Makes the buttons, sliders and drop-downs under root reachable with Tab
    (in the order they were added), without a mouse click moving the focus,
    so the focus outline only shows when the keyboard is used. */
void enableKeyboardNavigation (juce::Component& root);

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

    /** The dial's diameter, from the theme: theme::size::knob, knobLarge or knobCompact. */
    enum class Size { standard, large };

    explicit Knob (juce::String name = {}, Style = Style::labelled, Size = Size::standard);

    void attach (juce::AudioProcessorValueTreeState&, const juce::String& parameterId);

    /** Draws the arc from the centre, for gains and pan. */
    void setBipolar (bool);
    void setArcColour (juce::Colour);

    /** Replaces the tooltip (by default the parameter's name). Call after attach(). */
    void setTooltip (const juce::String& t)  { slider.setTooltip (t); }

    /** Makes the name a link (the sends' names open their effect on the FX page). */
    std::function<void()> onNameClick;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override       { updateNameHover(); }
    void mouseExit (const juce::MouseEvent&) override       { updateNameHover(); }

private:
    juce::Rectangle<float> getNameArea() const;
    void updateNameHover();

    bool nameHovered = false;
    const juce::String name;
    const Style style;
    const Size dialSize;
    ControlSlider slider;

    // Destroyed before the slider (members go in reverse order), which it must be.
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

/** A small button with a drawn chevron (previous / next), crisp at any scale. */
class ChevronButton final : public juce::Button
{
public:
    enum class Direction { left, right, down };

    ChevronButton (const juce::String& tooltip, Direction);
    void paintButton (juce::Graphics&, bool isMouseOver, bool isDown) override;

private:
    const Direction direction;
};

/** Strikes a drum once (an audition): an impact mark, the dot where the
    stick lands with short rays around it, to the left of the text "Strike".
    With no text it's just the mark, for a narrow button. Not a play
    triangle, which is the groove transport's, as a strike doesn't keep on
    playing. */
class StrikeButton final : public juce::TextButton
{
public:
    explicit StrikeButton (const juce::String& buttonText = "Strike") : juce::TextButton (buttonText) {}

    void paintButton (juce::Graphics&, bool isMouseOver, bool isDown) override;
};

/** The groove transport's Play / Stop button: a play triangle or a stop
    square to the left of its text, the two centred together. Lit while
    playing. */
class PlayStopButton final : public juce::TextButton
{
public:
    PlayStopButton() : juce::TextButton ("Play") {}

    void setPlaying (bool);
    bool isPlaying() const noexcept   { return getToggleState(); }

    void paintButton (juce::Graphics&, bool isMouseOver, bool isDown) override;
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

    /** The level at the top of the meter, so it can match a fader's scale
        (the bottom is always minDb). */
    void setTopDb (float db)                 { topDb = db; repaint(); }

    /** Where a level sits, 0 at the bottom to 1 at the top. */
    float proportionOf (float db) const noexcept    { return juce::jlimit (0.0f, 1.0f, (db - minDb) / (topDb - minDb)); }

    void paint (juce::Graphics&) override;

    static constexpr float minDb = -60.0f, maxDb = 6.0f;

private:
    const int channels;
    float topDb = maxDb;
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

//==============================================================================
/** A groove style chip, as the Grooves page's style row and the import
    summary draw it: the style's name and how many grooves it has, in a pill.
    A count below 0 leaves the count out. */
void drawStyleChip (juce::Graphics&, juce::Rectangle<float>, const juce::String& name, int count, bool on, bool hover);
float styleChipWidth (const juce::String& name, int count);

//==============================================================================
/** Draws the help mark: a "?" in a ring, the "?" centred by its ink. (Not an
    "i": the header's "i" button is the About panel.) */
void drawHelpMark (juce::Graphics&, juce::Rectangle<float> ring, bool highlighted);

/** What a help mark opens: a heading and a few short paragraphs, in a
    call-out pointing at the mark. */
class HelpPopover final : public juce::Component
{
public:
    HelpPopover (const juce::String& heading, const juce::StringArray& paragraphs);

    void paint (juce::Graphics&) override;

    /** Opens one in a call-out pointing at `target` (in `from`'s coordinates). */
    static void show (juce::Component& from, juce::Rectangle<int> target, const juce::String& heading, const juce::StringArray& paragraphs);

private:
    static constexpr int padding = 16;
    juce::TextLayout layout;
};

/** A "?" button that explains what it sits beside, for new users: a card's
    controls, say. Clicking it opens a HelpPopover. */
class HelpButton final : public juce::Button
{
public:
    HelpButton (juce::String heading, juce::StringArray paragraphs);

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    void clicked() override;

    static constexpr int size = 20;     // the button; the ring in it is 14 px

private:
    juce::String heading;
    juce::StringArray paragraphs;
};

} // namespace lithium
