#include "Installation.h"

namespace lithium::installer
{

namespace
{
    const juce::String appId = "io.github.thecalamityjoe87.lithiumdrums";

    juce::File home()          { return juce::File::getSpecialLocation (juce::File::userHomeDirectory); }
    // The program's own files: the standalone app, the licence, and the
    // manifest. In ~/.local/lib, like /usr/lib/<app> for a system install,
    // so ~/.local/share holds only the user's data folder ("Lithium Drums").
    juce::File programFolder() { return home().getChildFile (".local/lib/lithium-drums"); }

    // Where versions up to 0.9.5 put it. An update or Remove still finds an
    // install there (its manifest lists those paths) and clears it away.
    juce::File legacyProgramFolder() { return Installation::dataHome().getChildFile ("lithium-drums"); }

    // The menu entry. Exec is quoted because the path has a space in it;
    // StartupWMClass is JUCE's X11 window class (the app name), which lets
    // the desktop match the open window to this entry and its icon.
    juce::String desktopEntry (const juce::File& app)
    {
        return "[Desktop Entry]\n"
               "Type=Application\n"
               "Name=Lithium Drums\n"
               "GenericName=Drum Sampler\n"
               "Comment=Sampled acoustic drum kit (standalone; also installed as a plugin for your DAW)\n"
               "Exec=\"" + app.getFullPathName() + "\"\n"
               "Icon=" + appId + "\n"
               "Terminal=false\n"
               "Categories=AudioVideo;Audio;Music;\n"
               "Keywords=drums;sampler;midi;e-drums;\n"
               "StartupWMClass=Lithium Drums\n";
    }

    // Remove only refuses paths that aren't ours: inside the home folder, and
    // with "lithium" somewhere in them. The manifest is a file the user could
    // edit; this keeps a damaged one from deleting anything else.
    bool safeToDelete (const juce::File& f)
    {
        return f.isAChildOf (home()) && f.getFullPathName().containsIgnoreCase ("lithium");
    }

