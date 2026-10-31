#include "Controls.h"

#include "Theme.h"

namespace lithium
{

namespace
{
    constexpr float meterFallDbPerSecond = 26.0f;
    constexpr double peakHoldMs = 1200.0;
    constexpr float refreshSeconds = 1.0f / 30.0f;   // how often the pages push new levels

    float toDb (float linear)
    {
        return juce::Decibels::gainToDecibels (linear, LevelMeter::minDb);
    }
}

//==============================================================================
Knob::Knob (juce::String n, Style s) : name (std::move (n)), style (s)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    slider.setMouseDragSensitivity (180);
    slider.setPopupDisplayEnabled (style == Style::compact, false, nullptr);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);
}

void Knob::attach (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId)
{
    attachment.reset();   // the old attachment lets go of the slider first

    if (auto* p = state.getParameter (parameterId))
    {
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, parameterId, slider);
        slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
        slider.setTooltip (p->getName (64));
    }

    repaint();
}

void Knob::setBipolar (bool b)
{
    slider.getProperties().set ("bipolar", b);
    slider.repaint();
}

void Knob::setArcColour (juce::Colour c)
{
    slider.setColour (juce::Slider::rotarySliderFillColourId, c);
}

void Knob::paint (juce::Graphics& g)
{
    using namespace theme;

    if (style == Style::compact)
        return;

    auto area = getLocalBounds().toFloat();
    g.setColour (colours::textMuted);
    drawTrackedText (g, name.toUpperCase(), label (11.0f), 0.08f, area.removeFromTop (14.0f), juce::Justification::centred);

    // The attachment formats the value the way the parameter does.
    // Narrow knobs (a row of EQ bands) get a smaller value font, so "2.50 kHz" fits.
    g.setColour (colours::text);
    g.setFont (mono (getWidth() < 56 ? 10.0f : 11.0f));
    g.drawText (slider.getTextFromValue (slider.getValue()), area.removeFromBottom (14.0f), juce::Justification::centred, false);
}

void Knob::resized()
{
    auto area = getLocalBounds();

    if (style == Style::labelled)
    {
        area.removeFromTop (15);
        area.removeFromBottom (15);
    }

    const auto size = std::min (area.getWidth(), area.getHeight());
    slider.setBounds (area.withSizeKeepingCentre (size, size));
}

//==============================================================================
ParamToggle::ParamToggle (const juce::String& buttonText, const juce::String& tooltip) : juce::TextButton (buttonText)
{
    setClickingTogglesState (true);
    setTooltip (tooltip);
}

void ParamToggle::attach (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId)
{
    attachment.reset();

    if (state.getParameter (parameterId) != nullptr)
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, parameterId, *this);
}

//==============================================================================
void ParamCombo::attach (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId)
{
    attachment.reset();
    clear (juce::dontSendNotification);

    if (auto* p = state.getParameter (parameterId))
    {
        // An AudioParameterChoice lists its choices; an int parameter lists
        // every value as the parameter would print it.
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p))
        {
            addItemList (choice->choices, 1);
        }
        else if (auto* integer = dynamic_cast<juce::AudioParameterInt*> (p))
        {
            const auto range = integer->getRange();

            for (int v = range.getStart(); v <= range.getEnd(); ++v)
                addItem (static_cast<juce::AudioProcessorParameter*> (integer)->getText (integer->convertTo0to1 ((float) v), 64),
                         v - range.getStart() + 1);   // getText() is public on the base class
        }

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, parameterId, *this);
    }
}

//==============================================================================
void LevelMeter::push (float left, float right)
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const std::array<float, 2> peaks { left, right };

    for (size_t ch = 0; ch < (size_t) channels; ++ch)
    {
        const auto db = toDb (peaks[ch]);
        levelDb[ch] = std::max (db, levelDb[ch] - meterFallDbPerSecond * refreshSeconds);

        if (db >= holdDb[ch] || now > holdUntil[ch])
        {
            holdDb[ch] = db;
            holdUntil[ch] = now + peakHoldMs;
        }
    }

    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    using namespace theme::colours;

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (background);
    g.fillRoundedRectangle (bounds, 2.0f);

    const auto gap = channels > 1 ? 2.0f : 0.0f;
    const auto barWidth = (bounds.getWidth() - gap) / (float) channels;
    auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (minDb, maxDb, db), minDb, maxDb, bounds.getBottom(), bounds.getY()); };

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto x = bounds.getX() + (float) ch * (barWidth + gap);
        const auto bar = juce::Rectangle<float> (x, bounds.getY(), barWidth, bounds.getHeight());
        const auto top = yFor (levelDb[(size_t) ch]);

        // Teal up to -6 dB, then the cymbal yellow, then red above 0 dB.
        const auto y6 = yFor (-6.0f), y0 = yFor (0.0f);
        g.setColour (accent.withMultipliedAlpha (0.85f));
        g.fillRect (bar.withTop (std::max (top, y6)));

        if (top < y6)
        {
            g.setColour (groupCymbals);
            g.fillRect (bar.withTop (std::max (top, y0)).withBottom (y6));
        }

        if (top < y0)
        {
            g.setColour (warning);
            g.fillRect (bar.withTop (top).withBottom (y0));
        }

        if (holdDb[(size_t) ch] > minDb + 1.0f)
        {
            g.setColour (holdDb[(size_t) ch] > 0.0f ? warning : text.withAlpha (0.7f));
            g.fillRect (bar.withTop (yFor (holdDb[(size_t) ch])).withHeight (1.5f));
        }
    }

    g.setColour (border.withAlpha (0.8f));
    g.fillRect (bounds.withTop (yFor (0.0f)).withHeight (1.0f));
}

//==============================================================================
void ReductionMeter::push (float reductionDb)
{
    // Rises at once, falls back gently so short bursts stay readable.
    shownDb = std::max (reductionDb, shownDb - meterFallDbPerSecond * refreshSeconds);
    repaint();
}

void ReductionMeter::paint (juce::Graphics& g)
{
    using namespace theme::colours;

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (background);
    g.fillRoundedRectangle (bounds, 2.0f);

    const auto h = bounds.getHeight() * juce::jlimit (0.0f, 1.0f, shownDb / rangeDb);
    g.setColour (groupCymbals.withMultipliedAlpha (0.9f));
    g.fillRect (bounds.withHeight (h));
}

} // namespace lithium
