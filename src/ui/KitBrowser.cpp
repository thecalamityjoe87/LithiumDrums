#include "KitBrowser.h"

#include "Theme.h"

namespace lithium
{

namespace
{
    constexpr int packListWidth = 210;
    constexpr int detailsWidth = 300;
    constexpr int columnGap = 16;
    constexpr int tileHeight = 112;
    constexpr int tileGap = 10;
    constexpr int minTileWidth = 180;
    constexpr int sectionHeight = 28;

    juce::String formatBytes (std::int64_t bytes)
    {
        if (bytes <= 0)
            return "-";

        const auto mb = (double) bytes / (1024.0 * 1024.0);
        return mb >= 1000.0 ? juce::String (mb / 1024.0, 1) + " GB" : juce::String (juce::roundToInt (mb)) + " MB";
    }

    /** A small outlined tag (PRESET, SA) drawn right to left from `right`.
        Returns where the next one should end. */
    float drawBadge (juce::Graphics& g, const juce::String& text, float right, float centreY, juce::Colour colour)
    {
        const auto font = theme::label (10.5f, theme::Weight::semibold);
        const auto width = theme::getTrackedTextWidth (text, font, 0.08f) + 10.0f;
        const juce::Rectangle<float> r (right - width, centreY - 8.0f, width, 16.0f);

        g.setColour (colour.withAlpha (0.12f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (colour.withAlpha (0.7f));
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        g.setColour (colour);
        theme::drawTrackedText (g, text, font, 0.08f, r, juce::Justification::centred);
        return r.getX() - 5.0f;
    }

    juce::String subtitleFor (const kitlibrary::Entry& e, const kitlibrary::Pack* pack)
    {
        if (e.summary.isPreset)
            return "Preset of " + e.credits().basedOn;

        return pack != nullptr ? pack->name : juce::String ("Kit");
    }
}

//==============================================================================
/** The content packs down the left, with "All kits" on top. */
class KitBrowser::PackList final : public juce::Component
{
public:
    std::function<void (int)> onSelect;

    void setRows (const kitlibrary::Library& library)
    {
        rows.clear();
        rows.push_back ({ allPacks, "All kits", (int) library.kits.size(), false });

        for (size_t p = 0; p < library.packs.size(); ++p)
        {
            const auto n = (int) std::count_if (library.kits.begin(), library.kits.end(), [p] (const auto& k) { return k.pack == (int) p; });
            rows.push_back ({ (int) p, library.packs[p].name, n, library.packs[p].shareAlike });
        }

        if (const auto loose = (int) std::count_if (library.kits.begin(), library.kits.end(), [] (const auto& k) { return k.pack < 0; }); loose > 0)
            rows.push_back ({ loosePacks, "Other kits", loose, false });

        repaint();
    }

    void setSelected (int filter)
    {
        selectedFilter = filter;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        g.setColour (colours::textMuted);
        drawTrackedText (g, "CONTENT PACKS", label (12.0f), 0.1f, { 4.0f, 0.0f, (float) getWidth(), 20.0f },
                         juce::Justification::centredLeft);

        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            const auto r = rowBounds ((int) i).toFloat();
            const auto isSelected = row.filter == selectedFilter;

            if (isSelected || (int) i == hoverRow)
            {
                g.setColour (isSelected ? colours::raisedHigh : colours::raised);
                g.fillRoundedRectangle (r, cornerRadius - 2.0f);
            }

            if (isSelected)
            {
                g.setColour (colours::accent);
                g.fillRoundedRectangle (r.withWidth (3.0f).reduced (0.0f, 7.0f), 1.5f);
            }

            auto text = r.reduced (12.0f, 0.0f);
            g.setColour (colours::textMuted);
            g.setFont (mono (12.0f));
            g.drawText (juce::String (row.count), text.removeFromRight (28.0f), juce::Justification::centredRight, false);

            if (row.shareAlike)
                text.removeFromRight (drawBadge (g, "SA", text.getRight(), text.getCentreY(), colours::groupCymbals) > 0 ? 30.0f : 0.0f);

            g.setColour (isSelected ? colours::text : colours::text.withAlpha (0.85f));
            g.setFont (body (14.0f, isSelected ? Weight::semibold : Weight::regular));
            g.drawText (row.name, text, juce::Justification::centredLeft, true);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override       { setHover (rowAt (e.y)); }
    void mouseExit (const juce::MouseEvent&) override         { setHover (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (const auto i = rowAt (e.y); i >= 0 && onSelect)
            onSelect (rows[(size_t) i].filter);
    }

private:
    struct Row
    {
        int filter;
        juce::String name;
        int count;
        bool shareAlike;
    };

    static constexpr int top = 26, rowHeight = 34;

    juce::Rectangle<int> rowBounds (int i) const    { return { 0, top + i * rowHeight, getWidth(), rowHeight - 2 }; }
    int rowAt (int y) const                         { const auto i = (y - top) / rowHeight; return y >= top && i < (int) rows.size() ? i : -1; }

    void setHover (int i)
    {
        if (i != hoverRow)
        {
            hoverRow = i;
            repaint();
        }
    }

    std::vector<Row> rows;
    int selectedFilter = allPacks;
    int hoverRow = -1;
};

//==============================================================================
/** The cards' container, which also draws a heading above each pack's cards. */
class KitBrowser::Grid final : public juce::Component
{
public:
    struct Heading
    {
        juce::Rectangle<int> area;
        juce::String text, licence;
    };

    std::vector<Heading> headings;

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        for (const auto& h : headings)
        {
            auto r = h.area.toFloat();
            g.setColour (colours::textMuted);
            const auto font = label (13.0f);
            drawTrackedText (g, h.text.toUpperCase(), font, 0.1f, r, juce::Justification::centredLeft);

            const auto w = getTrackedTextWidth (h.text.toUpperCase(), font, 0.1f);
            g.setColour (colours::textMuted.withAlpha (0.7f));
            g.setFont (body (12.0f));
            g.drawText (h.licence, r.withTrimmedLeft (w + 10.0f), juce::Justification::centredLeft, true);

            g.setColour (colours::border);
            g.fillRect (r.withTrimmedLeft (w + 18.0f + theme::getTrackedTextWidth (h.licence, body (12.0f), 0.0f)).withHeight (1.0f)
                            .withY (r.getCentreY()));
        }
    }
};

//==============================================================================
/** One kit or preset. */
class KitBrowser::KitCard final : public juce::Component, public juce::SettableTooltipClient
{
public:
    KitCard (const kitlibrary::Entry& e, const kitlibrary::Pack* p, int i) : entry (e), pack (p), index (i)
    {
        setRepaintsOnMouseActivity (true);
        setTooltip (e.folder.getFullPathName());
    }

    std::function<void (int)> onClick, onDoubleClick;

    void setState (bool isSelected, bool isLoaded)
    {
        if (isSelected != selected || isLoaded != loaded)
        {
            selected = isSelected;
            loaded = isLoaded;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (selected ? colours::raisedHigh : (isMouseOver() ? colours::raised.brighter (0.03f) : colours::raised));
        g.fillRoundedRectangle (bounds, cornerRadius + 2.0f);
        g.setColour (selected ? colours::accent : (isMouseOver() ? colours::borderStrong : colours::border));
        g.drawRoundedRectangle (bounds, cornerRadius + 2.0f, selected ? 1.5f : 1.0f);

        auto area = getLocalBounds().toFloat().reduced (14.0f, 12.0f);

        // Badges, top right.
        auto top = area.removeFromTop (18.0f);
        auto right = top.getRight();

        if (entry.credits().shareAlike)
            right = drawBadge (g, "SA", right, top.getCentreY(), colours::groupCymbals);

        if (entry.summary.isPreset)
            right = drawBadge (g, "PRESET", right, top.getCentreY(), colours::groupToms);

        if (loaded)
        {
            g.setColour (colours::accent);
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ top.getX() + 3.5f, top.getCentreY() }));
            g.setFont (label (11.5f));
            drawTrackedText (g, "LOADED", label (11.5f), 0.1f, top.withTrimmedLeft (13.0f).withRight (right),
                             juce::Justification::centredLeft);
        }

        area.removeFromTop (6.0f);
        g.setColour (colours::text);
        g.setFont (body (16.0f, Weight::semibold));
        g.drawText (entry.credits().name, area.removeFromTop (22.0f), juce::Justification::centredLeft, true);

        g.setColour (colours::textMuted);
        g.setFont (body (12.5f));
        g.drawText (subtitleFor (entry, pack), area.removeFromTop (18.0f), juce::Justification::centredLeft, true);

        // Sizes, at the bottom.
        const auto& s = entry.summary;
        juce::String sizes;
        sizes << s.numPieces << " pcs   " << s.numMics << " mics   " << formatBytes (s.ramBytes);
        g.setColour (colours::text.withAlpha (0.75f));
        g.setFont (mono (11.5f));
        g.drawText (sizes, area.removeFromBottom (16.0f), juce::Justification::centredLeft, true);
    }

    void mouseDown (const juce::MouseEvent&) override          { if (onClick) onClick (index); }
    void mouseDoubleClick (const juce::MouseEvent&) override   { if (onDoubleClick) onDoubleClick (index); }

    const kitlibrary::Entry& entry;

private:
    const kitlibrary::Pack* pack;
    const int index;
    bool selected = false, loaded = false;
};

//==============================================================================
/** The selected kit: what it is, its sizes, credits and licence, and Load. */
class KitBrowser::Details final : public juce::Component
{
public:
    Details()
    {
        for (auto* link : { &sourceLink, &licenseLink })
        {
            link->setFont (theme::body (13.0f), false, juce::Justification::centredLeft);
            link->setColour (juce::HyperlinkButton::textColourId, theme::colours::accent);
            addChildComponent (*link);
        }

        loadButton.setColour (juce::TextButton::buttonColourId, theme::colours::accent);
        loadButton.setColour (juce::TextButton::textColourOffId, theme::colours::background);
        loadButton.onClick = [this] { if (onLoad) onLoad(); };
        addChildComponent (loadButton);
    }

