#pragma once

#include "KitSchema.h"

#include <juce_core/juce_core.h>

#include <array>
#include <vector>

// The Kit tab's slots: a fixed set of places for a kit's pieces, the same
// for every kit, so the page never changes shape and a kit's empty slots
// can be filled with pieces from other kits (see PieceSwap.h).
//
// Fifteen slots are named after the kind of piece they hold (Kick, Snare,
// Tom 1...). The nine Extra slots take any piece, and hold whatever a kit
// has beyond the named ones (a second kick, a cowbell, a gong), in the kit's
// order. Every installed library fits: the most a kit needs is all nine
// extras (Basalt Cobblestone's percussion).

namespace lithium::kitslots
{

enum class Kind { kick, snare, hihat, tom, floorTom, crash, splash, ride, china, extra };

struct Slot
{
    const char* id;      // saved in swaps that fill it ("slot-tom2"); never change one
    const char* name;    // shown on its card
    Kind kind;
};

inline constexpr int numSlots = maxPieces;   // every piece of a kit has a slot

const std::array<Slot, numSlots>& all();

/** The slot with this id, or -1. */
int find (const juce::String& slotId);

/** The base kit's piece in each slot, for the UI: what "the kit's own" is
    when a slot was emptied or swapped. An empty id: the kit has nothing there. */
struct Owner
{
    juce::String id, name;
};

using Owners = std::array<Owner, numSlots>;

/** Whether a piece could be put in a slot by the user: the same kind, with
    rack and floor toms interchangeable and any crash or splash in a crash
    or splash slot. Extra slots take anything. */
bool accepts (Kind, PieceRole, bool isSplash) noexcept;

/** The role a piece added to a slot plays as (a rack tom in a floor tom's
    slot is a floor tom, for its MIDI notes and its place on the stage), or
    for an Extra slot, the piece's own. */
PieceRole roleFor (Kind, PieceRole own) noexcept;

/** A kit's own pieces into slots, given in the kit's order: each to the
    first free slot of its own kind (a splash to Splash, a rack tom to the
    next Tom), anything else to the next Extra. -1 for a piece that doesn't
    fit (only possible beyond maxPieces). */
struct PieceKind
{
    PieceRole role;
    bool isSplash;
};

std::vector<int> assign (const std::vector<PieceKind>&);

/** Whether a piece is a splash, from its id or name. */
bool isSplash (PieceRole, const juce::String& id, const juce::String& name);

/** The id a piece added to an empty slot gets: "slot-" and the slot's id.
    It's also the swap's piece id (PieceSwap::piece). */
juce::String addedPieceId (int slot);

/** The slot an added piece's id names, or -1 if it isn't one. */
int slotOfAddedPiece (const juce::String& pieceId);

} // namespace lithium::kitslots
