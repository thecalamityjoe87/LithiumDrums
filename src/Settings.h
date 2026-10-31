#pragma once

#include "engine/StreamingSettings.h"

#include <array>

#include <juce_data_structures/juce_data_structures.h>

// Per-user settings, shared by every Lithium Drums instance and by the
// LithiumImport tool: where the kits live, and how they stream from disk.

namespace lithium
{

/** Per-user settings, shared by every Lithium Drums instance in the process
    (get one through juce::SharedResourcePointer<Settings>). Message thread. */
class Settings
{
public:
    Settings();

    /** The saved kits folder, or the default. The LITHIUM_KITS_FOLDER
        environment variable overrides both (the tests use it). */
    juce::File getKitsFolder() const;
    void setKitsFolder (const juce::File&);

    /** Linux: $XDG_DATA_HOME (or ~/.local/share)/Lithium Drums/Kits
        macOS: ~/Library/Application Support/Lithium Drums/Kits
        Windows: %LOCALAPPDATA%\Lithium Drums\Kits */
    static juce::File getDefaultKitsFolder();

    /** Streaming mode, preload size and cache folder. The environment
        variables LITHIUM_STREAMING and LITHIUM_CACHE_FOLDER override the mode
        and the folder. */
    StreamingSettings getStreaming() const;
    void setStreamingMode (StreamingMode);
    void setPreloadFrames (int);

    /** Linux: $XDG_CACHE_HOME (or ~/.cache)/Lithium Drums/Stream Cache
        macOS: ~/Library/Caches/Lithium Drums/Stream Cache
        Windows: %LOCALAPPDATA%\Lithium Drums\Stream Cache */
    static juce::File getDefaultCacheFolder();

    /** The preload sizes offered in the settings, in frames. */
    static constexpr std::array<int, 4> preloadChoices { 8192, 16384, 32768, 65536 };
    static constexpr int defaultPreloadFrames = 16384;

private:
    // Declared before the file, which keeps a pointer to it: members are
    // constructed in declaration order and destroyed in reverse.
    juce::InterProcessLock lock { "LithiumDrumsSettings" };
    std::unique_ptr<juce::PropertiesFile> file;
};

} // namespace lithium
