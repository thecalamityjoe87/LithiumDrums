#include "SampleFolderImporter.h"

#include "DrumGizmoImporter.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace lithium::importer
{

namespace
{
    struct ImportError
    {
        juce::String message;
    };

    [[noreturn]] void fail (const juce::String& message)
    {
        throw ImportError { message };
    }

    /** The built-in mapping whose marker file is in this folder, if any. */
    juce::var mappingFor (const juce::File& folder)
    {
        if (! folder.isDirectory())
            return {};

        for (const auto& mapping : builtInMappings ("samples"))
            if (const auto marker = mapping["marker"].toString(); marker.isNotEmpty() && folder.getChildFile (marker).existsAsFile())
                return mapping;

        return {};
    }

    /** The files a pattern names ("snare/snare-top*.wav", relative to the
        samples folder), in natural order (2 before 10). */
    juce::Array<juce::File> filesMatching (const juce::File& samples, const juce::String& pattern)
    {
        const auto dir = pattern.upToLastOccurrenceOf ("/", false, false);
        const auto name = pattern.fromLastOccurrenceOf ("/", false, false);
        auto files = samples.getChildFile (dir).findChildFiles (juce::File::findFiles, false, name);

        struct NaturalOrder
        {
            static int compareElements (const juce::File& a, const juce::File& b)
            {
                return a.getFileName().compareNatural (b.getFileName());
            }
        };

        NaturalOrder order;
        files.sort (order);
        return files;
    }

    /** A hit's loudness: the peak of its loudest channel, linear. */
    double peakOf (juce::AudioFormatManager& formats, const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
            fail ("Can't read " + file.getFullPathName() + " (only WAV and FLAC are supported)");

        std::vector<juce::Range<float>> levels ((size_t) reader->numChannels);
        reader->readMaxLevels (0, reader->lengthInSamples, levels.data(), (int) reader->numChannels);

        double peak = 0.0;

        for (const auto& r : levels)
            peak = std::max ({ peak, (double) std::abs (r.getStart()), (double) std::abs (r.getEnd()) });

        return peak;
    }

    KitModel convert (const juce::var& mapping, const juce::var& kitEntry, const juce::File& sourceRoot, const Options& options)
    {
        auto log = [&] (const juce::String& s) { if (options.log) options.log (s); };

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const auto samples = sourceRoot.getChildFile (mapping["root"].toString());

        KitModel kit;
        kit.id = kitEntry["id"].toString();
        kit.name = kitEntry["name"].toString();
        kit.description = kitEntry["description"].toString().isNotEmpty() ? kitEntry["description"].toString()
                                                                          : mapping["description"].toString();
        kit.tags = toStringArray (mapping["tags"]);

        auto importedFrom = object();
        put (importedFrom, "format", "samples");
        put (importedFrom, "folder", mapping["root"].toString());
        kit.importedFrom = importedFrom;

        for (const auto& m : *mapping["mics"].getArray())
        {
            KitModel::Mic mic;
            mic.id = m["id"].toString();
            mic.name = m["name"].toString();
            mic.role = m["role"].toString().isNotEmpty() ? m["role"].toString() : juce::String ("close");
            mic.channels = std::max (1, (int) m.getProperty ("channels", 2));
            mic.volumeDb = (double) m.getProperty ("volumeDb", 0.0);
            kit.mics.push_back (mic);
        }

        if (kit.mics.empty())
            fail ("The mapping has no mics");

        // Every file feeds the first mic: these libraries are one recording per hit.
        const auto stereo = kit.mics.front().channels == 2;

        for (const auto& p : *mapping["pieces"].getArray())
        {
            KitModel::Piece piece;
            piece.id = p["id"].toString();
            piece.name = p["name"].toString();
            piece.role = p["role"].toString();
            piece.tune = (double) p.getProperty ("tune", 0.0);
            piece.volumeDb = (double) p.getProperty ("volumeDb", 0.0);

            for (const auto& a : *p["articulations"].getArray())
            {
                KitModel::Articulation art;
                art.id = a["id"].toString();
                art.name = a["name"].toString();
                art.chokeGroup = a["chokeGroup"].toString();
                art.chokes = toStringArray (a["chokes"]);

                juce::Array<juce::File> files;

                for (const auto& pattern : toStringArray (a["files"]))
                    for (const auto& f : filesMatching (samples, pattern))
                        files.addIfNotAlreadyThere (f);

                for (const auto& pattern : toStringArray (a["exclude"]))
                    for (const auto& f : filesMatching (samples, pattern))
                        files.removeFirstMatchingValue (f);

                if (files.isEmpty())
                {
                    kit.warnings.add (piece.name + " " + art.name + ": no files match " + toStringArray (a["files"]).joinIntoString (", "));
                    continue;
                }

                log ("  " + piece.name + " / " + art.name + ": " + juce::String (files.size()) + " hits");

                // Quietest to loudest, then into velocity zones like DrumGizmo's power values.
                std::vector<double> peaks;

                for (const auto& f : files)
                    peaks.push_back (peakOf (formats, f));

                for (const auto& z : makeZones (peaks, options.maxZones, options.minRoundRobins))
                {
                    KitModel::Zone zone { z.velocityLow, z.velocityHigh, 0.0, (double) z.velocityRangeDb, {} };

                    for (const auto index : z.samples)
                    {
                        KitModel::Part part;
                        part.file = files[index].getRelativePathFrom (sourceRoot).replaceCharacter ('\\', '/');
                        part.channels.push_back ({ 0, stereo ? std::array<int, 2> { 1, 2 } : std::array<int, 2> { 1, 0 } });
                        zone.hits.push_back ({ { part } });
                    }

                    art.zones.push_back (std::move (zone));
                }

                piece.articulations.push_back (std::move (art));
            }

            if (! piece.articulations.empty())
                kit.pieces.push_back (std::move (piece));
        }

        if (kit.pieces.empty())
            fail ("None of the mapping's files were found in " + samples.getFullPathName());

        if (const auto* map = mapping["midiMap"].getArray())
            for (const auto& e : *map)
                kit.midiMap.push_back ({ (int) e["note"], e["piece"].toString(), e["articulation"].toString() });

        return kit;
    }
}

//==============================================================================
bool isSampleFolderSource (const juce::File& source)
{
    return ! mappingFor (source).isVoid();
}

juce::Result importSampleFolder (const Options& options, std::vector<ImportedKit>& results)
{
    try
    {
        juce::var mapping;

        if (options.mappingFile != juce::File())
        {
            if (auto r = juce::JSON::parse (options.mappingFile.loadFileAsString(), mapping); r.failed())
                fail ("Can't read the mapping file: " + r.getErrorMessage());
        }
        else
        {
            mapping = mappingFor (options.source);
        }

        if (mapping.isVoid())
            fail ("No mapping for the samples in " + options.source.getFullPathName());

        PackModel pack;
        pack.sourceRoot = options.source;
        pack.sourceFormat = "WAV sample";
        pack.id = mapping["pack"]["id"].toString();
        pack.name = mapping["pack"]["name"].toString();
        pack.author = mapping["author"].toString();
        pack.sourceUrl = mapping["sourceUrl"].toString();
        pack.credits = mapping["credits"].toString();
        pack.description = mapping["description"].toString();
        pack.licenseName = mapping["license"]["name"].toString();
        pack.licenseUrl = mapping["license"]["url"].toString();
        pack.licenseFiles = toStringArray (mapping["license"]["files"]);
        pack.licenseNote = mapping["licenseNote"].toString();
        pack.shareAlike = (bool) mapping["license"]["shareAlike"];
        pack.conversionNote = "The audio itself is unchanged. The files were grouped into pieces and articulations by\n"
                              "their names, and each hit's velocity zone was worked out from its peak level.";

        for (const auto& k : *mapping["kits"].getArray())
        {
            if (options.log)
                options.log ("Reading " + k["name"].toString());

            pack.kits.push_back (convert (mapping, k, options.source, options));
        }

        for (auto& p : presetsFromMapping (mapping, pack.kits))
            pack.presets.push_back (std::move (p));

        return writePack (pack, options, results);
    }
    catch (const ImportError& e)
    {
        return juce::Result::fail (e.message);
    }
}

} // namespace lithium::importer
