#include "KitManifest.h"

#include <algorithm>

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

    HitManifest hitFromFile (const juce::String& file, const ChannelMap& map)
    {
        HitManifest hit;

        for (const auto& [mic, channels] : map)
            hit.sources.push_back ({ mic, file, channels });

        return hit;
    }

    HitManifest parseHit (const juce::var& value, const KitManifest& kit, const ChannelMap* articulationMap,
                          const juce::String& where)
    {
        // 1. "file.wav": one multichannel file, read through the articulation's channel map.
        if (value.isString())
        {
            if (articulationMap == nullptr)
                fail (where, "a hit given as a file name needs a \"channelMap\" on its articulation");

            return hitFromFile (checkRelativePath (value.toString(), where), *articulationMap);
        }

        // 2. { "file": "...", "channelMap": {...} }: a multichannel file with its own map.
        if (value["file"].isString())
        {
            const auto file = checkRelativePath (value["file"].toString(), where);

            if (! value["channelMap"].isVoid())
                return hitFromFile (file, parseChannelMap (value["channelMap"], kit, where));

            if (articulationMap == nullptr)
                fail (where, "a hit needs a \"channelMap\", on itself or on its articulation");

            return hitFromFile (file, *articulationMap);
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
                                      parseChannelMap (part["channelMap"], kit, where));
                hit.sources.insert (hit.sources.end(), h.sources.begin(), h.sources.end());
            }

            return hit;
        }

        // 4. { "files": { "kickIn": "...", ... } }: one file per mic.
        if (auto* files = value["files"].getDynamicObject())
        {
            HitManifest hit;

            for (const auto& [name, v] : files->getProperties())
            {
                const auto mic = kit.findMic (name.toString());

                if (mic < 0)
                    fail (where, "\"files\" names \"" + name.toString() + "\", which is not one of the kit's mics");

                if (! v.isString())
                    fail (where, "the file for mic \"" + name.toString() + "\" must be a string");

                // A mono file on a stereo mic is handled by the loader (it plays centred).
                const auto stereo = kit.mics[(size_t) mic].channels == 2;
                hit.sources.push_back ({ mic, checkRelativePath (v.toString(), where), { 1, stereo ? 2 : 0 } });
            }

            return hit;
        }

        fail (where, "a hit must be a file name, { \"file\", \"channelMap\" }, a list of those, or { \"files\" }");
    }

    ZoneManifest parseZone (const juce::var& zone, const KitManifest& kit, const ChannelMap* map, const juce::String& where)
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
            result.roundRobins.push_back (parseHit (rr.getReference (i), kit, map,
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

        std::optional<ChannelMap> map;

        if (! art["channelMap"].isVoid())
            map = parseChannelMap (art["channelMap"], kit, where);

        const auto& zones = getArray (art, "zones", where);

        for (int i = 0; i < zones.size(); ++i)
            result.zones.push_back (parseZone (zones.getReference (i), kit, map.has_value() ? &*map : nullptr,
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

        // Reserved for kits that reuse another kit's samples (a later milestone).
        if (const auto type = root["type"].toString(); type.isNotEmpty() && type != "kit")
            fail ("kit.json", "kits of type \"" + type + "\" aren't supported by this version");

        return root;
    }
} // namespace

//==============================================================================
int KitManifest::findMic (const juce::String& id) const
{
    for (size_t i = 0; i < mics.size(); ++i)
        if (mics[i].id == id)
            return (int) i;

    return -1;
}

int KitManifest::findPiece (const juce::String& id) const
{
    for (size_t i = 0; i < pieces.size(); ++i)
        if (pieces[i].id == id)
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

    try
    {
        const juce::String top ("kit.json");
        const auto root = parseRoot (json);

        parseCredits (root, folder, result.credits);
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
            piece.chokeNote = (int) getNumber (p, "chokeNote", where, -1.0);

            if (! isValidId (piece.id) || result.findPiece (piece.id) >= 0)
                fail (where, "piece ids must be unique and use only letters, digits, '_', '-' and '.'");

            if (auto role = roleFromKey (getString (p, "role", where, true).toStdString()))
                piece.role = *role;
            else
                fail (where, "unknown \"role\" (kick, snare, hihat, tom, floor_tom, crash, ride, china or other)");

            if (piece.name.isEmpty())
                piece.name = juce::String (info (piece.role).displayName.data(), info (piece.role).displayName.size());

            if (piece.chokeNote < -1 || piece.chokeNote > 127)
                fail (where, "\"chokeNote\" must be a MIDI note (0-127)");

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

        if (result.pieces.empty() || (int) result.pieces.size() > maxPieces)
            fail (top, "a kit needs between 1 and " + juce::String (maxPieces) + " pieces");

        // Default MIDI map
        if (! root["midiMap"].isVoid())
        {
            for (const auto& [i, e] : juce::enumerate (getArray (root, "midiMap", top)))
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
    }
    catch (const ParseError& e)
    {
        return juce::Result::fail (e.message);
    }

    return juce::Result::ok();
}

bool readKitCredits (const juce::File& folder, KitCredits& result)
{
    const auto file = folder.getChildFile ("kit.json");

    if (! file.existsAsFile())
        return false;

    try
    {
        result = {};
        parseCredits (parseRoot (file.loadFileAsString()), folder, result);
        return true;
    }
    catch (const ParseError&)
    {
        return false;
    }
}

} // namespace lithium
