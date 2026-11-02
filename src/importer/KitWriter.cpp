#include "KitWriter.h"

#include "LithiumImportMaps.h"
#include "engine/KitManifest.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

namespace lithium::importer
{

namespace
{
    // Writing runs on whatever thread calls it (never the audio thread), so a
    // local exception is the simplest way to stop with a message from deep
    // inside it.
    struct WriteError
    {
        juce::String message;
    };

    [[noreturn]] void fail (const juce::String& message)
    {
        throw WriteError { message };
    }

    bool linkOrCopy (const juce::File& source, const juce::File& dest, bool allowLink)
    {
        if (! dest.getParentDirectory().createDirectory())
            return false;

        if (allowLink)
        {
            // A hard link shares the data on disk: kits made from the same
            // download (DRSKit's variants, Virtuosity with snares on and off)
            // use the same audio without copies of it. It only works on the
            // same file system, so fall back to copying.
            std::error_code error;
            std::filesystem::create_hard_link (source.getFullPathName().toStdString(), dest.getFullPathName().toStdString(), error);

            if (! error)
                return true;
        }

        return source.copyFileTo (dest);
    }

    struct AudioHeader
    {
        std::int64_t frames = 0;
        int channels = 0, bits = 0;
        double sampleRate = 0.0;
    };

    /** Replaces a folder only if it is clearly one of ours. */
    void prepareFolder (const juce::File& dest, bool overwrite)
    {
        if (dest.exists())
        {
            if (! overwrite)
                fail (dest.getFullPathName() + " already exists (use --overwrite to replace it)");

            if (! dest.getChildFile ("kit.json").existsAsFile() || ! dest.deleteRecursively())
                fail ("Can't replace " + dest.getFullPathName());
        }

        if (! dest.createDirectory())
            fail ("Can't create " + dest.getFullPathName());
    }

    juce::String licenseText (const PackModel& pack, const juce::String& title, const juce::StringArray& originals)
    {
        juce::String text;
        text << title << "\n"
             << juce::String::repeatedString ("=", title.length()) << "\n\n"
             << "Author: " << pack.author << "\n"
             << "Source: " << pack.sourceUrl << "\n"
             << "License: " << pack.licenseName << (pack.licenseUrl.isNotEmpty() ? " (" + pack.licenseUrl + ")" : juce::String()) << "\n\n";

        if (pack.credits.isNotEmpty())
            text << pack.credits << "\n\n";

        if (pack.licenseNote.isNotEmpty())
            text << pack.licenseNote << "\n\n";

        text << "These samples were converted from the original " << pack.sourceFormat << " library for use in Lithium Drums.\n"
             << pack.conversionNote << "\n";

        if (pack.shareAlike)
            text << "\nThis conversion is an adaptation of the original library, so it is licensed under the same\n"
                 << "licence, " << pack.licenseName << ". If you pass it on, pass it on under that licence and with\n"
                 << "this notice, as a content pack of its own. The authors state that the share-alike condition\n"
                 << "applies to the samples and to sample libraries made from them, not to music made with them.\n";

        if (! originals.isEmpty())
            text << "\nThe library's original notice is included as: " << originals.joinIntoString (", ") << "\n";

        return text;
    }

    juce::StringArray copyOriginals (const PackModel& pack, const juce::File& dest, juce::StringArray& warnings)
    {
        juce::StringArray copied;

        for (const auto& name : pack.licenseFiles)
        {
            auto source = pack.sourceRoot.getChildFile (name);

            // The download may have been unpacked one or two folders deeper
            // (or shallower) than the mapping expects.
            if (! source.existsAsFile())
                for (const auto& f : pack.sourceRoot.findChildFiles (juce::File::findFiles, true, source.getFileName()))
                    if (f.getRelativePathFrom (pack.sourceRoot).replaceCharacter ('\\', '/').endsWith (name) || ! source.existsAsFile())
                        source = f;

            const auto target = dest.getChildFile ("Original " + source.getFileName());

            if (source.existsAsFile() && source.copyFileTo (target))
                copied.add (target.getFileName());
            else
                warnings.addIfNotAlreadyThere ("Licence file \"" + name + "\" not found in the source");
        }

        return copied;
    }

    juce::var licenseObject (const PackModel& pack)
    {
        auto license = object();
        put (license, "name", pack.licenseName.isNotEmpty() ? pack.licenseName : juce::String ("Unknown"));

        if (pack.licenseUrl.isNotEmpty())
            put (license, "url", pack.licenseUrl);

        put (license, "file", "LICENSE.txt");

        if (pack.shareAlike)
            put (license, "shareAlike", true);

        return license;
    }

