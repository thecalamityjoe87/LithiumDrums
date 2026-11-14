#include "LithiumLookAndFeel.h"

#include "LithiumFonts.h"

namespace lithium
{

namespace
{
    juce::Typeface::Ptr loadTypeface (const char* data, int size)
    {
        return juce::Typeface::createSystemTypefaceFor (data, (size_t) size);
    }
}

EmbeddedFonts::EmbeddedFonts()
{
    using namespace LithiumFonts;
    barlowRegular     = loadTypeface (BarlowRegular_ttf, BarlowRegular_ttfSize);
    barlowMedium      = loadTypeface (BarlowMedium_ttf, BarlowMedium_ttfSize);
    barlowSemiBold    = loadTypeface (BarlowSemiBold_ttf, BarlowSemiBold_ttfSize);
    condensedMedium   = loadTypeface (BarlowCondensedMedium_ttf, BarlowCondensedMedium_ttfSize);
    condensedSemiBold = loadTypeface (BarlowCondensedSemiBold_ttf, BarlowCondensedSemiBold_ttfSize);
    condensedBold     = loadTypeface (BarlowCondensedBold_ttf, BarlowCondensedBold_ttfSize);
    monoRegular       = loadTypeface (IBMPlexMonoRegular_ttf, IBMPlexMonoRegular_ttfSize);
    monoMedium        = loadTypeface (IBMPlexMonoMedium_ttf, IBMPlexMonoMedium_ttfSize);
}

//==============================================================================
namespace theme
{
    namespace
    {
        // Sizes are given like CSS font sizes (the em size), which is what
        // withPointHeight() means in JUCE 8 and later.
        juce::Font make (juce::Typeface::Ptr tf, float size)
        {
            return juce::Font (juce::FontOptions (tf).withPointHeight (size));
        }
    }

    juce::Font body (float size, Weight w)
    {
        juce::SharedResourcePointer<EmbeddedFonts> f;

        switch (w)
        {
            case Weight::regular:  return make (f->barlowRegular, size);
            case Weight::medium:   return make (f->barlowMedium, size);
            case Weight::semibold:
            case Weight::bold:     return make (f->barlowSemiBold, size);
        }

        return make (f->barlowRegular, size);
    }

    juce::Font label (float size, Weight w)
    {
        juce::SharedResourcePointer<EmbeddedFonts> f;

        switch (w)
        {
            case Weight::regular:
            case Weight::medium:   return make (f->condensedMedium, size);
            case Weight::semibold: return make (f->condensedSemiBold, size);
            case Weight::bold:     return make (f->condensedBold, size);
        }

        return make (f->condensedSemiBold, size);
    }

    juce::Font mono (float size, Weight w)
    {
        juce::SharedResourcePointer<EmbeddedFonts> f;
        return make (w == Weight::regular ? f->monoRegular : f->monoMedium, size);
    }

    float getTrackedTextWidth (const juce::String& text, juce::Font font, float tracking)
    {
        return juce::GlyphArrangement::getStringWidth (font.withExtraKerningFactor (tracking), text);
    }

    void drawTrackedText (juce::Graphics& g, const juce::String& text, juce::Font font, float tracking,
                          juce::Rectangle<float> area, juce::Justification justification)
    {
        g.setFont (font.withExtraKerningFactor (tracking));
        g.drawText (text, area, justification, false);
    }
}

//==============================================================================
LithiumLookAndFeel::LithiumLookAndFeel()
{
    using namespace theme::colours;

    // The base scheme (used by e.g. the standalone app's settings dialog).
    // This resets every colour ID, so it must come before the setColour calls.
    setColourScheme ({ background, panel, raised, borderStrong, text, raisedHigh, text, accent, text });

    // JUCE draws plenty of text itself (popup menus, alert windows); make
    // Barlow the default for all of it.
    setDefaultSansSerifTypeface (fonts->barlowRegular);

    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::DocumentWindow::backgroundColourId, background);

