#include "KitManifest.h"

#include <algorithm>
#include <cmath>

namespace lithium
{

namespace
{
    // Parsing runs off the audio thread, so a local exception is the simplest
    // way to bail out of deeply nested validation with a useful message.
    struct ParseError
    {
        juce::String message;
    };

    [[noreturn]] void fail (const juce::String& where, const juce::String& what)
    {
        throw ParseError { where + ": " + what };
    }

    juce::String getString (const juce::var& obj, const char* key, const juce::String& where, bool required)
    {
        const auto& v = obj[key];

        if (v.isString())
            return v.toString();

        if (required || ! v.isVoid())
            fail (where, juce::String ("\"") + key + "\" must be a string");

        return {};
    }

    double getNumber (const juce::var& obj, const char* key, const juce::String& where, double fallback)
    {
        const auto& v = obj[key];

        if (v.isVoid())
            return fallback;

        if (! (v.isInt() || v.isInt64() || v.isDouble()))
            fail (where, juce::String ("\"") + key + "\" must be a number");

        return (double) v;
    }

    const juce::Array<juce::var>& getArray (const juce::var& obj, const char* key, const juce::String& where)
    {
        if (const auto* a = obj[key].getArray())
            return *a;

        fail (where, juce::String ("\"") + key + "\" must be an array");
    }

    // Kit-relative paths only: no absolute paths and no escaping the kit folder.
    juce::String checkRelativePath (const juce::String& path, const juce::String& where)
    {
        auto p = path.replaceCharacter ('\\', '/');

        if (p.isEmpty() || p.startsWithChar ('/') || p.containsChar (':')
            || p == ".." || p.startsWith ("../") || p.contains ("/../") || p.endsWith ("/.."))
            fail (where, "\"" + path + "\" must be a relative path inside the kit folder");

        return p;
    }

    bool isValidId (const juce::String& id)
    {
        return id.isNotEmpty() && id.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.");
    }

    // A channel map says which channels of a multichannel file feed which mic:
    // { "kickIn": 3, "overheads": [6, 7] }, with 1-based channel numbers.
    using ChannelMap = std::vector<std::pair<int, std::array<int, 2>>>;

    ChannelMap parseChannelMap (const juce::var& value, const KitManifest& kit, const juce::String& where)
    {
        auto* obj = value.getDynamicObject();

        if (obj == nullptr)
            fail (where, "\"channelMap\" must be an object mapping mic IDs to file channels");

        ChannelMap result;

        for (const auto& [name, v] : obj->getProperties())
        {
            const auto mic = kit.findMic (name.toString());

            if (mic < 0)
                fail (where, "\"channelMap\" names \"" + name.toString() + "\", which is not one of the kit's mics");

            const auto micChannels = kit.mics[(size_t) mic].channels;
            std::array<int, 2> channels {};

            if (v.isInt() || v.isInt64())
            {
                // A single file channel on a stereo mic plays centred.
                channels = { (int) v, micChannels == 2 ? (int) v : 0 };
            }
            else if (const auto* a = v.getArray(); a != nullptr && a->size() == micChannels)
            {
                channels[0] = (int) (*a)[0];

                if (micChannels == 2)
                    channels[1] = (int) (*a)[1];
            }
            else
            {
                fail (where, "channels for mic \"" + name.toString() + "\" must be a number or an array of "
                                 + juce::String (micChannels));
            }

            for (int ch = 0; ch < micChannels; ++ch)
                if (channels[(size_t) ch] < 1 || channels[(size_t) ch] > 256)
                    fail (where, "file channels are numbered from 1");

            result.emplace_back (mic, channels);
        }

        return result;
    }

    HitManifest hitFromFile (const juce::String& file, const ChannelMap& map, std::int64_t start)
    {
        HitManifest hit;

        for (const auto& [mic, channels] : map)
            hit.sources.push_back ({ mic, file, channels, start });

        return hit;
    }

    // "start": frames to skip at the start of the file (SFZ's "offset"), for
    // recordings with silence or pedal noise before the hit.
    std::int64_t getStart (const juce::var& obj, const juce::String& where, std::int64_t fallback)
    {
        const auto start = (std::int64_t) getNumber (obj, "start", where, (double) fallback);

        if (start < 0)
            fail (where, "\"start\" can't be negative");

        return start;
    }