    juce::var stringArray (const juce::StringArray& strings)
    {
        juce::Array<juce::var> a;

        for (const auto& s : strings)
            a.add (s);

        return a;
    }

    //==============================================================================
    // Hits, written in the most compact form that describes them (see
    // docs/kit-format.md, "Hits and channel maps").

    juce::var channelsVar (const KitModel& kit, int mic, const std::array<int, 2>& ch)
    {
        if (kit.mics[(size_t) mic].channels == 2 && ch[1] > 0)
            return juce::Array<juce::var> { ch[0], ch[1] };

        return ch[0];    // a mono mic, or one channel played centred on a stereo mic
    }

    juce::var channelMapVar (const KitModel& kit, const KitModel::Part& part)
    {
        auto map = object();

        for (const auto& [mic, ch] : part.channels)
            put (map, kit.mics[(size_t) mic].id, channelsVar (kit, mic, ch));

        return map;
    }

    juce::String audioPath (const juce::String& file)   { return "audio/" + file; }

    /** A part that is a whole file feeding one mic in its natural layout. */
    bool isWholeFileForOneMic (const KitModel& kit, const KitModel::Part& part)
    {
        if (part.channels.size() != 1)
            return false;

        const auto& [mic, ch] = part.channels.front();
        return kit.mics[(size_t) mic].channels == 1 ? ch == std::array<int, 2> { 1, 0 }
                                                     : ch == std::array<int, 2> { 1, 2 };
    }

    juce::var hitVar (const KitModel& kit, const KitModel::Hit& hit, bool useCommonMap)
    {
        if (useCommonMap)
            return audioPath (hit.parts.front().file);

        // One file per mic: { "files": { "kick": "...", "oh": "..." } }
        const auto oneFilePerMic = hit.parts.size() > 1
            && std::all_of (hit.parts.begin(), hit.parts.end(), [&] (const auto& p)
                            { return isWholeFileForOneMic (kit, p) && p.start == hit.parts.front().start; });

        if (oneFilePerMic)
        {
            auto files = object();

            for (const auto& p : hit.parts)
                put (files, kit.mics[(size_t) p.channels.front().first].id, audioPath (p.file));

            auto v = object();
            put (v, "files", files);

            if (hit.parts.front().start > 0)
                put (v, "start", (juce::int64) hit.parts.front().start);

            return v;
        }

        juce::Array<juce::var> parts;

        for (const auto& p : hit.parts)
        {
            auto v = object();
            put (v, "file", audioPath (p.file));
            put (v, "channelMap", channelMapVar (kit, p));

            if (p.start > 0)
                put (v, "start", (juce::int64) p.start);

            parts.add (v);
        }

        return parts.size() == 1 ? parts[0] : juce::var (parts);
    }

    juce::var articulationVar (const KitModel& kit, const KitModel::Articulation& a)
    {
        auto art = object();
        put (art, "id", a.id);
        put (art, "name", a.name.isNotEmpty() ? a.name : a.id);

        if (a.chokeGroup.isNotEmpty())
            put (art, "chokeGroup", a.chokeGroup);

        if (! a.chokes.isEmpty())
            put (art, "chokes", stringArray (a.chokes));

        // When every hit is one file with the same channel layout (DrumGizmo's
        // multichannel files), the channel map is written once and the hits
        // are plain file names, which keeps kit.json small.
        const KitModel::Part* first = nullptr;
        bool common = true;

        for (const auto& z : a.zones)
            for (const auto& h : z.hits)
            {
                if (h.parts.size() != 1)
                    common = false;
                else if (first == nullptr)
                    first = &h.parts.front();
                else if (h.parts.front().channels != first->channels || h.parts.front().start != first->start)
                    common = false;
            }

        common = common && first != nullptr;

        if (common)
        {
            put (art, "channelMap", channelMapVar (kit, *first));

            if (first->start > 0)
                put (art, "start", (juce::int64) first->start);
        }

        juce::Array<juce::var> zones;

        for (const auto& z : a.zones)
        {
            auto zone = object();
            put (zone, "velocity", juce::Array<juce::var> { z.velocityLow, z.velocityHigh });

            if (std::abs (z.gainDb) > 0.005)
                put (zone, "gainDb", std::round (z.gainDb * 100.0) / 100.0);

            put (zone, "velocityRangeDb", std::round (z.velocityRangeDb * 10.0) / 10.0);

            juce::Array<juce::var> hits;

            for (const auto& h : z.hits)
                hits.add (hitVar (kit, h, common));

            put (zone, "roundRobins", hits);
            zones.add (zone);
        }

        put (art, "zones", zones);
        return art;
    }

