#include "Logo.h"

#include "Theme.h"

namespace lithium::logo
{

namespace
{
    // The stick's half-width along its length, both as fractions of its
    // length (0 = butt, 1 = end of the tip).
    constexpr float shaft = 0.060f;      // half-width of the straight shaft
    constexpr float taperStart = 0.54f;  // where it starts to narrow
    constexpr float neck = 0.027f;       // half-width just below the tip
    constexpr float tipLength = 0.17f;   // the oval tip, end to end
    constexpr float tipWidth = 0.050f;   // its half-width at the widest

    float smoothstep (float t)
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        return t * t * (3.0f - 2.0f * t);
    }

    float halfWidth (float x)
    {
        const auto tipStart = 1.0f - tipLength;

        if (x < taperStart)
            return shaft;

        if (x < tipStart)
            return juce::jmap (smoothstep ((x - taperStart) / (tipStart - taperStart)), shaft, neck);

        // An ellipse centred in the tip. Its back half meets the neck, so the
        // neck width holds until the ellipse is wider than it.
        const auto u = (x - (1.0f - tipLength * 0.5f)) / (tipLength * 0.5f);
        const auto oval = tipWidth * std::sqrt (std::max (0.0f, 1.0f - u * u));
        return u < 0.0f ? std::max (neck, oval) : oval;
    }
}

juce::Path drumstick (juce::Point<float> butt, juce::Point<float> tip)
{
    const auto length = butt.getDistanceFrom (tip);
    const auto along = (tip - butt) / length;
    const juce::Point<float> across (-along.y, along.x);
    const auto at = [&] (float x, float y) { return butt + along * (x * length) + across * (y * length); };

    // One side from the butt to the tip, back down the other side, then a
    // half circle round the butt.
    constexpr int steps = 160;
    const auto start = shaft;   // the butt cap's centre
    juce::Path p;

    for (int i = 0; i <= steps; ++i)
    {
        const auto x = start + (1.0f - start) * (float) i / steps;
        const auto point = at (x, -halfWidth (x));
        i == 0 ? p.startNewSubPath (point) : p.lineTo (point);
    }

    for (int i = steps; i >= 0; --i)
    {
        const auto x = start + (1.0f - start) * (float) i / steps;
        p.lineTo (at (x, halfWidth (x)));
    }

    for (int i = 1; i < 24; ++i)
    {
        const auto a = juce::MathConstants<float>::halfPi + juce::MathConstants<float>::pi * (float) i / 24.0f;
        p.lineTo (at (start + shaft * std::cos (a), shaft * std::sin (a)));
    }

    p.closeSubPath();
    return p;
}

void drawMark (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour stick, juce::Colour background)
{
    const auto c = area.getCentre();
    const auto r = std::min (area.getWidth(), area.getHeight()) * 0.5f;

    // Left stick (tip top-left) first, then the right one over it.
    for (const auto angle : { -0.25f * juce::MathConstants<float>::pi, 0.25f * juce::MathConstants<float>::pi })
    {
        const juce::Point<float> dir (std::sin (angle), -std::cos (angle));
        const auto path = drumstick (c - dir * r * 0.98f, c + dir * r * 0.98f);

        if (angle > 0.0f)
        {
            g.setColour (background);
            g.strokePath (path, juce::PathStrokeType (r * 0.09f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour (stick);
        g.fillPath (path);
    }
}

juce::Image renderIcon (int size)
{
    using namespace theme;

    juce::Image image (juce::Image::ARGB, size, size, true);
    juce::Graphics g (image);

    const auto s = (float) size;
    const auto tile = juce::Rectangle<float> (s, s).reduced (s * 0.04f);
    const auto corner = s * 0.2f;

    // Raised-panel blue at the top to the app background at the bottom.
    g.setGradientFill (juce::ColourGradient (colours::raisedHigh, tile.getTopLeft(), colours::background, tile.getBottomLeft(), false));
    g.fillRoundedRectangle (tile, corner);
    g.setColour (colours::borderStrong);
    g.drawRoundedRectangle (tile.reduced (s * 0.006f), corner, std::max (1.0f, s * 0.012f));

    // The gap colour is the tile's colour behind the crossing (about halfway down).
    drawMark (g, tile.withSizeKeepingCentre (s * 0.66f, s * 0.66f), colours::accent,
              colours::raisedHigh.interpolatedWith (colours::background, 0.5f));
    return image;
}

}
