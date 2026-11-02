#include "MidiMap.h"

#include "LithiumAssets.h"

namespace lithium
{

namespace
{
    constexpr std::array<std::pair<Follow, const char*>, 3> followKeys {{
        { Follow::none, "none" },
        { Follow::pedal, "pedal" },
        { Follow::position, "position" },
    }};

    constexpr std::array<const char*, numPositionSources> positionKeys { "snarePosition", "ridePosition", "tomPosition", "hihatPosition" };
    constexpr std::array<const char*, numPositionSources> padKeys { "snare", "ride", "toms", "hihat" };
    constexpr std::array<const char*, 3> chickKeys { "off", "auto", "always" };

    /** A MIDI channel: 1-16, or 0 for any. */
    juce::Result parseChannel (const juce::var& object, const char* key, int& value)
    {
        const auto v = object[key];

        if (v.isVoid())
            return juce::Result::ok();

        value = (int) v;
        return juce::isPositiveAndNotGreaterThan (value, 16)
                 ? juce::Result::ok()
                 : juce::Result::fail ("\"" + juce::String (key) + "\" must be a MIDI channel (1-16), or 0 for any");
    }

    /** A calibration range: two controller values, low then high. */
    void parseRange (const juce::var& object, int& low, int& high)
    {
        low = juce::jlimit (0, 126, (int) object.getProperty ("min", low));
        high = juce::jlimit (low + 1, 127, (int) object.getProperty ("max", high));
    }

    float parseUnit (const juce::var& object, const char* key, float value, float low = 0.0f)
    {
        return juce::jlimit (low, 1.0f, (float) (double) object.getProperty (key, value));
    }

    /** A controller number from the "controllers" object: 0-127, or -1 (off). */
    juce::Result parseController (const juce::var& controllers, const char* key, int& value)
    {
        const auto v = controllers[key];

        if (v.isVoid())
            return juce::Result::ok();

        value = (int) v;
        return juce::isPositiveAndBelow (value, 128) || value == -1
                 ? juce::Result::ok()
                 : juce::Result::fail ("\"" + juce::String (key) + "\" must be a controller number (0-127), or -1 for off");
    }