    std::function<void()> onLoad;

    void show (const kitlibrary::Entry* e, const kitlibrary::Pack* p, bool isLoaded)
    {
        entry = e;
        pack = p;
        loaded = isLoaded;

        const auto has = entry != nullptr;
        loadButton.setVisible (has);
        loadButton.setEnabled (has && ! loaded);
        loadButton.setButtonText (loaded ? "Loaded" : "Load kit");

        if (has)
        {
            const auto& c = entry->credits();
            sourceLink.setButtonText ("Source");
            sourceLink.setURL (juce::URL (c.sourceUrl));
            sourceLink.setTooltip (c.sourceUrl);
            sourceLink.setVisible (c.sourceUrl.isNotEmpty());
            licenseLink.setButtonText (c.licenseName.isNotEmpty() ? c.licenseName : juce::String ("Licence"));
            licenseLink.setURL (juce::URL (c.licenseUrl));
            licenseLink.setTooltip (c.licenseUrl);
            licenseLink.setVisible (c.licenseUrl.isNotEmpty());
        }
        else
        {
            sourceLink.setVisible (false);
            licenseLink.setVisible (false);
        }

        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        g.setColour (colours::raised);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius + 2.0f);
        g.setColour (colours::border);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), cornerRadius + 2.0f, 1.0f);

