#pragma once

#include "InfoPanels.h"
#include "packs/PackInstaller.h"

#include <map>

namespace lithium
{

/** The kit browser: every installed kit and kit preset as a card, grouped
    by content pack, with search, a kits/kit presets filter, and a details pane
    with the selected kit's credits. Opened from the header's Browse button.

    Packs in the LithiumPacks catalog that aren't installed are there too,
    greyed out, with their kits as cards; the details pane downloads and
    installs one (packs::Installer), and the cards show how far it's got.

    It reads the kits folder each time it opens (kitlibrary::scan reads only
    kit.json and pack.json files), so it always shows what's installed now. */
class KitBrowser final : public OverlayPanel, private juce::ChangeListener, private juce::Timer
{
public:
    explicit KitBrowser (LithiumDrumsProcessor&);
    ~KitBrowser() override;

    /** Asks something over the whole UI (see PromptPanel); set by MainView. */
    std::function<void (PromptPanel::Request)> onPrompt;

    /** Re-reads the kits folder and selects the loaded kit. */
    void refresh();

    /** Makes the next refresh() open on this kit (by name; installed or
        not) in its pack, with the search and filters cleared. */
    void showKit (const juce::String& kitName)    { kitToShow = kitName; }

    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class PackList;
    class Grid;
    class KitCard;
    class Details;

    enum class TypeFilter { all, kits, presets };

    void layoutContent (juce::Rectangle<int>) override;
    void layoutGrid();
    void applyFilter();
    bool passesFilter (const kitlibrary::Entry&) const;
    void select (int entry, bool scrollIntoView = false);
    void moveSelection (int delta);
    void load (int entry);
    void removeSelected();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void addMissingPacks();
    const packs::CatalogPack* missingPackOf (const kitlibrary::Entry&) const;
    void updateInstallStatus();

    LithiumDrumsProcessor& processor;
    kitlibrary::Library library;
    juce::SharedResourcePointer<packs::Installer> installer;

    // Library packs that stand for catalog packs not installed yet, by pack index.
    std::map<int, const packs::CatalogPack*> missingPacks;

    juce::TextEditor search;
    juce::TextButton allButton { "All" }, kitsButton { "Kits" }, presetsButton { "Kit presets" };
    std::unique_ptr<PackList> packList;
    juce::Viewport gridView;
    std::unique_ptr<Grid> grid;
    std::vector<std::unique_ptr<KitCard>> cards;     // one per library entry, in library order
    std::unique_ptr<Details> details;
    juce::Rectangle<int> emptyMessageArea;

    static constexpr int allPacks = -2, loosePacks = -1;
    int packFilter = allPacks;                       // allPacks, loosePacks or a pack index
    TypeFilter typeFilter = TypeFilter::all;
    int selected = -1;                               // index into library.kits
    juce::String reselectName;                       // a kit to select after the next refresh
    juce::String kitToShow;                          // see showKit()
    int columns = 3;
};

} // namespace lithium
