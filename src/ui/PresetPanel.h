#pragma once

#include "Controls.h"
#include "KitLibrary.h"
#include "PluginProcessor.h"
#include "packs/PackInstaller.h"

#include <map>

namespace lithium
{

/** The preset box's drop-down: presets on the left (yours, then factory,
    with a search field), kits on the right (by content pack; choosing one
    starts afresh from it), and the preset actions along the bottom. (The
    kit browser has its own button beside the box.) A preset whose kit
    isn't installed is greyed, with the kit it needs; choosing it opens the
    kit browser on that kit, to download it. It's
    shown in a call-out under the preset box and closes when something is
    chosen. The header does the work through the callbacks, so it can ask
    for names and report problems. */
class PresetPanel final : public juce::Component
{
public:
    struct Callbacks
    {
        std::function<void (const juce::File&)> loadPreset, loadKit;
        std::function<void()> save, saveAs, rename, remove, browse;
        std::function<void (const juce::String& kitName)> showKit;   // opens the kit browser on that kit
    };

    PresetPanel (LithiumDrumsProcessor&, const kitlibrary::Library&, Callbacks);
    ~PresetPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int width = 640, height = 460;

private:
    struct Row
    {
        enum class Kind { heading, subheading, item, note };

        Kind kind = Kind::item;
        juce::String text, detail;      // detail: shown muted on the right (a pack's size)
        juce::File file;                // the preset, or the kit
        bool current = false;
        bool available = true;          // a kit that isn't installed yet is listed, greyed
        juce::String needsKit;          // a preset whose kit isn't installed: that kit's name
        bool info = false;              // a note with a "?" icon, which opens the preset help

        static Row heading (const juce::String& t)    { Row r; r.kind = Kind::heading; r.text = t; return r; }
        static Row subheading (const juce::String& t) { Row r; r.kind = Kind::subheading; r.text = t; return r; }
        static Row note (const juce::String& t)       { Row r; r.kind = Kind::note; r.text = t; return r; }

        static Row item (const juce::String& t, const juce::File& f, bool isCurrent, const juce::String& d = {}, bool isAvailable = true)
        {
            Row r;
            r.text = t;
            r.file = f;
            r.current = isCurrent;
            r.detail = d;
            r.available = isAvailable;
            return r;
        }
    };

    class Column;

    struct MissingKit
    {
        juce::String name;              // the kit's name, as the preset saved it
        const packs::CatalogPack* pack = nullptr;   // the pack that installs it, if the catalog has one
    };

    void rebuildPresets();
    void showPresetHelp (juce::Rectangle<int> icon);
    void close();
    void choosePreset (const Row&);
    void chooseKit (const Row&);

    LithiumDrumsProcessor& processor;
    Callbacks callbacks;
    std::vector<Row> presetRows, kitRows;
    std::map<juce::File, MissingKit> missingKits;     // presets whose kit isn't installed, read once on opening

    juce::TextEditor search;
    std::unique_ptr<Column> presetList, kitList;
    juce::TextButton saveButton { "Save" }, saveAsButton { "Save as..." }, renameButton { "Rename..." },
        deleteButton { "Delete..." };
};

} // namespace lithium
