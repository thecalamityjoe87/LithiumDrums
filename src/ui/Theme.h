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
    inline const juce::Colour warning      { 0xfff07167 };   // the kick red, for things that need attention
    inline const juce::Colour focus        { 0xff3cc4d6 };   // the keyboard-focus outline (the accent)

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

// Layout at 1x. The whole UI is laid out at this size and scaled as one
// (see LithiumDrumsEditor), so every size here is in these logical pixels at
// any window size or display scale.
inline constexpr int windowWidth = 1280;
inline constexpr int windowHeight = 800;
inline constexpr int headerHeight = 56;
inline constexpr int stageHeight = 744;   // the rendered stage fills the Kit tab (assets/kit-renders)
inline constexpr float cornerRadius = 6.0f;

/** The spacing scale: every margin, padding and gap is one of these. */
namespace space
{
    inline constexpr int xxs = 2, xs = 4, sm = 8, md = 12, lg = 16, xl = 24, xxl = 32;
    inline constexpr int page = lg;          // a page's outer margin
    inline constexpr int panelGap = sm;      // between cards and modules
}

/** Control sizes: controls of one kind are the same size everywhere. */
namespace size
{
    inline constexpr int button = 28;        // text buttons, drop-downs and toggles
    inline constexpr int buttonCompact = 18; // inside mixer strips
    inline constexpr int icon = 30;          // round icon buttons in the header
    inline constexpr int knob = 36;          // a labelled knob's dial
    inline constexpr int knobLarge = 56;     // the main level knobs (a drum's volume, a channel's fader)
    inline constexpr int knobCompact = 22;   // unlabelled knobs on mixer strips
    inline constexpr int faderTravel = 120;  // mixer faders: the cap's full movement, never stretched (a short bank, like AD2's)
    inline constexpr int faderCap = 16;      // the cap's height (JUCE keeps half of it free at each end)
    inline constexpr int meterWidth = 6;     // a mono channel meter; stereo is two of these
    inline constexpr int mixerStrip = 80;    // every mixer strip, so the bank reads as one row
    inline constexpr int focusRing = 2;      // the keyboard-focus outline
}

/** The type scale, in CSS-style em sizes (see body(), label(), mono()). */
namespace type
{
    inline constexpr float caption = 11.0f;  // section labels on controls, readout captions
    inline constexpr float small = 12.5f;    // secondary text, chips, tags
    inline constexpr float body = 14.0f;     // most text
    inline constexpr float heading = 15.0f;  // card and strip names
    inline constexpr float title = 21.0f;    // page headings (the groove's name)
}

/** How long the UI's short animations take. */
namespace motion
{
    inline constexpr double hitFlashMs = 140.0;
    inline constexpr double fadeMs = 120.0;
}

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
