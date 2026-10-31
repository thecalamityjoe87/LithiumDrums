#include "DrumGizmoImporter.h"

#include "LithiumImportMaps.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <numeric>
#include <set>

namespace lithium::importer
{

namespace
{
    // Import runs on whatever thread calls it (never the audio thread), so a
    // local exception is the simplest way to stop with a message from deep
    // inside the conversion.
    struct ImportError
    {
        juce::String message;
    };

    [[noreturn]] void fail (const juce::String& message)
    {
        throw ImportError { message };
    }

    //==============================================================================
    // DrumGizmo's format, as read from its XML files.

    struct DgAudio
    {
        juce::String channel;    // the kit channel this feeds (after the instrument's channelmap)
        juce::String file;       // relative to the kit's root folder
        int fileChannel = 1;     // 1-based
    };

    struct DgSample
    {
        juce::String name;
        double power = 0.0;
        std::vector<DgAudio> audio;
    };

    struct DgInstrument
    {
        juce::String name, group;
        juce::StringArray chokes;          // instruments this one chokes (drumkit format 2.x)
        std::vector<DgSample> samples;
    };

    struct DgKit
    {
        juce::File xmlFile, root;
        juce::String name, description, author, license, website;
        juce::StringArray channels;
        std::vector<DgInstrument> instruments;

        const DgInstrument* find (const juce::String& instrumentName) const
        {
            for (const auto& i : instruments)
                if (i.name == instrumentName)
                    return &i;

            return nullptr;
        }
    };

    std::unique_ptr<juce::XmlElement> parseXml (const juce::File& file)
    {
        auto xml = juce::XmlDocument::parse (file);

        if (xml == nullptr)
            fail ("Can't read " + file.getFullPathName() + " as XML");

        return xml;
    }

    juce::String childText (const juce::XmlElement* parent, const char* tag)
    {
        return parent != nullptr ? parent->getChildElementAllSubText (tag, {}).trim() : juce::String();
    }

    DgInstrument parseInstrument (const DgKit& kit, const juce::XmlElement& entry)
    {
        DgInstrument instrument;
        instrument.name = entry.getStringAttribute ("name");
        instrument.group = entry.getStringAttribute ("group");

        // The channelmap renames the instrument's channels ("in") to kit channels ("out").
        std::map<juce::String, juce::String> channelMap;

        for (auto* map : entry.getChildWithTagNameIterator ("channelmap"))
            channelMap[map->getStringAttribute ("in")] = map->getStringAttribute ("out");

        if (auto* chokes = entry.getChildByName ("chokes"))
            for (auto* choke : chokes->getChildWithTagNameIterator ("choke"))
                instrument.chokes.add (choke->getStringAttribute ("instrument"));

        const auto xmlFile = kit.root.getChildFile (entry.getStringAttribute ("file"));

        if (! xmlFile.existsAsFile())
            fail ("Instrument \"" + instrument.name + "\" refers to a missing file " + xmlFile.getFullPathName());

        const auto xml = parseXml (xmlFile);
        const auto folder = xmlFile.getParentDirectory();

        if (auto* samples = xml->getChildByName ("samples"))
        {
            for (auto* s : samples->getChildWithTagNameIterator ("sample"))
            {
                DgSample sample;
                sample.name = s->getStringAttribute ("name");
                sample.power = s->getDoubleAttribute ("power", -1.0);

                for (auto* a : s->getChildWithTagNameIterator ("audiofile"))
                {
                    const auto in = a->getStringAttribute ("channel");
                    const auto it = channelMap.find (in);
                    const auto audioFile = folder.getChildFile (a->getStringAttribute ("file"));

                    sample.audio.push_back ({ it != channelMap.end() ? it->second : in,
                                              audioFile.getRelativePathFrom (kit.root).replaceCharacter ('\\', '/'),
                                              a->getIntAttribute ("filechannel", 1) });
                }

                instrument.samples.push_back (std::move (sample));
            }
        }

        // Instrument format 1.0 has no power values; it assigns samples to
        // velocity ranges (0-1) instead. Use the middle of each sample's range.
        if (auto* velocities = xml->getChildByName ("velocities"))
        {
            for (auto& sample : instrument.samples)
            {
                if (sample.power >= 0.0)
                    continue;

                double sum = 0.0;
                int count = 0;

                for (auto* v : velocities->getChildWithTagNameIterator ("velocity"))
                {
                    for (auto* ref : v->getChildWithTagNameIterator ("sampleref"))
                    {
                        if (ref->getStringAttribute ("name") == sample.name)
                        {
                            sum += 0.5 * (v->getDoubleAttribute ("lower") + v->getDoubleAttribute ("upper"));
                            ++count;
                        }
                    }
                }

                sample.power = count > 0 ? sum / count : 0.0;
            }
        }

        for (auto& sample : instrument.samples)
            sample.power = std::max (0.0, sample.power);

        return instrument;
    }

