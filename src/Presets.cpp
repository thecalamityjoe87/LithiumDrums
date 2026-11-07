#include "Presets.h"

namespace lithium::presets
{

namespace
{
    const juce::Identifier rootType { "LithiumPreset" };
    const juce::Identifier versionId { "formatVersion" }, kitIdId { "kitId" }, kitPathId { "kitPath" }, kitNameId { "kitName" };
    const juce::Identifier descriptionId { "description" };

    std::vector<Entry> scanFolder (const juce::File& folder, bool factory)
    {
        std::vector<Entry> result;

        for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension))
            result.push_back ({ nameOf (f), f, factory, {} });

        // Factory presets come grouped by style, a subfolder each.
        if (factory)
            for (const auto& sub : folder.findChildFiles (juce::File::findDirectories, false))
                for (const auto& f : sub.findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension))
                    result.push_back ({ nameOf (f), f, factory, sub.getFileName() });

        std::sort (result.begin(), result.end(), [] (const Entry& a, const Entry& b)
        {
            if (const auto byGroup = a.group.compareNatural (b.group); byGroup != 0)
                return byGroup < 0;

            return a.name.compareNatural (b.name) < 0;
        });

        return result;
    }
}

std::vector<Entry> scan()
{
    auto result = scanFolder (Settings::getPresetsFolder(), false);
    const auto factory = scanFolder (Settings::getFactoryPresetsFolder(), true);
    result.insert (result.end(), factory.begin(), factory.end());
    return result;
}

juce::String nameOf (const juce::File& file)
{
    return file.getFileNameWithoutExtension();
}

bool isFactory (const juce::File& file)
{
    return file.isAChildOf (Settings::getFactoryPresetsFolder());
}

juce::File userFileFor (const juce::String& name)
{
    // createLegalFileName drops characters no OS allows; trimming keeps "  "
    // or "." from becoming a hidden or empty name.
    const auto safe = juce::File::createLegalFileName (name.trim()).trimCharactersAtStart (".").trim();

    if (safe.isEmpty())
        return {};

    return Settings::getPresetsFolder().getChildFile (safe + fileExtension);
}

juce::Result read (const juce::File& file, Contents& result)
{
    const auto xml = juce::XmlDocument::parse (file);

    if (xml == nullptr || ! xml->hasTagName (rootType.toString()))
        return juce::Result::fail ("\"" + nameOf (file) + "\" isn't a Lithium Drums preset, or it's damaged.");

    if (xml->getIntAttribute (versionId) > formatVersion)
        return juce::Result::fail ("\"" + nameOf (file) + "\" was saved by a newer version of Lithium Drums.");

    const auto tree = juce::ValueTree::fromXml (*xml);
    result = {};
    result.kitId = tree[kitIdId].toString();
    result.kitPath = tree[kitPathId].toString();
    result.kitName = tree[kitNameId].toString();
    result.description = tree[descriptionId].toString();
    result.version = (int) tree.getProperty (versionId, 1);
    result.swaps = swaps::fromValueTree (tree.getChildWithName (swaps::treeType));

    for (const auto& child : tree)
        if (! child.hasType (swaps::treeType))
            result.parameters = child.createCopy();

    if (! result.parameters.isValid() || (result.kitId.isEmpty() && result.kitPath.isEmpty()))
        return juce::Result::fail ("\"" + nameOf (file) + "\" is missing its kit or settings.");

    return juce::Result::ok();
}

juce::Result write (const juce::File& file, const Contents& contents)
{
    juce::ValueTree tree (rootType, { { versionId, formatVersion }, { kitIdId, contents.kitId },
                                      { kitPathId, contents.kitPath }, { kitNameId, contents.kitName } });

    if (contents.description.isNotEmpty())
        tree.setProperty (descriptionId, contents.description, nullptr);

    tree.appendChild (swaps::toValueTree (contents.swaps), nullptr);
    tree.appendChild (contents.parameters.createCopy(), nullptr);

    const auto xml = tree.createXml();

    if (xml == nullptr || ! file.getParentDirectory().createDirectory())
        return juce::Result::fail ("Couldn't create the presets folder " + file.getParentDirectory().getFullPathName());

    // Written next to the file and moved over it, so a failed save never
    // leaves half a preset behind.
    juce::TemporaryFile temp (file);

    if (! xml->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Couldn't write " + file.getFullPathName());

    return juce::Result::ok();
}

} // namespace lithium::presets
