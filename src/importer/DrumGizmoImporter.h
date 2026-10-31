#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

// Converts DrumGizmo kits into Lithium Drums' native kit format.
//
// A DrumGizmo kit is a folder with one or more drumkit XML files (variants of
// the kit), instrument XML files listing each instrument's samples with a
// "power" value, multichannel WAV files, and MIDI maps. See
// docs/kit-format.md ("Importing DrumGizmo kits") for how each part is
// converted, in particular how power values become velocity zones.
//
// This only depends on juce_core and juce_audio_formats, so the command-line
// tool, the tests and (later) the plugin can all use it.

namespace lithium::importer
{

struct Options
{
    juce::File source;             // a drumkit XML file, or a folder: every drumkit XML in it is imported
    juce::File kitsFolder;         // each imported kit goes into its own subfolder of this
    juce::File mappingFile;        // optional; otherwise a built-in mapping for known kits, or automatic

    int maxZones = 8;              // velocity zones per articulation, at most
    int minRoundRobins = 3;        // a zone gets at least this many samples (when there are enough)

    bool overwrite = false;        // replace kits that already exist
    bool copyAudio = false;        // copy audio even where a hard link would do

    std::function<void (const juce::String&)> log;
};

struct ImportedKit
{
    juce::String name;
    juce::File folder;
    int numPieces = 0, numMics = 0, numHits = 0, numFiles = 0;
    std::int64_t audioBytes = 0;
    bool linked = false;           // audio hard-linked rather than copied
    juce::StringArray warnings;
};

/** Imports every drumkit XML the options point at. Stops at the first error. */
juce::Result importKits (const Options&, std::vector<ImportedKit>& results);

//==============================================================================
// The velocity-zone conversion, exposed for tests and documentation.

struct ZoneSplit
{
    int velocityLow = 1, velocityHigh = 127;
    float velocityRangeDb = 0.0f;
    std::vector<int> samples;      // indices into the input, quietest first
};

/** Groups samples into velocity zones from their DrumGizmo power values.

    Power maps linearly onto velocity 1-127 across the instrument's range, as
    DrumGizmo itself does. The sorted values are then split into at most
    maxZones contiguous groups by optimal 1-D clustering (each group as tight
    as possible), with at least minRoundRobins samples per group, and each
    zone's velocity range reaches halfway to its neighbours. */
std::vector<ZoneSplit> makeZones (const std::vector<double>& powers, int maxZones, int minRoundRobins);

} // namespace lithium::importer