    setColour (juce::TextButton::buttonColourId, raised);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, textMuted);
    setColour (juce::TextButton::textColourOnId, background);

    setColour (juce::ComboBox::backgroundColourId, raised);
    setColour (juce::ComboBox::outlineColourId, border);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, textMuted);
    setColour (juce::ComboBox::focusedOutlineColourId, accent);

    setColour (juce::PopupMenu::backgroundColourId, raised);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, textMuted);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, raisedHigh);
    setColour (juce::PopupMenu::highlightedTextColourId, accent);

    setColour (juce::Label::textColourId, text);

    setColour (juce::TextEditor::backgroundColourId, raised);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, border);
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.3f));
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::Slider::trackColourId, accent);
    setColour (juce::Slider::backgroundColourId, raisedHigh);
    setColour (juce::Slider::thumbColourId, text);
    setColour (juce::Slider::rotarySliderFillColourId, accent);

    setColour (juce::TooltipWindow::backgroundColourId, raisedHigh);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, borderStrong);
}

juce::Typeface::Ptr LithiumLookAndFeel::getTypefaceForFont (const juce::Font& font)
{
    if (font.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
        return fonts->barlowRegular;

    return LookAndFeel_V4::getTypefaceForFont (font);
}

//==============================================================================
void LithiumLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                               bool isMouseOverButton, bool isButtonDown)
{
    using namespace theme::colours;

    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto on = button.getToggleState();

    auto fill = on ? button.findColour (juce::TextButton::buttonOnColourId)
                   : button.findColour (juce::TextButton::buttonColourId);

    if (isButtonDown)
        fill = fill.brighter (0.15f);
    else if (isMouseOverButton)
        fill = on ? fill.brighter (0.08f) : raisedHigh;

    if (! button.isEnabled())
        fill = fill.withMultipliedAlpha (0.5f);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, theme::cornerRadius - 1.0f);

    g.setColour (on ? fill : (isMouseOverButton ? borderStrong : border));
    g.drawRoundedRectangle (bounds, theme::cornerRadius - 1.0f, 1.0f);

    if (button.hasKeyboardFocus (false))
        drawFocusRing (g, bounds, theme::cornerRadius - 1.0f);
}

void LithiumLookAndFeel::drawFocusRing (juce::Graphics& g, juce::Rectangle<float> area, float radius)
{
    g.setColour (theme::colours::focus);
    g.drawRoundedRectangle (area.reduced (0.5f), radius, (float) theme::size::focusRing);
}

juce::Font LithiumLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    // Half the height, but never below a readable size (the mixer strips' small switches).
    return theme::label (juce::jlimit (9.5f, 14.0f, (float) buttonHeight * 0.5f), theme::Weight::semibold);
}

void LithiumLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    const auto colour = button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                                   : juce::TextButton::textColourOffId);
    g.setColour (button.isEnabled() ? colour : colour.withMultipliedAlpha (0.5f));
    theme::drawTrackedText (g, button.getButtonText().toUpperCase(), getTextButtonFont (button, button.getHeight()),
                            0.06f, button.getLocalBounds().toFloat(), juce::Justification::centred);
}

//==============================================================================
void LithiumLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                                       int, int, int, int, juce::ComboBox& box)
{
    using namespace theme::colours;

    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    const auto hover = box.isMouseOver (true);

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId).brighter (isButtonDown ? 0.08f : 0.0f));
    g.fillRoundedRectangle (bounds, theme::cornerRadius);

    g.setColour (box.hasKeyboardFocus (true) ? accent : (hover ? borderStrong : box.findColour (juce::ComboBox::outlineColourId)));
    g.drawRoundedRectangle (bounds, theme::cornerRadius, 1.0f);

    // Chevron (closer to the edge in narrow boxes, such as a mixer strip's output)
    const auto cx = (float) width - (width < 70 ? 9.0f : 14.0f);
    const auto cy = (float) height * 0.5f;
    juce::Path chevron;
    chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
    chevron.lineTo (cx, cy + 2.0f);
    chevron.lineTo (cx + 4.0f, cy - 2.0f);

    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font LithiumLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return theme::body (13.0f, theme::Weight::medium);
}

void LithiumLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    const auto narrow = box.getWidth() < 70;
    label.setBounds (narrow ? 2 : 4, 1, box.getWidth() - (narrow ? 15 : 26), box.getHeight() - 2);
    label.setFont (narrow ? theme::body (11.5f, theme::Weight::medium) : getComboBoxFont (box));
}

juce::Font LithiumLookAndFeel::getPopupMenuFont()
{
    return theme::body (14.0f);
}

void LithiumLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
    g.setColour (theme::colours::borderStrong);
    g.drawRect (0, 0, width, height);
}

