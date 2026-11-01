#include "MidiMapLibrary.h"

namespace lithium::midimaps
{

namespace
{
    juce::File fileFor (const juce::String& id)
    {
        return Settings::getMidiMapsFolder().getChildFile (id.fromFirstOccurrenceOf (userPrefix, false, false));
    }

    std::optional<Item> read (const juce::File& file)
    {
        Item item;

        if (MidiMap::parse (file.loadFileAsString(), item.map).failed())
            return std::nullopt;

        item.factory = false;
        item.file = file;
        item.map.id = userPrefix + file.getFileName();

        if (item.map.name.isEmpty())
            item.map.name = file.getFileNameWithoutExtension();

        return item;
    }
}

bool isFactory (const juce::String& id)
{
    return ! id.startsWith (userPrefix);
}

std::vector<Item> scan()
{
    std::vector<Item> items;

    for (auto& m : MidiMap::factoryMaps())
        items.push_back ({ std::move (m), true, {} });

    std::vector<Item> saved;

    for (const auto& f : Settings::getMidiMapsFolder().findChildFiles (juce::File::findFiles, false, "*.json"))
        if (auto item = read (f))
            saved.push_back (std::move (*item));

    std::sort (saved.begin(), saved.end(), [] (const auto& a, const auto& b) { return a.map.name.compareNatural (b.map.name) < 0; });
    std::move (saved.begin(), saved.end(), std::back_inserter (items));
    return items;
}

std::optional<MidiMap> find (const juce::String& id)
{
    if (isFactory (id))
    {
        for (auto& m : MidiMap::factoryMaps())
            if (m.id == id)
                return m;

        return std::nullopt;
    }

    if (auto item = read (fileFor (id)))
        return item->map;

    return std::nullopt;
}

juce::Result save (const MidiMap& map, const juce::String& name, MidiMap& saved)
{
    const auto fileName = juce::File::createLegalFileName (name.trim());

    if (fileName.isEmpty())
        return juce::Result::fail ("The map needs a name");

    const auto folder = Settings::getMidiMapsFolder();

    if (auto r = folder.createDirectory(); r.failed())
        return r;

    saved = map;
    saved.name = name.trim();
    saved.id = userPrefix + fileName + ".json";

    if (! fileFor (saved.id).replaceWithText (saved.toJson()))
        return juce::Result::fail ("Couldn't write " + fileFor (saved.id).getFullPathName());

    return juce::Result::ok();
}

bool remove (const juce::String& id)
{
    return ! isFactory (id) && fileFor (id).deleteFile();
}

MidiMap loadDefault (const Settings& settings)
{
    if (const auto env = juce::SystemStats::getEnvironmentVariable ("LITHIUM_MIDI_MAP", {}); env.isNotEmpty())
        return MidiMap::factoryMap (env);

    MidiMap map;

    if (const auto json = settings.getDefaultMidiMap(); json.isNotEmpty() && MidiMap::parse (json, map).wasOk())
        return map;

    return MidiMap::lithiumStandard();
}

} // namespace lithium::midimaps