    MidiMap builtIn (const char* data, int size)
    {
        MidiMap map;
        [[maybe_unused]] const auto r = MidiMap::parse (juce::String::fromUTF8 (data, size), map);
        jassert (r.wasOk());   // the maps are compiled in, so a failure here is a programming error
        return map;
    }
}

juce::Result MidiMap::parse (const juce::String& json, MidiMap& result)
{
    result = {};

    juce::var root;

    if (auto r = juce::JSON::parse (json, root); r.failed())
        return r;

    result.id = root["id"].toString();
    result.name = root["name"].toString();
    result.description = root["description"].toString();
    result.useKitMap = (bool) root.getProperty ("useKitMap", false);

    if (const auto& c = root["controllers"]; c.isObject())
    {
        for (auto [key, value] : { std::pair { "hihatPedal", &result.controllers.hihatPedal },
                                   std::pair { positionKeys[0], &result.controllers.position[0] },
                                   std::pair { positionKeys[1], &result.controllers.position[1] },
                                   std::pair { positionKeys[2], &result.controllers.position[2] },
                                   std::pair { positionKeys[3], &result.controllers.position[3] } })
            if (auto r = parseController (c, key, *value); r.failed())
                return r;

        auto& cs = result.controllers;
        cs.pedalInverted = (bool) c.getProperty ("pedalInverted", false);

        if (auto r = parseChannel (c, "hihatChannel", cs.hihatChannel); r.failed())
            return r;

        parseRange (c.getProperty ("pedalRange", {}), cs.pedalMin, cs.pedalMax);
        cs.pedalCurve = parseUnit (c, "pedalCurve", cs.pedalCurve, -1.0f);
        cs.chickSensitivity = parseUnit (c, "chickSensitivity", cs.chickSensitivity);
        cs.footSplash = (bool) c.getProperty ("footSplash", cs.footSplash);

        if (const auto mode = c["chickFromPedal"].toString(); mode.isNotEmpty())
            for (size_t i = 0; i < chickKeys.size(); ++i)
                if (mode == chickKeys[i])
                    cs.chickFromPedal = (ChickMode) i;

        // Thresholds must rise, each within 0..1.
        if (const auto* t = c["hatThresholds"].getArray(); t != nullptr && t->size() == ControllerSettings::numThresholds)
        {
            auto previous = 0.0f;

            for (size_t i = 0; i < cs.hatThresholds.size(); ++i)
                previous = cs.hatThresholds[i] = juce::jlimit (previous, 1.0f, (float) (double) (*t)[(int) i]);
        }

        if (const auto& pads = c["pads"]; pads.isObject())
        {
            for (size_t i = 0; i < padKeys.size(); ++i)
            {
                const auto& pad = pads[padKeys[i]];

                if (! pad.isObject())
                    continue;

                if (auto r = parseChannel (pad, "channel", cs.positionChannel[i]); r.failed())
                    return r;

                cs.positionEnabled[i] = (bool) pad.getProperty ("enabled", true);
                parseRange (pad, cs.positionMin[i], cs.positionMax[i]);
                cs.positionCurve[i] = parseUnit (pad, "curve", 0.0f, -1.0f);
                cs.positionEmulation[i] = parseUnit (pad, "emulation", cs.positionEmulation[i]);
            }
        }
    }

    const auto* notes = root["notes"].getArray();

    if (notes == nullptr)
        return juce::Result::fail ("MIDI map has no \"notes\" array");

    for (const auto& n : *notes)
    {
        RoleMapEntry e;
        e.note = (int) n["note"];
        e.nth = (int) n.getProperty ("nth", 0);
        e.articulation = n["articulation"].toString();
        e.name = n["name"].toString();
        e.otherwise = n["otherwise"].toString();
        e.exact = (bool) n.getProperty ("exact", false);

        const auto roleKey = n["role"].toString();
        e.anyTom = roleKey == "toms";
        const auto role = e.anyTom ? std::optional (PieceRole::tom) : roleFromKey (roleKey.toStdString());

        if (! juce::isPositiveAndBelow (e.note, 128) || ! role.has_value() || e.nth < 0)
            return juce::Result::fail ("MIDI map entries need a note (0-127), a known role and nth >= 0");

        e.role = *role;

        if (const auto follow = n["follow"].toString(); follow.isNotEmpty())
        {
            const auto* f = std::find_if (followKeys.begin(), followKeys.end(), [&] (const auto& k) { return follow == k.second; });

            if (f == followKeys.end())
                return juce::Result::fail ("\"follow\" must be \"pedal\", \"position\" or \"none\"");

            e.follow = f->first;
        }

        result.assign (e);   // a note listed twice keeps its last entry
    }

    return juce::Result::ok();
}

juce::String MidiMap::toJson() const
{
    // DynamicObject is reference-counted: the var that holds it owns it.
    auto* root = new juce::DynamicObject();
    const juce::var rootVar (root);

    root->setProperty ("format", "lithium-midi-map");
    root->setProperty ("version", 1);
    root->setProperty ("id", id);
    root->setProperty ("name", name);

    if (description.isNotEmpty())
        root->setProperty ("description", description);

    if (useKitMap)
        root->setProperty ("useKitMap", true);

    const auto& cs = controllers;
    auto* c = new juce::DynamicObject();
    c->setProperty ("hihatPedal", cs.hihatPedal);
    c->setProperty ("hihatChannel", cs.hihatChannel);
    c->setProperty ("pedalInverted", cs.pedalInverted);

    auto* range = new juce::DynamicObject();
    range->setProperty ("min", cs.pedalMin);
    range->setProperty ("max", cs.pedalMax);
    c->setProperty ("pedalRange", juce::var (range));
    c->setProperty ("pedalCurve", cs.pedalCurve);

    juce::Array<juce::var> thresholds;

    for (const auto t : cs.hatThresholds)
        thresholds.add (t);

    c->setProperty ("hatThresholds", thresholds);
    c->setProperty ("chickFromPedal", chickKeys[(size_t) cs.chickFromPedal]);
    c->setProperty ("chickSensitivity", cs.chickSensitivity);
    c->setProperty ("footSplash", cs.footSplash);

    auto* pads = new juce::DynamicObject();

    for (size_t i = 0; i < positionKeys.size(); ++i)
    {
        c->setProperty (positionKeys[i], cs.position[i]);

        auto* pad = new juce::DynamicObject();
        pad->setProperty ("channel", cs.positionChannel[i]);
        pad->setProperty ("enabled", cs.positionEnabled[i]);
        pad->setProperty ("min", cs.positionMin[i]);
        pad->setProperty ("max", cs.positionMax[i]);
        pad->setProperty ("curve", cs.positionCurve[i]);
        pad->setProperty ("emulation", cs.positionEmulation[i]);
        pads->setProperty (padKeys[i], juce::var (pad));
    }

    c->setProperty ("pads", juce::var (pads));

    root->setProperty ("controllers", juce::var (c));

    juce::Array<juce::var> notes;

    for (const auto& e : entries)
    {
        auto* n = new juce::DynamicObject();
        n->setProperty ("note", e.note);
        n->setProperty ("role", e.anyTom ? juce::String ("toms") : juce::String (info (e.role).key.data(), info (e.role).key.size()));

        if (e.nth != 0)
            n->setProperty ("nth", e.nth);

        n->setProperty ("articulation", e.articulation);

        if (e.name.isNotEmpty())
            n->setProperty ("name", e.name);

        if (e.otherwise.isNotEmpty())
            n->setProperty ("otherwise", e.otherwise);

        if (e.exact)
            n->setProperty ("exact", true);

        if (e.follow != Follow::none)
            n->setProperty ("follow", followKeys[(size_t) e.follow].second);

        notes.add (juce::var (n));
    }

    root->setProperty ("notes", notes);
    return juce::JSON::toString (rootVar);
}

const RoleMapEntry* MidiMap::find (int note) const
{
    const auto it = std::find_if (entries.begin(), entries.end(), [note] (const auto& e) { return e.note == note; });
    return it != entries.end() ? &*it : nullptr;
}

void MidiMap::assign (const RoleMapEntry& entry)
{
    remove (entry.note);

    // Kept sorted by note, so saved maps read in order.
    const auto pos = std::lower_bound (entries.begin(), entries.end(), entry.note,
                                       [] (const auto& e, int note) { return e.note < note; });
    entries.insert (pos, entry);
}

void MidiMap::remove (int note)
{
    std::erase_if (entries, [note] (const auto& e) { return e.note == note; });
}

//==============================================================================
MidiMap MidiMap::lithiumStandard()
{
    return builtIn (LithiumAssets::lithiumstandard_json, LithiumAssets::lithiumstandard_jsonSize);
}

MidiMap MidiMap::generalMidi()
{
    return builtIn (LithiumAssets::generalmidi_json, LithiumAssets::generalmidi_jsonSize);
}

MidiMap MidiMap::kitDefault()
{
    auto map = generalMidi();
    map.id = kitDefaultId;
    map.name = "Kit default";
    map.description = "The kit's own note map, with General MIDI on the notes it leaves free.";
    map.useKitMap = true;
    return map;
}

MidiMap MidiMap::rolandVDrums()
{
    return builtIn (LithiumAssets::rolandvdrums_json, LithiumAssets::rolandvdrums_jsonSize);
}

MidiMap MidiMap::alesis()
{
    return builtIn (LithiumAssets::alesis_json, LithiumAssets::alesis_jsonSize);
}

std::vector<MidiMap> MidiMap::factoryMaps()
{
    // The other e-kit makers' factory maps, from their own documentation
    // (named in each map's description).
    return { lithiumStandard(), kitDefault(), generalMidi(), rolandVDrums(), alesis(),
             builtIn (LithiumAssets::drumitfive_json, LithiumAssets::drumitfive_jsonSize),
             builtIn (LithiumAssets::efnote_json, LithiumAssets::efnote_jsonSize),
             builtIn (LithiumAssets::lemont950_json, LithiumAssets::lemont950_jsonSize),
             builtIn (LithiumAssets::milleniummps1000_json, LithiumAssets::milleniummps1000_jsonSize),
             builtIn (LithiumAssets::yamahadtx402_json, LithiumAssets::yamahadtx402_jsonSize) };
}

MidiMap MidiMap::factoryMap (const juce::String& id)
{
    for (auto& m : factoryMaps())
        if (m.id == id)
            return m;

    return lithiumStandard();
}

} // namespace lithium
