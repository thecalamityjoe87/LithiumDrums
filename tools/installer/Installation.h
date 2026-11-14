#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <vector>

// What the Linux installer installs, where, and how it removes it again.
// No UI here: InstallerMain.cpp drives it from a window, or from the command
// line (--install / --remove), which is also how it's tested.
//
// Everything goes into the user's home folder, so no password is needed:
//   VST3            ~/.vst3/Lithium Drums.vst3
//   CLAP            ~/.clap/Lithium Drums.clap
//   standalone app  ~/.local/lib/lithium-drums/Lithium Drums, a menu entry,
//                   icons, and ~/.local/bin/lithium-drums (a link to it)
//   LithiumImport   ~/.local/bin/LithiumImport
//   factory presets ~/.local/share/Lithium Drums/Factory Presets (where
//                   Settings::getFactoryPresetsFolder looks for a per-user install)
// (~/.local/share is $XDG_DATA_HOME when that's set.) The program's own
// folder is in ~/.local/lib, so ~/.local/share only holds the user's data
// folder; up to 0.9.5 it was ~/.local/share/lithium-drums, and an update or
// Remove still finds an install there.
//
// Every path it creates is listed in ~/.local/lib/lithium-drums/install.json,
// and Remove deletes exactly those, so the user's kits, presets and settings
// (which live next to them) are never touched.

namespace lithium::installer
{

struct Part
{
    juce::String id, name, description;
    juce::String where;            // shown to the user, e.g. "~/.vst3"
    bool selectedByDefault = true;
};

/** The choices offered, in the order shown. Factory presets aren't one:
    every format needs them, so they're always installed. */
const std::vector<Part>& components();

/** What's installed now, read from the manifest. */
struct Existing
{
    bool installed = false;
    juce::String version;
    juce::StringArray components;
};

class Installation
{
public:
    /** `payload` is the folder of files to install (next to the installer:
        plugins/, standalone/, tools/, presets/, icons/, and the licence
        files). `version` is the version being installed. */
    Installation (juce::File payload, juce::String version);

    /** Checks the payload holds everything; returns an error message, or
        an empty string. */
    juce::String checkPayload() const;

    Existing existing() const;

    /** Status lines and progress (0 to 1) as the work goes. Called on the
        thread that runs install() or remove(). */
    using Progress = std::function<void (const juce::String& status, float progress)>;

    /** Removes any earlier install, then installs `componentIds`. Runs on
        whatever thread calls it (the window runs it on a background thread
        so the UI stays live). Returns an error message, or an empty string. */
    juce::String install (const juce::StringArray& componentIds, const Progress&);

    /** Removes everything the manifest lists. Returns an error message, or
        an empty string. */
    juce::String remove (const Progress&);

    /** The installed standalone app, if it is installed. */
    juce::File standaloneApp() const;

    static juce::File dataHome();     // $XDG_DATA_HOME or ~/.local/share
    static juce::File manifestFile();   // the installed one, in the current or the old program folder

private:
    juce::File payload;
    juce::String version;
};

}