    DgKit parseDrumkit (const juce::File& xmlFile)
    {
        const auto xml = parseXml (xmlFile);

        if (! xml->hasTagName ("drumkit"))
            fail (xmlFile.getFileName() + " isn't a DrumGizmo drumkit file");

        DgKit kit;
        kit.xmlFile = xmlFile;
        kit.root = xmlFile.getParentDirectory();

        // Older kits (DRSKit, MuldjordKit) only have name and description
        // attributes; format 2.x adds a <metadata> block.
        kit.name = xml->getStringAttribute ("name");
        kit.description = xml->getStringAttribute ("description");

        if (auto* meta = xml->getChildByName ("metadata"))
        {
            if (auto title = childText (meta, "title"); title.isNotEmpty())
                kit.name = title;

            if (auto d = childText (meta, "description"); d.isNotEmpty())
                kit.description = d;

            kit.author = childText (meta, "author");
            kit.license = childText (meta, "license");
            kit.website = childText (meta, "website");
        }

        if (auto* channels = xml->getChildByName ("channels"))
            for (auto* c : channels->getChildWithTagNameIterator ("channel"))
                kit.channels.add (c->getStringAttribute ("name"));

        if (auto* instruments = xml->getChildByName ("instruments"))
            for (auto* i : instruments->getChildWithTagNameIterator ("instrument"))
                kit.instruments.push_back (parseInstrument (kit, *i));

        if (kit.channels.isEmpty() || kit.instruments.empty())
            fail (xmlFile.getFileName() + " lists no channels or no instruments");

        return kit;
    }

    std::vector<std::pair<int, juce::String>> readMidimap (const juce::File& file)
    {
        std::vector<std::pair<int, juce::String>> entries;

        if (file.existsAsFile())
        {
            const auto xml = parseXml (file);

            for (auto* map : xml->getChildWithTagNameIterator ("map"))
                if (const auto note = map->getIntAttribute ("note", -1); juce::isPositiveAndBelow (note, 128))
                    entries.emplace_back (note, map->getStringAttribute ("instr"));
        }

        return entries;
    }

    //==============================================================================
    // What the native kit will contain.

    struct OutMic
    {
        juce::String id, name, role;
        juce::StringArray channels;        // one (mono) or two (stereo L, R) DrumGizmo channels
        double volumeDb = 0.0, pan = 0.0;
        bool invertPolarity = false;
    };

    struct OutArticulation
    {
        juce::String instrument, id, name, chokeGroup;
        juce::StringArray chokes;
    };

    struct OutPiece
    {
        juce::String id, name, role;
        std::vector<OutArticulation> articulations;
    };

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
        if (n.contains ("hihat") || n.contains ("hi-hat") || n.startsWith ("hh"))                           return "hihat";
        if (n.contains ("floor") || n.contains ("ftom"))                                                     return "floor_tom";
        if (n.contains ("tom"))                                                                              return "tom";
        if (n.contains ("crash") || n.contains ("splash"))                                                   return "crash";
        if (n.contains ("ride"))                                                                             return "ride";
        if (n.contains ("china"))                                                                            return "china";
        return "other";
    }

    juce::String micRoleForChannel (const juce::String& name)
    {
        const auto n = name.toLowerCase();

        if (n.startsWith ("amb") || n.contains ("room"))       return "room";
        if (n.startsWith ("oh") || n.contains ("overhead"))    return "overheads";
        return "close";
    }

    /** Mics when there's no mapping: room and overhead channels whose names
        end in L and R become stereo pairs, everything else a mono close mic. */
    std::vector<OutMic> automaticMics (const juce::StringArray& channels)
    {
        std::vector<OutMic> mics;
        juce::StringArray used;

        for (const auto& c : channels)
        {
            if (used.contains (c))
                continue;

            const auto role = micRoleForChannel (c);

            if (role != "close" && (c.endsWithIgnoreCase ("L") || c.endsWithIgnoreCase ("R")))
            {
                const auto stem = c.dropLastCharacters (1);
                const auto left = channels.contains (stem + "L") ? stem + "L" : stem + "l";
                const auto right = channels.contains (stem + "R") ? stem + "R" : stem + "r";

                if (channels.contains (left) && channels.contains (right))
                {
                    // Audience perspective, like the kit stage: the drummer's left is on the right.
                    mics.push_back ({ toId (stem), stem.trimCharactersAtEnd ("_- "), role, juce::StringArray (right, left) });
                    used.addArray (juce::StringArray { left, right });
                    continue;
                }
            }

            mics.push_back ({ toId (c), c, role, juce::StringArray (c) });
            used.add (c);
        }

        return mics;
    }

