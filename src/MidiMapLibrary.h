#pragma once

#include "Settings.h"
#include "engine/MidiMap.h"

#include <vector>

// The MIDI maps to choose from: the ones built into the plugin (Lithium
// Standard, Kit default, General MIDI, Roland V-Drums, Alesis) and the ones
// the user has saved, one JSON file each in the MIDI Maps folder (see
// Settings::getMidiMapsFolder).
//
// A map's id says where it came from, so "Revert" can find it again: a
// factory map's own id ("roland-vdrums"), or "user:" plus the file name for
// a saved one. Message thread (it reads and writes files).

namespace lithium::midimaps
{

inline constexpr const char* userPrefix = "user:";

struct Item
{
    MidiMap map;
    bool factory = true;
    juce::File file;             // saved maps only
};

/** The factory maps in menu order, then the saved maps sorted by name. */
std::vector<Item> scan();

/** The map with the given id, as saved (or built in). */
std::optional<MidiMap> find (const juce::String& id);

/** Saves a map under a name, overwriting a saved map of the same name.
    Returns the saved map (with its new id and name), or a failure. */
juce::Result save (const MidiMap&, const juce::String& name, MidiMap& saved);

/** Deletes a saved map. Factory maps can't be deleted. */
bool remove (const juce::String& id);

bool isFactory (const juce::String& id);

/** The map new instances start with: LITHIUM_MIDI_MAP (a factory id, for
    the tests), else the one saved with "Set as default", else Lithium
    Standard, which means the same on every kit. */
MidiMap loadDefault (const Settings&);

} // namespace lithium::midimaps