        if (entry == nullptr)
            return;

        const auto& c = entry->credits();
        const auto& s = entry->summary;
        auto area = getLocalBounds().reduced (18, 16);

        g.setColour (colours::text);
        g.setFont (body (20.0f, Weight::semibold));
        g.drawFittedText (c.name, area.removeFromTop (28), juce::Justification::centredLeft, 1, 0.8f);

        g.setColour (colours::textMuted);
        g.setFont (body (13.0f));
        // A middle dot, spelled out in UTF-8: a plain literal would be read as Latin-1.
        const juce::String dot (juce::CharPointer_UTF8 ("  \xc2\xb7  "));
        g.drawText (subtitleFor (*entry, pack) + (s.isPreset && pack != nullptr ? dot + pack->name : juce::String()),
                    area.removeFromTop (20), juce::Justification::centredLeft, true);

        // Readouts, as in the header.
        area.removeFromTop (12);
        auto readouts = area.removeFromTop (36);
        const std::pair<juce::String, juce::String> values[] {
            { "PIECES", juce::String (s.numPieces) }, { "MICS", juce::String (s.numMics) }, { "RAM", formatBytes (s.ramBytes) },
        };

        const auto readoutWidth = readouts.getWidth() / 3;

        for (const auto& [name, value] : values)
        {
            auto r = readouts.removeFromLeft (readoutWidth).toFloat();
            g.setColour (colours::textMuted);
            drawTrackedText (g, name, label (11.0f), 0.12f, r.removeFromTop (14.0f), juce::Justification::centredLeft);
            g.setColour (colours::text);
            g.setFont (mono (15.0f, Weight::medium));
            g.drawText (value, r, juce::Justification::centredLeft, false);
        }

