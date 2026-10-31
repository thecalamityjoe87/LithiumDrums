#pragma once

#include "Settings.h"
#include "engine/Kit.h"

#include <vector>

// Where kits are found. Kits live outside the source tree, one folder per kit
// inside the user's kits folder (see Settings).

namespace lithium
{

namespace kitlibrary
{
    struct Entry
    {
        KitCredits credits;
        juce::File folder;
    };

    /** Every kit in the kits folder (each subfolder with a readable kit.json),
        sorted by name. Reads only each kit's metadata. */
    std::vector<Entry> findKits (const juce::File& kitsFolder);

    /** Where to look for a kit saved in a project: its saved path, or, if
        that's gone (the kits folder moved), a folder of the same name in the
        current kits folder. */
    juce::File resolveSavedKit (const juce::String& savedPath, const juce::File& kitsFolder);

    /** The kit a new instance starts with: DRSKit if it's installed,
        otherwise the first kit found. */
    juce::File findDefaultKit (const juce::File& kitsFolder);
}

} // namespace lithium
