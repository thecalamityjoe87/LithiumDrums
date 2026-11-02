#pragma once

#include "Settings.h"
#include "engine/PieceSwap.h"

#include <juce_data_structures/juce_data_structures.h>

#include <vector>

// Presets: the whole sound of the plugin saved under a name. A preset holds
// the kit (by id, with its folder as a fallback), the pieces swapped in from
// other kits, and every parameter: the mixer, the reverb and each piece's
// shaping. It doesn't hold the MIDI map (that's the player's setup, not the
// sound) or the Grooves page's state.
//
// Presets are files, one per preset, named after the preset:
//
//   <user presets folder>/Rock Room.lithiumpreset
//   <factory presets folder>/Studio Kit.lithiumpreset   (read-only, shipped later)
//
// The file is XML, so it can be read and diffed by hand.

namespace lithium::presets
{

inline constexpr auto fileExtension = ".lithiumpreset";
inline constexpr int formatVersion = 1;

struct Entry
{
    juce::String name;
    juce::File file;
    bool factory = false;
};

/** Every preset: the user's, then the factory ones, each sorted by name. */
std::vector<Entry> scan();

/** What a preset file holds. */
struct Contents
{
    juce::String kitId, kitPath, kitName;
    PieceSwaps swaps;
    juce::ValueTree parameters;            // the parameters' state, as the APVTS keeps it
};

juce::Result read (const juce::File&, Contents&);
juce::Result write (const juce::File&, const Contents&);

/** The preset's name: its file name. */
juce::String nameOf (const juce::File&);

/** Whether a preset is a factory one (and so can't be renamed, overwritten or deleted). */
bool isFactory (const juce::File&);

/** The file a user preset of this name is saved to. The name is made safe
    for a file name; empty if nothing is left of it. */
juce::File userFileFor (const juce::String& name);

} // namespace lithium::presets