        area.removeFromTop (14);
        g.setColour (colours::text.withAlpha (0.9f));
        g.setFont (body (13.5f));
        const auto description = c.description.isNotEmpty() ? c.description : juce::String ("No description.");
        g.drawFittedText (description, area.removeFromTop (58), juce::Justification::topLeft, 3, 1.0f);

        if (! s.tags.isEmpty())
        {
            auto tagRow = area.removeFromTop (22).toFloat();
            const auto font = body (12.0f);

            for (const auto& tag : s.tags)
            {
                const auto w = getTrackedTextWidth (tag, font, 0.0f) + 14.0f;

                if (w > tagRow.getWidth())
                    break;

                const auto r = tagRow.removeFromLeft (w).reduced (0.0f, 2.0f);
                tagRow.removeFromLeft (6.0f);
                g.setColour (colours::raisedHigh);
                g.fillRoundedRectangle (r, 9.0f);
                g.setColour (colours::textMuted);
                g.setFont (font);
                g.drawText (tag, r, juce::Justification::centred, false);
            }
        }

        // Credits: what CC licences ask users to give.
        area.removeFromTop (16);
        g.setColour (colours::textMuted);
        drawTrackedText (g, "CREDITS", label (12.0f), 0.1f, area.removeFromTop (18).toFloat(), juce::Justification::centredLeft);
        g.setColour (colours::text);
        g.setFont (body (13.0f, Weight::medium));
        g.drawFittedText (c.author.isNotEmpty() ? c.author : juce::String ("Unknown author"), area.removeFromTop (20),
                          juce::Justification::centredLeft, 1, 0.85f);
        g.setColour (colours::text.withAlpha (0.8f));
        g.setFont (body (12.5f));
        g.drawFittedText (c.credits, area.removeFromTop (54), juce::Justification::topLeft, 3, 1.0f);

        if (c.shareAlike)
        {
            area.removeFromTop (4);
            g.setColour (colours::groupCymbals.withAlpha (0.9f));
            g.setFont (body (12.0f));
            g.drawFittedText ("Share-alike: if you pass this kit on, it must keep this licence, as a content pack of its own. "
                              "Music made with it isn't affected.",
                              area.removeFromTop (44), juce::Justification::topLeft, 3, 1.0f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (18, 16);
        loadButton.setBounds (area.removeFromBottom (36));
        area.removeFromBottom (10);
        auto links = area.removeFromBottom (22);
        licenseLink.setBounds (links.removeFromLeft (130));
        links.removeFromLeft (10);
        sourceLink.setBounds (links.removeFromLeft (80));
    }

private:
    const kitlibrary::Entry* entry = nullptr;
    const kitlibrary::Pack* pack = nullptr;
    bool loaded = false;
    juce::HyperlinkButton sourceLink, licenseLink;
    juce::TextButton loadButton { "Load kit" };
};

//==============================================================================
KitBrowser::KitBrowser (LithiumDrumsProcessor& p)
    : OverlayPanel ("Kits", 1220, 720),
      processor (p),
      packList (std::make_unique<PackList>()),
      grid (std::make_unique<Grid>()),
      details (std::make_unique<Details>())
{
    search.setTextToShowWhenEmpty ("Search kits, packs, tags...", theme::colours::textMuted);
    search.setFont (theme::body (14.0f));
    search.setIndents (10, 7);
    search.onTextChange = [this] { applyFilter(); };
    search.onReturnKey = [this] { load (selected); };
    search.onEscapeKey = [this] { if (onClose) onClose(); };
    addAndMakeVisible (search);

    for (auto [button, filter] : { std::pair { &allButton, TypeFilter::all }, std::pair { &kitsButton, TypeFilter::kits },
                                   std::pair { &presetsButton, TypeFilter::presets } })
    {
        button->setClickingTogglesState (true);
        button->setRadioGroupId (1);
        button->onClick = [this, b = button, f = filter]
        {
            if (b->getToggleState())
            {
                typeFilter = f;
                applyFilter();
            }
        };
        addAndMakeVisible (*button);
    }

    allButton.setToggleState (true, juce::dontSendNotification);

    packList->onSelect = [this] (int filter)
    {
        packFilter = filter;
        packList->setSelected (filter);
        applyFilter();
    };
    addAndMakeVisible (*packList);

    gridView.setViewedComponent (grid.get(), false);
    gridView.setScrollBarsShown (true, false);
    gridView.setScrollBarThickness (8);
    gridView.getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, theme::colours::borderStrong);
    addAndMakeVisible (gridView);

