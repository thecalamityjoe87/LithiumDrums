#include "LithiumLookAndFeel.h"

#include "LithiumAssets.h"

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
    using namespace LithiumAssets;
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
}

juce::Font LithiumLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return theme::label (juce::jmin (14.0f, (float) buttonHeight * 0.5f), theme::Weight::semibold);
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
void LithiumLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float, float, juce::Slider::SliderStyle style,
                                           juce::Slider& slider)
{
    if (style == juce::Slider::LinearVertical)
    {
        drawFader (g, { x, y, width, height }, sliderPos, slider);
        return;
    }

    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, 0, 0, style, slider);
        return;
    }

    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto track = bounds.withSizeKeepingCentre (bounds.getWidth(), 4.0f);
    const auto enabled = slider.isEnabled();

    g.setColour (slider.findColour (juce::Slider::backgroundColourId));
    g.fillRoundedRectangle (track, 2.0f);

    auto fillColour = slider.findColour (juce::Slider::trackColourId);
    g.setColour (enabled ? fillColour : fillColour.withMultipliedAlpha (0.4f));
    g.fillRoundedRectangle (track.withRight (sliderPos), 2.0f);

    // Unity-gain tick, when the slider has one (volume faders).
    if (const auto unity = slider.getProperties()["unityValue"]; ! unity.isVoid())
    {
        const auto ux = (float) slider.getPositionOfValue ((double) unity);
        g.setColour (theme::colours::borderStrong);
        g.fillRect (juce::Rectangle<float> (ux - 0.5f, track.getY() - 3.0f, 1.0f, track.getHeight() + 6.0f));
    }

    const auto thumb = juce::Rectangle<float> (6.0f, 14.0f).withCentre ({ sliderPos, bounds.getCentreY() });
    g.setColour (slider.findColour (juce::Slider::thumbColourId).withMultipliedAlpha (enabled ? 1.0f : 0.5f));
    g.fillRoundedRectangle (thumb, 2.0f);
}

int LithiumLookAndFeel::getSliderThumbRadius (juce::Slider& slider)
{
    // The fader cap is taller than the horizontal slider's thumb; JUCE keeps
    // this much room at each end of the track.
    return slider.getSliderStyle() == juce::Slider::LinearVertical ? 8 : 6;
}

void LithiumLookAndFeel::drawFader (juce::Graphics& g, juce::Rectangle<int> area, float sliderPos, juce::Slider& slider)
{
    using namespace theme::colours;

    const auto bounds = area.toFloat();
    const auto track = bounds.withSizeKeepingCentre (4.0f, bounds.getHeight());
    const auto enabled = slider.isEnabled();

    g.setColour (slider.findColour (juce::Slider::backgroundColourId));
    g.fillRoundedRectangle (track, 2.0f);

    auto fillColour = slider.findColour (juce::Slider::trackColourId);
    g.setColour (enabled ? fillColour.withMultipliedAlpha (0.8f) : fillColour.withMultipliedAlpha (0.3f));
    g.fillRoundedRectangle (track.withTop (sliderPos), 2.0f);

    if (const auto unity = slider.getProperties()["unityValue"]; ! unity.isVoid())
    {
        const auto uy = (float) slider.getPositionOfValue ((double) unity);
        g.setColour (borderStrong);
        g.fillRect (juce::Rectangle<float> (track.getX() - 6.0f, uy - 0.5f, track.getWidth() + 12.0f, 1.0f));
    }

    // The cap: a small raised block with a line across its middle.
    const auto cap = juce::Rectangle<float> (std::min (26.0f, bounds.getWidth()), 14.0f).withCentre ({ bounds.getCentreX(), sliderPos });
    g.setColour (raisedHigh.brighter (0.25f).withMultipliedAlpha (enabled ? 1.0f : 0.5f));
    g.fillRoundedRectangle (cap, 3.0f);
    g.setColour (borderStrong.brighter (0.3f));
    g.drawRoundedRectangle (cap.reduced (0.5f), 3.0f, 1.0f);
    g.setColour (text);
    g.fillRect (cap.withSizeKeepingCentre (cap.getWidth() - 8.0f, 1.5f));
}

void LithiumLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                                           float startAngle, float endAngle, juce::Slider& slider)
{
    using namespace theme::colours;

    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto size = std::min (bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    const auto radius = size * 0.5f - 2.0f;
    const auto thickness = std::max (2.0f, size * 0.08f);
    const auto enabled = slider.isEnabled();
    const auto angle = startAngle + position * (endAngle - startAngle);

    // Bipolar knobs (gain, pan) draw their arc from the centre, the rest from the start.
    const auto bipolar = (bool) slider.getProperties()["bipolar"];
    const auto fromAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

    auto arc = [&] (float a0, float a1)
    {
        juce::Path p;
        p.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, a0, a1, true);
        return p;
    };

    const juce::PathStrokeType stroke (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour (raisedHigh.brighter (0.1f));
    g.strokePath (arc (startAngle, endAngle), stroke);

    const auto colour = slider.findColour (juce::Slider::rotarySliderFillColourId);
    g.setColour (enabled ? colour : colour.withMultipliedAlpha (0.35f));
    g.strokePath (arc (std::min (fromAngle, angle), std::max (fromAngle, angle)), stroke);

    // The knob body and its pointer.
    const auto body = radius - thickness - 2.0f;
    g.setColour (slider.isMouseOverOrDragging() && enabled ? raisedHigh.brighter (0.12f) : raisedHigh);
    g.fillEllipse (juce::Rectangle<float> (body * 2.0f, body * 2.0f).withCentre (centre));
    g.setColour (border);
    g.drawEllipse (juce::Rectangle<float> (body * 2.0f, body * 2.0f).withCentre (centre), 1.0f);

    const auto tip = centre.getPointOnCircumference (body - 1.5f, angle);
    const auto root = centre.getPointOnCircumference (body * 0.35f, angle);
    g.setColour (enabled ? text : textMuted);
    g.drawLine ({ root, tip }, std::max (1.5f, size * 0.05f));
}

//==============================================================================
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
