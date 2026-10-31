#pragma once

#include "KitSchema.h"

#include <juce_core/juce_core.h>

#include <vector>

// Note-to-articulation tables, loaded from JSON so new maps are data rather
// than code. There are two kinds:
//
//  - A kit's own default map (in its kit.json) names the kit's piece IDs
//    directly: MidiMapEntry.
//  - Kit-independent maps (General MIDI now, Roland and Alesis e-kit maps
//    later) can't know a kit's piece IDs, so they name a role and which piece
//    of that role: "the second rack tom", "the first ride, bell". MidiMap
//    holds those, and KitLoader resolves them against each kit.
//
// Either way the loader turns the map into a flat 128-entry lookup table the
// audio thread can use directly.

namespace lithium
{

struct MidiMapEntry
{
    int note = 0;
    juce::String piece;
    juce::String articulation;    // empty = the piece's first articulation
};

struct RoleMapEntry
{
    int note = 0;
    PieceRole role = PieceRole::other;
    int nth = 0;                  // which piece of the role; past the last one means the last one
    juce::String articulation;    // a standard articulation id; the piece's first articulation if it lacks it
};

struct MidiMap
{
    juce::String name;
    std::vector<RoleMapEntry> entries;

    static juce::Result parse (const juce::String& json, MidiMap& result);

    /** The built-in General MIDI drum map (assets/midi-maps/general-midi.json). */
    static MidiMap generalMidi();
};

} // namespace lithium
