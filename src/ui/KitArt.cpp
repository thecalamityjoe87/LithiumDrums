#include "KitArt.h"

#include "LithiumAssets.h"

#include <map>

namespace lithium::kitart
{

namespace
{
    /** An embedded file's bytes by its original file name (the resource
        names are made from it, but not reversibly). */
    const char* findData (const juce::String& fileName, int& size)
    {
        for (int i = 0; i < LithiumAssets::namedResourceListSize; ++i)
            if (fileName == LithiumAssets::getNamedResourceOriginalFilename (LithiumAssets::namedResourceList[i]))
                return LithiumAssets::getNamedResource (LithiumAssets::namedResourceList[i], size);

        return nullptr;
    }
}

juce::Image load (const juce::String& fileName)
{
    int size = 0;

    if (const auto* data = findData (fileName, size))
        return juce::ImageCache::getFromMemory (data, size);

    return {};
}

juce::String finishFor (const juce::String& kitId)
{
    // Read once; it's a few lines of JSON. (A function-local static is
    // initialised the first time through, safely even across threads.)
    // A plain std::map rather than a juce::var, so nothing of JUCE's
    // outlives it at shutdown (its leak checks would complain).
    static const auto aliases = []
    {
        std::map<juce::String, juce::String> result;
        int size = 0;

        // The parsed var is kept in a variable: getDynamicObject() points
        // into it, so a temporary would leave the pointer dangling.
        const auto* data = findData ("kit-finishes.json", size);
        const auto json = data != nullptr ? juce::JSON::parse (juce::String::fromUTF8 (data, size)) : juce::var();

        if (const auto* object = json["aliases"].getDynamicObject())
            for (const auto& p : object->getProperties())
                result[p.name.toString()] = p.value.toString();

        return result;
    }();

    const auto alias = aliases.find (kitId);
    return alias != aliases.end() ? alias->second : kitId;
}

juce::Image stageLayer (const juce::String& kitId, const juce::String& renderKey)
{
    if (const auto image = load ("finish-" + finishFor (kitId) + "-" + renderKey + ".png"); image.isValid())
        return image;

    return load (renderKey + ".png");
}

juce::Image slotPicture (const juce::String& kitId, const juce::String& name)
{
    if (const auto image = load ("finish-" + finishFor (kitId) + "-slot-" + name + ".png"); image.isValid())
        return image;

    return load ("slot-" + name + ".png");
}

} // namespace lithium::kitart
