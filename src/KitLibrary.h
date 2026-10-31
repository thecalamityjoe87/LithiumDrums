#pragma once

#include "Settings.h"
#include "engine/KitManifest.h"

#include <vector>

// Where kits are found. Kits live outside the source tree, in the user's kits
// folder (see Settings), either directly (one folder per kit) or grouped in
// content packs: a folder with a pack.json and one subfolder per kit or kit
// preset. Every import produces a pack, one per downloaded library ("DRSKit",
// "Basalt Kits" from the AVL Drumkits), so a pack carries one licence for everything
// in it. That matters for share-alike licences (CC BY-SA), whose kits must be
// passed on under the same licence: keeping them in their own packs keeps
// them separate from everything else, the plugin included.

namespace lithium
{

namespace kitlibrary
{
    struct Pack
    {
        juce::String id, name, author, sourceUrl, description, credits, licenseName, licenseUrl;
        juce::File folder, licenseFile;
        bool shareAlike = false;
    };

    struct Entry
    {
        KitSummary summary;          // name, credits, id, tags, preset or not, sizes
        juce::File folder;
        int pack = -1;               // index into Library::packs; -1 = directly in the kits folder

        const KitCredits& credits() const noexcept   { return summary.credits; }
    };

    struct Library
    {
        std::vector<Pack> packs;     // sorted by name
        std::vector<Entry> kits;     // grouped by pack (loose kits last), kits before presets, then by name

        const Pack* packOf (const Entry& e) const    { return e.pack >= 0 ? &packs[(size_t) e.pack] : nullptr; }
        const Entry* find (const juce::File& folder) const;
        const Entry* findById (const juce::String& id) const;
    };

    /** Every kit and preset in the kits folder, reading only their kit.json
        and pack.json files. Message thread (it reads files, so not on the
        audio thread). */
    Library scan (const juce::File& kitsFolder);

    /** Reads a pack.json. */
    bool readPack (const juce::File& folder, Pack& result);

    /** Where to find a kit saved in a project: its saved path if that still
        holds the same kit, otherwise the installed kit with the saved id (the
        kits folder moved, or the kit was re-imported into a pack), otherwise
        a folder of the same name. */
    juce::File resolveSavedKit (const juce::String& savedPath, const juce::String& savedId, const juce::File& kitsFolder);

    /** The kit a new instance starts with: DRSKit if it's installed,
        otherwise the first kit found. */
    juce::File findDefaultKit (const juce::File& kitsFolder);
}

} // namespace lithium