    HitManifest parseHit (const juce::var& value, const KitManifest& kit, const ChannelMap* articulationMap,
                          std::int64_t articulationStart, const juce::String& where)
    {
        // 1. "file.wav": one multichannel file, read through the articulation's channel map.
        if (value.isString())
        {
            if (articulationMap == nullptr)
                fail (where, "a hit given as a file name needs a \"channelMap\" on its articulation");

            return hitFromFile (checkRelativePath (value.toString(), where), *articulationMap, articulationStart);
        }

        // 2. { "file": "...", "channelMap": {...} }: a multichannel file with its own map.
        if (value["file"].isString())
        {
            const auto file = checkRelativePath (value["file"].toString(), where);
            const auto start = getStart (value, where, articulationStart);

            if (! value["channelMap"].isVoid())
                return hitFromFile (file, parseChannelMap (value["channelMap"], kit, where), start);

            if (articulationMap == nullptr)
                fail (where, "a hit needs a \"channelMap\", on itself or on its articulation");

            return hitFromFile (file, *articulationMap, start);
        }

        // 3. [ { "file", "channelMap" }, ... ]: mics spread over several multichannel files.
        if (const auto* parts = value.getArray())
        {
            HitManifest hit;

            for (const auto& part : *parts)
            {
                if (! part["file"].isString() || part["channelMap"].isVoid())
                    fail (where, "each part of a hit list needs a \"file\" and a \"channelMap\"");

                auto h = hitFromFile (checkRelativePath (part["file"].toString(), where),
                                      parseChannelMap (part["channelMap"], kit, where),
                                      getStart (part, where, articulationStart));
                hit.sources.insert (hit.sources.end(), h.sources.begin(), h.sources.end());
            }

            return hit;
        }

        // 4. { "files": { "kickIn": "...", ... } }: one file per mic.
        if (auto* files = value["files"].getDynamicObject())
        {
            HitManifest hit;
            const auto start = getStart (value, where, articulationStart);

            for (const auto& [name, v] : files->getProperties())
            {
                const auto mic = kit.findMic (name.toString());

                if (mic < 0)
                    fail (where, "\"files\" names \"" + name.toString() + "\", which is not one of the kit's mics");

                if (! v.isString())
                    fail (where, "the file for mic \"" + name.toString() + "\" must be a string");

                // A mono file on a stereo mic is handled by the loader (it plays centred).
                const auto stereo = kit.mics[(size_t) mic].channels == 2;
                hit.sources.push_back ({ mic, checkRelativePath (v.toString(), where), { 1, stereo ? 2 : 0 }, start });
            }

            return hit;
        }

        fail (where, "a hit must be a file name, { \"file\", \"channelMap\" }, a list of those, or { \"files\" }");
    }

    ZoneManifest parseZone (const juce::var& zone, const KitManifest& kit, const ChannelMap* map, std::int64_t start,
                            const juce::String& where)
    {
        ZoneManifest result;

        const auto& vel = getArray (zone, "velocity", where);

        if (vel.size() != 2 || ! vel[0].isInt() || ! vel[1].isInt())
            fail (where, "\"velocity\" must be [low, high]");

        result.velocityLow = (int) vel[0];
        result.velocityHigh = (int) vel[1];

        if (result.velocityLow < 1 || result.velocityHigh > 127 || result.velocityLow > result.velocityHigh)
            fail (where, "velocity range must lie within 1..127 with low <= high");

        result.gainDb = (float) getNumber (zone, "gainDb", where, 0.0);
        result.velocityRangeDb = (float) juce::jlimit (0.0, 24.0, getNumber (zone, "velocityRangeDb", where, 6.0));

        const auto& rr = getArray (zone, "roundRobins", where);

        for (int i = 0; i < rr.size(); ++i)
            result.roundRobins.push_back (parseHit (rr.getReference (i), kit, map, start,
                                                    where + ".roundRobins[" + juce::String (i) + "]"));

        if (result.roundRobins.empty() || result.roundRobins.size() > (size_t) maxRoundRobins)
            fail (where, "a zone needs between 1 and " + juce::String (maxRoundRobins) + " round-robins");

        return result;
    }