//==============================================================================
namespace
{
    juce::PathStrokeType roundStroke (float width)
    {
        return juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    }

    // The console fader's slot: sunk into the panel, so darker at its edges
    // than down its middle, with a faint highlight along its lower lip.
    void drawSlot (juce::Graphics& g, juce::Rectangle<float> slot)
    {
        using namespace theme::colours;

        const auto radius = std::min (slot.getWidth(), slot.getHeight()) * 0.5f;
        const bool vertical = slot.getHeight() > slot.getWidth();
        g.setColour (faderCapHighlight.withMultipliedAlpha (0.25f));
        g.fillRoundedRectangle (slot.translated (0.0f, 1.0f), radius);

        const auto edge = faderSlot.darker (0.6f);
        juce::ColourGradient shade (edge, slot.getTopLeft(), edge, vertical ? slot.getTopRight() : slot.getBottomLeft(), false);
        shade.addColour (0.5, faderSlot);
        g.setGradientFill (shade);
        g.fillRoundedRectangle (slot, radius);
    }

    // The console fader cap, as on Lithium Studio: ribbed, shaded along the
    // fader's travel, with a line across its middle in the channel's colour.
    // A horizontal cap has its ribs and line upright.
    void drawConsoleCap (juce::Graphics& g, juce::Rectangle<float> cap, bool vertical, juce::Colour line, bool enabled)
    {
        using namespace theme::colours;

        const auto corner = std::min (4.0f, std::min (cap.getWidth(), cap.getHeight()) * 0.25f);

        if (! enabled)
            g.beginTransparencyLayer (0.5f);

        // Its shadow: two soft steps below it (cheaper than blurring a
        // DropShadow on every repaint while the fader moves).
        g.setColour (faderCapShadow.withMultipliedAlpha (0.35f));
        g.fillRoundedRectangle (cap.translated (0.0f, 4.0f).expanded (1.0f), corner + 1.0f);
        g.setColour (faderCapShadow.withMultipliedAlpha (0.6f));
        g.fillRoundedRectangle (cap.translated (0.0f, 2.0f), corner);

        juce::ColourGradient body (faderCapLight, cap.getTopLeft(),
                                   faderCapBottom, vertical ? cap.getBottomLeft() : cap.getTopRight(), false);
        body.addColour (0.10, faderCapHigh);
        body.addColour (0.42, faderCapMid);
        body.addColour (0.50, faderCapGroove);
        body.addColour (0.58, faderCapMid);
        body.addColour (0.90, faderCapHigh);
        g.setGradientFill (body);
        g.fillRoundedRectangle (cap, corner);

        // The ribs: across the cap every 3px, either side of the line.
        const auto start = vertical ? cap.getY() : cap.getX();
        const auto mid = start + (vertical ? cap.getHeight() : cap.getWidth()) * 0.5f;
        const auto across = (vertical ? cap.getWidth() : cap.getHeight()) - 6.0f;
        g.setColour (faderCapRibs);

        for (auto t = start + 3.0f; t < mid - 3.0f; t += 3.0f)
            for (const auto rib : { t, 2.0f * mid - t - 1.0f })
                g.fillRect (vertical ? juce::Rectangle<float> (cap.getX() + 3.0f, rib, across, 1.0f)
                                     : juce::Rectangle<float> (rib, cap.getY() + 3.0f, 1.0f, across));

        g.setColour (faderCapBorder);
        g.drawRoundedRectangle (cap.reduced (0.5f), corner, 1.0f);
        g.setColour (faderCapHighlight);
        g.fillRect (juce::Rectangle<float> (cap.getX() + corner, cap.getY() + 1.0f, cap.getWidth() - 2.0f * corner, 1.0f));

        const auto lineArea = vertical ? juce::Rectangle<float> (cap.getWidth() - 4.0f, 2.0f).withCentre (cap.getCentre())
                                       : juce::Rectangle<float> (2.0f, cap.getHeight() - 4.0f).withCentre (cap.getCentre());
        g.setColour (line.withMultipliedAlpha (controlGlow));
        g.fillRoundedRectangle (lineArea.expanded (2.0f), 2.0f);
        g.setColour (line);
        g.fillRoundedRectangle (lineArea, 1.0f);

        if (! enabled)
            g.endTransparencyLayer();
    }
}

void LithiumLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float minSliderPos, float maxSliderPos,
                                           juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style == juce::Slider::LinearVertical)
    {
        drawFader (g, { x, y, width, height }, sliderPos, slider);
        return;
    }

    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto track = bounds.withSizeKeepingCentre (bounds.getWidth(), 4.0f);
    const auto enabled = slider.isEnabled();

    // A range (the Grooves page's tempo filter): the track filled between two thumbs.
    if (style == juce::Slider::TwoValueHorizontal)
    {
        g.setColour (slider.findColour (juce::Slider::backgroundColourId));
        g.fillRoundedRectangle (track, 2.0f);
        g.setColour (slider.findColour (juce::Slider::trackColourId).withMultipliedAlpha (enabled ? 1.0f : 0.4f));
        g.fillRoundedRectangle (track.withLeft (minSliderPos).withRight (maxSliderPos), 2.0f);
        g.setColour (slider.findColour (juce::Slider::thumbColourId).withMultipliedAlpha (enabled ? 1.0f : 0.5f));

        for (const auto pos : { minSliderPos, maxSliderPos })
            g.fillRoundedRectangle (juce::Rectangle<float> (6.0f, 14.0f).withCentre ({ pos, bounds.getCentreY() }), 2.0f);

        return;
    }

    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    // A volume (it has a unity tick): the console fader on its side, a
    // sunken slot, a tick at 0 dB and the ribbed cap.
    if (const auto unity = slider.getProperties()["unityValue"]; ! unity.isVoid())
    {
        const auto slot = bounds.withSizeKeepingCentre (bounds.getWidth() + 8.0f, 4.0f);
        drawSlot (g, slot);

        const auto ux = (float) slider.getPositionOfValue ((double) unity);
        g.setColour (theme::colours::borderStrong);
        g.fillRect (juce::Rectangle<float> (ux - 0.5f, slot.getY() - 4.0f, 1.0f, slot.getHeight() + 8.0f));

        const auto capWidth = 2.0f * (float) getSliderThumbRadius (slider);
        const auto cap = juce::Rectangle<float> (capWidth, std::min (18.0f, bounds.getHeight())).withCentre ({ sliderPos, bounds.getCentreY() });
        drawConsoleCap (g, cap, false, slider.findColour (juce::Slider::trackColourId), enabled);

        if (slider.hasKeyboardFocus (false))
            drawFocusRing (g, cap.expanded (3.0f), 4.0f);

        return;
    }

    g.setColour (slider.findColour (juce::Slider::backgroundColourId));
    g.fillRoundedRectangle (track, 2.0f);

    auto fillColour = slider.findColour (juce::Slider::trackColourId);
    g.setColour (enabled ? fillColour : fillColour.withMultipliedAlpha (0.4f));
    g.fillRoundedRectangle (track.withRight (sliderPos), 2.0f);

    const auto thumb = juce::Rectangle<float> (6.0f, 14.0f).withCentre ({ sliderPos, bounds.getCentreY() });
    g.setColour (slider.findColour (juce::Slider::thumbColourId).withMultipliedAlpha (enabled ? 1.0f : 0.5f));
    g.fillRoundedRectangle (thumb, 2.0f);

    if (slider.hasKeyboardFocus (false))
        drawFocusRing (g, thumb.expanded (3.0f), 3.0f);
}

int LithiumLookAndFeel::getSliderThumbRadius (juce::Slider& slider)
{
    // The fader cap is taller than the horizontal slider's thumb; JUCE keeps
    // this much room at each end of the track.
    return slider.getSliderStyle() == juce::Slider::LinearVertical ? 8 : 6;
}