    juce::var micVar (const KitModel::Mic& m)
    {
        auto mic = object();
        put (mic, "id", m.id);
        put (mic, "name", m.name);
        put (mic, "channels", m.channels);
        put (mic, "role", m.role);

        if (std::abs (m.volumeDb) > 1.0e-6)
            put (mic, "volumeDb", std::round (m.volumeDb * 100.0) / 100.0);

        if (std::abs (m.pan) > 1.0e-6)
            put (mic, "pan", std::round (m.pan * 1000.0) / 1000.0);

        if (m.invertPolarity)
            put (mic, "invertPolarity", true);

        return mic;
    }

    //==============================================================================
    /** Sample data in RAM for a parsed kit, the way KitLoader lays it out:
        one sample per hit per mic, at the mic's channel count, 16-bit if
        every file is, otherwise 24-bit. */
    std::int64_t ramFor (const KitManifest& m, const std::map<juce::String, AudioHeader>& headers)
    {
        int bits = 16;
        std::int64_t frames = 0;

        for (const auto& p : m.pieces)
            for (const auto& a : p.articulations)
                for (const auto& z : a.zones)
                    for (const auto& h : z.roundRobins)
                        for (const auto& s : h.sources)
                            if (const auto it = headers.find (s.file); it != headers.end())
                            {
                                bits = std::max (bits, it->second.bits);
                                frames += std::max<std::int64_t> (0, it->second.frames - s.start) * m.mics[(size_t) s.mic].channels;
                            }

        return frames * (bits <= 16 ? 2 : 3);
    }

    int countHits (const KitManifest& m)
    {
        int n = 0;

        for (const auto& p : m.pieces)
            for (const auto& a : p.articulations)
                for (const auto& z : a.zones)
                    n += (int) z.roundRobins.size();

        return n;
    }

    /** Parses a kit.json just written (which validates it), adds its stats
        and writes it again. */
    KitManifest addStats (const juce::File& folder, juce::var& root, const std::map<juce::String, AudioHeader>& headers,
                          ImportedKit& result)
    {
        KitManifest m;

        if (const auto r = KitManifest::parse (folder, m); r.failed())
            fail ("The converted kit \"" + result.name + "\" doesn't load: " + r.getErrorMessage());

        result.numPieces = (int) m.pieces.size();
        result.numMics = (int) m.mics.size();
        result.numHits = countHits (m);
        result.ramBytes = ramFor (m, headers);

        for (const auto& w : m.warnings)
            result.warnings.add (w);

        auto stats = object();
        put (stats, "pieces", result.numPieces);
        put (stats, "mics", result.numMics);
        put (stats, "hits", result.numHits);
        put (stats, "ramBytes", (juce::int64) result.ramBytes);
        put (root, "stats", stats);

        if (! folder.getChildFile ("kit.json").replaceWithText (juce::JSON::toString (root) + "\n"))
            fail ("Can't write kit.json in " + folder.getFullPathName());

        return m;
    }

    //==============================================================================
    /** The kit without any recording far longer than the others of the
        same hits: a whole take left in a library by mistake. Salamander's
        kick_OH_P_1.wav is 44 seconds with sixteen soft kicks in it, so a
        soft kick that picked it kept playing more kicks long after. Each
        file left out is a warning. */
    KitModel withoutWholeTakes (const KitModel& in, const juce::File& sourceRoot, std::map<juce::String, AudioHeader>& headers)
    {
        // Longer than this many times the zone's median, and than this many seconds.
        constexpr double maxTimesMedian = 6.0, minSeconds = 5.0;

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        auto secondsOf = [&] (const KitModel::Part& part)
        {
            if (headers.count (part.file) == 0)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (sourceRoot.getChildFile (part.file)));

                if (reader == nullptr)
                    return 0.0;   // the audio pass reports it

                headers[part.file] = { reader->lengthInSamples, (int) reader->numChannels,
                                       reader->usesFloatingPointData ? 32 : (int) reader->bitsPerSample, reader->sampleRate };
            }

