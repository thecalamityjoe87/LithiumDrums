#include "KitLibrary.h"

#include "engine/KitManifest.h"

namespace lithium
{

namespace kitlibrary
{

std::vector<Entry> findKits (const juce::File& kitsFolder)
{
    std::vector<Entry> kits;

    for (const auto& folder : kitsFolder.findChildFiles (juce::File::findDirectories, false))
    {
        Entry e;

        if (readKitCredits (folder, e.credits))
        {
            e.folder = folder;
            kits.push_back (std::move (e));
        }
    }

    std::sort (kits.begin(), kits.end(), [] (const auto& a, const auto& b)
    {
        return a.credits.name.compareNatural (b.credits.name) < 0;
    });

    return kits;
}

juce::File resolveSavedKit (const juce::String& savedPath, const juce::File& kitsFolder)
{
    if (savedPath.isEmpty() || ! juce::File::isAbsolutePath (savedPath))
        return {};

    const juce::File saved (savedPath);

    if (saved.getChildFile ("kit.json").existsAsFile())
        return saved;

    const auto moved = kitsFolder.getChildFile (saved.getFileName());
    return moved.getChildFile ("kit.json").existsAsFile() ? moved : saved;
}

juce::File findDefaultKit (const juce::File& kitsFolder)
{
    const auto kits = findKits (kitsFolder);

    for (const auto& k : kits)
        if (k.credits.name.equalsIgnoreCase ("DRSKit"))
            return k.folder;

    return kits.empty() ? juce::File() : kits.front().folder;
}

} // namespace kitlibrary

} // namespace lithium