    ArticulationManifest parseArticulation (const juce::var& art, const KitManifest& kit, const juce::String& where)
    {
        ArticulationManifest result;
        result.id = getString (art, "id", where, true);
        result.name = getString (art, "name", where, false);
        result.chokeGroup = getString (art, "chokeGroup", where, false);

        if (! isValidId (result.id))
            fail (where, "\"" + result.id + "\" is not a valid id (letters, digits, '_', '-', '.')");

        if (result.name.isEmpty())
            result.name = result.id;

        if (! art["chokes"].isVoid())
            for (const auto& g : getArray (art, "chokes", where))
                result.chokes.addIfNotAlreadyThere (g.toString());

        auto unitValue = [&] (const char* key) -> std::optional<float>
        {
            if (art[key].isVoid())
                return std::nullopt;

            const auto v = (float) getNumber (art, key, where, -1.0);

            if (v < 0.0f || v > 1.0f)
                fail (where, "\"" + juce::String (key) + "\" must be within 0..1");

            return v;
        };

        result.openness = unitValue ("openness");
        result.position = unitValue ("position");

        if (const auto stroke = getString (art, "stroke", where, false); stroke.isNotEmpty())
        {
            result.stroke = articulations::strokeFromKey (stroke.toStdString());

            if (! result.stroke.has_value())
                fail (where, "\"stroke\" must be \"tip\" or \"shank\"");
        }

        std::optional<ChannelMap> map;

        if (! art["channelMap"].isVoid())
            map = parseChannelMap (art["channelMap"], kit, where);

        const auto start = getStart (art, where, 0);
        const auto& zones = getArray (art, "zones", where);

        for (int i = 0; i < zones.size(); ++i)
            result.zones.push_back (parseZone (zones.getReference (i), kit, map.has_value() ? &*map : nullptr, start,
                                               where + ".zones[" + juce::String (i) + "]"));

        if (result.zones.empty())
            fail (where, "an articulation needs at least one velocity zone");

        std::sort (result.zones.begin(), result.zones.end(),
                   [] (const auto& a, const auto& b) { return a.velocityLow < b.velocityLow; });

        for (size_t i = 1; i < result.zones.size(); ++i)
            if (result.zones[i].velocityLow <= result.zones[i - 1].velocityHigh)
                fail (where, "velocity zones overlap");

        return result;
    }

    void parseCredits (const juce::var& root, const juce::File& folder, KitCredits& c)
    {
        const juce::String top ("kit.json");
        c.name = getString (root, "name", top, true);
        c.author = getString (root, "author", top, false);
        c.sourceUrl = getString (root, "sourceUrl", top, false);
        c.credits = getString (root, "credits", top, false);
        c.description = getString (root, "description", top, false);

        if (const auto& license = root["license"]; ! license.isVoid())
        {
            c.licenseName = getString (license, "name", "kit.json license", true);
            c.licenseUrl = getString (license, "url", "kit.json license", false);

            if (const auto file = getString (license, "file", "kit.json license", false); file.isNotEmpty())
                c.licenseFile = folder.getChildFile (checkRelativePath (file, "kit.json license"));

            // Explicit, or recognised from the usual names ("CC BY-SA 3.0").
            c.shareAlike = license["shareAlike"].isVoid() ? c.licenseName.containsIgnoreCase ("-SA")
                                                          : (bool) license["shareAlike"];
        }
    }

    juce::var parseRoot (const juce::String& json)
    {
        juce::var root;

        if (auto r = juce::JSON::parse (json, root); r.failed())
            fail ("kit.json", "not valid JSON: " + r.getErrorMessage());

        if (root.getDynamicObject() == nullptr)
            fail ("kit.json", "the top level must be an object");

        const auto version = (int) root["formatVersion"];

        if (version != kitFormatVersion)
            fail ("kit.json", "unsupported formatVersion " + root["formatVersion"].toString()
                                  + " (this build reads version " + juce::String (kitFormatVersion) + ")");

        if (const auto type = root["type"].toString(); type.isNotEmpty() && type != "kit" && type != "preset")
            fail ("kit.json", "kits of type \"" + type + "\" aren't supported by this version");

        return root;
    }

