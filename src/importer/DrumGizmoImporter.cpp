#include "DrumGizmoImporter.h"

#include <cmath>
#include <map>
#include <numeric>
#include <optional>
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
        juce::String file;                 // the instrument XML, relative to the kit's root folder
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
        instrument.file = entry.getStringAttribute ("file");

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
    juce::String kitIdFor (const juce::String& name)     { return toId (name).replaceCharacter ('_', '-'); }

    /** One drumkit XML, converted into the kit model. */
    struct Conversion
    {
        juce::File xmlFile;
        DgKit dg;
        KitModel kit;
    };

    Conversion convert (const juce::File& xmlFile, const juce::var& mapping, const Options& options)
    {
        Conversion c;
        c.xmlFile = xmlFile;
        c.dg = parseDrumkit (xmlFile);

        const auto& dg = c.dg;
        auto& kit = c.kit;
        const auto haveMapping = ! mapping.isVoid();
        const auto variant = mapping["variants"][juce::Identifier (xmlFile.getFileName())];

        kit.name = variant["name"].toString();

        if (kit.name.isEmpty())
            kit.name = dg.name.isNotEmpty() ? dg.name : xmlFile.getFileNameWithoutExtension();

        kit.id = variant["id"].toString().isNotEmpty() ? variant["id"].toString() : kitIdFor (kit.name);
        kit.description = variant["description"].toString();
        kit.tags = toStringArray (variant["tags"].isVoid() ? mapping["tags"] : variant["tags"]);

        if (kit.description.isEmpty())
            kit.description = mapping["description"].toString().isNotEmpty() ? mapping["description"].toString() : dg.description;

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

                    // A variant without any of the mic's channels doesn't get the mic.
                    if (std::none_of (mic.channels.begin(), mic.channels.end(), [&] (const auto& ch) { return dg.channels.contains (ch); }))
                        continue;

                    mics.push_back (mic);
                }
            }

            // Kit channels the mapping doesn't mention still get a mic, so nothing is lost.
            for (const auto& ch : dg.channels)
            {
                if (ignored.contains (ch))
                    continue;

                if (std::none_of (mics.begin(), mics.end(), [&] (const auto& m) { return m.channels.contains (ch); }))
                {
                    kit.warnings.add ("Channel \"" + ch + "\" isn't in the mapping; added as a mono close mic");
                    mics.push_back ({ toId (ch), ch, "close", juce::StringArray (ch) });
                }
            }
        }
        else
        {
            juce::StringArray channels;

            for (const auto& ch : dg.channels)
                if (! ignored.contains (ch))
                    channels.add (ch);

            mics = automaticMics (channels);
        }

        for (const auto& m : mics)
            kit.mics.push_back ({ m.id, m.name, m.role.isNotEmpty() ? m.role : juce::String ("close"), m.channels.size(),
                                  m.volumeDb, m.pan, m.invertPolarity });

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

        if (haveMapping)
            for (const auto* i : unmapped)
                kit.warnings.add ("Instrument \"" + i->name + "\" isn't in the mapping; imported as its own piece");

        for (auto& p : automaticPieces (unmapped))
            pieces.push_back (std::move (p));

        if (pieces.empty() || pieces.size() > 24)
            fail (kit.name + ": " + juce::String ((int) pieces.size()) + " pieces (Lithium Drums takes 1 to 24)");

        if (mics.empty() || mics.size() > 24)
            fail (kit.name + ": " + juce::String ((int) mics.size()) + " mics (Lithium Drums takes 1 to 24)");

        // --- Articulations, zones and hits ------------------------------------------
        std::map<juce::String, std::pair<juce::String, juce::String>> instrumentTarget;   // instrument -> (piece, articulation)

        for (const auto& p : pieces)
        {
            KitModel::Piece piece { p.id, p.name, p.role, 0.0, 0.0, {} };

            for (const auto& a : p.articulations)
            {
                const auto& instrument = *dg.find (a.instrument);
                instrumentTarget[a.instrument] = { p.id, a.id };

                if (instrument.samples.empty())
                {
                    kit.warnings.add ("Instrument \"" + a.instrument + "\" has no samples; skipped");
                    continue;
                }

                KitModel::Articulation art { a.id, a.name.isNotEmpty() ? a.name : a.id, a.chokeGroup, a.chokes, {}, -1.0, {} };

                // Each sample is a hit; its audio usually comes from one
                // multichannel file, with one file channel per kit channel.
                auto hitFor = [&] (const DgSample& s)
                {
                    std::map<juce::String, std::map<juce::String, int>> byFile;   // file -> (kit channel -> file channel)

                    for (const auto& audio : s.audio)
                        byFile[audio.file][audio.channel] = audio.fileChannel;

                    KitModel::Hit hit;

                    for (const auto& [file, channels] : byFile)
                    {
                        KitModel::Part part { file, {}, 0 };

                        for (size_t m = 0; m < mics.size(); ++m)
                        {
                            std::vector<int> found;

                            for (const auto& ch : mics[m].channels)
                                if (const auto it = channels.find (ch); it != channels.end())
                                    found.push_back (it->second);

                            if (found.empty())
                                continue;

                            // Mono, a stereo pair, or one side of a pair (which plays centred).
                            const auto pair = mics[m].channels.size() == 2 && found.size() == 2;
                            part.channels.push_back ({ (int) m, { found[0], pair ? found[1] : 0 } });
                        }

                        if (! part.channels.empty())
                            hit.parts.push_back (std::move (part));
                    }

                    return hit;
                };

                std::vector<double> powers;

                for (const auto& s : instrument.samples)
                    powers.push_back (s.power);

                for (const auto& z : makeZones (powers, options.maxZones, options.minRoundRobins))
                {
                    KitModel::Zone zone { z.velocityLow, z.velocityHigh, 0.0, (double) z.velocityRangeDb, {} };

                    for (const auto index : z.samples)
                        zone.hits.push_back (hitFor (instrument.samples[(size_t) index]));

                    art.zones.push_back (std::move (zone));
                }

                piece.articulations.push_back (std::move (art));
            }

            if (! piece.articulations.empty())
                kit.pieces.push_back (std::move (piece));
        }

        // Variants can list articulations to put first, so that a click on a
        // piece plays what the variant is about (brushes, in a brush kit).
        if (const auto prefer = toStringArray (variant["preferArticulations"]); ! prefer.isEmpty())
        {
            for (auto& piece : kit.pieces)
                std::stable_sort (piece.articulations.begin(), piece.articulations.end(), [&] (const auto& a, const auto& b)
                {
                    const auto rank = [&] (const auto& x) { const auto i = prefer.indexOf (x.id); return i < 0 ? prefer.size() : i; };
                    return rank (a) < rank (b);
                });
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

        for (const auto& [note, instrument] : readMidimap (dg.root.getChildFile (midimapFile)))
        {
            const auto it = instrumentTarget.find (instrument);

            if (it == instrumentTarget.end())
            {
                kit.warnings.add ("MIDI map note " + juce::String (note) + " plays \"" + instrument + "\", which isn't in the kit");
                continue;
            }

            kit.midiMap.push_back ({ note, it->second.first, it->second.second });
        }

        auto importedFrom = object();
        put (importedFrom, "format", "DrumGizmo");
        put (importedFrom, "file", xmlFile.getFileName());
        put (importedFrom, "maxZones", options.maxZones);
        put (importedFrom, "minRoundRobins", options.minRoundRobins);
        kit.importedFrom = importedFrom;

        return c;
    }

    /** A variant that plays a subset of another variant's instruments (same
        instrument files, same pieces) becomes a kit preset of it rather than a
        second copy of the kit. Returns nothing, and says why, if it can't. */
    std::optional<PresetModel> asPreset (const Conversion& variant, const Conversion& base, juce::String& whyNot)
    {
        for (const auto& i : variant.dg.instruments)
        {
            const auto* b = base.dg.find (i.name);

            if (b == nullptr || b->file != i.file)
            {
                whyNot = "instrument \"" + i.name + "\" isn't the same in " + base.xmlFile.getFileName();
                return std::nullopt;
            }
        }

        PresetModel preset;
        preset.id = variant.kit.id;
        preset.name = variant.kit.name;
        preset.description = variant.kit.description;
        preset.tags = variant.kit.tags;
        preset.baseId = base.kit.id;

        juce::Array<juce::var> pieces;

        for (const auto& vp : variant.kit.pieces)
        {
            const auto* bp = base.kit.findPiece (vp.id);

            if (bp == nullptr || bp->role != vp.role)
            {
                whyNot = "piece \"" + vp.id + "\" isn't in " + base.kit.name;
                return std::nullopt;
            }

            auto choice = object();
            put (choice, "piece", vp.id);

            juce::StringArray variantArts, baseArts;

            for (const auto& a : vp.articulations)
                variantArts.add (a.id);

            for (const auto& a : bp->articulations)
                baseArts.add (a.id);

            if (variantArts != baseArts)
            {
                juce::Array<juce::var> ids;

                for (const auto& id : variantArts)
                    ids.add (id);

                put (choice, "articulations", ids);
            }

            if (vp.name != bp->name)
                put (choice, "name", vp.name);

            pieces.add (choice);
        }

        preset.pieces = pieces;

        // Mics the variant doesn't record are left out (and never loaded).
        auto mics = object();
        bool anyLeftOut = false;

        for (const auto& m : base.kit.mics)
        {
            if (variant.kit.findMic (m.id) < 0)
            {
                auto change = object();
                put (change, "enabled", false);
                put (mics, m.id, change);
                anyLeftOut = true;
            }
        }

        if (anyLeftOut)
            preset.mics = mics;

        if (! variant.kit.midiMap.empty())
        {
            juce::Array<juce::var> map;

            for (const auto& e : variant.kit.midiMap)
            {
                auto entry = object();
                put (entry, "note", e.note);
                put (entry, "piece", e.piece);
                put (entry, "articulation", e.articulation);
                map.add (entry);
            }

            preset.midiMap = map;
        }

        return preset;
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


bool isDrumGizmoSource (const juce::File& source)
{
    auto isDrumkit = [] (const juce::File& f)
    {
        auto xml = juce::XmlDocument::parse (f);
        return xml != nullptr && xml->hasTagName ("drumkit");
    };

    if (source.existsAsFile())
        return source.hasFileExtension ("xml") && isDrumkit (source);

    for (const auto& f : source.findChildFiles (juce::File::findFiles, false, "*.xml"))
        if (isDrumkit (f))
            return true;

    return false;
}

juce::Result importDrumGizmo (const Options& options, std::vector<ImportedKit>& results)
{
    auto log = [&] (const juce::String& s) { if (options.log) options.log (s); };

    try
    {
        juce::var explicitMapping;

        if (options.mappingFile != juce::File())
        {
            if (auto r = juce::JSON::parse (options.mappingFile.loadFileAsString(), explicitMapping); r.failed())
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

        // Every variant of the kit is converted with the same mapping.
        const auto first = parseDrumkit (xmlFiles.front());
        const auto mapping = explicitMapping.isVoid() ? findBuiltInMapping ({ first.name }, "drumgizmo") : explicitMapping;

        log (mapping.isVoid() ? "No mapping file for \"" + first.name + "\": automatic mapping"
                              : "Using the mapping for \"" + first.name + "\"");

        std::vector<Conversion> conversions;

        for (const auto& f : xmlFiles)
        {
            log ("Reading " + f.getFileName());
            conversions.push_back (convert (f, mapping, options));
        }

        // --- The pack ---------------------------------------------------------------
        PackModel pack;
        pack.sourceRoot = first.root;
        pack.sourceFormat = "DrumGizmo";
        pack.name = mapping["pack"]["name"].toString();

        if (pack.name.isEmpty())
            pack.name = first.name.isNotEmpty() ? first.name : options.source.getFileNameWithoutExtension();

        pack.id = mapping["pack"]["id"].toString().isNotEmpty() ? mapping["pack"]["id"].toString() : kitIdFor (pack.name);
        pack.author = mapping["author"].toString().isNotEmpty() ? mapping["author"].toString() : first.author;
        pack.sourceUrl = mapping["sourceUrl"].toString().isNotEmpty() ? mapping["sourceUrl"].toString() : first.website;
        pack.credits = mapping["credits"].toString();
        pack.description = mapping["description"].toString().isNotEmpty() ? mapping["description"].toString() : first.description;
        pack.licenseName = mapping["license"]["name"].toString();
        pack.licenseUrl = mapping["license"]["url"].toString();
        pack.licenseFiles = toStringArray (mapping["license"]["files"]);
        pack.licenseNote = mapping["licenseNote"].toString();
        pack.conversionNote = "The audio itself is unchanged. The velocity zones were derived from DrumGizmo's\n"
                              "per-sample power values, and the mics and pieces were renamed and regrouped.";

        // Without a mapping, take the usual licence notices and recognise the licence in them.
        if (pack.licenseFiles.isEmpty())
            for (const auto& f : first.root.findChildFiles (juce::File::findFiles, false))
                if (f.getFileName().startsWithIgnoreCase ("license") || f.getFileName().startsWithIgnoreCase ("copying")
                    || f.getFileName().startsWithIgnoreCase ("readme"))
                    pack.licenseFiles.add (f.getFileName());

        if (pack.licenseName.isEmpty())
        {
            auto text = first.license;

            for (const auto& name : pack.licenseFiles)
                text << "\n" << first.root.getChildFile (name).loadFileAsString();

            std::tie (pack.licenseName, pack.licenseUrl) = detectLicense (text);

            if (pack.licenseName.isEmpty() && first.license.isNotEmpty())
                pack.licenseName = first.license;   // whatever the kit says, verbatim
        }

        if (pack.licenseName.isEmpty())
            pack.warnings.add ("No licence found. Check the kit's terms before using or sharing it.");

        pack.shareAlike = mapping["license"]["shareAlike"].isVoid() ? pack.licenseName.containsIgnoreCase ("-SA")
                                                                    : (bool) mapping["license"]["shareAlike"];

        // --- Kits and presets ---------------------------------------------------------
        for (const auto& c : conversions)
        {
            const auto baseFile = mapping["variants"][juce::Identifier (c.xmlFile.getFileName())]["presetOf"].toString();
            const auto base = std::find_if (conversions.begin(), conversions.end(),
                                            [&] (const auto& b) { return baseFile.isNotEmpty() && b.xmlFile.getFileName() == baseFile; });

            if (base != conversions.end() && &*base != &c)
            {
                juce::String whyNot;

                if (auto preset = asPreset (c, *base, whyNot))
                {
                    pack.presets.push_back (std::move (*preset));
                    continue;
                }

                pack.warnings.add (c.kit.name + " imported as a kit of its own: " + whyNot);
            }

            pack.kits.push_back (c.kit);
        }

        // Presets written by hand in the mapping (other piece choices, tuning, mixes).
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
