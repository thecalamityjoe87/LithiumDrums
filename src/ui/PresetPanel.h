#pragma once

#include "Controls.h"
#include "KitLibrary.h"
#include "PluginProcessor.h"

namespace lithium
{

/** The preset box's drop-down: presets on the left (yours, then factory,
    with a search field), kits on the right (by content pack; choosing one
    starts afresh from it), and the preset actions along the bottom. (The
    kit browser has its own button beside the box.) It's
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
    };

    PresetPanel (LithiumDrumsProcessor&, const kitlibrary::Library&, Callbacks);
    ~PresetPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int width = 640, height = 460;

private:
    struct Row
    {
        enum class Kind { heading, item, note };

        Kind kind = Kind::item;
        juce::String text, detail;      // detail: shown muted on the right (a pack's size)
        juce::File file;                // the preset, or the kit
        bool current = false;
        bool available = true;          // a kit that isn't installed yet is listed, greyed

        static Row heading (const juce::String& t)    { Row r; r.kind = Kind::heading; r.text = t; return r; }
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

    void rebuildPresets();
    void close();
    void choosePreset (const Row&);
    void chooseKit (const Row&);

    LithiumDrumsProcessor& processor;
    Callbacks callbacks;
    std::vector<Row> presetRows, kitRows;

    juce::TextEditor search;
    std::unique_ptr<Column> presetList, kitList;
    juce::TextButton saveButton { "Save" }, saveAsButton { "Save as..." }, renameButton { "Rename..." },
        deleteButton { "Delete..." };
};

} // namespace lithium
