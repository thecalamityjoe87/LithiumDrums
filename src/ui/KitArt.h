#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The rendered kit's pictures, and each kit's own finish of them.
//
// assets/renders/ holds them as WebP (made from the PNG originals in
// assets/render-sources/ by scripts/make-webp.sh): stage/ has the stage's
// layers (kick.webp, tom1.webp, ...) in the default finish, and slots/ every
// slot card's picture (slot-kick.webp, ...). Kits with a finish of their own
// (assets/kit-finishes.json) also have their drums rendered in it:
// finishes/<kit id>/finish-<kit id>-kick.webp for the stage and
// slots/finish-<kit id>-slot-kick.webp for the cards, cropped to exactly the
// same rectangles, so a kit can mix pieces from several finishes (a swapped
// piece shows the finish of the kit it came from). The build embeds them by
// file name alone, so the names stay unique across the folders.

namespace lithium::kitart
{

/** An embedded image, by its file name (e.g. "slot-kick.webp"). Decoded once
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
