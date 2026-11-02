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
ControlSlider::ControlSlider()
{
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setSliderSnapsToMousePosition (false);
    setScrollWheelEnabled (true);
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (false);
}

void ControlSlider::mouseDown (const juce::MouseEvent& e)
{
    lastDragPosition = e.position;
    juce::Slider::mouseDown (e);   // starts the gesture (and the host's automation touch)
}

void ControlSlider::mouseDrag (const juce::MouseEvent& e)
{
    // Two-value sliders (the Grooves page's tempo range) keep JUCE's handling.
    if (! isEnabled() || getSliderStyle() == TwoValueHorizontal || getSliderStyle() == TwoValueVertical)
    {
        juce::Slider::mouseDrag (e);
        return;
    }

    // Relative to the last position rather than the drag's start, so
    // pressing or releasing Shift mid-drag never makes the value jump.
    const auto delta = e.position - lastDragPosition;
    lastDragPosition = e.position;

    const auto style = getSliderStyle();
    const auto pixels = style == LinearHorizontal ? (double) delta.x
                      : style == LinearVertical   ? (double) -delta.y
                                                  : (double) (delta.x - delta.y);   // rotary: right or up turns it up
    const auto travel = style == LinearVertical   ? std::max (40.0, (double) getHeight() - 16.0)
                      : style == LinearHorizontal ? std::max (40.0, (double) getWidth() - 12.0)
                                                  : pixelsForFullRange;
    const auto step = pixels / (travel * (e.mods.isShiftDown() ? fineFactor : 1.0));
    const auto proportion = juce::jlimit (0.0, 1.0, valueToProportionOfLength (getValue()) + step);

    setValue (proportionOfLengthToValue (proportion), juce::sendNotificationSync);
}

void ControlSlider::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! isEnabled() || getSliderStyle() == TwoValueHorizontal || getSliderStyle() == TwoValueVertical)
    {
        juce::Slider::mouseWheelMove (e, wheel);
        return;
    }

    // A notch moves every control by 2% of its range (0.2% with Shift).
    const auto amount = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? -wheel.deltaX : wheel.deltaY;

    if (juce::approximatelyEqual (amount, 0.0f))
        return;

    const auto notches = (double) amount * (wheel.isReversed ? -1.0 : 1.0) / 0.25;   // JUCE reports about 0.25 per notch
    const auto step = juce::jlimit (-0.2, 0.2, notches * 0.02 / (e.mods.isShiftDown() ? fineFactor : 1.0));
    const auto proportion = juce::jlimit (0.0, 1.0, valueToProportionOfLength (getValue()) + step);

    // Wrapped as a gesture, so the host records it as one automation move.
    const ScopedDragNotification gesture (*this);
    setValue (proportionOfLengthToValue (proportion), juce::sendNotificationSync);
}

void enableKeyboardNavigation (juce::Component& root)
{
    for (auto* child : root.getChildren())
    {
        if (dynamic_cast<juce::Button*> (child) != nullptr || dynamic_cast<juce::Slider*> (child) != nullptr
            || dynamic_cast<juce::ComboBox*> (child) != nullptr)
        {
            child->setWantsKeyboardFocus (true);
            child->setMouseClickGrabsKeyboardFocus (false);
        }

        enableKeyboardNavigation (*child);
    }
}

//==============================================================================
Knob::Knob (juce::String n, Style s, Size z) : name (std::move (n)), style (s), dialSize (z)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
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

    // The name sits just above the dial and the value just below it, so the
    // three read as one control however tall the knob's box is.
    const auto dial = slider.getBounds().toFloat();
    const auto width = (float) getWidth();
    g.setColour (! isEnabled() ? colours::textMuted.withMultipliedAlpha (0.6f)
                 : nameHovered ? colours::accent
                               : colours::textMuted);
    drawTrackedText (g, name.toUpperCase(), label (type::caption), 0.08f, getNameArea(), juce::Justification::centred);

    if (nameHovered)
    {
        // Underlined, like a link.
        const auto textWidth = getTrackedTextWidth (name.toUpperCase(), label (type::caption), 0.08f);
        g.fillRect (juce::Rectangle<float> ((width - textWidth) * 0.5f, getNameArea().getBottom() - 1.0f, textWidth, 1.0f));
    }

    // The attachment formats the value the way the parameter does.
    // Narrow knobs (a row of EQ bands) get a smaller value font, so "2.50 kHz" fits.
    g.setColour (isEnabled() ? colours::text : colours::textMuted);
    g.setFont (mono (getWidth() < 56 ? 10.0f : type::caption));
    g.drawText (slider.getTextFromValue (slider.getValue()), juce::Rectangle<float> (0.0f, dial.getBottom() + 1.0f, width, 14.0f),
                juce::Justification::centred, false);
}

