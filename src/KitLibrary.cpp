#include "KitLibrary.h"

namespace lithium
{

namespace kitlibrary
{

const Entry* Library::find (const juce::File& folder) const
{
    for (const auto& k : kits)
        if (k.folder == folder)
            return &k;

    return nullptr;
}

const Entry* Library::findById (const juce::String& id) const
{
    for (const auto& k : kits)
        if (k.summary.id == id)
            return &k;

    return nullptr;
}

bool readPack (const juce::File& folder, Pack& result)
{
    const auto file = folder.getChildFile ("pack.json");

    if (! file.existsAsFile())
        return false;

    juce::var root;

    if (juce::JSON::parse (file.loadFileAsString(), root).failed() || root.getDynamicObject() == nullptr)
        return false;

    result = {};
    result.folder = folder;
    result.id = root["id"].toString();
    result.name = root["name"].toString();
    result.author = root["author"].toString();
    result.sourceUrl = root["sourceUrl"].toString();
    result.description = root["description"].toString();
    result.credits = root["credits"].toString();

    const auto& license = root["license"];
    result.licenseName = license["name"].toString();
    result.licenseUrl = license["url"].toString();
    result.shareAlike = license["shareAlike"].isVoid() ? result.licenseName.containsIgnoreCase ("-SA")
                                                       : (bool) license["shareAlike"];

    if (const auto f = license["file"].toString(); f.isNotEmpty() && ! f.contains (".."))
        result.licenseFile = folder.getChildFile (f);

    if (result.name.isEmpty())
        result.name = folder.getFileName();

    if (result.id.isEmpty())
        result.id = defaultKitId (folder);

    return true;
}

Library scan (const juce::File& kitsFolder)
{
    Library library;

    auto addKit = [&library] (const juce::File& folder, int pack)
    {
        Entry e;

        if (readKitSummary (folder, e.summary))
        {
            e.folder = folder;
            e.pack = pack;
            library.kits.push_back (std::move (e));
        }
    };

    auto folders = kitsFolder.findChildFiles (juce::File::findDirectories, false);
    folders.sort();

    for (const auto& folder : folders)
    {
        // ".downloads" holds packs being installed (see packs::Installer).
        if (folder.getFileName().startsWithChar ('.'))
            continue;

        Pack pack;

        if (! readPack (folder, pack))
        {
            addKit (folder, -1);
            continue;
        }

        library.packs.push_back (std::move (pack));
    }

    std::sort (library.packs.begin(), library.packs.end(),
               [] (const auto& a, const auto& b) { return a.name.compareNatural (b.name) < 0; });

    for (size_t p = 0; p < library.packs.size(); ++p)
    {
        auto kits = library.packs[p].folder.findChildFiles (juce::File::findDirectories, false);
        kits.sort();

        for (const auto& folder : kits)
            addKit (folder, (int) p);
    }

    // Packs in name order, loose kits after them; in each, kits before presets.
    std::stable_sort (library.kits.begin(), library.kits.end(), [] (const Entry& a, const Entry& b)
    {
        const auto packA = a.pack < 0 ? std::numeric_limits<int>::max() : a.pack;
        const auto packB = b.pack < 0 ? std::numeric_limits<int>::max() : b.pack;

        if (packA != packB)
            return packA < packB;

        if (a.summary.isPreset != b.summary.isPreset)
            return ! a.summary.isPreset;

        return a.credits().name.compareNatural (b.credits().name) < 0;
    });

    return library;
}

juce::File resolveSavedKit (const juce::String& savedPath, const juce::String& savedId, const juce::File& kitsFolder)
{
    // The saved path usually still holds the kit; then the kits folder needn't be read.
    const auto saved = savedPath.isNotEmpty() && juce::File::isAbsolutePath (savedPath) ? juce::File (savedPath) : juce::File();

    if (saved != juce::File() && saved.getChildFile ("kit.json").existsAsFile()
        && (savedId.isEmpty() || readKitId (saved) == savedId))
        return saved;

    return resolveSavedKit (savedPath, savedId, scan (kitsFolder));
}

juce::File resolveSavedKit (const juce::String& savedPath, const juce::String& savedId, const Library& library)
{
    const auto saved = savedPath.isNotEmpty() && juce::File::isAbsolutePath (savedPath) ? juce::File (savedPath) : juce::File();

    if (saved != juce::File() && saved.getChildFile ("kit.json").existsAsFile()
        && (savedId.isEmpty() || readKitId (saved) == savedId))
        return saved;

    if (const auto* byId = library.findById (savedId); byId != nullptr && savedId.isNotEmpty())
        return byId->folder;

    for (const auto& k : library.kits)
        if (saved != juce::File() && k.folder.getFileName() == saved.getFileName())
            return k.folder;

    return saved;   // gone: loading it fails with a clear message
}

juce::File findDefaultKit (const juce::File& kitsFolder)
{
    const auto library = scan (kitsFolder);

    for (const auto& k : library.kits)
        if (k.summary.id == "drskit" || k.credits().name.equalsIgnoreCase ("DRSKit"))
            return k.folder;

    return library.kits.empty() ? juce::File() : library.kits.front().folder;
}

} // namespace kitlibrary

} // namespace lithium