    bool isPresetRoot (const juce::var& root)   { return root["type"].toString() == "preset"; }

    juce::String idFrom (const juce::var& root, const juce::File& folder)
    {
        const auto id = root["id"].toString();

        if (id.isNotEmpty() && ! isValidId (id))
            fail ("kit.json", "\"" + id + "\" is not a valid kit id (letters, digits, '_', '-', '.')");

        return id.isNotEmpty() ? id : defaultKitId (folder);
    }

    juce::StringArray tagsFrom (const juce::var& root)
    {
        juce::StringArray tags;

        if (const auto* a = root["tags"].getArray())
            for (const auto& t : *a)
                tags.addIfNotAlreadyThere (t.toString().trim().toLowerCase());

        tags.removeEmptyStrings();
        return tags;
    }

    juce::var loadRoot (const juce::File& folder)
    {
        const auto file = folder.getChildFile ("kit.json");

        if (! file.existsAsFile())
            fail ("kit.json", "no kit.json in " + folder.getFullPathName());

        return parseRoot (file.loadFileAsString());
    }

    PieceRole parseRole (const juce::String& key, const juce::String& where)
    {
        if (auto role = roleFromKey (key.toStdString()))
            return *role;

        fail (where, "unknown \"role\" (kick, snare, hihat, tom, floor_tom, crash, ride, china or other)");
    }

    /** The settings a kit or a preset can give a piece, on top of its recordings. */
    void parsePieceSettings (const juce::var& p, const juce::String& where, PieceManifest& piece)
    {
        piece.chokeNote = (int) getNumber (p, "chokeNote", where, (double) piece.chokeNote);
        piece.tuneSemitones = (float) getNumber (p, "tune", where, (double) piece.tuneSemitones);
        piece.volumeDb = (float) getNumber (p, "volumeDb", where, (double) piece.volumeDb);
        piece.decay = (float) getNumber (p, "decay", where, (double) piece.decay * 100.0) / 100.0f;

        if (piece.chokeNote < -1 || piece.chokeNote > 127)
            fail (where, "\"chokeNote\" must be a MIDI note (0-127)");

        if (std::abs (piece.tuneSemitones) > 24.0f)
            fail (where, "\"tune\" must be within -24..24 semitones");

        if (piece.volumeDb < -60.0f || piece.volumeDb > 24.0f)
            fail (where, "\"volumeDb\" must be within -60..24");

        if (piece.decay < 0.0f || piece.decay > 1.0f)
            fail (where, "\"decay\" must be 0-100 (100 = natural)");
    }

    void parseMidiMap (const juce::var& root, KitManifest& result)
    {
        if (root["midiMap"].isVoid())
            return;

        for (const auto& [i, e] : juce::enumerate (getArray (root, "midiMap", "kit.json")))
        {
            const auto where = "midiMap[" + juce::String (i) + "]";
            MidiMapEntry entry;
            entry.note = (int) getNumber (e, "note", where, -1.0);
            entry.piece = getString (e, "piece", where, true);
            entry.articulation = getString (e, "articulation", where, false);

            if (! juce::isPositiveAndBelow (entry.note, 128))
                fail (where, "\"note\" must be 0-127");

            const auto piece = result.findPiece (entry.piece);

            if (piece < 0)
            {
                result.warnings.add (where + ": unknown piece \"" + entry.piece + "\" ignored");
                continue;
            }

            const auto& arts = result.pieces[(size_t) piece].articulations;

            if (entry.articulation.isNotEmpty()
                && std::none_of (arts.begin(), arts.end(), [&] (const auto& a) { return a.id == entry.articulation; }))
            {
                result.warnings.add (where + ": unknown articulation \"" + entry.articulation + "\" ignored");
                continue;
            }

            result.midiMap.push_back (std::move (entry));
        }
    }

    void checkCounts (const KitManifest& result)
    {
        if (result.mics.empty() || (int) result.mics.size() > maxMics)
            fail ("kit.json", "a kit needs between 1 and " + juce::String (maxMics) + " mics");

        if (result.pieces.empty() || (int) result.pieces.size() > maxPieces)
            fail ("kit.json", "a kit needs between 1 and " + juce::String (maxPieces) + " pieces");
    }