juce::Rectangle<float> Knob::getNameArea() const
{
    return { 0.0f, (float) slider.getY() - 15.0f, (float) getWidth(), 14.0f };
}

void Knob::updateNameHover()
{
    const auto hover = onNameClick != nullptr && style == Style::labelled && isMouseOver()
                       && getNameArea().contains (getMouseXYRelative().toFloat());

    if (hover != nameHovered)
    {
        nameHovered = hover;
        setMouseCursor (hover ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void Knob::mouseDown (const juce::MouseEvent& e)
{
    if (onNameClick != nullptr && getNameArea().contains (e.position))
        onNameClick();
}

void Knob::resized()
{
    auto area = getLocalBounds();

    if (style == Style::labelled)
    {
        area.removeFromTop (15);
        area.removeFromBottom (15);
    }

    // A fixed dial size from the theme (smaller only if the box is), so
    // knobs of a kind match wherever they are.
    const auto wanted = style == Style::compact ? theme::size::knobCompact
                      : dialSize == Size::large ? theme::size::knobLarge
                                                : theme::size::knob;
    const auto size = std::min ({ wanted, area.getWidth(), area.getHeight() });
    slider.setBounds (area.withSizeKeepingCentre (size, size));
}

//==============================================================================
ChevronButton::ChevronButton (const juce::String& tooltip, Direction d) : juce::Button (tooltip), direction (d)
{
    setTooltip (tooltip);
}

void ChevronButton::paintButton (juce::Graphics& g, bool isMouseOver, bool isDown)
{
    getLookAndFeel().drawButtonBackground (g, *this, {}, isMouseOver, isDown);

    const auto c = getLocalBounds().toFloat().getCentre();
    const auto r = 3.5f;
    juce::Path chevron;

    switch (direction)
    {
        case Direction::left:   chevron.startNewSubPath (c.x + r * 0.5f, c.y - r); chevron.lineTo (c.x - r * 0.5f, c.y); chevron.lineTo (c.x + r * 0.5f, c.y + r); break;
        case Direction::right:  chevron.startNewSubPath (c.x - r * 0.5f, c.y - r); chevron.lineTo (c.x + r * 0.5f, c.y); chevron.lineTo (c.x - r * 0.5f, c.y + r); break;
        case Direction::down:   chevron.startNewSubPath (c.x - r, c.y - r * 0.5f); chevron.lineTo (c.x, c.y + r * 0.5f); chevron.lineTo (c.x + r, c.y - r * 0.5f); break;
    }

    const auto colour = isMouseOver ? theme::colours::text : theme::colours::textMuted;
    g.setColour (isEnabled() ? colour : colour.withMultipliedAlpha (0.4f));
    g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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
    auto changed = false;

    for (size_t ch = 0; ch < (size_t) channels; ++ch)
    {
        const auto db = toDb (peaks[ch]);
        const auto level = std::max (db, levelDb[ch] - meterFallDbPerSecond * refreshSeconds);
        changed = changed || ! juce::exactlyEqual (level, levelDb[ch]);
        levelDb[ch] = level;

        if (db >= holdDb[ch] || now > holdUntil[ch])
        {
            changed = changed || ! juce::exactlyEqual (db, holdDb[ch]);
            holdDb[ch] = db;
            holdUntil[ch] = now + peakHoldMs;
        }
    }

    // A silent meter (most of them, most of the time) doesn't repaint at all.
    if (changed)
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
    auto yFor = [&] (float db) { return bounds.getBottom() - proportionOf (db) * bounds.getHeight(); };

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
    const auto shown = std::max (0.0f, std::max (reductionDb, shownDb - meterFallDbPerSecond * refreshSeconds));

    if (! juce::exactlyEqual (shown, shownDb))
    {
        shownDb = shown;
        repaint();
    }
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
