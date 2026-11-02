#include "Settings.h"

namespace lithium
{

namespace
{
    const juce::Identifier kitsFolderKey { "kitsFolder" };
    const juce::Identifier streamingKey { "streaming" };
    const juce::Identifier preloadKey { "preloadFrames" };
    const juce::Identifier midiMapKey { "defaultMidiMap" };

    constexpr std::array<std::pair<StreamingMode, const char*>, 3> modeNames {{
        { StreamingMode::automatic, "auto" },
        { StreamingMode::always, "always" },
        { StreamingMode::never, "never" },
    }};

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

StreamingSettings Settings::getStreaming() const
{
    StreamingSettings result;
    result.mode = StreamingMode::automatic;

    // LITHIUM_STREAMING (auto, always or never) overrides the saved mode, for
    // the tests and for trying things out.
    const auto envMode = juce::SystemStats::getEnvironmentVariable ("LITHIUM_STREAMING", {});
    const auto mode = envMode.isNotEmpty() ? envMode : file->getValue (streamingKey.toString());

    for (const auto& [m, name] : modeNames)
        if (mode == name)
            result.mode = m;

    const auto preload = file->getIntValue (preloadKey.toString(), defaultPreloadFrames);
    result.preloadFrames = std::find (preloadChoices.begin(), preloadChoices.end(), preload) != preloadChoices.end()
                             ? preload : defaultPreloadFrames;

    const auto env = juce::SystemStats::getEnvironmentVariable ("LITHIUM_CACHE_FOLDER", {});
    result.cacheFolder = env.isNotEmpty() && juce::File::isAbsolutePath (env) ? juce::File (env) : getDefaultCacheFolder();
    return result;
}

void Settings::setStreamingMode (StreamingMode mode)
{
    for (const auto& [m, name] : modeNames)
        if (m == mode)
            file->setValue (streamingKey.toString(), name);

    file->saveIfNeeded();
}

void Settings::setPreloadFrames (int frames)
{
    file->setValue (preloadKey.toString(), frames);
    file->saveIfNeeded();
}

juce::String Settings::getDefaultMidiMap() const
{
    return file->getValue (midiMapKey.toString());
}

void Settings::setDefaultMidiMap (const juce::String& json)
{
    file->setValue (midiMapKey.toString(), json);
    file->saveIfNeeded();
}

juce::File Settings::getMidiMapsFolder()
{
    if (const auto env = juce::SystemStats::getEnvironmentVariable ("LITHIUM_MIDI_MAPS_FOLDER", {});
        env.isNotEmpty() && juce::File::isAbsolutePath (env))
        return juce::File (env);

    return getDefaultKitsFolder().getSiblingFile ("MIDI Maps");
}

juce::File Settings::getGroovesFolder()
{
    if (const auto env = juce::SystemStats::getEnvironmentVariable ("LITHIUM_GROOVES_FOLDER", {});
        env.isNotEmpty() && juce::File::isAbsolutePath (env))
        return juce::File (env);

    return getDefaultKitsFolder().getSiblingFile ("Grooves");
}

juce::File Settings::getPresetsFolder()
{
    if (const auto env = juce::SystemStats::getEnvironmentVariable ("LITHIUM_PRESETS_FOLDER", {});
        env.isNotEmpty() && juce::File::isAbsolutePath (env))
        return juce::File (env);

    return getDefaultKitsFolder().getSiblingFile ("Presets");
}

juce::File Settings::getFactoryPresetsFolder()
{
    if (const auto env = juce::SystemStats::getEnvironmentVariable ("LITHIUM_FACTORY_PRESETS_FOLDER", {});
        env.isNotEmpty() && juce::File::isAbsolutePath (env))
        return juce::File (env);

   #if JUCE_MAC
    // commonApplicationDataDirectory is /Library on macOS.
    return juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
               .getChildFile ("Application Support/Lithium Drums/Presets");
   #elif JUCE_WINDOWS
    // C:\ProgramData
    return juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
               .getChildFile ("Lithium Drums").getChildFile ("Presets");
   #else
    return juce::File ("/usr/share/lithium-drums/presets");
   #endif
}

juce::File Settings::getDefaultCacheFolder()
{
    // A cache folder, not the data folder: the OS (and backup tools) know
    // its contents can be thrown away and rebuilt.
   #if JUCE_WINDOWS
    const auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("Lithium Drums");
   #elif JUCE_MAC
    const auto base = juce::File ("~/Library/Caches/Lithium Drums");
   #else
    const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_CACHE_HOME", {});
    const auto base = (xdg.isNotEmpty() && juce::File::isAbsolutePath (xdg) ? juce::File (xdg) : juce::File ("~/.cache"))
                        .getChildFile ("Lithium Drums");
   #endif

    return base.getChildFile ("Stream Cache");
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
