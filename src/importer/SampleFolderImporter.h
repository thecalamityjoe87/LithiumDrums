#pragma once

#include "Importer.h"
#include "KitWriter.h"

// Converts a folder of one-shot WAV files into Lithium Drums' kit format:
// libraries that come as plain samples, one file per hit, without an SFZ or
// DrumGizmo description (The Open Source Drum Kit).
//
// A mapping (format "samples") says which files make each articulation, by
// wildcard. The importer measures each file's peak level, sorts the hits from
// quietest to loudest and spreads them over velocity zones exactly as the
// DrumGizmo importer does with its power values (makeZones), so the quietest
// hits play at low velocities and every zone has round-robins. See
// docs/kit-format.md ("Importing folders of samples").

namespace lithium::importer
{

/** True if the folder holds a library a built-in "samples" mapping knows,
    recognised by the mapping's "marker" file. */
bool isSampleFolderSource (const juce::File&);

/** Imports the library into one content pack. */
juce::Result importSampleFolder (const Options&, std::vector<ImportedKit>& results);

} // namespace lithium::importer
