#include "PresetPanel.h"

#include "Theme.h"
#include "packs/PackInstaller.h"

namespace lithium
{

namespace
{
    constexpr int listRowHeight = 26, columnHeadingHeight = 22, actionBarHeight = 48;
}

//==============================================================================
/** One of the two lists: headings, items and notes, drawn alike. */
class PresetPanel::Column final : public juce::ListBox, private juce::ListBoxModel
{
public:
    Column (std::vector<Row>& r, std::function<void (const Row&)> chosen) : rows (r), onChosen (std::move (chosen))
    {
        setModel (this);
        setRowHeight (listRowHeight);
        setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, theme::colours::borderStrong);
        getViewport()->setScrollBarThickness (8);
        setMouseMoveSelectsRows (false);
    }

    ~Column() override    { setModel (nullptr); }

    void refresh()
    {
        updateContent();
        repaint();

        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].current)
                scrollToEnsureRowIsOnscreen ((int) i);
    }

private:
    int getNumRows() override    { return (int) rows.size(); }

    void paintListBoxItem (int index, juce::Graphics& g, int w, int h, bool) override
    {
        using namespace theme;

        if (! juce::isPositiveAndBelow (index, (int) rows.size()))
            return;

        const auto& r = rows[(size_t) index];
        auto area = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);

        switch (r.kind)
        {
            case Row::Kind::heading:
                g.setColour (colours::textMuted);
                drawTrackedText (g, r.text.toUpperCase(), label (type::caption), 0.1f, area.reduced (8.0f, 0.0f).withTrimmedTop (6.0f),
                                 juce::Justification::centredLeft);
                return;

            case Row::Kind::note:
                g.setColour (colours::textMuted);
                g.setFont (body (type::small));
                g.drawText (r.text, area.reduced (12.0f, 0.0f), juce::Justification::centredLeft, true);
                return;

            case Row::Kind::item:
                break;
        }

        const auto mouse = getMouseXYRelative();
        const auto hover = r.available && isMouseOver (true) && getRowContainingPosition (mouse.x, mouse.y) == index;

        if (r.current || hover)
        {
            g.setColour (r.current ? colours::raisedHigh : colours::raised);
            g.fillRoundedRectangle (area.reduced (2.0f, 1.0f), cornerRadius - 2.0f);
        }

        auto text = area.reduced (10.0f, 0.0f);

        if (r.current)
        {
            g.setColour (colours::accent);
            g.fillEllipse (text.removeFromLeft (7.0f).withSizeKeepingCentre (7.0f, 7.0f));
            text.removeFromLeft (8.0f);
        }
        else
        {
            text.removeFromLeft (15.0f);
        }

        if (r.detail.isNotEmpty())
        {
            g.setColour (colours::textMuted);
            g.setFont (mono (type::caption));
            g.drawText (r.detail, text.removeFromRight (72.0f), juce::Justification::centredRight, false);
        }

        g.setColour (! r.available ? colours::textMuted.withAlpha (0.7f) : r.current ? colours::text : colours::text.withAlpha (0.88f));
        g.setFont (body (13.5f, r.current ? Weight::semibold : Weight::regular));
        g.drawText (r.text, text, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int index, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow (index, (int) rows.size()) && rows[(size_t) index].kind == Row::Kind::item)
            onChosen (rows[(size_t) index]);
    }

    std::vector<Row>& rows;
    std::function<void (const Row&)> onChosen;
};

//==============================================================================
PresetPanel::PresetPanel (LithiumDrumsProcessor& p, const kitlibrary::Library& library, Callbacks c)
    : processor (p), callbacks (std::move (c))
{
    using namespace theme;

    search.setTextToShowWhenEmpty ("Search presets...", colours::textMuted);
    search.setFont (body (type::body));
    search.setIndents (10, 6);
    search.onTextChange = [this] { rebuildPresets(); };
    search.onEscapeKey = [this] { close(); };
    addAndMakeVisible (search);

    presetList = std::make_unique<Column> (presetRows, [this] (const Row& r) { choosePreset (r); });
    kitList = std::make_unique<Column> (kitRows, [this] (const Row& r) { chooseKit (r); });
    addAndMakeVisible (*presetList);
    addAndMakeVisible (*kitList);

    // The kits, grouped by content pack, each pack's kits before its presets.
    const auto currentKit = processor.getKitFolder();
    const auto noPreset = processor.getPresetFile() == juce::File();
    int lastPack = std::numeric_limits<int>::min();

    for (const auto& kit : library.kits)
    {
        if (kit.pack != lastPack)
        {
            const auto* pack = library.packOf (kit);
            kitRows.push_back (Row::heading (pack != nullptr ? pack->name : juce::String ("Other kits")));
            lastPack = kit.pack;
        }

        kitRows.push_back (Row::item (kit.credits().name, kit.folder, noPreset && kit.folder == currentKit));
    }

    // Libraries that aren't installed: greyed, with their download size. Choosing one opens the kit browser.
    auto headed = false;

    for (const auto& pack : packs::catalog())
    {
        if (pack.type != packs::CatalogPack::Type::kits
            || std::any_of (library.packs.begin(), library.packs.end(), [&] (const auto& lp) { return lp.id == pack.id; }))
            continue;

        if (! headed)
            kitRows.push_back (Row::heading ("Not installed: download in Browse"));

        headed = true;
        kitRows.push_back (Row::item (pack.name, {}, false, packs::formatSize (pack.downloadSize()), false));
    }

    if (library.kits.empty() && ! headed)
        kitRows.push_back (Row::note ("No kits in the kits folder."));

    // The actions. Save only means something for a changed preset of your own.
    const auto preset = processor.getPresetFile();
    const auto isUserPreset = preset.existsAsFile() && ! presets::isFactory (preset);
    saveButton.setEnabled (isUserPreset && processor.isPresetModified());
    saveAsButton.setEnabled (processor.getKitInfo() != nullptr);
    renameButton.setEnabled (isUserPreset);
    deleteButton.setEnabled (isUserPreset);

    saveButton.setTooltip ("Save the changes into the current preset");
    saveAsButton.setTooltip ("Save everything (the kit, swapped pieces and every setting) as a new preset");
    deleteButton.setColour (juce::TextButton::textColourOffId, colours::warning);

    const std::pair<juce::TextButton*, std::function<void()>*> actions[] {
        { &saveButton, &callbacks.save }, { &saveAsButton, &callbacks.saveAs }, { &renameButton, &callbacks.rename },
        { &deleteButton, &callbacks.remove },
    };

    for (auto [button, action] : actions)
    {
        // Closed first: the action may show a dialog, which must not open under the call-out.
        button->onClick = [this, action]
        {
            auto run = *action;
            close();

            if (run)
                run();
        };
        addAndMakeVisible (*button);
    }

    rebuildPresets();
    kitList->refresh();
    enableKeyboardNavigation (*this);
    setSize (width, height);
}

