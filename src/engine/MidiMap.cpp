#include "MidiMap.h"

#include "LithiumAssets.h"

namespace lithium
{

juce::Result MidiMap::parse (const juce::String& json, MidiMap& result)
{
    result = {};

    juce::var root;

    if (auto r = juce::JSON::parse (json, root); r.failed())
        return r;

    result.name = root["name"].toString();
    const auto* notes = root["notes"].getArray();

    if (notes == nullptr)
        return juce::Result::fail ("MIDI map has no \"notes\" array");

    for (const auto& n : *notes)
    {
        RoleMapEntry e;
        e.note = (int) n["note"];
        e.nth = (int) n["nth"];
        e.articulation = n["articulation"].toString();
        const auto role = roleFromKey (n["role"].toString().toStdString());

        if (! juce::isPositiveAndBelow (e.note, 128) || ! role.has_value() || e.nth < 0)
            return juce::Result::fail ("MIDI map entries need a note (0-127), a known role and nth >= 0");

        e.role = *role;
        result.entries.push_back (std::move (e));
    }

    return juce::Result::ok();
}

MidiMap MidiMap::generalMidi()
{
    MidiMap map;
    const auto json = juce::String::fromUTF8 (LithiumAssets::generalmidi_json, LithiumAssets::generalmidi_jsonSize);
    [[maybe_unused]] const auto r = parse (json, map);
    jassert (r.wasOk());   // the map is compiled in, so a failure here is a programming error
    return map;
}

} // namespace lithium
