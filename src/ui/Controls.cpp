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
namespace
{
    // An icon and a text button's text as one group, centred in the button,
    // in the text's colour (as the look and feel draws it: see drawButtonText).
    // With no text, the icon alone, lit on hover.
    template <typename DrawIcon>
    void paintIconButton (juce::Graphics& g, juce::TextButton& b, bool isMouseOver, bool isDown, float iconSize, DrawIcon&& drawIcon)
    {
        using juce::TextButton;
        auto& lf = b.getLookAndFeel();
        const auto on = b.getToggleState();
        lf.drawButtonBackground (g, b, b.findColour (on ? TextButton::buttonOnColourId : TextButton::buttonColourId), isMouseOver, isDown);

        const auto text = b.getButtonText().toUpperCase();
        const auto colour = text.isNotEmpty() ? b.findColour (on ? TextButton::textColourOnId : TextButton::textColourOffId)
                                              : (isMouseOver ? theme::colours::accent : theme::colours::textMuted);
        g.setColour (b.isEnabled() ? colour : colour.withMultipliedAlpha (0.5f));

        constexpr float gap = 7.0f;
        const auto font = lf.getTextButtonFont (b, b.getHeight());
        const auto textWidth = text.isEmpty() ? 0.0f : theme::getTrackedTextWidth (text, font, 0.06f);
        const auto bounds = b.getLocalBounds().toFloat();
        // The icon's corner on a whole physical pixel, so its lines land the
        // same way every time rather than smeared across two pixels.
        const auto scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const auto snap = [scale] (float v) { return std::round (v * scale) / scale; };
        const auto left = snap (bounds.getCentreX() - (iconSize + (text.isEmpty() ? 0.0f : gap + textWidth)) * 0.5f);
        drawIcon (juce::Rectangle<float> (left, snap (bounds.getCentreY() - iconSize * 0.5f), iconSize, iconSize));

        if (text.isNotEmpty())
            theme::drawTrackedText (g, text, font, 0.06f, bounds.withLeft (left + iconSize + gap).withWidth (textWidth + 2.0f),
                                    juce::Justification::centredLeft);
    }
}