            const auto& h = headers[part.file];
            return h.sampleRate > 0.0 ? (double) (h.frames - part.start) / h.sampleRate : 0.0;
        };

        auto kit = in;

        for (auto& p : kit.pieces)
        {
            for (auto& a : p.articulations)
            {
                for (auto& z : a.zones)
                {
                    if (z.hits.size() < 3)
                        continue;   // too few to tell what's normal

                    std::vector<double> lengths;

                    for (const auto& h : z.hits)
                    {
                        auto longest = 0.0;

                        for (const auto& part : h.parts)
                            longest = std::max (longest, secondsOf (part));

                        lengths.push_back (longest);
                    }

                    auto sorted = lengths;
                    std::nth_element (sorted.begin(), sorted.begin() + (std::ptrdiff_t) sorted.size() / 2, sorted.end());
                    const auto median = sorted[sorted.size() / 2];
                    std::vector<KitModel::Hit> kept;

                    for (size_t i = 0; i < z.hits.size(); ++i)
                    {
                        if (lengths[i] > maxTimesMedian * median && lengths[i] > minSeconds)
                            kit.warnings.add (p.name + " " + a.name + ": left out " + z.hits[i].parts.front().file + ", "
                                              + juce::String (lengths[i], 1) + " s long, a whole take rather than one hit (the others are about "
                                              + juce::String (median, 1) + " s)");
                        else
                            kept.push_back (z.hits[i]);
                    }

                    z.hits = std::move (kept);
                }
            }
        }