    /** Pieces when there's no mapping: one piece per instrument, except that
        instruments in a choke group (DrumGizmo's "group" attribute, normally
        the hi-hat) become articulations of one piece. In a group, "open"
        instruments can be choked, and the others choke them. */
    std::vector<OutPiece> automaticPieces (const std::vector<const DgInstrument*>& instruments)
    {
        std::vector<OutPiece> pieces;
        std::map<juce::String, size_t> groupPiece;

        for (const auto* i : instruments)
        {
            OutArticulation art { i->name, toId (i->name), i->name, {}, {} };

            if (i->group.isNotEmpty())
            {
                const auto group = toId (i->group);

                if (i->name.containsIgnoreCase ("open"))
                    art.chokeGroup = group + "_open";
                else
                    art.chokes.add (group + "_open");

                if (const auto it = groupPiece.find (group); it != groupPiece.end())
                {
                    pieces[it->second].articulations.push_back (art);
                    continue;
                }

                groupPiece[group] = pieces.size();
                pieces.push_back ({ group, i->group, roleForName (i->group + " " + i->name), { art } });
                continue;
            }

            art.id = "hit";
            art.name = "Hit";
            pieces.push_back ({ toId (i->name), i->name, roleForName (i->name), { art } });
        }

        // DrumGizmo 2.x <chokes>: a choked instrument joins a group named after
        // it (if it isn't in one already), and the choking one chokes that group.
        for (const auto* i : instruments)
        {
            for (const auto& target : i->chokes)
            {
                for (auto& p : pieces)
                {
                    for (auto& a : p.articulations)
                    {
                        if (a.instrument == target && a.chokeGroup.isEmpty())
                            a.chokeGroup = "choke_" + toId (target);

                        if (a.instrument == i->name)
                            for (auto& b : p.articulations)
                                if (b.instrument == target)
                                    a.chokes.addIfNotAlreadyThere (b.chokeGroup.isNotEmpty() ? b.chokeGroup : "choke_" + toId (target));
                    }
                }
            }
        }

        // Give pieces with the same id unique ones.
        std::map<juce::String, int> seen;

        for (auto& p : pieces)
            if (const auto n = ++seen[p.id]; n > 1)
                p.id += "_" + juce::String (n);

        return pieces;
    }

    //==============================================================================
    juce::var findBuiltInMapping (const DgKit& kit)
    {
        for (int i = 0; i < LithiumImportMaps::namedResourceListSize; ++i)
        {
            int size = 0;
            const auto* data = LithiumImportMaps::getNamedResource (LithiumImportMaps::namedResourceList[i], size);
            const auto mapping = juce::JSON::parse (juce::String::fromUTF8 (data, size));

            if (const auto* matches = mapping["matches"].getArray())
                for (const auto& m : *matches)
                    if (m.toString().equalsIgnoreCase (kit.name))
                        return mapping;
        }

        return {};
    }

    juce::StringArray toStringArray (const juce::var& v)
    {
        juce::StringArray result;

        if (const auto* a = v.getArray())
            for (const auto& s : *a)
                result.add (s.toString());

        return result;
    }

    //==============================================================================
    juce::var object()                         { return juce::var (new juce::DynamicObject()); }

    void put (juce::var& obj, const juce::Identifier& key, const juce::var& value)
    {
        obj.getDynamicObject()->setProperty (key, value);
    }

    juce::var channelMapFor (const std::map<juce::String, int>& channels, const std::vector<OutMic>& mics)
    {
        auto map = object();

        for (const auto& mic : mics)
        {
            std::vector<int> found;

            for (const auto& c : mic.channels)
                if (const auto it = channels.find (c); it != channels.end())
                    found.push_back (it->second);

            if (found.empty())
                continue;

            if (mic.channels.size() == 2 && found.size() == 2)
                put (map, mic.id, juce::Array<juce::var> { found[0], found[1] });
            else
                put (map, mic.id, found[0]);    // mono, or one side of a stereo pair (plays centred)
        }

        return map;
    }

    juce::String writeLicenseText (const juce::String& name, const juce::String& author, const juce::String& sourceUrl,
                                   const juce::String& licenseName, const juce::String& licenseUrl,
                                   const juce::String& credits, const juce::StringArray& originalFiles)
    {
        juce::String text;
        text << name << "\n"
             << juce::String::repeatedString ("=", name.length()) << "\n\n"
             << "Author: " << author << "\n"
             << "Source: " << sourceUrl << "\n"
             << "License: " << licenseName << (licenseUrl.isNotEmpty() ? " (" + licenseUrl + ")" : juce::String()) << "\n\n";

        if (credits.isNotEmpty())
            text << credits << "\n\n";

        text << "These samples were converted from the original DrumGizmo kit for use in Lithium Drums.\n"
             << "The audio itself is unchanged. The velocity zones were derived from DrumGizmo's\n"
             << "per-sample power values, and the mics and pieces were renamed and regrouped.\n";

        if (! originalFiles.isEmpty())
            text << "\nThe kit's original notice is included as: " << originalFiles.joinIntoString (", ") << "\n";

        return text;
    }

