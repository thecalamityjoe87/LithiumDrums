#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The rendered kit's pictures, and each kit's own finish of them.
//
// assets/kit-renders holds the stage's layers (kick.png, tom1.png, ...) and
// the slot cards' pictures (slot-kick.png, ...) in the default finish. Kits
// with a finish of their own (assets/kit-finishes.json) also have their
// drums rendered in it: finish-<kit id>-kick.png for the stage and
// finish-<kit id>-slot-kick.png for the cards, cropped to exactly the same
// rectangles, so a kit can mix pieces from several finishes (a swapped
// piece shows the finish of the kit it came from).

namespace lithium::kitart
{

/** An embedded image, by its file name in assets/kit-renders. Decoded once
    (juce::ImageCache keeps it). Null if there's no such file. Message thread. */
juce::Image load (const juce::String& fileName);

/** The finish a kit's pieces are drawn in: its id, or the kit it shares a
    finish with (an alias in kit-finishes.json). */
juce::String finishFor (const juce::String& kitId);

/** A stage layer ("kick", "tom1", ...) in a kit's finish, or the default. */
juce::Image stageLayer (const juce::String& kitId, const juce::String& renderKey);

/** A slot card's picture ("kick", "splash", ...) in a kit's finish, or the default. */
juce::Image slotPicture (const juce::String& kitId, const juce::String& name);

/** The hit flash, as on the Kit tab's stage: a brightened copy of a drum's
    picture, drawn over it at 0.8 opacity and faded out over flashDurationMs. */
juce::Image makeFlashImage (const juce::Image&);
inline constexpr double flashDurationMs = 240.0;

} // namespace lithium::kitart