        return kit;
    }

    ImportedKit writeKit (const PackModel& pack, const KitModel& original, const juce::File& packFolder, const Options& options,
                          std::map<juce::String, AudioHeader>& headers, const juce::StringArray& originals)
    {
        auto log = [&] (const juce::String& s) { if (options.log) options.log (s); };
        const auto kit = withoutWholeTakes (original, pack.sourceRoot, headers);

        ImportedKit result;
        result.name = kit.name;
        result.warnings = kit.warnings;

        const auto dest = packFolder.getChildFile (juce::File::createLegalFileName (kit.name));
        result.folder = dest;
        prepareFolder (dest, options.overwrite);

        // --- Audio ---------------------------------------------------------------
        std::set<juce::String> files;

        for (const auto& p : kit.pieces)
            for (const auto& a : p.articulations)
                for (const auto& z : a.zones)
                    for (const auto& h : z.hits)
                        for (const auto& part : h.parts)
                            files.insert (part.file);

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        double sampleRate = 0.0;
        juce::String firstFile;

        for (const auto& f : files)
        {
            const auto source = pack.sourceRoot.getChildFile (f);

            if (! source.existsAsFile())
                fail ("Missing audio file " + source.getFullPathName());

            if (headers.count (f) == 0)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));

                if (reader == nullptr)
                    fail ("Can't read " + source.getFullPathName() + " (only WAV and FLAC are supported)");

                headers[f] = { reader->lengthInSamples, (int) reader->numChannels,
                               reader->usesFloatingPointData ? 32 : (int) reader->bitsPerSample, reader->sampleRate };
            }

            const auto& h = headers[f];

            if (sampleRate <= 0.0)
            {
                sampleRate = h.sampleRate;
                firstFile = f;
            }
            else if (std::abs (h.sampleRate - sampleRate) > 0.5)
            {
                fail (kit.name + ": " + f + " is " + juce::String (h.sampleRate) + " Hz, but " + firstFile + " is "
                      + juce::String (sampleRate) + " Hz. A kit's samples must all have one rate.");
            }

            const auto target = dest.getChildFile (audioPath (f));

            if (! linkOrCopy (source, target, ! options.copyAudio))
                fail ("Can't write " + target.getFullPathName());

            result.linked = result.linked || ! options.copyAudio;
            result.audioBytes += source.getSize();
            ++result.numFiles;

            if (result.numFiles % 500 == 0)
                log ("  " + juce::String (result.numFiles) + " of " + juce::String ((int) files.size()) + " audio files");
        }

        if (sampleRate <= 0.0)
            fail (kit.name + ": no audio found");

        // --- Licence --------------------------------------------------------------
        juce::StringArray kitOriginals;

        for (const auto& name : originals)
            if (packFolder.getChildFile (name).copyFileTo (dest.getChildFile (name)))
                kitOriginals.add (name);

        dest.getChildFile ("LICENSE.txt").replaceWithText (licenseText (pack, kit.name, kitOriginals));

        // --- kit.json, written last: its presence means a complete kit -------------
        auto root = object();
        put (root, "formatVersion", 2);
        put (root, "type", "kit");
        put (root, "id", kit.id);
        put (root, "name", kit.name);
        put (root, "author", pack.author);
        put (root, "sourceUrl", pack.sourceUrl);
        put (root, "license", licenseObject (pack));

        if (pack.credits.isNotEmpty())
            put (root, "credits", pack.credits);

        if (const auto d = kit.description.isNotEmpty() ? kit.description : pack.description; d.isNotEmpty())
            put (root, "description", d);

        if (! kit.tags.isEmpty())
            put (root, "tags", stringArray (kit.tags));

        put (root, "sampleRate", sampleRate);

        if (! kit.importedFrom.isVoid())
            put (root, "importedFrom", kit.importedFrom);

        juce::Array<juce::var> mics;

        for (const auto& m : kit.mics)
            mics.add (micVar (m));

        put (root, "mics", mics);

        juce::Array<juce::var> pieces;

        for (const auto& p : kit.pieces)
        {
            auto piece = object();
            put (piece, "id", p.id);
            put (piece, "name", p.name);
            put (piece, "role", p.role);

            if (std::abs (p.tune) > 1.0e-6)
                put (piece, "tune", p.tune);

            if (std::abs (p.volumeDb) > 1.0e-6)
                put (piece, "volumeDb", std::round (p.volumeDb * 100.0) / 100.0);

            juce::Array<juce::var> arts;

            for (const auto& a : p.articulations)
                arts.add (articulationVar (kit, a));

            put (piece, "articulations", arts);
            pieces.add (piece);
        }

        put (root, "pieces", pieces);

        if (! kit.midiMap.empty())
        {
            juce::Array<juce::var> map;

            for (const auto& e : kit.midiMap)
            {
                auto entry = object();
                put (entry, "note", e.note);
                put (entry, "piece", e.piece);

                if (e.articulation.isNotEmpty())
                    put (entry, "articulation", e.articulation);

                map.add (entry);
            }

            put (root, "midiMap", map);
        }

        if (! dest.getChildFile ("kit.json").replaceWithText (juce::JSON::toString (root) + "\n"))
            fail ("Can't write kit.json in " + dest.getFullPathName());

        // The audio paths in kit.json are "audio/<file>"; the stats need the
        // headers under those names.
        std::map<juce::String, AudioHeader> byKitPath;

        for (const auto& f : files)
            byKitPath[audioPath (f)] = headers[f];

        addStats (dest, root, byKitPath, result);
        return result;
    }

    ImportedKit writePreset (const PresetModel& preset, const juce::File& packFolder, const Options& options,
                             const std::map<juce::String, AudioHeader>& headers)
    {
        ImportedKit result;
        result.name = preset.name;
        result.isPreset = true;

        const auto dest = packFolder.getChildFile (juce::File::createLegalFileName (preset.name));
        result.folder = dest;
        prepareFolder (dest, options.overwrite);

        auto root = object();
        put (root, "formatVersion", 2);
        put (root, "type", "preset");
        put (root, "id", preset.id);
        put (root, "name", preset.name);
        put (root, "base", preset.baseId);

        if (const auto d = preset.description; d.isNotEmpty())
            put (root, "description", d);

        if (! preset.tags.isEmpty())
            put (root, "tags", stringArray (preset.tags));

        if (! preset.pieces.isVoid())
            put (root, "pieces", preset.pieces);

        if (! preset.mics.isVoid())
            put (root, "mics", preset.mics);

        if (! preset.midiMap.isVoid())
            put (root, "midiMap", preset.midiMap);

        if (! dest.getChildFile ("kit.json").replaceWithText (juce::JSON::toString (root) + "\n"))
            fail ("Can't write kit.json in " + dest.getFullPathName());

        addStats (dest, root, headers, result);
        return result;
    }
} // namespace

//==============================================================================
int KitModel::findMic (const juce::String& micId) const
{
    for (size_t i = 0; i < mics.size(); ++i)
        if (mics[i].id == micId)
            return (int) i;

    return -1;
}

const KitModel::Piece* KitModel::findPiece (const juce::String& pieceId) const
{
    for (const auto& p : pieces)
        if (p.id == pieceId)
            return &p;

    return nullptr;
}

