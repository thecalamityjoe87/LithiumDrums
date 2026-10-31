// LithiumImport: converts a downloaded drum library into a Lithium Drums
// content pack.
//
//   LithiumImport <folder, drumkit.xml or .sfz> [options]
//
//   --kits-folder <dir>   where to put the pack (default: the plugin's kits folder)
//   --mapping <file>      a mapping file (default: built in for known kits, else automatic)
//   --zones <n>           DrumGizmo: velocity zones per articulation, at most (default 8)
//   --min-rr <n>          DrumGizmo: round-robins per zone, at least (default 3)
//   --overwrite           replace kits that are already there
//   --copy                copy the audio even where a hard link would do
//
// DrumGizmo kits and SFZ instruments are recognised by their files. A folder
// imports everything the mapping (or, without one, the folder) holds: DRSKit
// becomes one kit and four kit presets, the AVL Drumkits the five Basalt Kits. See
// docs/kit-format.md for what the conversion does.

#include "Settings.h"
#include "importer/Importer.h"

#include <iostream>

namespace
{
    int usage()
    {
        std::cerr << "usage: LithiumImport <folder, drumkit.xml or .sfz> [--kits-folder <dir>] [--mapping <file>]\n"
                     "                     [--zones <n>] [--min-rr <n>] [--overwrite] [--copy]\n";
        return 2;
    }

    juce::String formatBytes (std::int64_t bytes)
    {
        const auto mb = (double) bytes / (1024.0 * 1024.0);
        return mb >= 1000.0 ? juce::String (mb / 1024.0, 2) + " GB" : juce::String (mb, 1) + " MB";
    }
}

int main (int argc, char* argv[])
{
    // Settings uses JUCE's file and message classes, which expect JUCE to be initialised.
    juce::ScopedJuceInitialiser_GUI juce;

    const auto cwd = juce::File::getCurrentWorkingDirectory();
    lithium::importer::Options options;
    options.kitsFolder = lithium::Settings().getKitsFolder();
    options.log = [] (const juce::String& s) { std::cout << s << std::endl; };

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        const auto hasValue = i + 1 < argc;

        if (arg == "--kits-folder" && hasValue)       options.kitsFolder = cwd.getChildFile (argv[++i]);
        else if (arg == "--mapping" && hasValue)      options.mappingFile = cwd.getChildFile (argv[++i]);
        else if (arg == "--zones" && hasValue)        options.maxZones = juce::jlimit (1, 127, juce::String (argv[++i]).getIntValue());
        else if (arg == "--min-rr" && hasValue)       options.minRoundRobins = juce::jlimit (1, 64, juce::String (argv[++i]).getIntValue());
        else if (arg == "--overwrite")                options.overwrite = true;
        else if (arg == "--copy")                     options.copyAudio = true;
        else if (! arg.startsWith ("--") && options.source == juce::File())
                                                      options.source = cwd.getChildFile (arg);
        else                                          return usage();
    }

    if (options.source == juce::File())
        return usage();

    std::cout << "Kits folder: " << options.kitsFolder.getFullPathName() << std::endl;

    std::vector<lithium::importer::ImportedKit> kits;
    const auto result = lithium::importer::importKits (options, kits);

    for (const auto& k : kits)
    {
        std::cout << "  " << k.name << (k.isPreset ? " (preset)" : "") << ": " << k.numPieces << " pieces, "
                  << k.numMics << " mics, " << k.numHits << " hits, " << formatBytes (k.ramBytes) << " in RAM";

        if (! k.isPreset)
            std::cout << ", " << k.numFiles << " files (" << formatBytes (k.audioBytes)
                      << (k.linked ? " on disk, hard-linked where possible" : " on disk, copied") << ")";

        std::cout << "\n    -> " << k.folder.getFullPathName() << std::endl;
    }

    if (result.failed())
    {
        std::cerr << "Import failed: " << result.getErrorMessage() << std::endl;
        return 1;
    }

    return 0;
}
