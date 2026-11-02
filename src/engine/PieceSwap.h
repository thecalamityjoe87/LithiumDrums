#pragma once

#include "KitManifest.h"

#include <juce_data_structures/juce_data_structures.h>

#include <optional>
#include <vector>

// Piece swapping: playing one of a kit's pieces with another kit's
// recordings of that kind of drum (MuldjordKit's snare in DRSKit, say).
//
// A swap is applied to the kit's manifest before the kit loads: the piece
// keeps its id, name, role and slot (so its parameters, stage place and MIDI
// notes stay put), but its articulations become the other kit's, with their
// mic sources mapped onto this kit's mics. From then on the loader and the
// engine see an ordinary kit whose piece reads its audio from another folder.
// The mapping rules are in docs/kit-format.md ("Piece swapping").
//
// The same list also fills and empties the Kit tab's slots (KitSlots.h). A
// swap whose piece is an empty slot's id ("slot-tom2") adds a piece there,
// played the same way; one marked `removed` takes the kit's own piece out.

namespace lithium
{

struct PieceSwap
{
    juce::String piece;          // the id of this kit's piece it replaces, or of the empty slot it fills (kitslots::addedPieceId())
    juce::String sourceKit;      // the id of the kit the recordings come from
    juce::String sourcePiece;    // the piece's id in that kit
    bool ambience = true;        // bring its overhead and room mics too, not just its close mics
    bool removed = false;        // instead: leave the slot empty (no source)

    bool operator== (const PieceSwap&) const = default;
};

using PieceSwaps = std::vector<PieceSwap>;

namespace swaps
{
    /** Whether a piece of one role can stand in for another: the same role,
        except that rack and floor toms can stand in for each other. */
    bool canReplace (PieceRole target, PieceRole source) noexcept;

    /** Where each of the source kit's mics goes when sourcePiece replaces
        targetPiece: an index into target.mics, or -1 if it's left out. */
    std::vector<int> mapMics (const KitManifest& source, int sourcePiece, const KitManifest& target, int targetPiece,
                              bool ambience);

    /** The close mics that recorded a piece itself (rather than picking it
        up from across the kit), worked out from the mics' names. */
    std::vector<int> ownMics (const KitManifest&, int piece);

    /** Puts a parsed kit's pieces in their slots (PieceManifest::slot,
        KitManifest::slotOwners), then applies the swaps, reading the other
        kits' manifests from the kits folder: replacing pieces, filling empty
        slots (added at the end of the kit, so the pieces before keep their
        places) and removing pieces. Swaps for pieces the kit doesn't have,
        or for slots that aren't empty, are ignored. A swap that can't be
        made (its kit isn't installed any more, say) leaves the piece as it
        was, with the reason in its swapProblem; a slot it would have filled
        stays empty. Not for the audio thread: it reads files. */
    void apply (KitManifest&, const PieceSwaps&);

    /** The swap for a piece id, if there is one. A copy rather than a
        pointer into the list, because the list is usually a copy too
        (LithiumDrumsProcessor::getPieceSwaps() returns one by value): a
        pointer into it would dangle as soon as that copy went away. */
    std::optional<PieceSwap> find (const PieceSwaps&, const juce::String& pieceId);

    /** Sets the swap for swap.piece, or with an empty sourceKit (and not
        `removed`), takes it off the list: the kit's own piece, or an empty slot. */
    void set (PieceSwaps&, const PieceSwap&);

    /** For saving with projects and presets. */
    juce::ValueTree toValueTree (const PieceSwaps&);
    PieceSwaps fromValueTree (const juce::ValueTree&);

    inline const juce::Identifier treeType { "swaps" };
}

} // namespace lithium
