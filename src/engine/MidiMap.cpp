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

    juce::String roleKey (PieceRole role)
    {
        return { info (role).key.data(), info (role).key.size() };
    }

    /** The "velocity" object: the global setting and each piece's curve and adjustments. */
    juce::Result parseVelocity (const juce::var& object, VelocitySettings& result)
    {
        result.global = parseUnit (object, "global", 0.0f, -1.0f);

        const auto* pieces = object["pieces"].getArray();

        if (pieces == nullptr)
            return juce::Result::ok();

        for (const auto& p : *pieces)
        {
            PieceVelocity pv;
            const auto role = roleFromKey (p["role"].toString().toStdString());
            pv.nth = (int) p.getProperty ("nth", 0);
            pv.name = p["name"].toString();

            if (! role.has_value() || pv.nth < 0)
                return juce::Result::fail ("Velocity settings need a known role and nth >= 0");

            pv.role = *role;

            if (const auto* nodes = p["curve"].getArray())
            {
                for (const auto& n : *nodes)
                {
                    const auto* xy = n.getArray();

                    if (xy == nullptr || xy->size() != 2)
                        return juce::Result::fail ("A velocity curve is a list of [in, out] pairs");

                    pv.curve.nodes.push_back ({ (float) (double) (*xy)[0], (float) (double) (*xy)[1] });
                }

                pv.curve = pv.curve.sanitised();
            }

            if (const auto* adjust = p["adjust"].getDynamicObject())
                for (const auto& [articulation, amount] : adjust->getProperties())
                    pv.setAdjust (articulation.toString(), (int) amount);

            // A piece listed twice keeps its first settings.
            const auto known = std::any_of (result.pieces.begin(), result.pieces.end(), [&] (const auto& o) { return o.samePiece (pv); });

            if (! pv.isNeutral() && ! known)
                result.pieces.push_back (std::move (pv));
        }

        return juce::Result::ok();
    }

    juce::var velocityToVar (const VelocitySettings& settings)
    {
        auto* v = new juce::DynamicObject();
        const juce::var result (v);
        v->setProperty ("global", settings.global);

        juce::Array<juce::var> pieces;

        for (const auto& pv : settings.pieces)
        {
            auto* p = new juce::DynamicObject();
            p->setProperty ("role", roleKey (pv.role));

            if (pv.nth != 0)
                p->setProperty ("nth", pv.nth);

            if (pv.name.isNotEmpty())
                p->setProperty ("name", pv.name);

            if (! pv.curve.isLinear())
            {
                juce::Array<juce::var> nodes;

                for (const auto& n : pv.curve.nodes)
                    nodes.add (juce::Array<juce::var> { juce::roundToInt (n.in), std::round (n.out * 10.0f) / 10.0f });

                p->setProperty ("curve", nodes);
            }

            if (! pv.adjust.empty())
            {
                auto* adjust = new juce::DynamicObject();

                for (const auto& [articulation, amount] : pv.adjust)
                    adjust->setProperty (articulation, amount);

                p->setProperty ("adjust", juce::var (adjust));
            }

            pieces.add (juce::var (p));
        }

        v->setProperty ("pieces", pieces);
        return result;
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
                cs.positionVelocity[i] = parseUnit (pad, "edgeVelocity", 0.0f, -1.0f);
            }
        }
    }

    // Maps saved before velocity curves have no "velocity": they play as they always did.
    if (const auto& v = root["velocity"]; v.isObject())
        if (auto r = parseVelocity (v, result.velocity); r.failed())
            return r;

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
            // An iterator, not a pointer: std::array's iterator is only a raw
            // pointer in some standard libraries (GCC's), not in MSVC's.
            const auto f = std::find_if (followKeys.begin(), followKeys.end(), [&] (const auto& k) { return follow == k.second; });

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
        pad->setProperty ("edgeVelocity", cs.positionVelocity[i]);
        pads->setProperty (padKeys[i], juce::var (pad));
    }

    c->setProperty ("pads", juce::var (pads));

    root->setProperty ("controllers", juce::var (c));

    if (! velocity.isNeutral())
        root->setProperty ("velocity", velocityToVar (velocity));

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
float VelocityCurve::operator() (float in) const
{
    in = juce::jlimit (0.0f, 127.0f, in);
    const auto n = std::min ((int) nodes.size(), maxNodes);

    if (n < 2)
        return in;

    // A monotone cubic (Fritsch and Carlson): the tangent at each node is
    // the mean of the slopes either side, or flat where the curve turns,
    // then scaled down wherever it would overshoot. A handful of nodes, so
    // the tangents are worked out on the stack each time.
    std::array<float, maxNodes> slope {}, tangent {};

    for (int k = 0; k + 1 < n; ++k)
        slope[(size_t) k] = (nodes[(size_t) k + 1].out - nodes[(size_t) k].out) / std::max (1.0f, nodes[(size_t) k + 1].in - nodes[(size_t) k].in);

    tangent[0] = slope[0];
    tangent[(size_t) n - 1] = slope[(size_t) n - 2];

    for (int k = 1; k + 1 < n; ++k)
    {
        const auto before = slope[(size_t) k - 1], after = slope[(size_t) k];
        tangent[(size_t) k] = before * after <= 0.0f ? 0.0f : 0.5f * (before + after);
    }

    for (int k = 0; k + 1 < n; ++k)
    {
        const auto d = slope[(size_t) k];

        if (juce::exactlyEqual (d, 0.0f))
        {
            tangent[(size_t) k] = tangent[(size_t) k + 1] = 0.0f;
            continue;
        }

        const auto a = tangent[(size_t) k] / d, b = tangent[(size_t) k + 1] / d;

        if (const auto s = a * a + b * b; s > 9.0f)
        {
            const auto t = 3.0f / std::sqrt (s);
            tangent[(size_t) k] = t * a * d;
            tangent[(size_t) k + 1] = t * b * d;
        }
    }

    int k = 0;

    while (k + 2 < n && in > nodes[(size_t) k + 1].in)
        ++k;

    const auto& p0 = nodes[(size_t) k];
    const auto& p1 = nodes[(size_t) k + 1];
    const auto h = std::max (1.0f, p1.in - p0.in);
    const auto t = juce::jlimit (0.0f, 1.0f, (in - p0.in) / h);
    const auto t2 = t * t, t3 = t2 * t;

    const auto y = (2 * t3 - 3 * t2 + 1) * p0.out + (t3 - 2 * t2 + t) * h * tangent[(size_t) k]
                 + (-2 * t3 + 3 * t2) * p1.out + (t3 - t2) * h * tangent[(size_t) k + 1];

    return juce::jlimit (0.0f, 127.0f, y);
}