    /** Recognises the usual Creative Commons licences in a kit's notice. */
    std::pair<juce::String, juce::String> detectLicense (const juce::String& text)
    {
        struct Known { const char* phrase; const char* name; const char* url; };

        // More specific phrases first: "Attribution-ShareAlike" also contains "Attribution".
        static const Known known[] {
            { "Attribution-NonCommercial-ShareAlike 4.0", "CC BY-NC-SA 4.0", "https://creativecommons.org/licenses/by-nc-sa/4.0/" },
            { "Attribution-NonCommercial 4.0",            "CC BY-NC 4.0",    "https://creativecommons.org/licenses/by-nc/4.0/" },
            { "Attribution-ShareAlike 4.0",               "CC BY-SA 4.0",    "https://creativecommons.org/licenses/by-sa/4.0/" },
            { "Attribution-ShareAlike 3.0",               "CC BY-SA 3.0",    "https://creativecommons.org/licenses/by-sa/3.0/" },
            { "Attribution 4.0",                          "CC BY 4.0",       "https://creativecommons.org/licenses/by/4.0/" },
            { "Attribution 3.0",                          "CC BY 3.0",       "https://creativecommons.org/licenses/by/3.0/" },
            { "CC0",                                      "CC0 1.0",         "https://creativecommons.org/publicdomain/zero/1.0/" },
        };

        const auto squashed = text.replace ("\n", " ").replace (" license", " License");

        for (const auto& k : known)
            if (squashed.containsIgnoreCase (k.phrase) || squashed.containsIgnoreCase (k.name))
                return { k.name, k.url };

        return {};
    }

    bool linkOrCopy (const juce::File& source, const juce::File& dest, bool allowLink)
    {
        if (! dest.getParentDirectory().createDirectory())
            return false;

        if (allowLink)
        {
            // A hard link shares the data on disk: the five DRSKit variants
            // use the same audio without five copies of it. It only works on
            // the same file system, so fall back to copying.
            std::error_code error;
            std::filesystem::create_hard_link (source.getFullPathName().toStdString(), dest.getFullPathName().toStdString(), error);

            if (! error)
                return true;
        }

        return source.copyFileTo (dest);
    }