juce::Result writePack (const PackModel& pack, const Options& options, std::vector<ImportedKit>& results)
{
    auto log = [&] (const juce::String& s) { if (options.log) options.log (s); };

    try
    {
        if (! options.kitsFolder.createDirectory())
            fail ("Can't create the kits folder " + options.kitsFolder.getFullPathName());

        // A pack folder is replaced kit by kit, so other kits a user added to
        // the pack aren't touched.
        const auto packFolder = options.kitsFolder.getChildFile (juce::File::createLegalFileName (pack.name));

        if (packFolder.exists() && ! packFolder.getChildFile ("pack.json").existsAsFile())
            fail (packFolder.getFullPathName() + " exists but isn't a content pack. Move it aside and import again.");

        if (! packFolder.createDirectory())
            fail ("Can't create " + packFolder.getFullPathName());

        juce::StringArray packWarnings = pack.warnings;
        const auto originals = copyOriginals (pack, packFolder, packWarnings);
        packFolder.getChildFile ("LICENSE.txt").replaceWithText (licenseText (pack, pack.name, originals));

        auto packRoot = object();
        put (packRoot, "formatVersion", 1);
        put (packRoot, "id", pack.id);
        put (packRoot, "name", pack.name);
        put (packRoot, "author", pack.author);
        put (packRoot, "sourceUrl", pack.sourceUrl);

        if (pack.description.isNotEmpty())
            put (packRoot, "description", pack.description);

        if (pack.credits.isNotEmpty())
            put (packRoot, "credits", pack.credits);

        put (packRoot, "license", licenseObject (pack));

        if (! packFolder.getChildFile ("pack.json").replaceWithText (juce::JSON::toString (packRoot) + "\n"))
            fail ("Can't write pack.json in " + packFolder.getFullPathName());

        for (const auto& w : packWarnings)
            log ("  warning: " + w);

        // Kits first: presets are checked by loading them, which needs their base kit.
        std::map<juce::String, std::map<juce::String, AudioHeader>> headersByKit;   // kit id -> audio path -> header
        std::map<juce::String, AudioHeader> sourceHeaders;

        for (const auto& kit : pack.kits)
        {
            log ("Writing \"" + kit.name + "\"");
            auto result = writeKit (pack, kit, packFolder, options, sourceHeaders, originals);

            for (const auto& w : result.warnings)
                log ("  warning: " + w);

            auto& kitHeaders = headersByKit[kit.id];

            for (const auto& [file, header] : sourceHeaders)
                kitHeaders[audioPath (file)] = header;

            results.push_back (std::move (result));
        }

        for (const auto& preset : pack.presets)
        {
            log ("Writing preset \"" + preset.name + "\" (of " + preset.baseId + ")");
            auto result = writePreset (preset, packFolder, options, headersByKit[preset.baseId]);

            for (const auto& w : result.warnings)
                log ("  warning: " + w);

            results.push_back (std::move (result));
        }
    }
    catch (const WriteError& e)
    {
        return juce::Result::fail (e.message);
    }

    return juce::Result::ok();
}

//==============================================================================
juce::String toId (const juce::String& name)
{
    auto id = name.toLowerCase().replaceCharacters (" /\\:", "____");
    id = id.retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789_-.");
    return id.isNotEmpty() ? id : juce::String ("item");
}

juce::String roleForName (const juce::String& name)
{
    const auto n = name.toLowerCase();

    if (n.contains ("kick") || n.contains ("kdrum") || n.contains ("bassdrum") || n.startsWith ("bd"))  return "kick";
    if (n.contains ("snare"))                                                                           return "snare";
    if (n.contains ("hihat") || n.contains ("hi-hat") || n.contains ("hat") || n.startsWith ("hh"))     return "hihat";
    if (n.contains ("floor") || n.contains ("ftom") || n.contains ("lotom") || n.contains ("low tom"))   return "floor_tom";
    if (n.contains ("tom"))                                                                              return "tom";
    if (n.contains ("china"))                                                                            return "china";
    if (n.contains ("crash") || n.contains ("splash"))                                                   return "crash";
    if (n.contains ("ride"))                                                                             return "ride";
    return "other";
}

