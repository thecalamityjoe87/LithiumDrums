#pragma once

#include <juce_data_structures/juce_data_structures.h>

// Per-user settings, shared by every Lithium Drums instance and by the
// LithiumImport tool. For now that's just where the kits live.

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

private:
    // Declared before the file, which keeps a pointer to it: members are
    // constructed in declaration order and destroyed in reverse.
    juce::InterProcessLock lock { "LithiumDrumsSettings" };
    std::unique_ptr<juce::PropertiesFile> file;
};

} // namespace lithium