    details->onLoad = [this] { load (selected); };
    addAndMakeVisible (*details);

    processor.addChangeListener (this);
}

KitBrowser::~KitBrowser()
{
    processor.removeChangeListener (this);
}

void KitBrowser::refresh()
{
    // Cards refer to entries in `library`, so they go before it's replaced.
    cards.clear();
    library = kitlibrary::scan (processor.getKitsFolder());

    if (packFilter >= (int) library.packs.size())
        packFilter = allPacks;

    packList->setRows (library);
    packList->setSelected (packFilter);

    for (size_t i = 0; i < library.kits.size(); ++i)
    {
        auto card = std::make_unique<KitCard> (library.kits[i], library.packOf (library.kits[i]), (int) i);
        card->onClick = [this] (int index) { select (index); };
        card->onDoubleClick = [this] (int index) { load (index); };
        grid->addChildComponent (*card);
        cards.push_back (std::move (card));
    }

    selected = -1;
    const auto loadedFolder = processor.getKitFolder();

    for (size_t i = 0; i < library.kits.size(); ++i)
        if (library.kits[i].folder == loadedFolder)
            selected = (int) i;

    applyFilter();
    search.grabKeyboardFocus();
}

bool KitBrowser::passesFilter (const kitlibrary::Entry& e) const
{
    if (packFilter != allPacks && e.pack != packFilter)
        return false;

    if ((typeFilter == TypeFilter::kits && e.summary.isPreset) || (typeFilter == TypeFilter::presets && ! e.summary.isPreset))
        return false;

    // Every word of the search must appear somewhere in what the card shows.
    const auto* pack = library.packOf (e);
    const auto& c = e.credits();
    const auto haystack = c.name + " " + c.basedOn + " " + c.author + " " + c.description + " " + c.licenseName + " "
                          + e.summary.tags.joinIntoString (" ") + " " + (pack != nullptr ? pack->name : juce::String())
                          + (e.summary.isPreset ? " preset" : " kit");

    for (const auto& word : juce::StringArray::fromTokens (search.getText(), true))
        if (! haystack.containsIgnoreCase (word))
            return false;

    return true;
}

void KitBrowser::applyFilter()
{
    for (auto& card : cards)
        card->setVisible (passesFilter (card->entry));

    if (selected < 0 || ! cards[(size_t) selected]->isVisible())
    {
        const auto first = std::find_if (cards.begin(), cards.end(), [] (const auto& c) { return c->isVisible(); });
        selected = first != cards.end() ? (int) (first - cards.begin()) : -1;
    }

    select (selected);
    layoutGrid();
}

void KitBrowser::select (int entry)
{
    selected = entry;
    const auto loadedFolder = processor.getKitFolder();

    for (size_t i = 0; i < cards.size(); ++i)
        cards[i]->setState ((int) i == selected, cards[i]->entry.folder == loadedFolder);

    const auto* e = juce::isPositiveAndBelow (selected, (int) library.kits.size()) ? &library.kits[(size_t) selected] : nullptr;
    details->show (e, e != nullptr ? library.packOf (*e) : nullptr, e != nullptr && e->folder == loadedFolder);

    if (e != nullptr && cards[(size_t) selected]->isVisible())
    {
        // Keep the selected card in view (arrow keys can move off screen).
        const auto r = cards[(size_t) selected]->getBounds();
        auto view = gridView.getViewArea();

        if (r.getY() < view.getY())
            gridView.setViewPosition (0, std::max (0, r.getY() - sectionHeight));
        else if (r.getBottom() > view.getBottom())
            gridView.setViewPosition (0, r.getBottom() - view.getHeight());
    }
}