    //==============================================================================
    void parseKitBody (const juce::var& root, KitManifest& result)
    {
        const juce::String top ("kit.json");

        parseCredits (root, result.folder, result.credits);
        result.sampleRate = getNumber (root, "sampleRate", top, 0.0);

        if (result.sampleRate < 8000.0 || result.sampleRate > 384000.0)
            fail (top, "\"sampleRate\" is missing or out of range");

        // Mics
        int busChannels = 0;

        for (const auto& [i, m] : juce::enumerate (getArray (root, "mics", top)))
        {
            const auto where = "mics[" + juce::String (i) + "]";
            MicManifest mic;
            mic.id = getString (m, "id", where, true);
            mic.name = getString (m, "name", where, false);
            mic.channels = (int) getNumber (m, "channels", where, 1.0);
            mic.volumeDb = (float) getNumber (m, "volumeDb", where, 0.0);
            mic.pan = (float) juce::jlimit (-1.0, 1.0, getNumber (m, "pan", where, 0.0));
            mic.invertPolarity = (bool) m["invertPolarity"];

            if (! isValidId (mic.id) || result.findMic (mic.id) >= 0)
                fail (where, "mic ids must be unique and use only letters, digits, '_', '-' and '.'");

            if (mic.channels != 1 && mic.channels != 2)
                fail (where, "\"channels\" must be 1 (mono) or 2 (stereo)");

            const auto role = getString (m, "role", where, false);

            if (role.isNotEmpty())
            {
                if (auto r = micRoleFromKey (role.toStdString()))
                    mic.role = *r;
                else
                    fail (where, "\"role\" must be close, overheads or room");
            }

            if (mic.name.isEmpty())
                mic.name = mic.id;

            busChannels += mic.channels;
            result.mics.push_back (std::move (mic));
        }

        if (result.mics.empty() || (int) result.mics.size() > maxMics)
            fail (top, "a kit needs between 1 and " + juce::String (maxMics) + " mics");

        jassert (busChannels <= maxMicBusChannels);

        // Pieces
        for (const auto& [i, p] : juce::enumerate (getArray (root, "pieces", top)))
        {
            PieceManifest piece;
            piece.id = getString (p, "id", "pieces[" + juce::String (i) + "]", true);
            const auto where = "pieces." + piece.id;
            piece.name = getString (p, "name", where, false);

            if (! isValidId (piece.id) || result.findPiece (piece.id) >= 0)
                fail (where, "piece ids must be unique and use only letters, digits, '_', '-' and '.'");

            piece.role = parseRole (getString (p, "role", where, true), where);
            parsePieceSettings (p, where, piece);

            if (piece.name.isEmpty())
                piece.name = juce::String (info (piece.role).displayName.data(), info (piece.role).displayName.size());

            const auto& arts = getArray (p, "articulations", where);

            for (int a = 0; a < arts.size(); ++a)
                piece.articulations.push_back (parseArticulation (arts.getReference (a), result,
                                                                  where + ".articulations[" + juce::String (a) + "]"));

            if (piece.articulations.empty())
                fail (where, "a piece needs at least one articulation");

            for (size_t a = 0; a < piece.articulations.size(); ++a)
                for (size_t b = a + 1; b < piece.articulations.size(); ++b)
                    if (piece.articulations[a].id == piece.articulations[b].id)
                        fail (where, "duplicate articulation id \"" + piece.articulations[a].id + "\"");

            result.pieces.push_back (std::move (piece));
        }

        checkCounts (result);
        parseMidiMap (root, result);
    }

    //==============================================================================
    // Kit presets. A preset is a base kit plus changes, so it's parsed by
    // parsing the base kit and then applying the changes to the result:
    // which pieces (and articulations) to keep, in what order, with what
    // names, tuning, level and decay; which mics to keep and how to set them;
    // and optionally its own MIDI map. Audio paths keep pointing into the
    // base kit's folder, so no audio is ever duplicated.

