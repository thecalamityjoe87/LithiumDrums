#pragma once

#include "Importer.h"
#include "KitWriter.h"

// Converts DrumGizmo kits into Lithium Drums' native kit format.
//
// A DrumGizmo kit is a folder with one or more drumkit XML files (variants of
// the kit), instrument XML files listing each instrument's samples with a
// "power" value, multichannel WAV files, and MIDI maps. See
// docs/kit-format.md ("Importing DrumGizmo kits") for how each part is
// converted, in particular how power values become velocity zones.

namespace lithium::importer
{

/** True if the file is a DrumGizmo drumkit XML, or the folder holds one. */
bool isDrumGizmoSource (const juce::File&);

/** Imports every drumkit XML the options point at into one content pack.
    Variants the mapping marks as "presetOf" another become kit presets. */
juce::Result importDrumGizmo (const Options&, std::vector<ImportedKit>& results);

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
