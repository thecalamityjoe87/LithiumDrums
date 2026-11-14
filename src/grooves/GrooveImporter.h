#pragma once

#include <juce_core/juce_core.h>

#include <functional>

// Converts a downloaded groove library into a groove pack (see
// GrooveLibrary.h and docs/grooves.md). One library is understood so far:
// the Groove MIDI Dataset (Google Magenta, CC BY 4.0), 1,150 performances by
// ten drummers recorded on a Roland TD-11, with an info.csv describing each.
//
// The MIDI files are copied as they were recorded; the importer only works
// out what the files don't say: names, how many bars each take is, and
// which fills go with which groove.

namespace lithium::grooves
{

/** Whether a folder is the Groove MIDI Dataset (its "groove" folder, or the
    folder the zip was unpacked into, which holds that). */
bool isGrooveMidiDataset (const juce::File& folder);

struct ImportOptions
{
    juce::File source;
    juce::File groovesFolder;      // the pack becomes a subfolder of this
    int phraseBars = 4;            // how long the page's phrases are
    bool overwrite = false;
    std::function<void (const juce::String&)> log;
};

struct ImportedPack
{
    juce::String name;
    juce::File folder;
    int numGrooves = 0, numFills = 0, numPhrases = 0;
    juce::StringArray warnings;
};

juce::Result importGrooveMidiDataset (const ImportOptions&, ImportedPack& result);

//==============================================================================
// Any folder of standard MIDI files (.mid, .midi), such as a groove pack
// bought or downloaded elsewhere, for the user's own use. Nothing about the
// files is known beyond what they say, so:
//
// - Styles come from the folders: the first folder under the one chosen is
//   the style ("Metal/Fast/blast 01.mid" is Metal, with Fast as its
//   substyle). Folders that only say what's in them ("MIDI", "Grooves",
//   "Fills", "Beats") are skipped; files with no other folder take the
//   pack's name as their style.
// - Tempo and meter are the file's first tempo and time signature (120 BPM
//   and 4/4 without them).
// - A file with "fill" in its name, or in one of its folders, is a fill. It
//   goes with every groove in its folder, or failing that in the nearest
//   folder above it that has grooves. Fills with no groove anywhere above
//   them are grooves of their own.
//
// The pack is named by the user and reads its notes through the map they
// pick (General MIDI by default).

struct MidiFolderOptions
{
    juce::File source;
    juce::File groovesFolder;
    juce::String name;                     // the pack's name; empty = the source folder's
    juce::String noteMap = "general-midi"; // a factory map id (see MidiMap::factoryMaps)
    int phraseBars = 4;
    bool overwrite = false;
    std::function<void (const juce::String&)> log;
};

/** The folder a pack of this name is written to, so a caller can ask before
    replacing one. */
juce::File packFolderFor (const juce::File& groovesFolder, const juce::String& name);

juce::Result importMidiFolder (const MidiFolderOptions&, ImportedPack& result);

} // namespace lithium::grooves