    void choosePieces (const juce::var& list, KitManifest& result, std::vector<juce::String>& sourceIds)
    {
        const auto base = std::move (result.pieces);
        result.pieces.clear();

        for (const auto& [i, sel] : juce::enumerate (*list.getArray()))
        {
            const auto where = "pieces[" + juce::String (i) + "]";
            const auto source = getString (sel, "piece", where, true);
            const auto it = std::find_if (base.begin(), base.end(), [&] (const auto& p) { return p.id == source; });

            if (it == base.end())
                fail (where, "the base kit has no piece \"" + source + "\"");

            auto piece = *it;

            if (! sel["articulations"].isVoid())
            {
                piece.articulations.clear();

                for (const auto& a : getArray (sel, "articulations", where))
                {
                    const auto art = std::find_if (it->articulations.begin(), it->articulations.end(),
                                                   [&] (const auto& x) { return x.id == a.toString(); });

                    if (art == it->articulations.end())
                        fail (where, "piece \"" + source + "\" has no articulation \"" + a.toString() + "\"");

                    piece.articulations.push_back (*art);
                }

                if (piece.articulations.empty())
                    fail (where, "a piece needs at least one articulation");
            }

            if (auto id = getString (sel, "id", where, false); id.isNotEmpty())
                piece.id = id;

            if (auto name = getString (sel, "name", where, false); name.isNotEmpty())
                piece.name = name;

            if (auto role = getString (sel, "role", where, false); role.isNotEmpty())
                piece.role = parseRole (role, where);

            parsePieceSettings (sel, where, piece);

            if (! isValidId (piece.id) || result.findPiece (piece.id) >= 0)
                fail (where, "piece \"" + piece.id + "\" is used twice; give the second one its own \"id\"");

            result.pieces.push_back (std::move (piece));
            sourceIds.push_back (source);
        }
    }

    void changeMics (const juce::DynamicObject& changes, KitManifest& result)
    {
        std::vector<bool> keep (result.mics.size(), true);

        for (const auto& [name, change] : changes.getProperties())
        {
            const auto where = "mics." + name.toString();
            const auto mic = result.findMic (name.toString());

            if (mic < 0)
                fail (where, "the base kit has no mic \"" + name.toString() + "\"");

            auto& m = result.mics[(size_t) mic];
            keep[(size_t) mic] = change["enabled"].isVoid() || (bool) change["enabled"];
            m.volumeDb = (float) getNumber (change, "volumeDb", where, (double) m.volumeDb);
            m.pan = (float) juce::jlimit (-1.0, 1.0, getNumber (change, "pan", where, (double) m.pan));

            if (auto newName = getString (change, "name", where, false); newName.isNotEmpty())
                m.name = newName;
        }

        if (std::none_of (keep.begin(), keep.end(), [] (bool k) { return k; }))
            fail ("mics", "a preset must keep at least one mic");

        // Renumber the kept mics, and drop the left-out mics' audio from every
        // hit, so it is never loaded.
        std::vector<int> newIndex (result.mics.size(), -1);
        std::vector<MicManifest> mics;

        for (size_t m = 0; m < result.mics.size(); ++m)
        {
            if (keep[m])
            {
                newIndex[m] = (int) mics.size();
                mics.push_back (std::move (result.mics[m]));
            }
        }

        result.mics = std::move (mics);

        for (auto& p : result.pieces)
            for (auto& a : p.articulations)
                for (auto& z : a.zones)
                    for (auto& hit : z.roundRobins)
                    {
                        std::vector<MicSource> kept;

                        for (auto& s : hit.sources)
                            if (newIndex[(size_t) s.mic] >= 0)
                                kept.push_back ({ newIndex[(size_t) s.mic], s.file, s.fileChannels, s.start });

                        hit.sources = std::move (kept);
                    }
    }

