#include "Importer.h"

#include "DrumGizmoImporter.h"
#include "SfzImporter.h"

namespace lithium::importer
{

juce::Result importKits (const Options& options, std::vector<ImportedKit>& results)
{
    results.clear();

    if (! options.source.exists())
        return juce::Result::fail ("Nothing to import at " + options.source.getFullPathName());

    // DrumGizmo first: its kit folders hold XML files, never .sfz ones.
    if (isDrumGizmoSource (options.source))
        return importDrumGizmo (options, results);

    if (isSfzSource (options.source))
        return importSfz (options, results);

    return juce::Result::fail ("No DrumGizmo kit or SFZ instrument found at " + options.source.getFullPathName());
}

} // namespace lithium::importer