void LithiumLookAndFeel::drawFader (juce::Graphics& g, juce::Rectangle<int> area, float sliderPos, juce::Slider& slider)
{
    // The console fader, as on Lithium Studio: a sunken slot, a tick across
    // it at unity gain (the "unityValue" property, if set), and the ribbed
    // cap with its line in the channel's colour (trackColourId). The cap is
    // as tall as JUCE keeps free at each end of the travel (size::faderCap),
    // so it never leaves the slider's bounds.
    const auto bounds = area.toFloat();
    const auto slot = bounds.withSizeKeepingCentre (6.0f, bounds.getHeight() + 6.0f);
    drawSlot (g, slot);

    if (const auto unity = slider.getProperties()["unityValue"]; ! unity.isVoid())
    {
        const auto uy = (float) slider.getPositionOfValue ((double) unity);
        g.setColour (theme::colours::borderStrong);
        g.fillRect (juce::Rectangle<float> (slot.getX() - 6.0f, uy - 0.5f, slot.getWidth() + 12.0f, 1.0f));
    }

    const auto capHeight = 2.0f * (float) getSliderThumbRadius (slider);
    const auto cap = juce::Rectangle<float> (std::min (26.0f, bounds.getWidth()), capHeight).withCentre ({ slot.getCentreX(), sliderPos });
    drawConsoleCap (g, cap, true, slider.findColour (juce::Slider::trackColourId), slider.isEnabled());

    if (slider.hasKeyboardFocus (false))
        drawFocusRing (g, cap.expanded (2.0f), 4.0f);
}

void LithiumLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                                           float startAngle, float endAngle, juce::Slider& slider)
{
    using namespace theme::colours;

    // The machined knob, as on Lithium Studio. Drawn in units of a hundredth
    // of the knob's size (u), so it scales. Outside in: the tick scale, the
    // value arc in its groove, the skirt with its shadow, the turned-metal
    // cap with two bands of sheen, and the pointer. The shading is fixed
    // (lit from the top left); only the arc and pointer move with the value.
    // A compact knob (the mixer strips' 22px dials) has no ticks and a plain
    // pointer.
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto size = std::min (bounds.getWidth(), bounds.getHeight());
    const auto compact = size < 32.0f;
    const auto u = size / 100.0f;
    const auto c = bounds.getCentre();
    const auto enabled = slider.isEnabled();
    const auto angle = startAngle + position * (endAngle - startAngle);

    // Bipolar knobs (gain, pan) draw their arc from the centre, the rest from the start.
    const auto bipolar = (bool) slider.getProperties()["bipolar"];
    const auto fromAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

    auto circle = [&] (float radius, float dy = 0.0f)
    {
        return juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c.translated (0.0f, dy));
    };
    auto arc = [&] (float radius, float a0, float a1)
    {
        juce::Path p;
        p.addCentredArc (c.x, c.y, radius, radius, 0.0f, a0, a1, true);
        return p;
    };

    if (! compact)
    {
        juce::Path ticks;

        for (int i = 0; i <= 10; ++i)
        {
            const auto a = startAngle + (endAngle - startAngle) * (float) i / 10.0f;
            ticks.startNewSubPath (c.getPointOnCircumference (45.5f * u, a));
            ticks.lineTo (c.getPointOnCircumference (49.5f * u, a));
        }

        g.setColour (knobTicks);
        g.strokePath (ticks, roundStroke (std::max (1.0f, 1.6f * u)));
    }

    // The value arc, in rotarySliderFillColourId, with a soft glow.
    const auto arcRadius = (compact ? 44.0f : 40.0f) * u;
    const auto arcWidth = std::max (2.0f, (compact ? 7.0f : 4.5f) * u);
    g.setColour (knobTrack);
    g.strokePath (arc (arcRadius, startAngle, endAngle), roundStroke (arcWidth));

    if (std::abs (angle - fromAngle) > 0.001f)
    {
        const auto value = arc (arcRadius, std::min (fromAngle, angle), std::max (fromAngle, angle));
        auto colour = slider.findColour (juce::Slider::rotarySliderFillColourId);

        if (! enabled)
            colour = colour.withMultipliedAlpha (0.35f);
        else if (! compact)
        {
            g.setColour (colour.withMultipliedAlpha (controlGlow));
            g.strokePath (value, roundStroke (arcWidth * 2.2f));
        }

        g.setColour (colour);
        g.strokePath (value, roundStroke (arcWidth));
    }

    // The skirt, on its shadow.
    const auto skirt = circle ((compact ? 31.0f : 32.0f) * u);
    g.setColour (knobShadow);
    g.fillEllipse (circle (skirt.getWidth() * 0.5f, 3.5f * u));
    g.setGradientFill (juce::ColourGradient (knobSkirtTop, skirt.getTopLeft(), knobSkirtBottom, skirt.getBottomLeft(), false));
    g.fillEllipse (skirt);
    g.setColour (knobSkirtEdge);
    g.drawEllipse (skirt.reduced (0.5f), 1.0f);

    // The cap: lit from the top left, two bands of sheen from the bottom
    // left to the top right, and (on big knobs) faint turning rings.
    const auto cap = circle ((compact ? 22.0f : 24.0f) * u);
    juce::ColourGradient capFill (knobCapLight, cap.getX() + cap.getWidth() * 0.15f, cap.getY() + cap.getHeight() * 0.1f,
                                  knobCapDark, cap.getX() + cap.getWidth() * 0.85f, cap.getY() + cap.getHeight() * 0.95f, false);
    capFill.addColour (0.5, knobCapMid);
    g.setGradientFill (capFill);
    g.fillEllipse (cap);

    juce::ColourGradient sheenFill (knobSheen.withAlpha (0.0f), cap.getBottomLeft(), knobSheen.withAlpha (0.0f), cap.getTopRight(), false);
    sheenFill.addColour (0.32, knobSheen);
    sheenFill.addColour (0.5, knobSheen.withAlpha (0.0f));
    sheenFill.addColour (0.68, knobSheen.withMultipliedAlpha (0.8f));
    g.setGradientFill (sheenFill);
    g.fillEllipse (cap);

    if (slider.isMouseOverOrDragging() && enabled)
    {
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillEllipse (cap);
    }

    if (! compact)
    {
        g.setColour (knobRings);
        g.drawEllipse (circle (18.0f * u), 1.0f);
        g.drawEllipse (circle (11.0f * u), 1.0f);
        g.setColour (knobCapEdge);
        g.drawEllipse (cap, 1.0f);
    }

    // The pointer, drawn pointing up and turned to the value (getPointOnCircumference
    // and AffineTransform::rotation both go clockwise from 12 o'clock, like
    // the slider). A big knob has a bright mark on the skirt that runs into
    // an engraved groove across the cap; a compact one a plain bar.
    const auto turn = juce::AffineTransform::rotation (angle, c.x, c.y);
    auto line = [&] (float r0, float r1, float lineWidth, juce::Colour colour, float dx = 0.0f)
    {
        juce::Path p;
        p.startNewSubPath (c.x + dx, c.y - r0);
        p.lineTo (c.x + dx, c.y - r1);
        g.setColour (colour);
        g.strokePath (p, roundStroke (lineWidth), turn);
    };

    const auto pointer = enabled ? text : textMuted;

    if (compact)
    {
        line (29.0f * u, 14.0f * u, std::max (1.5f, 6.0f * u), pointer);
    }
    else
    {
        line (30.5f * u, 24.0f * u, std::max (1.5f, 3.2f * u), pointer);
        line (22.0f * u, 10.0f * u, std::max (1.2f, 2.6f * u), knobGroove);
        line (21.5f * u, 10.5f * u, std::max (0.6f, 0.8f * u), knobGrooveHighlight, 1.2f * u);
    }

    if (slider.hasKeyboardFocus (false))
    {
        g.setColour (focus);
        g.drawEllipse (skirt.expanded (1.5f), (float) theme::size::focusRing);
    }
}

