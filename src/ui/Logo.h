#pragma once

#include <juce_graphics/juce_graphics.h>

// The Lithium Drums logo mark (two drumsticks crossed in an X) and the app
// icon built from it. The header, the icon generator (tools/icongen) and the
// Linux installer all draw it from here, so the logo and the icon can't drift
// apart. Plain juce_graphics, no component code: safe to use off the message
// thread for rendering into an Image.

namespace lithium::logo
{

/** The outline of one drumstick from its butt to the end of its tip: a
    straight shaft with a rounded butt, a taper down to a narrow neck, and an
    oval tip. The proportions are stylised (a real stick is about 28 times as
    long as it is wide) so that the taper and tip still read at 26 px. */
juce::Path drumstick (juce::Point<float> butt, juce::Point<float> tip);

/** Two sticks crossed in an X, tips up, filling `area` (best square).
    The right-hand stick lies on top; `background` is painted in a thin
    outline round it so the crossing reads as one stick over the other. */
void drawMark (juce::Graphics&, juce::Rectangle<float> area, juce::Colour stick, juce::Colour background);

/** The app icon at `size` x `size` pixels: the mark on a dark rounded tile,
    transparent outside the tile. */
juce::Image renderIcon (int size);

}