std::pair<juce::String, juce::String> detectLicense (const juce::String& text)
{
    struct Known { const char* phrase; const char* name; const char* url; };

    // More specific phrases first: "Attribution-ShareAlike" also contains "Attribution".
    static const Known known[] {
        { "Attribution-NonCommercial-ShareAlike 4.0", "CC BY-NC-SA 4.0", "https://creativecommons.org/licenses/by-nc-sa/4.0/" },
        { "Attribution-NonCommercial 4.0",            "CC BY-NC 4.0",    "https://creativecommons.org/licenses/by-nc/4.0/" },
        { "Attribution-ShareAlike 4.0",               "CC BY-SA 4.0",    "https://creativecommons.org/licenses/by-sa/4.0/" },
        { "Attribution-ShareAlike 3.0",               "CC BY-SA 3.0",    "https://creativecommons.org/licenses/by-sa/3.0/" },
        { "licenses/by-sa/4.0",                       "CC BY-SA 4.0",    "https://creativecommons.org/licenses/by-sa/4.0/" },
        { "licenses/by-sa/3.0",                       "CC BY-SA 3.0",    "https://creativecommons.org/licenses/by-sa/3.0/" },
        { "Attribution 4.0",                          "CC BY 4.0",       "https://creativecommons.org/licenses/by/4.0/" },
        { "Attribution 3.0",                          "CC BY 3.0",       "https://creativecommons.org/licenses/by/3.0/" },
        { "licenses/by/4.0",                          "CC BY 4.0",       "https://creativecommons.org/licenses/by/4.0/" },
        { "licenses/by/3.0",                          "CC BY 3.0",       "https://creativecommons.org/licenses/by/3.0/" },
        { "CC0 1.0",                                  "CC0 1.0",         "https://creativecommons.org/publicdomain/zero/1.0/" },
        { "CC0",                                      "CC0 1.0",         "https://creativecommons.org/publicdomain/zero/1.0/" },
    };

    const auto squashed = text.replace ("\n", " ").replace (" license", " License");

    for (const auto& k : known)
        if (squashed.containsIgnoreCase (k.phrase) || squashed.containsIgnoreCase (k.name))
            return { k.name, k.url };

    return {};
}

juce::var findBuiltInMapping (const juce::StringArray& candidates, const juce::String& format)
{
    for (int i = 0; i < LithiumImportMaps::namedResourceListSize; ++i)
    {
        int size = 0;
        const auto* data = LithiumImportMaps::getNamedResource (LithiumImportMaps::namedResourceList[i], size);
        const auto mapping = juce::JSON::parse (juce::String::fromUTF8 (data, size));
        const auto mappingFormat = mapping["format"].toString();

        if (! (mappingFormat.isEmpty() ? juce::String ("drumgizmo") : mappingFormat).equalsIgnoreCase (format))
            continue;

        if (const auto* matches = mapping["matches"].getArray())
            for (const auto& m : *matches)
                for (const auto& c : candidates)
                    if (m.toString().equalsIgnoreCase (c))
                        return mapping;
    }

    return {};
}

std::vector<juce::var> builtInMappings (const juce::String& format)
{
    std::vector<juce::var> result;

    for (int i = 0; i < LithiumImportMaps::namedResourceListSize; ++i)
    {
        int size = 0;
        const auto* data = LithiumImportMaps::getNamedResource (LithiumImportMaps::namedResourceList[i], size);
        const auto mapping = juce::JSON::parse (juce::String::fromUTF8 (data, size));

        if (mapping["format"].toString().equalsIgnoreCase (format))
            result.push_back (mapping);
    }

    return result;
}

std::vector<PresetModel> presetsFromMapping (const juce::var& mapping, const std::vector<KitModel>& kits)
{
    std::vector<PresetModel> presets;

    if (const auto* list = mapping["presets"].getArray())
    {
        for (const auto& p : *list)
        {
            const auto base = p["base"].toString();

            if (std::any_of (kits.begin(), kits.end(), [&] (const auto& k) { return k.id == base; }))
                presets.push_back ({ p["id"].toString(), p["name"].toString(), p["description"].toString(), base,
                                     toStringArray (p["tags"]), p["pieces"], p["mics"], p["midiMap"] });
        }
    }

    return presets;
}

juce::StringArray toStringArray (const juce::var& v)
{
    juce::StringArray result;

    if (const auto* a = v.getArray())
        for (const auto& s : *a)
            result.add (s.toString());

    return result;
}

juce::var object()                         { return juce::var (new juce::DynamicObject()); }

void put (juce::var& obj, const juce::Identifier& key, const juce::var& value)
{
    obj.getDynamicObject()->setProperty (key, value);
}

} // namespace lithium::importer