//==============================================================================
void LithiumLookAndFeel::drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&)
{
    // A soft shadow, then the panel with its border, like the overlay cards.
    juce::DropShadow (juce::Colours::black.withAlpha (0.45f), 14, { 0, 3 }).drawForPath (g, path);
    g.setColour (theme::colours::panel);
    g.fillPath (path);
    g.setColour (theme::colours::borderStrong);
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

void LithiumLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto bounds = juce::Rectangle<int> (width, height).toFloat();
    g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (findColour (juce::TooltipWindow::outlineColourId));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.0f);
    g.setColour (findColour (juce::TooltipWindow::textColourId));
    g.setFont (theme::body (13.0f));
    g.drawFittedText (text, bounds.toNearestInt().reduced (8, 4), juce::Justification::centredLeft, 12);
}

juce::Rectangle<int> LithiumLookAndFeel::getTooltipBounds (const juce::String& text, juce::Point<int> screenPos,
                                                           juce::Rectangle<int> parentArea)
{
    const auto lines = juce::StringArray::fromLines (text);
    float widest = 0.0f;

    for (const auto& line : lines)
        widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidth (theme::body (13.0f), line));

    const auto w = juce::jmin (480, (int) widest + 20);
    const auto h = 10 + 17 * juce::jmax (1, lines.size());
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6,
                                 w, h)
        .constrainedWithin (parentArea);
}

} // namespace lithium
