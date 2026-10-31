#pragma once

#include "engine/KitSchema.h"

#include <juce_graphics/juce_graphics.h>

// Lithium Drums' visual identity in one place: colours, sizes and fonts.
// Components take everything from here (or from the LookAndFeel), never from
// literals of their own.

namespace lithium::theme
{

namespace colours
{
    inline const juce::Colour background   { 0xff0e1318 };
    inline const juce::Colour panel        { 0xff141b22 };
    inline const juce::Colour raised       { 0xff18212a };
    inline const juce::Colour raisedHigh   { 0xff1d2833 };
    inline const juce::Colour border       { 0xff22303b };
    inline const juce::Colour borderStrong { 0xff2a3946 };
    inline const juce::Colour text         { 0xffe6edf2 };
    inline const juce::Colour textMuted    { 0xff93a3b1 };
    inline const juce::Colour accent       { 0xff3cc4d6 };

    inline const juce::Colour groupKick    { 0xfff07167 };
    inline const juce::Colour groupSnare   { 0xff3cc4d6 };
    inline const juce::Colour groupHats    { 0xff8bd17c };
    inline const juce::Colour groupToms    { 0xffb39dff };
    inline const juce::Colour groupCymbals { 0xffe7d27c };
}

inline juce::Colour groupColour (PieceGroup g)
{
    switch (g)
    {
        case PieceGroup::kick:    return colours::groupKick;
        case PieceGroup::snare:   return colours::groupSnare;
        case PieceGroup::hats:    return colours::groupHats;
        case PieceGroup::toms:    return colours::groupToms;
        case PieceGroup::cymbals: return colours::groupCymbals;
        case PieceGroup::other:   return colours::textMuted;   // percussion beyond the drum kit
    }

    return colours::text;
}

// Layout at 1x. The whole UI is laid out at this size and scaled as one.
inline constexpr int windowWidth = 1280;
inline constexpr int windowHeight = 800;
inline constexpr int headerHeight = 56;
inline constexpr int stageHeight = 520;
inline constexpr float cornerRadius = 6.0f;

enum class Weight { regular, medium, semibold, bold };

/** Barlow, for body text. */
juce::Font body (float size, Weight = Weight::regular);

/** Barlow Condensed, for uppercase section labels, tabs and the wordmark. */
juce::Font label (float size, Weight = Weight::semibold);

/** IBM Plex Mono, for numbers. */
juce::Font mono (float size, Weight = Weight::regular);

/** Draws text with extra letter spacing (tracking), in em. */
void drawTrackedText (juce::Graphics&, const juce::String&, juce::Font, float tracking,
                      juce::Rectangle<float> area, juce::Justification);

float getTrackedTextWidth (const juce::String&, juce::Font, float tracking);

} // namespace lithium::theme
