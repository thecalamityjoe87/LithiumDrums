#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

// Converts downloaded drum libraries into Lithium Drums' native kit format.
//
// Two source formats are understood: DrumGizmo kits (DrumGizmoImporter) and
// SFZ instruments (SfzImporter). Both turn their source into the same
// in-memory model (KitWriter.h), which KitWriter writes out as a content pack:
// a folder in the kits folder holding the converted kits, any kit presets,
// and the licence. See docs/kit-format.md.
//
// This only depends on juce_core and juce_audio_formats, so the command-line
// tool, the tests and (later) the plugin can all use it.

namespace lithium::importer
{

struct Options
{
    juce::File source;             // a drumkit XML or .sfz file, or a folder holding them
    juce::File kitsFolder;         // each import becomes a content pack (a subfolder) of this
    juce::File mappingFile;        // optional; otherwise a built-in mapping for known kits, or automatic

    int maxZones = 8;              // DrumGizmo: velocity zones per articulation, at most
    int minRoundRobins = 3;        // DrumGizmo: a zone gets at least this many samples (when there are enough)

    bool overwrite = false;        // replace kits that already exist
    bool copyAudio = false;        // copy audio even where a hard link would do

    std::function<void (const juce::String&)> log;
};

struct ImportedKit
{
    juce::String name;
    juce::File folder;
    bool isPreset = false;
    int numPieces = 0, numMics = 0, numHits = 0, numFiles = 0;
    std::int64_t audioBytes = 0;   // on disk
    std::int64_t ramBytes = 0;     // sample data in RAM with every mic loaded
    bool linked = false;           // audio hard-linked rather than copied
    juce::StringArray warnings;
};

/** Imports what the options point at, detecting the source format. Stops at
    the first error. */
juce::Result importKits (const Options&, std::vector<ImportedKit>& results);

} // namespace lithium::importer