    //==============================================================================
    ImportedKit importOne (const juce::File& xmlFile, const juce::var& explicitMapping, const Options& options)
    {
        auto log = [&] (const juce::String& s) { if (options.log) options.log (s); };

        const auto dg = parseDrumkit (xmlFile);
        const auto mapping = explicitMapping.isVoid() ? findBuiltInMapping (dg) : explicitMapping;
        const auto haveMapping = ! mapping.isVoid();
        const auto variant = mapping["variants"][juce::Identifier (xmlFile.getFileName())];

        ImportedKit result;
        result.name = variant["name"].toString();

        if (result.name.isEmpty())
            result.name = dg.name.isNotEmpty() ? dg.name : xmlFile.getFileNameWithoutExtension();

        log ("Importing " + xmlFile.getFileName() + " as \"" + result.name + "\""
             + (haveMapping ? juce::String() : " (no mapping file: automatic mapping)"));

        // --- Mics ---------------------------------------------------------------
        std::vector<OutMic> mics;
        const auto ignored = toStringArray (mapping["ignoreChannels"]);

        if (haveMapping)
        {
            if (const auto* list = mapping["mics"].getArray())
            {
                for (const auto& m : *list)
                {
                    OutMic mic { m["id"].toString(), m["name"].toString(), m["role"].toString(), toStringArray (m["channels"]) };

                    // "levelDb" trims every mic's default fader alike: summed at
                    // unity, a dozen hot mics clip, and this keeps their balance.
                    mic.volumeDb = (double) m.getProperty ("volumeDb", 0.0) + (double) mapping.getProperty ("levelDb", 0.0);
                    mic.pan = (double) m.getProperty ("pan", 0.0);
                    mic.invertPolarity = (bool) m["invertPolarity"];

                    if (mic.channels.size() < 1 || mic.channels.size() > 2)
                        fail ("Mapping mic \"" + mic.id + "\" needs one or two channels");

                    mics.push_back (mic);
                }
            }

            // Kit channels the mapping doesn't mention still get a mic, so nothing is lost.
            for (const auto& c : dg.channels)
            {
                if (ignored.contains (c))
                    continue;

                if (std::none_of (mics.begin(), mics.end(), [&] (const auto& m) { return m.channels.contains (c); }))
                {
                    result.warnings.add ("Channel \"" + c + "\" isn't in the mapping; added as a mono close mic");
                    mics.push_back ({ toId (c), c, "close", juce::StringArray (c) });
                }
            }
        }
        else
        {
            juce::StringArray channels;

            for (const auto& c : dg.channels)
                if (! ignored.contains (c))
                    channels.add (c);

            mics = automaticMics (channels);
        }

        // --- Pieces -------------------------------------------------------------
        std::vector<OutPiece> pieces;
        std::set<juce::String> mappedInstruments;

        if (haveMapping)
        {
            if (const auto* list = mapping["pieces"].getArray())
            {
                for (const auto& p : *list)
                {
                    OutPiece piece { p["id"].toString(), p["name"].toString(), p["role"].toString(), {} };

                    if (const auto* arts = p["articulations"].getArray())
                    {
                        for (const auto& a : *arts)
                        {
                            const auto instrument = a["instrument"].toString();
                            mappedInstruments.insert (instrument);

                            if (dg.find (instrument) == nullptr)
                                continue;   // not in this variant of the kit

                            piece.articulations.push_back ({ instrument, a["id"].toString(), a["name"].toString(),
                                                             a["chokeGroup"].toString(), toStringArray (a["chokes"]) });
                        }
                    }

                    if (! piece.articulations.empty())
                        pieces.push_back (std::move (piece));
                }
            }
        }

        std::vector<const DgInstrument*> unmapped;

        for (const auto& i : dg.instruments)
            if (mappedInstruments.count (i.name) == 0)
                unmapped.push_back (&i);

        if (haveMapping && ! unmapped.empty())
            for (const auto* i : unmapped)
                result.warnings.add ("Instrument \"" + i->name + "\" isn't in the mapping; imported as its own piece");

        for (auto& p : automaticPieces (unmapped))
            pieces.push_back (std::move (p));

        if (pieces.empty() || pieces.size() > 24)
            fail (result.name + ": " + juce::String ((int) pieces.size()) + " pieces (Lithium Drums takes 1 to 24)");

        if (mics.empty() || mics.size() > 24)
            fail (result.name + ": " + juce::String ((int) mics.size()) + " mics (Lithium Drums takes 1 to 24)");

        // --- Destination --------------------------------------------------------
        const auto dest = options.kitsFolder.getChildFile (juce::File::createLegalFileName (result.name));
        result.folder = dest;

        if (dest.exists())
        {
            if (! options.overwrite)
                fail (dest.getFullPathName() + " already exists (use --overwrite to replace it)");

            // Only ever delete something that is clearly a kit.
            if (! dest.getChildFile ("kit.json").existsAsFile() || ! dest.deleteRecursively())
                fail ("Can't replace " + dest.getFullPathName());
        }

        if (! dest.createDirectory())
            fail ("Can't create " + dest.getFullPathName());

        // --- Articulations, zones and hits ------------------------------------------
        std::set<juce::String> audioFiles;
        juce::Array<juce::var> pieceList;
        std::map<juce::String, std::pair<juce::String, juce::String>> instrumentTarget;   // instrument -> (piece, articulation)

        for (const auto& p : pieces)
        {
            auto piece = object();
            put (piece, "id", p.id);
            put (piece, "name", p.name);
            put (piece, "role", p.role);

            juce::Array<juce::var> articulations;

            for (const auto& a : p.articulations)
            {
                const auto& instrument = *dg.find (a.instrument);
                instrumentTarget[a.instrument] = { p.id, a.id };

                if (instrument.samples.empty())
                {
                    result.warnings.add ("Instrument \"" + a.instrument + "\" has no samples; skipped");
                    continue;
                }

                // Where each sample's audio comes from: file -> (kit channel -> file channel).
                std::vector<std::map<juce::String, std::map<juce::String, int>>> sampleSources;

                for (const auto& s : instrument.samples)
                {
                    std::map<juce::String, std::map<juce::String, int>> byFile;

                    for (const auto& audio : s.audio)
                    {
                        byFile[audio.file][audio.channel] = audio.fileChannel;
                        audioFiles.insert (audio.file);
                    }

                    sampleSources.push_back (std::move (byFile));
                }

                // An articulation-wide channel map lets hits be plain file names,
                // which keeps kit.json small: DrumGizmo's multichannel files
                // normally use the same layout for every sample.
                const auto common = sampleSources.front().size() == 1 ? sampleSources.front().begin()->second
                                                                       : std::map<juce::String, int>();
                const auto allCommon = ! common.empty()
                    && std::all_of (sampleSources.begin(), sampleSources.end(),
                                    [&] (const auto& src) { return src.size() == 1 && src.begin()->second == common; });

                auto art = object();
                put (art, "id", a.id);
                put (art, "name", a.name.isNotEmpty() ? a.name : a.id);

                if (a.chokeGroup.isNotEmpty())
                    put (art, "chokeGroup", a.chokeGroup);

                if (! a.chokes.isEmpty())
                {
                    juce::Array<juce::var> chokes;

                    for (const auto& c : a.chokes)
                        chokes.add (c);

                    put (art, "chokes", chokes);
                }

                if (allCommon)
                    put (art, "channelMap", channelMapFor (common, mics));

                std::vector<double> powers;

                for (const auto& s : instrument.samples)
                    powers.push_back (s.power);

                juce::Array<juce::var> zones;

                for (const auto& z : makeZones (powers, options.maxZones, options.minRoundRobins))
                {
                    auto zone = object();
                    put (zone, "velocity", juce::Array<juce::var> { z.velocityLow, z.velocityHigh });
                    put (zone, "velocityRangeDb", std::round ((double) z.velocityRangeDb * 10.0) / 10.0);

                    juce::Array<juce::var> hits;

                    for (const auto index : z.samples)
                    {
                        const auto& src = sampleSources[(size_t) index];
                        const auto toAudioPath = [] (const juce::String& f) { return "audio/" + f; };

                        if (allCommon)
                        {
                            hits.add (toAudioPath (src.begin()->first));
                        }
                        else
                        {
                            juce::Array<juce::var> parts;

                            for (const auto& [file, channels] : src)
                            {
                                auto part = object();
                                put (part, "file", toAudioPath (file));
                                put (part, "channelMap", channelMapFor (channels, mics));
                                parts.add (part);
                            }

                            hits.add (parts.size() == 1 ? parts[0] : juce::var (parts));
                        }

                        ++result.numHits;
                    }

                    put (zone, "roundRobins", hits);
                    zones.add (zone);
                }

                put (art, "zones", zones);
                articulations.add (art);
            }

            put (piece, "articulations", articulations);
            pieceList.add (piece);
        }

        // --- MIDI map -----------------------------------------------------------------
        auto midimapFile = variant["midimap"].toString();

        if (midimapFile.isEmpty())
        {
            // DrumGizmo kits pair DRSKit_basic.xml with Midimap_basic.xml, or
            // have a single Midimap.xml.
            const auto stem = xmlFile.getFileNameWithoutExtension();
            const auto suffix = stem.contains ("_") ? stem.fromFirstOccurrenceOf ("_", false, false) : juce::String();

            for (const auto& candidate : { "Midimap_" + suffix + ".xml", juce::String ("Midimap.xml"), juce::String ("midimap.xml") })
                if (dg.root.getChildFile (candidate).existsAsFile() && midimapFile.isEmpty())
                    midimapFile = candidate;
        }

        juce::Array<juce::var> midiMap;

        for (const auto& [note, instrument] : readMidimap (dg.root.getChildFile (midimapFile)))
        {
            const auto it = instrumentTarget.find (instrument);

            if (it == instrumentTarget.end())
            {
                result.warnings.add ("MIDI map note " + juce::String (note) + " plays \"" + instrument + "\", which isn't in the kit");
                continue;
            }

            auto entry = object();
            put (entry, "note", note);
            put (entry, "piece", it->second.first);
            put (entry, "articulation", it->second.second);
            midiMap.add (entry);
        }

        // --- Audio ----------------------------------------------------------------------
        double sampleRate = 0.0;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        for (const auto& f : audioFiles)
        {
            const auto source = dg.root.getChildFile (f);

            if (! source.existsAsFile())
                fail ("Missing audio file " + source.getFullPathName());

            if (sampleRate <= 0.0)
                if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source)); reader != nullptr)
                    sampleRate = reader->sampleRate;

            const auto target = dest.getChildFile ("audio/" + f);

            if (! linkOrCopy (source, target, ! options.copyAudio))
                fail ("Can't write " + target.getFullPathName());

            result.linked = result.linked || ! options.copyAudio;
            result.audioBytes += source.getSize();
            ++result.numFiles;

            if (result.numFiles % 500 == 0)
                log ("  " + juce::String (result.numFiles) + " of " + juce::String ((int) audioFiles.size()) + " audio files");
        }

        if (sampleRate <= 0.0)
            fail ("Can't read the sample rate of the kit's audio");

        // --- Licence and credits --------------------------------------------------------
        auto licenseName = mapping["license"]["name"].toString();
        auto licenseUrl = mapping["license"]["url"].toString();
        const auto author = mapping["author"].toString().isNotEmpty() ? mapping["author"].toString() : dg.author;
        const auto sourceUrl = mapping["sourceUrl"].toString().isNotEmpty() ? mapping["sourceUrl"].toString() : dg.website;
        const auto credits = mapping["credits"].toString();

        // Copy the kit's own licence notice. Without a mapping, take the usual suspects.
        auto licenseFiles = toStringArray (mapping["license"]["files"]);

        if (licenseFiles.isEmpty())
            for (const auto& f : dg.root.findChildFiles (juce::File::findFiles, false))
                if (f.getFileName().startsWithIgnoreCase ("license") || f.getFileName().startsWithIgnoreCase ("copying")
                    || f.getFileName().startsWithIgnoreCase ("readme"))
                    licenseFiles.add (f.getFileName());

        juce::StringArray copied;

        for (const auto& name : licenseFiles)
        {
            const auto source = dg.root.getChildFile (name);

            if (source.existsAsFile() && source.copyFileTo (dest.getChildFile ("Original " + name)))
                copied.add ("Original " + name);
            else
                result.warnings.add ("Licence file \"" + name + "\" not found in the kit");
        }

        // Without a mapping, recognise the licence from the kit's metadata or its notice.
        if (licenseName.isEmpty())
        {
            auto text = dg.license;

            for (const auto& name : licenseFiles)
                text << "\n" << dg.root.getChildFile (name).loadFileAsString();

            std::tie (licenseName, licenseUrl) = detectLicense (text);

            if (licenseName.isEmpty() && dg.license.isNotEmpty())
                licenseName = dg.license;   // whatever the kit says, verbatim
        }

        if (licenseName.isEmpty())
            result.warnings.add ("No licence found. Check the kit's terms before using or sharing it.");

        dest.getChildFile ("LICENSE.txt").replaceWithText (writeLicenseText (result.name, author, sourceUrl,
                                                                             licenseName.isNotEmpty() ? licenseName : "Unknown",
                                                                             licenseUrl, credits, copied));

        // --- kit.json, written last: its presence means a complete kit ---------------------
        auto kit = object();
        put (kit, "formatVersion", 2);
        put (kit, "type", "kit");
        put (kit, "name", result.name);
        put (kit, "author", author);
        put (kit, "sourceUrl", sourceUrl);

        auto license = object();
        put (license, "name", licenseName.isNotEmpty() ? licenseName : "Unknown");

        if (licenseUrl.isNotEmpty())
            put (license, "url", licenseUrl);

        put (license, "file", "LICENSE.txt");
        put (kit, "license", license);

        if (credits.isNotEmpty())
            put (kit, "credits", credits);

        const auto description = mapping["description"].toString().isNotEmpty() ? mapping["description"].toString() : dg.description;

        if (description.isNotEmpty())
            put (kit, "description", description);

        put (kit, "sampleRate", sampleRate);

        auto importedFrom = object();
        put (importedFrom, "format", "DrumGizmo");
        put (importedFrom, "file", xmlFile.getFileName());
        put (importedFrom, "maxZones", options.maxZones);
        put (importedFrom, "minRoundRobins", options.minRoundRobins);
        put (kit, "importedFrom", importedFrom);

        juce::Array<juce::var> micList;

        for (const auto& m : mics)
        {
            auto mic = object();
            put (mic, "id", m.id);
            put (mic, "name", m.name);
            put (mic, "channels", m.channels.size());
            put (mic, "role", m.role);

            if (std::abs (m.volumeDb) > 1.0e-6)
                put (mic, "volumeDb", m.volumeDb);

            if (std::abs (m.pan) > 1.0e-6)
                put (mic, "pan", m.pan);

            if (m.invertPolarity)
                put (mic, "invertPolarity", true);

            micList.add (mic);
        }

        put (kit, "mics", micList);
        put (kit, "pieces", pieceList);

        if (! midiMap.isEmpty())
            put (kit, "midiMap", midiMap);

        if (! dest.getChildFile ("kit.json").replaceWithText (juce::JSON::toString (kit) + "\n"))
            fail ("Can't write kit.json in " + dest.getFullPathName());

        result.numPieces = (int) pieces.size();
        result.numMics = (int) mics.size();

        for (const auto& w : result.warnings)
            log ("  warning: " + w);

        return result;
    }
} // namespace

