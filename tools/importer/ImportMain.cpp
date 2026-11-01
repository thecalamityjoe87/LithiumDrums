// LithiumImport: converts a downloaded drum library into a Lithium Drums
// content pack, or a groove library into a groove pack.
//
//   LithiumImport <folder, drumkit.xml or .sfz> [options]
//   LithiumImport --install <pack id | all> [--kits-folder <dir>] [--grooves-folder <dir>]
//   LithiumImport --list-packs
//
//   --kits-folder <dir>   where to put the pack (default: the plugin's kits folder)
//   --grooves-folder <dir> where to put a groove pack (default: the plugin's grooves folder)
//   --phrase-bars <n>     grooves: bars per phrase on the Grooves page (default 4)
//   --mapping <file>      a mapping file (default: built in for known kits, else automatic)
//   --zones <n>           DrumGizmo: velocity zones per articulation, at most (default 8)
//   --min-rr <n>          DrumGizmo: round-robins per zone, at least (default 3)
//   --overwrite           replace kits that are already there
//   --copy                copy the audio even where a hard link would do
//
// DrumGizmo kits and SFZ instruments are recognised by their files. A folder
// imports everything the mapping (or, without one, the folder) holds: DRSKit
// becomes one kit and four kit presets, the AVL Drumkits the five Basalt Kits. See
// docs/kit-format.md for what the conversion does. The Groove MIDI Dataset is
// recognised by its info.csv and becomes a groove pack (docs/grooves.md).
//
// --install downloads a pack from the built-in catalog, unpacks and imports
// it, as the plugin's Download buttons do (docs/packs.md); --list-packs shows
// the catalog and what's installed.

#include "Settings.h"
#include "grooves/GrooveImporter.h"
#include "importer/Importer.h"
#include "packs/PackInstaller.h"

#include <iostream>

namespace
{
    int usage()
    {
        std::cerr << "usage: LithiumImport <folder, drumkit.xml or .sfz> [--kits-folder <dir>] [--mapping <file>]\n"
                     "                     [--zones <n>] [--min-rr <n>] [--overwrite] [--copy]\n"
                     "       LithiumImport <Groove MIDI Dataset folder> [--grooves-folder <dir>] [--phrase-bars <n>] [--overwrite]\n"
                     "       LithiumImport --install <pack id | all> [--kits-folder <dir>] [--grooves-folder <dir>]\n"
                     "       LithiumImport --list-packs\n";
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

    lithium::grooves::ImportOptions grooveOptions;
    grooveOptions.groovesFolder = lithium::Settings::getGroovesFolder();
    grooveOptions.log = options.log;

    juce::String installId;
    auto listPacks = false;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        const auto hasValue = i + 1 < argc;

        if (arg == "--kits-folder" && hasValue)       options.kitsFolder = cwd.getChildFile (argv[++i]);
        else if (arg == "--grooves-folder" && hasValue) grooveOptions.groovesFolder = cwd.getChildFile (argv[++i]);
        else if (arg == "--phrase-bars" && hasValue)  grooveOptions.phraseBars = juce::jlimit (1, 64, juce::String (argv[++i]).getIntValue());
        else if (arg == "--mapping" && hasValue)      options.mappingFile = cwd.getChildFile (argv[++i]);
        else if (arg == "--zones" && hasValue)        options.maxZones = juce::jlimit (1, 127, juce::String (argv[++i]).getIntValue());
        else if (arg == "--min-rr" && hasValue)       options.minRoundRobins = juce::jlimit (1, 64, juce::String (argv[++i]).getIntValue());
        else if (arg == "--overwrite")                options.overwrite = true;
        else if (arg == "--install" && hasValue)      installId = argv[++i];
        else if (arg == "--list-packs")               listPacks = true;
        else if (arg == "--copy")                     options.copyAudio = true;
        else if (! arg.startsWith ("--") && options.source == juce::File())
                                                      options.source = cwd.getChildFile (arg);
        else                                          return usage();
    }

    if (listPacks)
    {
        for (const auto& p : lithium::packs::catalog())
            std::cout << p.id.paddedRight (' ', 28) << p.name.paddedRight (' ', 22)
                      << lithium::packs::formatSize (p.downloadSize()).paddedLeft (' ', 8) << "  " << p.licenseName << std::endl;

        return 0;
    }

    if (installId.isNotEmpty())
    {
        auto failures = 0;

        for (const auto& p : lithium::packs::catalog())
        {
            if (installId != "all" && p.id != installId)
                continue;

            std::cout << "Installing " << p.name << " (" << lithium::packs::formatSize (p.downloadSize()) << ")" << std::endl;
            const auto result = lithium::packs::Installer::installNow (p, options.kitsFolder, grooveOptions.groovesFolder,
                                                                       [] (const juce::String& s) { std::cout << "  " << s << std::endl; });

            if (result.failed())
            {
                std::cerr << "  Failed: " << result.getErrorMessage() << std::endl;
                ++failures;
            }
        }

        if (installId != "all" && lithium::packs::findPack (installId) == nullptr)
        {
            std::cerr << "No pack called " << installId << " (see --list-packs)" << std::endl;
            return 2;
        }

        return failures == 0 ? 0 : 1;
    }

    if (options.source == juce::File())
        return usage();

    if (lithium::grooves::isGrooveMidiDataset (options.source))
    {
        grooveOptions.source = options.source;
        grooveOptions.overwrite = options.overwrite;
        std::cout << "Grooves folder: " << grooveOptions.groovesFolder.getFullPathName() << std::endl;

        lithium::grooves::ImportedPack pack;
        const auto result = lithium::grooves::importGrooveMidiDataset (grooveOptions, pack);

        for (const auto& w : pack.warnings)
            std::cout << "  warning: " << w << std::endl;

        if (result.failed())
        {
            std::cerr << "Import failed: " << result.getErrorMessage() << std::endl;
            return 1;
        }

        std::cout << "  " << pack.name << ": " << pack.numGrooves << " grooves (" << pack.numPhrases << " phrases of "
                  << grooveOptions.phraseBars << " bars), " << pack.numFills << " fills\n    -> "
                  << pack.folder.getFullPathName() << std::endl;
        return 0;
    }

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