void StrikeButton::paintButton (juce::Graphics& g, bool isMouseOver, bool isDown)
{
    paintIconButton (g, *this, isMouseOver, isDown, 18.0f, [&g] (juce::Rectangle<float> icon)
    {
        // A stick coming down onto a drum, with a jagged burst where it
        // lands, on the side away from the stick. Points are in the icon's
        // 0..1 square.
        const auto w = icon.getWidth() / 14.0f;   // line widths were set at 14 px; they grow with the icon
        const auto at = [icon] (float x, float y) { return icon.getRelativePoint (x, y); };
        const auto line = [&g] (juce::Point<float> a, juce::Point<float> b, float thickness)
        {
            g.drawLine ({ a, b }, thickness);
            g.fillEllipse (juce::Rectangle<float> (thickness, thickness).withCentre (a));   // round ends
            g.fillEllipse (juce::Rectangle<float> (thickness, thickness).withCentre (b));
        };

        // The drum: an even cylinder seen from a little above. Its head is an
        // ellipse; the shell's sides drop straight down to a bottom edge
        // curving the same way as the head's front, with three vertical lines
        // (the lugs) down the shell.
        //
        // Laid out on whole physical pixels so it stays crisp: every line is
        // a whole number of pixels wide, and the five vertical lines (the two
        // sides and the lugs) sit on pixel centres, the same whole number of
        // pixels apart. (A line one pixel wide centred on a pixel boundary
        // would be smeared over two pixels at half strength.)
        const auto scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const auto pixels = [scale] (float v) { return std::max (1.0f, std::round (v * scale)) / scale; };
        const auto thickness = pixels (1.1f * w);
        const auto oddWidth = juce::roundToInt (thickness * scale) % 2 == 1;
        const auto onCentre = [scale, oddWidth] (float v)   // to a pixel centre for odd widths, else a pixel edge
        {
            return oddWidth ? (std::floor (v * scale) + 0.5f) / scale : std::round (v * scale) / scale;
        };

        const auto size = icon.getWidth();
        const auto spacing = pixels (0.72f * size / 4.0f);                     // between the five vertical lines
        const auto x0 = onCentre (icon.getX() + 0.5f * size - 2.0f * spacing);  // the left side
        const auto cx = x0 + 2.0f * spacing, rx = 2.0f * spacing;
        const auto headTop = onCentre (icon.getY() + 0.40f * size);
        const auto ry = pixels (0.18f * size) * 0.5f;
        const auto headCy = headTop + ry;
        const auto bottomCy = onCentre (icon.getBottom()) - ry;
        const auto pi = juce::MathConstants<float>::pi;

        g.drawEllipse (x0, headTop, 2.0f * rx, 2.0f * ry, thickness);

        juce::Path bottomEdge;
        // From 9 o'clock round through 6 to 3 (JUCE's angles run clockwise from 12).
        bottomEdge.addCentredArc (cx, bottomCy, rx, ry, 0.0f, 1.5f * pi, 0.5f * pi, true);
        g.strokePath (bottomEdge, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));

        for (int i = 0; i <= 4; ++i)
        {
            // The sides go from the head's widest point; the lugs from the
            // front of the head to the front of the bottom edge.
            const auto x = x0 + (float) i * spacing;
            const auto t = (x - cx) / rx;
            const auto drop = (i == 0 || i == 4) ? 0.0f : ry * std::sqrt (1.0f - t * t);
            g.fillRect (juce::Rectangle<float> (x - thickness * 0.5f, headCy + drop, thickness, bottomCy - headCy));
        }

        line (at (1.10f, 0.11f), at (0.55f, 0.46f), 1.8f * w);                          // the stick, about 30 degrees
        // The impact: a filled burst fanning out from the tip, its spikes of
        // uneven length, from just above the head on the left round to just
        // clear of the stick. Angles are in degrees from the right, upwards.
        {
            const auto tip = juce::Point<float> (0.52f, 0.46f);   // just above where the stick meets the head
            const auto point = [&] (float degrees, float radius)
            {
                const auto a = juce::degreesToRadians (degrees);
                return at (tip.x + std::cos (a) * radius, tip.y - std::sin (a) * radius);
            };

            constexpr float inner = 0.24f;
            constexpr std::pair<float, float> spikes[] { { 150.0f, 0.44f }, { 118.0f, 0.52f }, { 88.0f, 0.50f }, { 60.0f, 0.38f } };
            juce::Path burst;
            burst.startNewSubPath (at (tip.x, tip.y));
            burst.lineTo (point (172.0f, inner));

            for (size_t i = 0; i < std::size (spikes); ++i)
            {
                burst.lineTo (point (spikes[i].first, spikes[i].second));
                burst.lineTo (point (i + 1 < std::size (spikes) ? (spikes[i].first + spikes[i + 1].first) * 0.5f : 42.0f, inner));
            }

            burst.closeSubPath();
            g.fillPath (burst);
        }
    });
}

void PlayStopButton::setPlaying (bool playing)
{
    setButtonText (playing ? "Stop" : "Play");
    setToggleState (playing, juce::dontSendNotification);
}

void PlayStopButton::paintButton (juce::Graphics& g, bool isMouseOver, bool isDown)
{
    paintIconButton (g, *this, isMouseOver, isDown, 10.0f, [&g, playing = getToggleState()] (juce::Rectangle<float> icon)
    {
        icon = icon.reduced (0.5f);

        if (playing)
        {
            g.fillRoundedRectangle (icon.reduced (0.5f), 1.5f);   // stop
        }
        else
        {
            juce::Path triangle;
            triangle.addTriangle (icon.getX() + 0.5f, icon.getY(), icon.getX() + 0.5f, icon.getBottom(), icon.getRight(), icon.getCentreY());
            g.fillPath (triangle);
        }
    });
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