    void parsePreset (const juce::var& root, KitManifest& result)
    {
        const juce::String top ("kit.json");
        const auto baseId = getString (root, "base", top, false);

        if (baseId.isEmpty())
            fail (top, "a kit preset needs a \"base\": the id of the kit whose recordings it plays");

        const auto baseFolder = findKitById (baseId, result.folder);

        if (baseFolder == juce::File())
            fail (top, "the preset's base kit \"" + baseId + "\" isn't installed");

        if (isPresetRoot (loadRoot (baseFolder)))
            fail (top, "a preset's base must be a kit, not another preset (\"" + baseId + "\")");

        KitManifest base;

        if (const auto r = KitManifest::parse (baseFolder, base); r.failed())
            fail (top, "the preset's base kit \"" + baseId + "\" doesn't load: " + r.getErrorMessage());

        // Start from the base kit, keeping the preset's own identity.
        const auto folder = result.folder;
        const auto id = result.id;
        const auto tags = result.tags;
        result = std::move (base);
        result.folder = folder;
        result.id = id;
        result.isPreset = true;
        result.baseId = baseId;

        if (! tags.isEmpty())
            result.tags = tags;

        result.credits.basedOn = result.credits.name;
        result.credits.name = getString (root, "name", top, true);

        if (auto description = getString (root, "description", top, false); description.isNotEmpty())
            result.credits.description = description;

        std::vector<juce::String> sourceIds;

        if (! root["pieces"].isVoid())
        {
            if (root["pieces"].getArray() == nullptr)
                fail (top, "\"pieces\" must be an array");

            choosePieces (root["pieces"], result, sourceIds);
        }
        else
        {
            for (const auto& p : result.pieces)
                sourceIds.push_back (p.id);
        }

        if (! root["mics"].isVoid())
        {
            const auto* changes = root["mics"].getDynamicObject();

            if (changes == nullptr)
                fail (top, "\"mics\" must be an object of mic ids");

            changeMics (*changes, result);
        }

        checkCounts (result);

        // The preset's own MIDI map, or the base kit's, following the pieces
        // the preset kept (under their new ids).
        if (! root["midiMap"].isVoid())
        {
            result.midiMap.clear();
            parseMidiMap (root, result);
        }
        else
        {
            std::vector<MidiMapEntry> map;

            for (const auto& e : result.midiMap)
            {
                const auto source = std::find (sourceIds.begin(), sourceIds.end(), e.piece);

                if (source == sourceIds.end())
                    continue;

                const auto& piece = result.pieces[(size_t) (source - sourceIds.begin())];
                const auto& arts = piece.articulations;

                if (e.articulation.isEmpty()
                    || std::any_of (arts.begin(), arts.end(), [&] (const auto& a) { return a.id == e.articulation; }))
                    map.push_back ({ e.note, piece.id, e.articulation });
            }

            result.midiMap = std::move (map);
        }
    }
} // namespace

//==============================================================================
int KitManifest::findMic (const juce::String& micId) const
{
    for (size_t i = 0; i < mics.size(); ++i)
        if (mics[i].id == micId)
            return (int) i;

    return -1;
}

int KitManifest::findPiece (const juce::String& pieceId) const
{
    for (size_t i = 0; i < pieces.size(); ++i)
        if (pieces[i].id == pieceId)
            return (int) i;

    return -1;
}

juce::Result KitManifest::parse (const juce::File& folder, KitManifest& result)
{
    const auto file = folder.getChildFile ("kit.json");

    if (! file.existsAsFile())
        return juce::Result::fail ("No kit.json in " + folder.getFullPathName());

    return parse (file.loadFileAsString(), folder, result);
}

juce::Result KitManifest::parse (const juce::String& json, const juce::File& folder, KitManifest& result)
{
    result = {};
    result.folder = folder;
    result.audioFolder = folder;

    try
    {
        const auto root = parseRoot (json);
        result.id = idFrom (root, folder);
        result.tags = tagsFrom (root);

        if (isPresetRoot (root))
            parsePreset (root, result);
        else
            parseKitBody (root, result);
    }
    catch (const ParseError& e)
    {
        return juce::Result::fail (e.message);
    }

    return juce::Result::ok();
}

//==============================================================================
juce::String defaultKitId (const juce::File& folder)
{
    auto id = folder.getFileName().toLowerCase().replaceCharacter (' ', '-');
    id = id.retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789_-.");
    return id.isNotEmpty() ? id : juce::String ("kit");
}

juce::String readKitId (const juce::File& folder)
{
    // Finding a preset's base kit reads the id of every kit nearby, and
    // kit.json files can be large. The importer writes "id" near the top, so
    // try the first few KB first: an "id" before any "mics" or "pieces" key
    // is the kit's own, not a mic's or a piece's.
    if (juce::FileInputStream in (folder.getChildFile ("kit.json")); in.openedOk())
    {
        juce::MemoryBlock block;
        in.readIntoMemoryBlock (block, 4096);
        const auto head = block.toString();
        const auto idAt = head.indexOf ("\"id\"");
        auto firstOf = [&head] (const char* key) { const auto i = head.indexOf (key); return i < 0 ? head.length() : i; };

        if (idAt >= 0 && idAt < std::min (firstOf ("\"mics\""), firstOf ("\"pieces\"")))
        {
            const auto value = head.substring (idAt + 4).fromFirstOccurrenceOf (":", false, false).trimStart();

            if (value.startsWithChar ('"'))
                if (const auto id = value.substring (1).upToFirstOccurrenceOf ("\"", false, false); isValidId (id))
                    return id;
        }
    }
    else
    {
        return {};
    }

    try
    {
        return idFrom (loadRoot (folder), folder);
    }
    catch (const ParseError&)
    {
        return {};
    }
}

juce::File findKitById (const juce::String& id, const juce::File& near)
{
    if (id.isEmpty())
        return {};

    auto sortedFolders = [] (const juce::File& dir)
    {
        auto folders = dir.findChildFiles (juce::File::findDirectories, false);
        folders.sort();
        return folders;
    };

    auto matches = [&] (const juce::File& f) { return f != near && readKitId (f) == id; };

    // First the kit's own content pack (or kits folder): presets live next to their base kit.
    const auto parent = near.getParentDirectory();

    for (const auto& f : sortedFolders (parent))
        if (matches (f))
            return f;

    // Then everything else in the kits folder.
    const auto kitsFolder = parent.getChildFile ("pack.json").existsAsFile() ? parent.getParentDirectory() : parent;

    for (const auto& f : sortedFolders (kitsFolder))
    {
        if (f == parent)
            continue;

        if (f.getChildFile ("pack.json").existsAsFile())
        {
            for (const auto& k : sortedFolders (f))
                if (matches (k))
                    return k;
        }
        else if (matches (f))
        {
            return f;
        }
    }

    return {};
}

bool readKitSummary (const juce::File& folder, KitSummary& result)
{
    try
    {
        result = {};
        const auto root = loadRoot (folder);
        result.id = idFrom (root, folder);
        result.tags = tagsFrom (root);
        result.isPreset = isPresetRoot (root);

        const auto& stats = root["stats"];
        result.numPieces = (int) stats["pieces"];
        result.numMics = (int) stats["mics"];
        result.ramBytes = (juce::int64) stats["ramBytes"];

        if (! result.isPreset)
        {
            parseCredits (root, folder, result.credits);

            if (const auto* pieces = root["pieces"].getArray())
            {
                for (const auto& p : *pieces)
                {
                    const auto role = roleFromKey (p["role"].toString().toStdString());
                    result.pieces.push_back ({ p["id"].toString(), p["name"].toString(), role.value_or (PieceRole::other) });
                }
            }

            if (result.numPieces <= 0)
                if (const auto* a = root["pieces"].getArray())
                    result.numPieces = a->size();

            if (result.numMics <= 0)
                if (const auto* a = root["mics"].getArray())
                    result.numMics = a->size();

            return true;
        }

        // A preset shows its base kit's credits, under its own name.
        const auto baseFolder = findKitById (root["base"].toString(), folder);
        KitSummary base;

        if (baseFolder == juce::File() || ! readKitSummary (baseFolder, base) || base.isPreset)
            return false;

        result.credits = base.credits;
        result.credits.basedOn = base.credits.name;
        result.credits.name = getString (root, "name", "kit.json", true);

        if (auto description = root["description"].toString(); description.isNotEmpty())
            result.credits.description = description;

        if (result.tags.isEmpty())
            result.tags = base.tags;

        if (result.numPieces <= 0)
            result.numPieces = root["pieces"].isArray() ? root["pieces"].getArray()->size() : base.numPieces;

        if (result.numMics <= 0)
            result.numMics = base.numMics;

        return true;
    }
    catch (const ParseError&)
    {
        return false;
    }
}

bool readKitCredits (const juce::File& folder, KitCredits& result)
{
    KitSummary summary;

    if (! readKitSummary (folder, summary))
        return false;

    result = summary.credits;
    return true;
}

} // namespace lithium
