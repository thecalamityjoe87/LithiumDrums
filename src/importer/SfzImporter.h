#pragma once

#include "Importer.h"
#include "KitWriter.h"
#include "SfzParser.h"

// Converts SFZ drum instruments into Lithium Drums' native kit format.
//
// An SFZ instrument is a list of regions: a sample, the key and velocity range
// it plays on, and conditions (round-robin position, controller ranges). A
// multi-mic SFZ kit has one region per mic for every hit, all on the same key
// and velocity range, switched on and off by controllers. The importer
// evaluates the instrument at a fixed controller state (every mic on), finds
// which regions play together, and turns them into pieces, articulations,
// velocity zones and round-robins, with every mic of a hit sample-aligned.
// See docs/kit-format.md ("Importing SFZ instruments").

namespace lithium::importer
{

/** True if the file is an .sfz, or the folder holds one (up to two levels down). */
bool isSfzSource (const juce::File&);

/** Imports the SFZ instruments the options point at into one content pack. */
juce::Result importSfz (const Options&, std::vector<ImportedKit>& results);

/** The linear level an SFZ region plays at for a velocity, from its
    amp_veltrack and amp_velcurve_N opcodes (1.0 at full level). */
double velocityGain (const sfz::Region&, int velocity);

} // namespace lithium::importer