bool VelocityCurve::isLinear() const
{
    return std::all_of (nodes.begin(), nodes.end(), [] (const auto& n) { return std::abs (n.out - n.in) < 0.05f; });
}

VelocityCurve VelocityCurve::sanitised() const
{
    auto sorted = nodes;

    for (auto& n : sorted)
    {
        n.in = juce::jlimit (0.0f, 127.0f, std::round (n.in));
        n.out = juce::jlimit (0.0f, 127.0f, n.out);
    }

    std::stable_sort (sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.in < b.in; });

    if (sorted.size() < 2)
        return {};

    sorted.front().in = 0.0f;
    sorted.back().in = 127.0f;

    // Inner nodes at least a velocity from their neighbours, and no more than maxNodes in all.
    VelocityCurve result;
    result.nodes.push_back (sorted.front());

    for (size_t i = 1; i + 1 < sorted.size(); ++i)
        if (sorted[i].in >= result.nodes.back().in + 1.0f && sorted[i].in <= 126.0f && (int) result.nodes.size() < maxNodes - 1)
            result.nodes.push_back (sorted[i]);

    result.nodes.push_back (sorted.back());

    if (result.isLinear())
        result.nodes.clear();

    return result;
}

float applyGlobalVelocity (float velocity, float global) noexcept
{
    global = juce::jlimit (-1.0f, 1.0f, global);
    const auto low = 1.0f + 59.0f * std::max (0.0f, global);     // Harder: the minimum rises to 60
    const auto high = 127.0f - 37.0f * std::max (0.0f, -global); // Softer: the maximum falls to 90
    return low + (juce::jlimit (1.0f, 127.0f, velocity) - 1.0f) * (high - low) / 126.0f;
}

int PieceVelocity::adjustFor (const juce::String& articulation) const
{
    for (const auto& [a, amount] : adjust)
        if (a == articulation)
            return amount;

    return 0;
}

void PieceVelocity::setAdjust (const juce::String& articulation, int amount)
{
    amount = juce::jlimit (-maxAdjust, maxAdjust, amount);
    std::erase_if (adjust, [&] (const auto& a) { return a.first == articulation; });

    if (amount != 0 && articulation.isNotEmpty())
        adjust.emplace_back (articulation, amount);
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
             builtIn (LithiumAssets::hxmm019_json, LithiumAssets::hxmm019_jsonSize),
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
