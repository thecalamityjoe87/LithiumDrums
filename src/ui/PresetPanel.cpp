#include "PresetPanel.h"

#include "Theme.h"
#include "packs/PackInstaller.h"

namespace lithium
{

namespace
{
    constexpr int listRowHeight = 26, columnHeadingHeight = 22, actionBarHeight = 48;
    constexpr float infoIconSize = 14.0f;

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

            case Row::Kind::subheading:
                g.setColour (colours::text);
                drawTrackedText (g, r.text.toUpperCase(), label (type::caption), 0.1f,
                                 area.reduced (14.0f, 0.0f).withTrimmedTop (4.0f), juce::Justification::centredLeft);
                return;

            case Row::Kind::note:
            {
                const auto font = body (type::small);
                auto text = area.reduced (12.0f, 0.0f);
                g.setColour (colours::textMuted);
                g.setFont (font);
                g.drawText (r.text, text, juce::Justification::centredLeft, true);

                if (r.info)
                {
                    // A "?" after the text: click for help.
                    const auto icon = infoIconBounds (r, w, h);
                    const auto hover = isMouseOver (true) && getRowContainingPosition (getMouseXYRelative().x, getMouseXYRelative().y) == index;
                    drawHelpMark (g, icon, hover);
                }
                return;
            }

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

        if (r.needsKit.isNotEmpty())
        {
            // A down arrow and "804 MB" at the right edge, in the same column as the kits' sizes: the
            // arrow just before the size, as on the kit browser's cards. The tooltip
            // says which kit.
            auto column = text.removeFromRight (72.0f);
            g.setColour (colours::textMuted);

            if (r.detail.isNotEmpty())
            {
                const auto sizeFont = mono (type::caption);
                g.setFont (sizeFont);
                g.drawText (r.detail, column.removeFromRight (juce::GlyphArrangement::getStringWidth (sizeFont, r.detail) + 1.0f),
                            juce::Justification::centredRight, false);
                column.removeFromRight (3.0f);
            }

            const juce::String arrow (juce::CharPointer_UTF8 ("\xe2\x86\x93"));
            const auto arrowFont = mono (12.0f);
            g.setFont (arrowFont);
            g.drawText (arrow, column.removeFromRight (juce::GlyphArrangement::getStringWidth (arrowFont, arrow) + 1.0f),
                        juce::Justification::centredRight, false);
        }
        else if (r.detail.isNotEmpty())
        {
            g.setColour (colours::textMuted);
            g.setFont (mono (type::caption));
            g.drawText (r.detail, text.removeFromRight (72.0f), juce::Justification::centredRight, false);
        }

        g.setColour (! r.available ? colours::textMuted.withAlpha (0.7f) : r.current ? colours::text : colours::text.withAlpha (0.88f));
        g.setFont (body (13.5f, r.current ? Weight::semibold : Weight::regular));
        g.drawText (r.text, text, juce::Justification::centredLeft, true);
    }

    juce::String getTooltipForRow (int index) override
    {
        if (! juce::isPositiveAndBelow (index, (int) rows.size()) || rows[(size_t) index].needsKit.isEmpty())
            return {};

        const auto& r = rows[(size_t) index];
        return "Requires " + r.needsKit + (r.detail.isNotEmpty() ? " (" + r.detail + " download)" : juce::String())
               + ", which isn't installed. Click to see it in the kit browser, where Download installs it.";
    }

    void listBoxItemClicked (int index, const juce::MouseEvent&) override
    {
        if (! juce::isPositiveAndBelow (index, (int) rows.size()))
            return;

        const auto& r = rows[(size_t) index];

        if (r.kind == Row::Kind::item)
            onChosen (r);
        else if (r.info && onInfo)
        {
            // The whole note opens the help, not just its icon: a bigger target.
            const auto row = getRowPosition (index, true);
            onInfo (infoIconBounds (r, row.getWidth(), row.getHeight()).toNearestInt() + row.getPosition());
        }
    }

    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override {}

    juce::MouseCursor getMouseCursorForRow (int index) override
    {
        return juce::isPositiveAndBelow (index, (int) rows.size()) && rows[(size_t) index].info ? juce::MouseCursor::PointingHandCursor
                                                                                                : juce::MouseCursor::NormalCursor;
    }

    /** Where a note's help icon is, in its row: just after the text. */
    static juce::Rectangle<float> infoIconBounds (const Row& r, int w, int h)
    {
        const auto font = theme::body (theme::type::small);
        const auto textWidth = juce::GlyphArrangement::getStringWidth (font, r.text);
        const auto x = std::min (12.0f + textWidth + 7.0f, (float) w - infoIconSize - 4.0f);

        // Level with the middle of the text's capitals (where drawText's centred
        // line puts them), not the row's middle, which sits higher.
        juce::GlyphArrangement cap;
        cap.addLineOfText (font, "N", 0.0f, ((float) h - font.getHeight()) * 0.5f + font.getAscent());
        const auto centreY = cap.getBoundingBox (0, -1, true).getCentreY();
        return { x, centreY - infoIconSize * 0.5f, infoIconSize, infoIconSize };
    }

public:
    std::function<void (juce::Rectangle<int>)> onInfo;   // a note's help icon was clicked; its bounds, in the list

private:

    std::vector<Row>& rows;
    std::function<void (const Row&)> onChosen;
};

//==============================================================================
PresetPanel::PresetPanel (LithiumDrumsProcessor& p, const kitlibrary::Library& library, Callbacks c)
    : processor (p), callbacks (std::move (c))
{
    using namespace theme;

    search.setTextToShowWhenEmpty ("Search sound presets...", colours::textMuted);
    search.setFont (body (type::body));
    search.setIndents (10, 6);
    search.onTextChange = [this] { rebuildPresets(); };
    search.onEscapeKey = [this] { close(); };
    addAndMakeVisible (search);

    presetList = std::make_unique<Column> (presetRows, [this] (const Row& r) { choosePreset (r); });
    presetList->onInfo = [this] (juce::Rectangle<int> icon) { showPresetHelp (icon); };
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
            kitRows.push_back (Row::subheading (pack != nullptr ? pack->name : juce::String ("Other kits")));
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
            kitRows.push_back (Row::subheading ("Not installed: download in Browse"));

        headed = true;
        kitRows.push_back (Row::item (pack.name, {}, false, packs::formatSize (pack.downloadSize()), false));
    }

    if (library.kits.empty() && ! headed)
        kitRows.push_back (Row::note ("No kits in the kits folder."));

    // Presets whose kit isn't installed (read once: searching rebuilds the list on every key).
    for (const auto& e : presets::scan())
        if (const auto kit = presets::readKit (e.file); ! presets::isKitInstalled (kit, library))
            missingKits[e.file] = { kit.name, packs::findPackWithKit (kit.name) };

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
        presetRows.push_back (Row::heading (factory ? "Factory" : "Your presets"));
        auto any = false, matched = false;
        juce::String group;

        for (const auto& e : all)
        {
            if (e.factory != factory)
                continue;

            any = true;

            // A word can match the preset's style too ("jazz").
            if (! std::all_of (words.begin(), words.end(), [&] (const auto& w)
                               { return e.name.containsIgnoreCase (w) || e.group.containsIgnoreCase (w); }))
                continue;

            // Factory presets are grouped by style, a sub-heading each under "Factory".
            if (e.group.isNotEmpty() && e.group != group)
            {
                group = e.group;
                presetRows.push_back (Row::subheading (group));
            }

            matched = true;
            const auto isCurrent = e.file == current;
            auto row = Row::item (e.name + (isCurrent && modified ? " *" : ""), e.file, isCurrent);

            // Its kit isn't installed: greyed, with the kit it needs and that kit's download.
            if (const auto missing = missingKits.find (e.file); missing != missingKits.end())
            {
                const auto& [kitName, pack] = missing->second;
                row.available = false;
                row.needsKit = kitName.isNotEmpty() ? kitName : juce::String ("its kit");
                row.detail = pack != nullptr ? packs::formatSize (pack->downloadSize()) : juce::String();
            }

            presetRows.push_back (std::move (row));
        }

        if (any && ! matched)
            presetRows.push_back (Row::note ("None match the search."));
        else if (! any && factory)
            presetRows.push_back (Row::note ("None installed yet."));
        else if (! any)
        {
            auto note = Row::note ("No custom presets yet.");
            note.info = true;
            presetRows.push_back (std::move (note));
        }
    }

    presetList->refresh();
}

void PresetPanel::showPresetHelp (juce::Rectangle<int> icon)
{
    // In a call-out of its own, pointing at the icon. The preset menu stays
    // open under it; a click anywhere else closes the help.
    HelpPopover::show (*presetList, icon, "Your presets",
                       { "A sound preset saves your whole sound: the kit, any pieces you swapped in, how each drum is tuned "
                         "and shaped, the mix and the effects. Your MIDI map and the Grooves page aren't part of it.",
                         "To make one, change anything you like on the Kit, Edit, Mixer or FX pages, then open this menu and "
                         "click Save as... at the bottom. It's listed here from then on, and loads in any project." });
}

void PresetPanel::close()
{
    // The call-out owns this panel; dismissing it deletes us asynchronously.
    if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
        box->dismiss();
}

void PresetPanel::choosePreset (const Row& r)
{
    // Callbacks copied first: closing deletes this panel.
    const auto file = r.file;
    const auto kitName = r.available ? juce::String() : missingKits[file].name;
    auto load = r.available ? callbacks.loadPreset : nullptr;
    auto showKit = callbacks.showKit;
    close();

    if (load)
        load (file);
    else if (showKit)
        showKit (kitName);
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
    drawTrackedText (g, "SOUND PRESETS", label (12.5f, Weight::bold), 0.1f,
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