//==============================================================================
std::vector<ZoneSplit> makeZones (const std::vector<double>& powers, int maxZones, int minRoundRobins)
{
    const auto n = (int) powers.size();

    if (n == 0)
        return {};

    // Map power linearly onto velocity 1-127 over the instrument's range, the
    // way DrumGizmo maps velocity onto power.
    const auto [minIt, maxIt] = std::minmax_element (powers.begin(), powers.end());
    const auto pMin = *minIt, pMax = *maxIt;
    const auto span = pMax - pMin;

    auto toVelocity = [&] (double p) { return span > 0.0 ? 1.0 + 126.0 * (p - pMin) / span : 127.0; };
    auto toPower = [&] (double v) { return span > 0.0 ? pMin + (v - 1.0) / 126.0 * span : pMax; };

    std::vector<int> order ((size_t) n);
    std::iota (order.begin(), order.end(), 0);
    std::stable_sort (order.begin(), order.end(), [&] (int a, int b) { return powers[(size_t) a] < powers[(size_t) b]; });

    std::vector<double> v ((size_t) n);

    for (int i = 0; i < n; ++i)
        v[(size_t) i] = toVelocity (powers[(size_t) order[(size_t) i]]);

    // Fewer samples than the minimum makes one zone holding all of them.
    const auto minSize = std::max (1, std::min (minRoundRobins, n));
    const auto zones = std::clamp (std::min (maxZones, n / minSize), 1, n);

    // Optimal 1-D clustering by dynamic programming ("natural breaks"): split
    // the sorted values into `zones` contiguous groups, each at least minSize
    // long, minimising the total squared distance of values from their
    // group's mean. Prefix sums make each group's cost O(1).
    std::vector<double> sum ((size_t) n + 1, 0.0), sumSq ((size_t) n + 1, 0.0);

    for (int i = 0; i < n; ++i)
    {
        sum[(size_t) i + 1] = sum[(size_t) i] + v[(size_t) i];
        sumSq[(size_t) i + 1] = sumSq[(size_t) i] + v[(size_t) i] * v[(size_t) i];
    }

    auto cost = [&] (int from, int to)   // values [from, to)
    {
        const auto count = (double) (to - from);
        const auto s = sum[(size_t) to] - sum[(size_t) from];
        return (sumSq[(size_t) to] - sumSq[(size_t) from]) - s * s / count;
    };

    constexpr auto infinity = std::numeric_limits<double>::infinity();
    std::vector<std::vector<double>> best ((size_t) zones + 1, std::vector<double> ((size_t) n + 1, infinity));
    std::vector<std::vector<int>> split ((size_t) zones + 1, std::vector<int> ((size_t) n + 1, 0));
    best[0][0] = 0.0;

    for (int k = 1; k <= zones; ++k)
    {
        for (int end = k * minSize; end <= n; ++end)
        {
            for (int start = (k - 1) * minSize; start <= end - minSize; ++start)
            {
                if (std::isinf (best[(size_t) k - 1][(size_t) start]))
                    continue;

                const auto c = best[(size_t) k - 1][(size_t) start] + cost (start, end);

                if (c < best[(size_t) k][(size_t) end])
                {
                    best[(size_t) k][(size_t) end] = c;
                    split[(size_t) k][(size_t) end] = start;
                }
            }
        }
    }

    std::vector<std::pair<int, int>> groups;   // [start, end) in sorted order

    for (int k = zones, end = n; k > 0; --k)
    {
        const auto start = split[(size_t) k][(size_t) end];
        groups.insert (groups.begin(), { start, end });
        end = start;
    }

    // Each zone's velocity range reaches halfway to its neighbours.
    std::vector<ZoneSplit> result;

    for (size_t g = 0; g < groups.size(); ++g)
    {
        const auto [start, end] = groups[g];
        ZoneSplit z;
        z.velocityLow = g == 0 ? 1 : result.back().velocityHigh + 1;
        z.velocityHigh = 127;

        if (g + 1 < groups.size())
        {
            const auto boundary = (int) std::floor (0.5 * (v[(size_t) end - 1] + v[(size_t) groups[g + 1].first]));
            z.velocityHigh = juce::jlimit (z.velocityLow, 127 - (int) (groups.size() - g - 1), boundary);
        }

        // How much quieter the bottom of the zone is than its top, from the
        // power at each end, so level stays continuous across the zone.
        const auto pLow = toPower (z.velocityLow), pHigh = toPower (z.velocityHigh);
        z.velocityRangeDb = pLow > 0.0 ? (float) juce::jlimit (0.0, 12.0, 20.0 * std::log10 (pHigh / pLow)) : 12.0f;

        for (int i = start; i < end; ++i)
            z.samples.push_back (order[(size_t) i]);

        result.push_back (std::move (z));
    }

    return result;
}

