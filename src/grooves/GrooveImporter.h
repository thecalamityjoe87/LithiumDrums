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

} // namespace lithium::grooves