void KitBrowser::moveSelection (int delta)
{
    std::vector<int> visible;

    for (size_t i = 0; i < cards.size(); ++i)
        if (cards[i]->isVisible())
            visible.push_back ((int) i);

    if (visible.empty())
        return;

    const auto at = std::find (visible.begin(), visible.end(), selected);
    const auto position = at == visible.end() ? 0 : (int) (at - visible.begin());
    select (visible[(size_t) juce::jlimit (0, (int) visible.size() - 1, position + delta)]);
}

void KitBrowser::load (int entry)
{
    if (! juce::isPositiveAndBelow (entry, (int) library.kits.size()))
        return;

    // Choosing a kit here starts it with neutral settings, as from the menu.
    processor.loadKit (library.kits[(size_t) entry].folder, true);

    if (onClose)
        onClose();
}

void KitBrowser::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (isVisible())
        select (selected);   // the loaded kit may have changed
}

bool KitBrowser::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey)              { load (selected); return true; }
    if (key == juce::KeyPress::leftKey)                { moveSelection (-1); return true; }
    if (key == juce::KeyPress::rightKey)               { moveSelection (1); return true; }
    if (key == juce::KeyPress::upKey)                  { moveSelection (-columns); return true; }
    if (key == juce::KeyPress::downKey)                { moveSelection (columns); return true; }

    return OverlayPanel::keyPressed (key);
}

//==============================================================================
void KitBrowser::paint (juce::Graphics& g)
{
    OverlayPanel::paint (g);

    if (! emptyMessageArea.isEmpty())
    {
        g.setColour (theme::colours::textMuted);
        g.setFont (theme::body (14.0f));
        g.drawFittedText (library.kits.empty()
                              ? "No kits are installed yet. The README explains where to download kits and how to import "
                                "them with LithiumImport; they'll show up here."
                              : "No kits match.",
                          emptyMessageArea, juce::Justification::centred, 3, 1.0f);
    }
}

void KitBrowser::layoutContent (juce::Rectangle<int> area)
{
    // The search and the filter sit in the title row, to the right of the title.
    auto titleRow = juce::Rectangle<int> (area.getX(), area.getY() - 48, area.getWidth(), 32);
    auto filters = titleRow.removeFromRight (detailsWidth);

    for (auto* b : { &presetsButton, &kitsButton, &allButton })
    {
        b->setBounds (filters.removeFromRight (84));
        filters.removeFromRight (6);
    }

    titleRow.removeFromRight (columnGap);
    search.setBounds (titleRow.removeFromRight (320));

    packList->setBounds (area.removeFromLeft (packListWidth));
    area.removeFromLeft (columnGap);
    details->setBounds (area.removeFromRight (detailsWidth));
    area.removeFromRight (columnGap);
    gridView.setBounds (area);

    layoutGrid();
}

void KitBrowser::layoutGrid()
{
    const auto width = gridView.getWidth() - gridView.getScrollBarThickness() - 4;

    if (width <= 0)
        return;

    columns = std::max (1, (width + tileGap) / (minTileWidth + tileGap));
    const auto tileWidth = (width - (columns - 1) * tileGap) / columns;

    // In the "All kits" view, each pack's cards start on a row of their own
    // under a heading naming the pack and its licence.
    grid->headings.clear();
    int x = 0, y = 0, column = 0, lastPack = std::numeric_limits<int>::min();
    bool any = false;

    for (const auto& card : cards)
    {
        if (! card->isVisible())
            continue;

        const auto pack = card->entry.pack;

        if (packFilter == allPacks && pack != lastPack)
        {
            if (any)
                y += tileHeight + tileGap + 6;

            const auto* p = library.packOf (card->entry);
            grid->headings.push_back ({ { 0, y, width, sectionHeight - 6 },
                                        p != nullptr ? p->name : juce::String ("Other kits"),
                                        p != nullptr ? p->licenseName : juce::String() });
            y += sectionHeight;
            column = 0;
            lastPack = pack;
        }
        else if (column == columns)
        {
            column = 0;
            y += tileHeight + tileGap;
        }

        x = column * (tileWidth + tileGap);
        card->setBounds (x, y, tileWidth, tileHeight);
        ++column;
        any = true;
    }

    grid->setSize (width, any ? y + tileHeight : 0);
    grid->repaint();

    emptyMessageArea = any ? juce::Rectangle<int>() : gridView.getBounds().reduced (40);
    repaint (gridView.getBounds());
}

} // namespace lithium