juce::Result importKits (const Options& options, std::vector<ImportedKit>& results)
{
    results.clear();

    try
    {
        juce::var mapping;

        if (options.mappingFile != juce::File())
        {
            if (auto r = juce::JSON::parse (options.mappingFile.loadFileAsString(), mapping); r.failed())
                fail ("Can't read the mapping file: " + r.getErrorMessage());
        }

        std::vector<juce::File> xmlFiles;

        if (options.source.isDirectory())
        {
            for (const auto& f : options.source.findChildFiles (juce::File::findFiles, false, "*.xml"))
                if (auto xml = juce::XmlDocument::parse (f); xml != nullptr && xml->hasTagName ("drumkit"))
                    xmlFiles.push_back (f);

            std::sort (xmlFiles.begin(), xmlFiles.end());
        }
        else if (options.source.existsAsFile())
        {
            xmlFiles.push_back (options.source);
        }

        if (xmlFiles.empty())
            fail ("No DrumGizmo drumkit XML found at " + options.source.getFullPathName());

        if (! options.kitsFolder.createDirectory())
            fail ("Can't create the kits folder " + options.kitsFolder.getFullPathName());

        for (const auto& f : xmlFiles)
            results.push_back (importOne (f, mapping, options));
    }
    catch (const ImportError& e)
    {
        return juce::Result::fail (e.message);
    }

    return juce::Result::ok();
}

} // namespace lithium::importer