PresetPanel::~PresetPanel() = default;

void PresetPanel::rebuildPresets()
{
    presetRows.clear();

    const auto all = presets::scan();
    const auto current = processor.getPresetFile();
    const auto modified = processor.isPresetModified();

    juce::StringArray words;
    words.addTokens (search.getText(), " ", "\"");
    words.removeEmptyStrings();

    for (const auto factory : { false, true })
    {
        presetRows.push_back (Row::heading (factory ? "Factory" : "Yours"));
        auto any = false, matched = false;

        for (const auto& e : all)
        {
            if (e.factory != factory)
                continue;

            any = true;

            if (! std::all_of (words.begin(), words.end(), [&] (const auto& w) { return e.name.containsIgnoreCase (w); }))
                continue;

            matched = true;
            const auto isCurrent = e.file == current;
            presetRows.push_back (Row::item (e.name + (isCurrent && modified ? " *" : ""), e.file, isCurrent));
        }

        if (! matched)
            presetRows.push_back (Row::note (any ? "None match the search."
                                             : factory ? "None installed yet."
                                                       : "None yet. Save as... keeps the current sound."));
    }

    presetList->refresh();
}

void PresetPanel::close()
{
    // The call-out owns this panel; dismissing it deletes us asynchronously.
    if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
        box->dismiss();
}

void PresetPanel::choosePreset (const Row& r)
{
    const auto file = r.file;
    auto load = callbacks.loadPreset;
    close();

    if (load)
        load (file);
}

void PresetPanel::chooseKit (const Row& r)
{
    const auto file = r.file;
    auto action = r.available ? callbacks.loadKit : nullptr;
    auto browse = callbacks.browse;
    close();

    if (action)
        action (file);
    else if (browse)
        browse();
}

void PresetPanel::paint (juce::Graphics& g)
{
    using namespace theme;

    const auto half = (getWidth() - 3 * space::md) / 2;
    g.setColour (colours::textMuted);
    drawTrackedText (g, "PRESETS", label (12.5f, Weight::bold), 0.1f,
                     { (float) space::md, (float) space::sm, (float) half, (float) columnHeadingHeight }, juce::Justification::centredLeft);
    drawTrackedText (g, "START FROM A KIT", label (12.5f, Weight::bold), 0.1f,
                     { (float) (2 * space::md + half), (float) space::sm, (float) half, (float) columnHeadingHeight },
                     juce::Justification::centredLeft);

    // A rule between the columns, and one over the action bar.
    g.setColour (colours::border);
    g.fillRect (space::md + half + space::md / 2, space::sm, 1, getHeight() - actionBarHeight - 2 * space::sm);
    g.fillRect (space::md, getHeight() - actionBarHeight, getWidth() - 2 * space::md, 1);
}

void PresetPanel::resized()
{
    using namespace theme;

    auto area = getLocalBounds().reduced (space::md, 0);
    auto bar = area.removeFromBottom (actionBarHeight).withSizeKeepingCentre (area.getWidth(), size::button);
    area.removeFromTop (space::sm + columnHeadingHeight + space::xs);
    area.removeFromBottom (space::sm);

    const auto half = (area.getWidth() - space::md) / 2;
    auto left = area.removeFromLeft (half);
    area.removeFromLeft (space::md);
    search.setBounds (left.removeFromTop (size::button));
    left.removeFromTop (space::xs);
    presetList->setBounds (left);
    kitList->setBounds (area);

    // Save, Save as, Rename and Delete, one width for all.
    constexpr int buttonWidth = 104;

    for (auto* b : { &saveButton, &saveAsButton, &renameButton, &deleteButton })
    {
        b->setBounds (bar.removeFromLeft (buttonWidth));
        bar.removeFromLeft (space::sm);
    }
}

} // namespace lithium