    // Nudges the desktop to re-read menu entries and icons. Desktops watch
    // these folders' modification times, so touching them is enough. (Not
    // gtk-update-icon-cache: a cache file left in the user's icon folder goes
    // stale and hides icons other apps add later.)
    void refreshDesktop()
    {
        const auto data = Installation::dataHome();

        for (const auto* folder : { "applications", "icons/hicolor" })
            if (const auto f = data.getChildFile (folder); f.isDirectory())
                f.setLastModificationTime (juce::Time::getCurrentTime());
    }
}

const std::vector<Part>& components()
{
    static const std::vector<Part> list {
        { "standalone", "Standalone app", "Runs on its own, no DAW needed. Adds it to your app menu.", "~/.local + app menu", true },
        { "vst3", "VST3 plugin", "For most Linux DAWs: Reaper, Bitwig, Ardour, Renoise...", "~/.vst3", true },
        { "clap", "CLAP plugin", "For Bitwig, Reaper and other CLAP hosts.", "~/.clap", true },
        { "tools", "LithiumImport", "Command-line kit importer, for advanced users.", "~/.local/bin", false },
    };
    return list;
}

Installation::Installation (juce::File p, juce::String v) : payload (std::move (p)), version (std::move (v)) {}

juce::File Installation::dataHome()
{
    const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    return xdg.isNotEmpty() && juce::File::isAbsolutePath (xdg) ? juce::File (xdg) : home().getChildFile (".local/share");
}

juce::File Installation::manifestFile()
{
    // The install that's there: the current place, else the old one.
    const auto current = programFolder().getChildFile ("install.json");
    const auto legacy = legacyProgramFolder().getChildFile ("install.json");
    return ! current.existsAsFile() && legacy.existsAsFile() ? legacy : current;
}

juce::File Installation::standaloneApp() const
{
    const auto app = programFolder().getChildFile ("Lithium Drums");
    return app.existsAsFile() ? app : juce::File();
}

juce::String Installation::checkPayload() const
{
    for (const auto* path : { "plugins/Lithium Drums.vst3", "plugins/Lithium Drums.clap", "standalone/Lithium Drums", "presets", "icons", "LICENSE" })
        if (! payload.getChildFile (path).exists())
            return "The installer's files are incomplete (no " + juce::String (path) + " in " + payload.getFullPathName()
                   + "). Unpack the whole download again and run the installer from there.";

    return {};
}

Existing Installation::existing() const
{
    Existing result;
    const auto json = juce::JSON::parse (manifestFile());

    if (auto* object = json.getDynamicObject())
    {
        result.installed = true;
        result.version = object->getProperty ("version").toString();

        if (auto* list = object->getProperty ("components").getArray())
            for (const auto& c : *list)
                result.components.add (c.toString());
    }

    return result;
}

juce::String Installation::remove (const Progress& progress)
{
    const auto json = juce::JSON::parse (manifestFile());
    juce::StringArray paths;

    if (auto* object = json.getDynamicObject())
        if (auto* list = object->getProperty ("paths").getArray())
            for (const auto& p : *list)
                paths.add (p.toString());

    juce::StringArray failed;

    for (int i = 0; i < paths.size(); ++i)
    {
        const juce::File f (paths[i]);
        progress ("Removing " + f.getFileName(), (float) i / (float) std::max (1, paths.size()));

        if (! safeToDelete (f))
            continue;

        // A symlink is deleted as a link, never followed.
        const auto ok = f.isSymbolicLink() || ! f.isDirectory() ? f.deleteFile() : f.deleteRecursively();

        if (! ok && f.exists())
            failed.add (f.getFullPathName());
    }

    manifestFile().deleteFile();

    // The program folder (and the old one), if nothing else is left in it.
    for (const auto& folder : { programFolder(), legacyProgramFolder() })
        if (folder.isDirectory() && folder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
            folder.deleteFile();

    refreshDesktop();
    progress ("Removed", 1.0f);
    return failed.isEmpty() ? juce::String() : "Couldn't remove:\n" + failed.joinIntoString ("\n");
}

juce::String Installation::install (const juce::StringArray& ids, const Progress& progress)
{
    if (const auto error = checkPayload(); error.isNotEmpty())
        return error;

    // A fresh start: an update shouldn't leave behind a format the user has
    // now unticked, or files an older version had and this one doesn't.
    if (manifestFile().existsAsFile())
        if (const auto error = remove ([&] (const juce::String& s, float p) { progress (s, p * 0.1f); }); error.isNotEmpty())
            return error;

    // Each step: what to copy from the payload, and where to.
    struct Step { juce::String status; juce::File from, to; bool executable = false; };
    std::vector<Step> steps;
    const auto data = dataHome();
    const auto program = programFolder();
    const auto bin = home().getChildFile (".local/bin");

    if (ids.contains ("vst3"))
        steps.push_back ({ "Copying the VST3 plugin", payload.getChildFile ("plugins/Lithium Drums.vst3"), home().getChildFile (".vst3/Lithium Drums.vst3") });

    if (ids.contains ("clap"))
        steps.push_back ({ "Copying the CLAP plugin", payload.getChildFile ("plugins/Lithium Drums.clap"), home().getChildFile (".clap/Lithium Drums.clap") });

    if (ids.contains ("standalone"))
    {
        steps.push_back ({ "Copying the standalone app", payload.getChildFile ("standalone/Lithium Drums"), program.getChildFile ("Lithium Drums"), true });

        for (const auto& icon : payload.getChildFile ("icons").findChildFiles (juce::File::findFiles, false, "icon-*.png"))
        {
            const auto size = icon.getFileNameWithoutExtension().fromFirstOccurrenceOf ("-", false, false);
            steps.push_back ({ "Adding the icons", icon, data.getChildFile ("icons/hicolor/" + size + "x" + size + "/apps/" + appId + ".png") });
        }
    }

    if (ids.contains ("tools"))
        steps.push_back ({ "Copying LithiumImport", payload.getChildFile ("tools/LithiumImport"), bin.getChildFile ("LithiumImport"), true });

    steps.push_back ({ "Copying the factory presets", payload.getChildFile ("presets"), data.getChildFile ("Lithium Drums/Factory Presets") });

    for (const auto* doc : { "LICENSE", "THIRD-PARTY-NOTICES.txt", "README.md" })
        if (payload.getChildFile (doc).existsAsFile())
            steps.push_back ({ "Copying the licence", payload.getChildFile (doc), program.getChildFile (doc) });

    juce::StringArray installed;

    for (size_t i = 0; i < steps.size(); ++i)
    {
        const auto& step = steps[i];
        progress (step.status + "...", 0.1f + 0.85f * (float) i / (float) steps.size());

        if (! step.from.exists())
        {
            if (step.status.contains ("LithiumImport"))
                continue;   // optional in a build without the importer

            return "Missing from the installer: " + step.from.getFullPathName();
        }

        step.to.getParentDirectory().createDirectory();

        // Delete first, then write a new file: a DAW that has the old plugin
        // loaded keeps its (now unlinked) copy instead of having it
        // overwritten under it, which could crash it.
        step.to.isDirectory() ? step.to.deleteRecursively() : step.to.deleteFile();
        const auto ok = step.from.isDirectory() ? step.from.copyDirectoryTo (step.to) : step.from.copyFileTo (step.to);

        if (! ok)
            return "Couldn't write " + step.to.getFullPathName() + ". Is the disk full, or the folder read-only?";

        if (step.executable)
            step.to.setExecutePermission (true);

        installed.add (step.to.getFullPathName());
    }

    if (ids.contains ("standalone"))
    {
        progress ("Adding Lithium Drums to the app menu...", 0.96f);
        const auto app = program.getChildFile ("Lithium Drums");
        const auto entry = data.getChildFile ("applications/" + appId + ".desktop");
        entry.getParentDirectory().createDirectory();

        if (! entry.replaceWithText (desktopEntry (app)))
            return "Couldn't write " + entry.getFullPathName();

        installed.add (entry.getFullPathName());

        // A command for the terminal too. ~/.local/bin is on the PATH on
        // most distributions; if it isn't, the menu entry still works.
        const auto link = bin.getChildFile ("lithium-drums");
        bin.createDirectory();

        if (app.createSymbolicLink (link, true))
            installed.add (link.getFullPathName());
    }

    // The manifest last, so a failed install doesn't claim to be one.
    auto manifest = std::make_unique<juce::DynamicObject>();
    manifest->setProperty ("version", version);
    manifest->setProperty ("components", juce::var (ids));
    manifest->setProperty ("paths", juce::var (installed));
    program.createDirectory();

    if (! manifestFile().replaceWithText (juce::JSON::toString (juce::var (manifest.release()))))
        return "Couldn't write " + manifestFile().getFullPathName();

    refreshDesktop();
    progress ("Installed", 1.0f);
    return {};
}

}
