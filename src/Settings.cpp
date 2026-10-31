#include "Settings.h"

namespace lithium
{

namespace
{
    const juce::Identifier kitsFolderKey { "kitsFolder" };

    juce::PropertiesFile::Options settingsOptions (juce::InterProcessLock& lock)
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "Lithium Drums";
        options.folderName = "Lithium Drums";
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        options.storageFormat = juce::PropertiesFile::storeAsXML;

        // Plugin instances in different host processes may share the file;
        // the lock stops them writing over each other.
        options.processLock = &lock;
        return options;
    }
}

Settings::Settings() : file (std::make_unique<juce::PropertiesFile> (settingsOptions (lock))) {}

juce::File Settings::getKitsFolder() const
{
    // For tests and development: point every instance at another folder
    // without touching the saved setting.
    if (const auto env = juce::SystemStats::getEnvironmentVariable ("LITHIUM_KITS_FOLDER", {});
        env.isNotEmpty() && juce::File::isAbsolutePath (env))
        return juce::File (env);

    const auto saved = file->getValue (kitsFolderKey.toString());
    return saved.isNotEmpty() && juce::File::isAbsolutePath (saved) ? juce::File (saved) : getDefaultKitsFolder();
}

void Settings::setKitsFolder (const juce::File& folder)
{
    file->setValue (kitsFolderKey.toString(), folder.getFullPathName());
    file->saveIfNeeded();
}

juce::File Settings::getDefaultKitsFolder()
{
   #if JUCE_WINDOWS
    // Local rather than roaming AppData: gigabytes of samples shouldn't roam
    // with the user's profile.
    const auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
   #elif JUCE_MAC
    const auto base = juce::File ("~/Library/Application Support");
   #else
    const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    const auto base = xdg.isNotEmpty() && juce::File::isAbsolutePath (xdg) ? juce::File (xdg) : juce::File ("~/.local/share");
   #endif

    return base.getChildFile ("Lithium Drums").getChildFile ("Kits");
}

} // namespace lithium
