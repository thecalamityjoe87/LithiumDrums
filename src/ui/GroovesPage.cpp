#include "GroovesPage.h"

#include "Settings.h"
#include "Theme.h"
#include "grooves/GrooveMidi.h"

#include <map>

namespace lithium
{

namespace
{
    constexpr int margin = 16;
    constexpr int chipRowHeight = 32;
    constexpr int leftWidth = 330;
    constexpr int transportHeight = 60;
    constexpr int grooveRowHeight = 46;
    constexpr int cardHeight = 128;
    constexpr int cardGap = 10;
    constexpr int minCardWidth = 200;
    constexpr int sectionHeight = 30;
    constexpr int detailHeight = 212;
    constexpr double minTempo = 40.0, maxTempo = 240.0;

    // Settings kept with the project (see LithiumDrumsProcessor::getGrooveSetting).
    const juce::Identifier settingPack { "pack" }, settingGroove { "groove" }, settingPart { "part" }, settingStyle { "style" };

    const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));   // a middle dot, in UTF-8

    //==============================================================================
    // What the cards and the detail pane show of a clip.

    /** The detail pane's lanes, top to bottom, and the kinds of piece in each. */
    enum class Lane { crash, ride, hihat, snare, toms, kick, other };
    constexpr std::array allLanes { Lane::crash, Lane::ride, Lane::hihat, Lane::snare, Lane::toms, Lane::kick, Lane::other };

    Lane laneOf (std::optional<PieceRole> role)
    {
        switch (role.value_or (PieceRole::other))
        {
            case PieceRole::crash:
            case PieceRole::china:    return Lane::crash;
            case PieceRole::ride:     return Lane::ride;
            case PieceRole::hihat:    return Lane::hihat;
            case PieceRole::snare:    return Lane::snare;
            case PieceRole::tom:
            case PieceRole::floorTom: return Lane::toms;
            case PieceRole::kick:     return Lane::kick;
            case PieceRole::other:    break;
        }

        return Lane::other;
    }

    const char* laneName (Lane l)
    {
        switch (l)
        {
            case Lane::crash: return "Crash";
            case Lane::ride:  return "Ride";
            case Lane::hihat: return "Hi-hat";
            case Lane::snare: return "Snare";
            case Lane::toms:  return "Toms";
            case Lane::kick:  return "Kick";
            case Lane::other: break;
        }

        return "Other";
    }

    juce::Colour laneColour (Lane l)
    {
        using namespace theme::colours;

        switch (l)
        {
            case Lane::crash:
            case Lane::ride:  return groupCymbals;
            case Lane::hihat: return groupHats;
            case Lane::snare: return groupSnare;
            case Lane::toms:  return groupToms;
            case Lane::kick:  return groupKick;
            case Lane::other: break;
        }

        return textMuted;
    }

    using RoleTable = std::array<std::optional<PieceRole>, 128>;

    /** A clip's hits, from `start` (in beats) for `bars` bars, with beats counted from its start. */
    std::vector<grooves::Hit> hitsOf (const std::vector<grooves::Hit>& hits, double start, int bars, double beatsPerBar)
    {
        std::vector<grooves::Hit> result;
        const auto length = bars * beatsPerBar;

        for (auto h : hits)
        {
            h.beat -= start;

            if (h.beat >= -grooves::earlyHitBeats && h.beat < length && juce::isPositiveAndBelow (h.note, 128))
            {
                h.beat = std::max (0.0, h.beat);
                result.push_back (h);
            }
        }

        return result;
    }

    /** What a card shows of a clip. */
    struct Summary
    {
        juce::String feel;           // "Straight", "Shuffle" or "Half-time"
        int intensity = 1;           // 1 (sparse) to 3 (busy)
        int steps = 16;              // per bar
        // Bar 1 on three rows (kick, snare, hats or ride): the loudest hit on each step, 0 for none.
        std::array<std::vector<std::uint8_t>, 3> preview;
    };

    Summary summarise (const std::vector<grooves::Hit>& hits, int bars, const grooves::Groove& groove, const RoleTable& roles)
    {
        Summary s;
        const auto beatsPerBar = groove.beatsPerBar();
        s.steps = std::max (4, juce::roundToInt (beatsPerBar * 4.0));

        for (auto& row : s.preview)
            row.assign ((size_t) s.steps, 0);

        int counted = 0, offbeats = 0, strongOnTwoAndFour = 0, strongOnThree = 0;
        double offbeatSum = 0.0;

        for (const auto& h : hits)
        {
            const auto role = roles[(size_t) h.note];
            const auto lane = laneOf (role);

            if (h.velocity >= 30)
                ++counted;

            // Bar 1, on the step grid. Toms, cymbal crashes and ghost notes
            // stay out of the card's preview (the detail pane has them).
            if (h.beat < beatsPerBar)
            {
                const auto step = juce::jlimit (0, s.steps - 1, juce::roundToInt (h.beat * 4.0));
                const auto row = lane == Lane::kick ? 0 : lane == Lane::snare ? 1 : (lane == Lane::hihat || lane == Lane::ride) ? 2 : -1;

                if (row >= 0 && ! (row == 1 && h.velocity < 40))
                {
                    auto& v = s.preview[(size_t) row][(size_t) step];
                    v = (std::uint8_t) std::max ((int) v, h.velocity);
                }
            }

            // Feel: where the off-beat eighths fall, and where the backbeat is.
            const auto inBeat = h.beat - std::floor (h.beat);

            if (inBeat > 0.35 && inBeat < 0.8 && lane != Lane::other)
            {
                ++offbeats;
                offbeatSum += inBeat;
            }

            if (lane == Lane::snare && h.velocity >= 60)
            {
                const auto inBar = std::fmod (h.beat, beatsPerBar);

                if (std::abs (inBar - 1.0) < 0.15 || std::abs (inBar - 3.0) < 0.15)
                    ++strongOnTwoAndFour;
                else if (std::abs (inBar - 2.0) < 0.15)
                    ++strongOnThree;
            }
        }

        const auto perBar = (double) counted / std::max (1, bars);
        s.intensity = perBar <= 14.0 ? 1 : perBar <= 24.0 ? 2 : 3;

        const auto fourFour = groove.numerator == 4 && groove.denominator == 4;

        if (offbeats >= 2 * bars && offbeatSum / offbeats >= 0.6)
            s.feel = "Shuffle";
        else if (fourFour && strongOnThree >= bars * 0.6 && strongOnTwoAndFour <= strongOnThree * 0.3)
            s.feel = "Half-time";
        else
            s.feel = "Straight";

        return s;
    }

    /** A phrase tile's title: "Bars 5-8". */
    juce::String phraseTitle (juce::Range<int> bars)
    {
        return "Bars " + juce::String (bars.getStart() + 1) + "-" + juce::String (bars.getEnd());
    }

    juce::String encode (GroovesPage::Part p)
    {
        switch (p.kind)
        {
            case GroovesPage::Part::Kind::whole:  return "whole";
            case GroovesPage::Part::Kind::fill:   return "fill:" + juce::String (p.index);
            case GroovesPage::Part::Kind::phrase: break;
        }

        return "phrase:" + juce::String (p.index);
    }

    GroovesPage::Part decode (const juce::String& s)
    {
        using Kind = GroovesPage::Part::Kind;
        const auto index = std::max (0, s.fromFirstOccurrenceOf (":", false, false).getIntValue());

        if (s == "whole")           return { Kind::whole, 0 };
        if (s.startsWith ("fill"))  return { Kind::fill, index };
        return { Kind::phrase, index };
    }

    void styleToggle (juce::TextButton& b, int radioGroup)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (radioGroup);
    }
}

//==============================================================================
/** The styles across the top: All, then each style with its number of
    grooves, most first. Those that don't fit are under "More". */
class GroovesPage::StyleChips final : public juce::Component
{
public:
    explicit StyleChips (GroovesPage& p) : page (p)   { setRepaintsOnMouseActivity (true); }

    /** Back to "All" (after an import, so the new pack shows), or one style. */
    void showAll()                                  { choose ({}); }
    void showStyle (const juce::String& key)        { choose (key); }

    void setStyles (const grooves::Library& library)
    {
        std::map<juce::String, std::pair<juce::String, int>> counts;   // key -> name, count
        int total = 0;

        for (const auto& pack : library.packs)
        {
            for (const auto& g : pack.grooves)
            {
                auto& c = counts[g.style];
                c.first = g.styleName;
                ++c.second;
                ++total;
            }
        }

        chips.clear();
        chips.push_back ({ {}, "All", total, {} });

        for (const auto& [key, c] : counts)
            chips.push_back ({ key, c.first, c.second, {} });

        std::stable_sort (chips.begin() + 1, chips.end(), [] (const Chip& a, const Chip& b) { return a.count > b.count; });
        resized();
        repaint();
    }

    /** Lays the chips out in up to `available` pixels, and returns how many
        they take: the chips that fit, then "More" (only as wide as its
        label) for the rest. The page gives what's left to the search box,
        so there's no gap after the last chip. */
    int layOut (int available)
    {
        auto x = 0.0f;
        visibleCount = 0;
        const auto moreWidth = moreChipWidth();

        for (size_t i = 0; i < chips.size(); ++i)
        {
            const auto w = chipWidth (chips[i]);
            const auto roomNeeded = i + 1 < chips.size() ? moreWidth + gap : 0.0f;

            if (x + w + roomNeeded > (float) available)
                break;

            chips[i].bounds = { x, 0.0f, w, (float) getHeight() };
            x += w + gap;
            ++visibleCount;
        }

        if (visibleCount < (int) chips.size())
        {
            moreBounds = { x, 0.0f, moreWidth, (float) getHeight() };
            x += moreWidth + gap;
        }
        else
        {
            moreBounds = {};
        }

        return juce::roundToInt (std::max (0.0f, x - gap));
    }

    void resized() override    { layOut (getWidth()); }

    void paint (juce::Graphics& g) override
    {
        const auto mouse = getMouseXYRelative().toFloat();
        const auto hovering = isMouseOver();

        for (int i = 0; i < visibleCount; ++i)
        {
            const auto& c = chips[(size_t) i];
            drawChip (g, c.bounds, c.name, c.count, c.key == page.styleFilter, hovering && c.bounds.contains (mouse));
        }

        if (! moreBounds.isEmpty())
            drawChip (g, moreBounds, moreLabel(), -1, isHiddenStyleChosen(), hovering && moreBounds.contains (mouse));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto pos = e.position;

        for (int i = 0; i < visibleCount; ++i)
            if (chips[(size_t) i].bounds.contains (pos))
                return choose (chips[(size_t) i].key);

        if (moreBounds.contains (pos))
        {
            juce::PopupMenu menu;

            for (size_t i = (size_t) visibleCount; i < chips.size(); ++i)
                menu.addItem ((int) i + 1, chips[i].name + "  (" + juce::String (chips[i].count) + ")", true,
                              chips[i].key == page.styleFilter);

            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (
                                    localAreaToGlobal (moreBounds.toNearestInt())),
                                [safe = juce::Component::SafePointer (this)] (int result)
                                {
                                    if (safe != nullptr && result > 0)
                                        safe->choose (safe->chips[(size_t) result - 1].key);
                                });
        }
    }

private:
    struct Chip
    {
        juce::String key, name;
        int count;
        juce::Rectangle<float> bounds;
    };

    static constexpr float gap = 6.0f;

    void choose (const juce::String& key)
    {
        page.styleFilter = key;
        page.processor.setGrooveSetting (settingStyle, key);
        page.applyFilter();
        page.resized();   // "More" may now name the style, and be wider
        repaint();
    }

    /** "More", or the style chosen from it. */
    juce::String moreLabel() const
    {
        juce::String name ("More");

        for (size_t i = (size_t) visibleCount; i < chips.size(); ++i)
            if (chips[i].key == page.styleFilter && page.styleFilter.isNotEmpty())
                name = chips[i].name;

        return name + juce::String (juce::CharPointer_UTF8 (" \xe2\x96\xbe"));
    }

    bool isHiddenStyleChosen() const
    {
        for (size_t i = (size_t) visibleCount; i < chips.size(); ++i)
            if (chips[i].key == page.styleFilter && page.styleFilter.isNotEmpty())
                return true;

        return false;
    }

    float moreChipWidth() const
    {
        // As wide as the longest label it could show, so choosing a style from it doesn't reflow the row.
        const juce::String arrow (juce::CharPointer_UTF8 (" \xe2\x96\xbe"));
        const auto font = theme::body (13.5f, theme::Weight::medium);
        auto widest = juce::GlyphArrangement::getStringWidth (font, "More" + arrow);

        for (const auto& c : chips)
            if (c.key == page.styleFilter && c.key.isNotEmpty())
                widest = std::max (widest, juce::GlyphArrangement::getStringWidth (font, c.name + arrow));

        return widest + 32.0f;
    }

    static float chipWidth (const Chip& c)
    {
        return styleChipWidth (c.name, c.count);
    }

    static void drawChip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& name, int count, bool on, bool hover)
    {
        drawStyleChip (g, r, name, count, on, hover);
    }

    GroovesPage& page;
    std::vector<Chip> chips;
    int visibleCount = 0;
    juce::Rectangle<float> moreBounds;
};

//==============================================================================
/** The grooves that pass the filters, one row each. */
class GroovesPage::GrooveList final : public juce::ListBox, private juce::ListBoxModel
{
public:
    explicit GrooveList (GroovesPage& p) : page (p)
    {
        setModel (this);
        setRowHeight (grooveRowHeight);
        setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, theme::colours::borderStrong);
        getViewport()->setScrollBarThickness (8);
    }

    int getNumRows() override    { return (int) page.filtered.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
    {
        using namespace theme;

        if (! juce::isPositiveAndBelow (row, (int) page.filtered.size()))
            return;

        const auto ref = page.filtered[(size_t) row];
        const auto& pack = page.library.packs[(size_t) ref.pack];
        const auto& groove = pack.grooves[(size_t) ref.groove];
        const auto isSelected = ref.pack == page.selected.pack && ref.groove == page.selected.groove;
        const auto isPlaying = page.processor.isGroovePlaying() && ref.pack == page.playingGroove.pack
                                && ref.groove == page.playingGroove.groove;
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (2.0f, 2.0f);

        if (isSelected)
        {
            g.setColour (colours::raisedHigh);
            g.fillRoundedRectangle (r, cornerRadius - 2.0f);
            g.setColour (colours::accent);
            g.fillRoundedRectangle (r.withWidth (3.0f).reduced (0.0f, 9.0f), 1.5f);
        }

        auto text = r.reduced (14.0f, 5.0f);
        auto right = text.removeFromRight (64.0f);

        g.setColour (colours::text);
        g.setFont (mono (13.0f, Weight::medium));
        g.drawText (juce::String (juce::roundToInt (groove.bpm)) + " BPM", right.removeFromTop (right.getHeight() * 0.5f),
                    juce::Justification::centredRight, false);
        g.setColour (colours::textMuted);
        g.setFont (mono (11.0f));
        g.drawText (juce::String (groove.bars) + " bars", right, juce::Justification::centredRight, false);

        auto top = text.removeFromTop (text.getHeight() * 0.5f);

        if (isPlaying)
        {
            g.setColour (colours::accent);
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ top.getX() + 3.5f, top.getCentreY() }));
            top.removeFromLeft (14.0f);
        }

        g.setColour (isSelected ? colours::text : colours::text.withAlpha (0.9f));
        g.setFont (body (14.5f, Weight::semibold));
        g.drawText (groove.name, top, juce::Justification::centredLeft, true);

        auto sub = groove.styleName;

        if (groove.drummer.isNotEmpty())   // imported MIDI has none
            sub << dot << groove.drummer;

        if (! groove.fills.empty())
            sub << dot << (int) groove.fills.size() << (groove.fills.size() == 1 ? " fill" : " fills");

        if (groove.meter() != "4/4")
            sub << dot << groove.meter();

        g.setColour (colours::textMuted);
        g.setFont (body (12.5f));
        g.drawText (sub, text, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow (row, (int) page.filtered.size()))
            page.selectGroove (page.filtered[(size_t) row], true);
    }

    void selectedRowsChanged (int) override {}

private:
    GroovesPage& page;
};

//==============================================================================
/** The selected groove's cards: the whole take and its phrases, then its
    fills, all the same size. Each card has what someone browsing needs at a
    glance (its bars, the tempo and meter, the style and feel, how busy it
    is) and a short preview of its first bar on three rows: kick, snare and
    hi-hat (or ride). The whole of the selected card is in the detail pane
    below. One component draws every card. */
class GroovesPage::TileGrid final : public juce::Component, public juce::TooltipClient
{
public:
    explicit TileGrid (GroovesPage& p) : page (p)   { setRepaintsOnMouseActivity (true); }

    /** Rebuilds the cards for the selected groove, at this width. */
    void rebuild (int width)
    {
        tiles.clear();
        headings.clear();

        const auto* pack = page.selectedPack();
        const auto* groove = page.selectedGroove();

        if (pack == nullptr || groove == nullptr)
        {
            setSize (width, 0);
            return;
        }

        const auto roles = page.roleTable();
        const auto columns = std::max (1, (width + cardGap) / (minCardWidth + cardGap));
        const auto cardWidth = (width - cardGap * (columns - 1)) / columns;
        auto y = 0;

        auto addSection = [&] (const juce::String& heading, std::vector<Tile> section)
        {
            if (section.empty())
                return;

            headings.push_back ({ { 0, y, width, sectionHeight }, heading });
            y += sectionHeight;

            for (size_t i = 0; i < section.size(); ++i)
            {
                auto t = std::move (section[i]);
                const auto column = (int) i % columns;
                t.bounds = { column * (cardWidth + cardGap), y, cardWidth, cardHeight };
                tiles.push_back (std::move (t));

                if (column == columns - 1 || i + 1 == section.size())
                    y += cardHeight + cardGap;
            }

            y += 6;
        };

        auto tile = [&] (Part part, const juce::String& title, const std::vector<grooves::Hit>& hits, double start, int bars)
        {
            return Tile { part, title, bars, summarise (hitsOf (hits, start, bars, groove->beatsPerBar()), bars, *groove, roles), {} };
        };

        const auto beatsPerBar = groove->beatsPerBar();

        if (page.beatsButton.getToggleState())
        {
            std::vector<Tile> section;
            section.push_back (tile ({ Part::Kind::whole, 0 }, "Whole take", page.takeHits, 0.0, groove->bars));

            for (int i = 0; i < grooves::numPhrases (*pack, *groove); ++i)
            {
                const auto bars = grooves::phraseBars (*pack, *groove, i);
                section.push_back (tile ({ Part::Kind::phrase, i }, phraseTitle (bars),
                                         page.takeHits, bars.getStart() * beatsPerBar, bars.getLength()));
            }

            addSection ("PHRASES" + dot + juce::String (pack->phraseBars) + " BARS EACH", std::move (section));
        }

        if (page.fillsButton.getToggleState())
        {
            std::vector<Tile> section;

            for (size_t i = 0; i < groove->fills.size() && i < page.fillHits.size(); ++i)
                section.push_back (tile ({ Part::Kind::fill, (int) i }, groove->fills[i].name, page.fillHits[i], 0.0, groove->fills[i].bars));

            addSection ("FILLS" + dot + "RECORDED WITH THIS GROOVE", std::move (section));
        }

        setSize (width, std::max (tiles.empty() ? 40 : 0, y));
        repaint();
    }

    juce::Rectangle<int> boundsOf (Part part) const
    {
        for (const auto& t : tiles)
            if (t.part == part)
                return t.bounds;

        return {};
    }

    /** Repaints the card that's playing, for its progress line. */
    void repaintPlaying()
    {
        if (const auto r = boundsOf (page.playingPart); ! r.isEmpty())
            repaint (r);

        if (lastPlayingBounds != boundsOf (page.playingPart))
            repaint (lastPlayingBounds);

        lastPlayingBounds = boundsOf (page.playingPart);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        if (tiles.empty() && page.selectedGroove() != nullptr)
        {
            g.setColour (colours::textMuted);
            g.setFont (body (type::body));
            g.drawText (page.beatsButton.getToggleState() ? "This groove has no phrases to show."
                                                          : "No fills were recorded with this groove. Switch Beats on to see its phrases.",
                        getLocalBounds().withHeight (40), juce::Justification::centredLeft, false);
        }

        for (const auto& h : headings)
        {
            g.setColour (colours::textMuted);
            drawTrackedText (g, h.text, label (12.5f), 0.1f, h.area.toFloat().withTrimmedBottom (6.0f),
                             juce::Justification::bottomLeft);
        }

        const auto* groove = page.selectedGroove();
        const auto& feedback = page.processor.getGroovePlayer().getFeedback();
        const auto playingThis = page.processor.isGroovePlaying() && feedback.clipId.load() == page.playingClipId
                                  && page.playingGroove.pack == page.selected.pack && page.playingGroove.groove == page.selected.groove;

        for (const auto& t : tiles)
        {
            if (! g.clipRegionIntersects (t.bounds) || groove == nullptr)
                continue;

            const auto progress = playingThis && t.part == page.playingPart
                                    ? feedback.position.load() / (t.bars * groove->beatsPerBar()) : -1.0;
            paintTile (g, t, *groove, t.part == page.selectedPart, isMouseOver() && t.bounds.contains (getMouseXYRelative()), progress);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressed = tileAt (e.getPosition());
        dragStarted = false;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // Dragging a card out of the window drops its MIDI into the host.
        if (pressed < 0 || dragStarted || e.getDistanceFromDragStart() < 6)
            return;

        dragStarted = true;

        if (const auto file = page.writeMidi (tiles[(size_t) pressed].part); file.existsAsFile())
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragStarted || pressed < 0 || tileAt (e.getPosition()) != pressed)
            return;

        const auto part = tiles[(size_t) pressed].part;
        const auto isPlaying = page.processor.isGroovePlaying() && part == page.playingPart
                                && page.playingGroove.pack == page.selected.pack && page.playingGroove.groove == page.selected.groove;

        // Clicking the card that's playing stops it; any other card plays.
        if (isPlaying)
            page.togglePlay();
        else
            page.selectPart (part, true);
    }

    juce::String getTooltip() override
    {
        const auto i = tileAt (getMouseXYRelative());

        if (i < 0)
            return {};

        const auto& t = tiles[(size_t) i];
        return t.title + ": " + t.summary.feel.toLowerCase() + ", " + juce::String (t.summary.intensity) + " of 3 for how busy it is.\n"
               "Click to play, drag into your host for the MIDI. The preview shows bar 1: kick, snare (without ghost notes) "
               "and hi-hat or ride.";
    }

private:
    struct Tile
    {
        Part part;
        juce::String title;
        int bars;
        Summary summary;
        juce::Rectangle<int> bounds;
    };

    struct Heading
    {
        juce::Rectangle<int> area;
        juce::String text;
    };

    int tileAt (juce::Point<int> p) const
    {
        for (size_t i = 0; i < tiles.size(); ++i)
            if (tiles[i].bounds.contains (p))
                return (int) i;

        return -1;
    }

    /** Small rounded tags ("Rock", "Shuffle"), as many as fit. */
    static void paintTags (juce::Graphics& g, juce::StringArray tags, juce::Rectangle<float> area)
    {
        using namespace theme;
        const auto font = body (11.0f, Weight::medium);

        for (const auto& tag : tags)
        {
            const auto w = juce::GlyphArrangement::getStringWidth (font, tag) + 14.0f;

            if (w > area.getWidth())
                break;

            const auto r = area.removeFromLeft (w);
            area.removeFromLeft (4.0f);
            g.setColour (colours::background.withAlpha (0.6f));
            g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            g.setColour (colours::border);
            g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.0f);
            g.setColour (colours::textMuted);
            g.setFont (font);
            g.drawText (tag, r, juce::Justification::centred, false);
        }
    }

    static void paintTile (juce::Graphics& g, const Tile& t, const grooves::Groove& groove, bool isSelected, bool hover, double progress)
    {
        using namespace theme;

        const auto bounds = t.bounds.toFloat().reduced (0.5f);
        const auto playing = progress >= 0.0;

        g.setColour (isSelected || playing ? colours::raisedHigh : (hover ? colours::raised.brighter (0.04f) : colours::raised));
        g.fillRoundedRectangle (bounds, cornerRadius + 1.0f);
        g.setColour (playing || isSelected ? colours::accent.withAlpha (playing ? 1.0f : 0.7f)
                                           : (hover ? colours::borderStrong : colours::border));
        g.drawRoundedRectangle (bounds, cornerRadius + 1.0f, playing || isSelected ? 1.5f : 1.0f);

        auto area = bounds.reduced (12.0f, 10.0f);

        // Name, with the intensity dots on the right.
        auto top = area.removeFromTop (20.0f);
        auto dots = top.removeFromRight (3 * 6.0f + 2 * 4.0f);

        for (int i = 0; i < 3; ++i)
        {
            g.setColour (i < t.summary.intensity ? colours::accent : colours::borderStrong);
            g.fillEllipse (dots.removeFromLeft (6.0f).withSizeKeepingCentre (6.0f, 6.0f));
            dots.removeFromLeft (4.0f);
        }

        top.removeFromRight (6.0f);
        g.setColour (playing ? colours::accent : colours::text);
        g.setFont (body (14.5f, Weight::semibold));
        g.drawText (t.title, top, juce::Justification::centredLeft, true);

        // The facts.
        g.setColour (colours::textMuted);
        g.setFont (mono (type::caption));
        g.drawText (juce::String (juce::roundToInt (groove.bpm)) + " BPM" + dot + groove.meter() + dot + juce::String (t.bars)
                        + (t.bars == 1 ? " BAR" : " BARS"),
                    area.removeFromTop (16.0f), juce::Justification::centredLeft, true);
        area.removeFromTop (6.0f);

        juce::StringArray tags { groove.styleName };

        if (groove.substyle.isNotEmpty() && groove.substyle != groove.styleName)
            tags.add (groove.substyle);

        tags.add (t.summary.feel);
        paintTags (g, tags, area.removeFromTop (18.0f));
        area.removeFromTop (8.0f);

        // Bar 1 on three rows.
        constexpr std::array<Lane, 3> rows { Lane::kick, Lane::snare, Lane::hihat };
        constexpr std::array<const char*, 3> letters { "K", "S", "H" };
        auto preview = area.removeFromTop (36.0f);
        auto labels = preview.removeFromLeft (14.0f);
        const auto rowHeight = preview.getHeight() / 3.0f;
        const auto stepWidth = preview.getWidth() / (float) t.summary.steps;

        for (size_t r = 0; r < rows.size(); ++r)
        {
            const auto row = preview.withY (preview.getY() + rowHeight * (float) r).withHeight (rowHeight);
            g.setColour (colours::textMuted);
            g.setFont (label (10.0f));
            g.drawText (letters[r], labels.withY (row.getY()).withHeight (rowHeight), juce::Justification::centredLeft, false);

            for (int step = 0; step < t.summary.steps; ++step)
            {
                const auto centre = juce::Point<float> (row.getX() + stepWidth * ((float) step + 0.5f), row.getCentreY());
                const auto velocity = t.summary.preview[r][(size_t) step];

                if (velocity == 0)
                {
                    // An empty step: a faint mark, a little brighter on the beat.
                    g.setColour (step % 4 == 0 ? colours::borderStrong : colours::border);
                    g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre (centre));
                    continue;
                }

                // Velocity shows as size and brightness, never smaller than readable.
                const auto v = (float) velocity / 127.0f;
                const auto size = std::min (rowHeight - 2.0f, 4.0f + 4.5f * v);
                g.setColour (laneColour (rows[r]).withAlpha (0.45f + 0.55f * v));
                g.fillEllipse (juce::Rectangle<float> (size, size).withCentre (centre));
            }
        }

        if (playing)
        {
            const auto line = bounds.reduced (cornerRadius, 0.0f).removeFromBottom (2.0f).translated (0.0f, -2.0f);
            g.setColour (colours::accent.withAlpha (0.25f));
            g.fillRect (line);
            g.setColour (colours::accent);
            g.fillRect (line.withWidth (line.getWidth() * (float) juce::jlimit (0.0, 1.0, progress)));
        }
    }

    GroovesPage& page;
    std::vector<Tile> tiles;
    std::vector<Heading> headings;
    int pressed = -1;
    bool dragStarted = false;
    juce::Rectangle<int> lastPlayingBounds;
};

//==============================================================================
/** The selected card in full: a lane per kind of piece across all its bars,
    every hit as a mark (louder hits brighter), with the playhead while it
    plays. Long takes scroll sideways, following the playhead. Like the cards,
    clicking plays it and dragging drops its MIDI into the host. */
class GroovesPage::DetailPane final : public juce::Component
{
public:
    explicit DetailPane (GroovesPage& p) : page (p), roll (*this)
    {
        view.setViewedComponent (&roll, false);
        view.setScrollBarsShown (false, true);
        view.setScrollBarThickness (8);
        view.getHorizontalScrollBar().setColour (juce::ScrollBar::thumbColourId, theme::colours::borderStrong);
        addAndMakeVisible (view);
    }

    void show (const juce::String& newTitle, std::vector<grooves::Hit> hits, int numBars, double perBar, const RoleTable& roles)
    {
        title = newTitle;
        bars = std::max (1, numBars);
        beatsPerBar = perBar;
        lanes.clear();
        marks.clear();

        std::array<bool, allLanes.size()> used {};

        for (const auto& h : hits)
            used[(size_t) laneOf (roles[(size_t) h.note])] = true;

        for (size_t i = 0; i < allLanes.size(); ++i)
            if (used[i])
                lanes.push_back (allLanes[i]);

        for (const auto& h : hits)
        {
            const auto lane = (int) std::distance (lanes.begin(), std::find (lanes.begin(), lanes.end(), laneOf (roles[(size_t) h.note])));
            marks.push_back ({ h.beat, lane, h.velocity });
        }

        playhead = -1.0;
        view.setViewPosition (0, 0);
        resized();
        repaint();
    }

    void clear()
    {
        title = {};
        lanes.clear();
        marks.clear();
        repaint();
        roll.repaint();
    }

    /** Where the playhead is, in beats, or -1 when this part isn't playing. */
    void setPlayhead (double beats)
    {
        if (juce::approximatelyEqual (beats, playhead))
            return;

        // Only the strips the playhead leaves and enters are repainted.
        roll.repaint (playheadArea (playhead));
        playhead = beats;
        roll.repaint (playheadArea (playhead));

        // Keep it in view on long takes.
        if (beats >= 0.0 && roll.getWidth() > view.getWidth())
        {
            const auto x = xOf (beats);

            if (x < view.getViewPositionX() || x > view.getViewPositionX() + view.getWidth() - 40)
                view.setViewPosition (juce::jlimit (0, roll.getWidth() - view.getWidth(), x - 40), 0);
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        g.setColour (colours::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius + 2.0f);
        g.setColour (colours::border);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), cornerRadius + 2.0f, 1.0f);

        auto header = getLocalBounds().reduced (space::md, 0).removeFromTop (headerHeight).toFloat();
        g.setColour (colours::textMuted);
        drawTrackedText (g, (title.isEmpty() ? juce::String ("FULL VIEW") : title.toUpperCase() + dot + "FULL VIEW"),
                         label (12.5f), 0.1f, header, juce::Justification::centredLeft);
        g.setFont (body (type::small));
        g.drawText ("Click to play, drag into your host for the MIDI", header, juce::Justification::centredRight, false);

        if (lanes.empty())
        {
            g.drawText (title.isEmpty() ? "Pick a phrase or a fill above to see all of its hits." : "No hits in this part.",
                        getLocalBounds().withTrimmedTop (headerHeight), juce::Justification::centred, false);
            return;
        }

        // The lane names, in a column of their own beside the scrolling roll.
        const auto laneHeight = getLaneHeight();

        for (size_t i = 0; i < lanes.size(); ++i)
        {
            const auto r = juce::Rectangle<int> (space::md, view.getY() + barNumberHeight + (int) i * laneHeight, labelWidth - space::sm, laneHeight);
            g.setColour (laneColour (lanes[i]));
            g.fillEllipse (r.toFloat().removeFromLeft (6.0f).withSizeKeepingCentre (6.0f, 6.0f));
            g.setColour (colours::text);
            g.setFont (body (type::small, Weight::medium));
            g.drawText (laneName (lanes[i]), r.withTrimmedLeft (12), juce::Justification::centredLeft, true);
        }
    }

    void resized() override
    {
        using namespace theme;
        view.setBounds (getLocalBounds().withTrimmedTop (headerHeight).withTrimmedLeft (space::md + labelWidth).withTrimmedRight (space::md)
                            .withTrimmedBottom (space::xs));

        // At least minBarWidth per bar; wider when the bars fit.
        const auto height = view.getHeight() - view.getScrollBarThickness();
        const auto width = std::max (view.getWidth(), bars * minBarWidth);
        roll.setSize (width, height);
    }

private:
    static constexpr int headerHeight = 30, labelWidth = 72, barNumberHeight = 14, minBarWidth = 120;

    struct Mark
    {
        double beat;
        int lane;
        int velocity;
    };

    /** The roll itself, inside the viewport. */
    class Roll final : public juce::Component
    {
    public:
        explicit Roll (DetailPane& p) : pane (p)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void paint (juce::Graphics& g) override       { pane.paintRoll (g); }
        void mouseDown (const juce::MouseEvent&) override   { dragStarted = false; }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragStarted || e.getDistanceFromDragStart() < 6)
                return;

            dragStarted = true;

            if (const auto file = pane.page.writeMidi (pane.page.selectedPart); file.existsAsFile())
                juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (! dragStarted)
                pane.page.togglePlay();
        }

    private:
        DetailPane& pane;
        bool dragStarted = false;
    };

    int getLaneHeight() const
    {
        return lanes.empty() ? 0 : juce::jlimit (12, 40, (roll.getHeight() - barNumberHeight) / (int) lanes.size());
    }

    int xOf (double beats) const
    {
        return juce::roundToInt (beats / (bars * beatsPerBar) * roll.getWidth());
    }

    juce::Rectangle<int> playheadArea (double beats) const
    {
        return beats < 0.0 ? juce::Rectangle<int>() : juce::Rectangle<int> (xOf (beats) - 2, 0, 4, roll.getHeight());
    }

    void paintRoll (juce::Graphics& g)
    {
        using namespace theme;

        if (lanes.empty())
            return;

        const auto laneHeight = getLaneHeight();
        const auto top = barNumberHeight;
        const auto bottom = top + laneHeight * (int) lanes.size();
        const auto totalBeats = bars * beatsPerBar;
        const auto width = (float) roll.getWidth();
        const auto beatWidth = width / (float) totalBeats;

        // Alternate lanes slightly, for following a row across.
        for (size_t i = 0; i < lanes.size(); i += 2)
        {
            g.setColour (colours::background.withAlpha (0.5f));
            g.fillRect (0, top + (int) i * laneHeight, roll.getWidth(), laneHeight);
        }

        // Sixteenths (when there's room), beats and bars, with bar numbers.
        const auto sixteenths = (int) std::round (totalBeats * 4.0);

        for (int s = 0; s <= sixteenths; ++s)
        {
            const auto x = (float) s * beatWidth / 4.0f;
            const auto isBeat = s % 4 == 0;
            const auto isBar = std::abs (std::fmod ((double) s / 4.0, beatsPerBar)) < 0.001;

            if (! isBeat && beatWidth < 24.0f)
                continue;

            g.setColour (isBar ? colours::borderStrong : isBeat ? colours::border : colours::border.withAlpha (0.4f));
            g.fillRect (juce::Rectangle<float> (x, (float) top, 1.0f, (float) (bottom - top)));

            if (isBar && s < sixteenths)
            {
                g.setColour (colours::textMuted);
                g.setFont (mono (10.0f));
                g.drawText (juce::String (juce::roundToInt ((double) s / 4.0 / beatsPerBar) + 1),
                            juce::Rectangle<float> (x + 3.0f, 0.0f, 40.0f, (float) barNumberHeight), juce::Justification::centredLeft, false);
            }
        }

        // The hits: short bars, brighter for louder hits.
        const auto markWidth = juce::jlimit (3.0f, 8.0f, beatWidth / 4.0f * 0.55f);

        for (const auto& m : marks)
        {
            if (! juce::isPositiveAndBelow (m.lane, (int) lanes.size()))
                continue;

            const auto v = (float) m.velocity / 127.0f;
            const auto x = (float) m.beat * beatWidth;
            const auto row = juce::Rectangle<float> (x, (float) (top + m.lane * laneHeight), markWidth, (float) laneHeight);
            g.setColour (laneColour (lanes[(size_t) m.lane]).withAlpha (0.35f + 0.65f * v));
            g.fillRoundedRectangle (row.reduced (0.0f, (float) laneHeight * (0.42f - 0.28f * v)), 1.5f);
        }

        if (playhead >= 0.0)
        {
            const auto x = (float) playhead * beatWidth;
            g.setColour (colours::accent);
            g.fillRect (juce::Rectangle<float> (x - 0.75f, 0.0f, 1.5f, (float) bottom));
        }
    }

    GroovesPage& page;
    Roll roll;
    juce::Viewport view;
    juce::String title;
    int bars = 1;
    double beatsPerBar = 4.0, playhead = -1.0;
    std::vector<Lane> lanes;
    std::vector<Mark> marks;
};

//==============================================================================
/** The tempo: what the preview plays at, and in Custom mode a value to drag
    (or scroll) to change. */
class GroovesPage::TempoField final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit TempoField (GroovePlayer& p) : player (p)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        setTooltip ("Drag up or down to set the tempo (Custom)");
    }

    void setShown (float bpm, bool isEditable)
    {
        if (! juce::approximatelyEqual (bpm, shown) || isEditable != editable)
        {
            shown = bpm;
            editable = isEditable;
            setMouseCursor (editable ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto r = getLocalBounds().toFloat().reduced (0.5f);

        g.setColour (colours::background);
        g.fillRoundedRectangle (r, cornerRadius - 2.0f);
        g.setColour (editable ? colours::borderStrong : colours::border);
        g.drawRoundedRectangle (r, cornerRadius - 2.0f, 1.0f);

        g.setColour (editable ? colours::text : colours::textMuted);
        g.setFont (mono (14.0f, Weight::medium));
        g.drawText (shown > 0.0f ? juce::String (shown, 1) : juce::String ("--"), r.reduced (8.0f, 0.0f).withTrimmedRight (30.0f),
                    juce::Justification::centredRight, false);
        g.setColour (colours::textMuted);
        g.setFont (label (11.0f));
        g.drawText ("BPM", r.reduced (8.0f, 0.0f).removeFromRight (26.0f), juce::Justification::centredRight, false);
    }

    void mouseDown (const juce::MouseEvent&) override       { dragStart = player.customBpm.load(); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (editable)
            set (dragStart - (float) e.getDistanceFromDragStartY() * (e.mods.isShiftDown() ? 0.05f : 0.5f));
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (editable)
            set (player.customBpm.load() + (wheel.deltaY > 0.0f ? 1.0f : -1.0f) * (e.mods.isShiftDown() ? 0.1f : 1.0f));
    }

private:
    void set (float bpm)
    {
        player.customBpm = juce::jlimit (20.0f, 400.0f, std::round (bpm * 10.0f) / 10.0f);
        setShown (player.customBpm.load(), editable);
    }

    GroovePlayer& player;
    float shown = 0.0f, dragStart = 120.0f;
    bool editable = false;
};

//==============================================================================
/** Drags the selected tile's MIDI into the host. */
class GroovesPage::DragHandle final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit DragHandle (GroovesPage& p) : page (p)
    {
        setRepaintsOnMouseActivity (true);
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        const auto on = isEnabled();

        g.setColour (on && isMouseOver() ? colours::accent.withAlpha (0.22f) : colours::accent.withAlpha (on ? 0.12f : 0.04f));
        g.fillRoundedRectangle (r, cornerRadius);
        g.setColour (colours::accent.withAlpha (on ? 0.9f : 0.3f));
        g.drawRoundedRectangle (r, cornerRadius, 1.0f);

        // Six grip dots, then the label.
        auto area = r.reduced (12.0f, 0.0f);
        const auto grip = area.removeFromLeft (10.0f);

        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 2; ++col)
                g.fillEllipse (grip.getX() + (float) col * 6.0f, grip.getCentreY() - 7.0f + (float) row * 6.0f, 3.0f, 3.0f);

        g.setColour (on ? colours::text : colours::textMuted);
        drawTrackedText (g, "DRAG MIDI", label (13.5f, Weight::semibold), 0.1f, area, juce::Justification::centred);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragStarted || e.getDistanceFromDragStart() < 4)
            return;

        dragStarted = true;

        if (const auto file = page.writeMidi (page.selectedPart); file.existsAsFile())
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
    }

    void mouseUp (const juce::MouseEvent&) override    { dragStarted = false; }

private:
    GroovesPage& page;
    bool dragStarted = false;
};

//==============================================================================
GroovesPage::GroovesPage (LithiumDrumsProcessor& p)
    : processor (p),
      chips (std::make_unique<StyleChips> (*this)),
      list (std::make_unique<GrooveList> (*this)),
      tiles (std::make_unique<TileGrid> (*this)),
      detail (std::make_unique<DetailPane> (*this)),
      tempoField (std::make_unique<TempoField> (p.getGroovePlayer())),
      dragHandle (std::make_unique<DragHandle> (*this))
{
    using namespace theme;
    auto& player = processor.getGroovePlayer();

    styleFilter = processor.getGrooveSetting (settingStyle).toString();
    addAndMakeVisible (*chips);

    search.setTextToShowWhenEmpty ("Search grooves, styles, drummers...", colours::textMuted);
    search.setFont (body (14.0f));
    search.setIndents (10, 7);
    search.onTextChange = [this] { applyFilter(); };
    addAndMakeVisible (search);

    addAndMakeVisible (*list);

    // Beats and Fills: which tiles are shown. At least one stays on.
    for (auto* b : { &beatsButton, &fillsButton })
    {
        b->setClickingTogglesState (true);
        b->setToggleState (true, juce::dontSendNotification);
        b->onClick = [this, b]
        {
            if (! beatsButton.getToggleState() && ! fillsButton.getToggleState())
                b->setToggleState (true, juce::dontSendNotification);

            applyFilter();
            tiles->rebuild (tileView.getMaximumVisibleWidth());
        };
        addAndMakeVisible (*b);
    }

    beatsButton.setTooltip ("Show each groove's phrases");
    fillsButton.setTooltip ("Show the fills recorded with each groove (with Beats off, only grooves that have fills)");

    tempoRange.setRange (minTempo, maxTempo, 1.0);
    tempoRange.setMinAndMaxValues (minTempo, maxTempo, juce::dontSendNotification);
    tempoRange.setColour (juce::Slider::backgroundColourId, colours::raisedHigh);
    tempoRange.setColour (juce::Slider::trackColourId, colours::accent.withAlpha (0.7f));
    tempoRange.setColour (juce::Slider::thumbColourId, colours::text);
    tempoRange.onValueChange = [this] { applyFilter(); repaint (leftArea); };
    addAndMakeVisible (tempoRange);

    importButton.setTooltip ("Make a groove pack from a folder of MIDI files");
    importButton.onClick = [this] { chooseImportFolder(); };
    addAndMakeVisible (importButton);

    rescanButton.setTooltip ("Read the grooves folder again");
    rescanButton.onClick = [this] { rescan(); };
    addAndMakeVisible (rescanButton);

    downloadButton.setColour (juce::TextButton::buttonColourId, colours::accent);
    downloadButton.setColour (juce::TextButton::textColourOffId, colours::background);
    downloadButton.onClick = [this]
    {
        if (const auto* pack = missingGroovePack())
        {
            if (const auto install = installer->getStatus (pack->id); install.has_value() && install->isActive())
                installer->cancel (pack->id);
            else
                installer->install (pack->id, processor.getKitsFolder(), Settings::getGroovesFolder());
        }
    };
    addChildComponent (downloadButton);
    installer->addChangeListener (this);

    tileView.setViewedComponent (tiles.get(), false);
    tileView.setScrollBarsShown (true, false);
    tileView.setScrollBarThickness (8);
    tileView.getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, colours::borderStrong);
    addAndMakeVisible (tileView);
    addAndMakeVisible (*detail);

    // Transport.
    playButton.onClick = [this] { togglePlay(); };
    addAndMakeVisible (playButton);

    loopButton.setClickingTogglesState (true);
    loopButton.setToggleState (player.loop.load(), juce::dontSendNotification);
    loopButton.setTooltip ("Loop the preview, or play it once");
    loopButton.onClick = [this] { processor.getGroovePlayer().loop = loopButton.getToggleState(); };
    addAndMakeVisible (loopButton);

    const std::pair<juce::TextButton*, GrooveTempo> tempoButtons[] {
        { &hostTempoButton, GrooveTempo::host }, { &grooveTempoButton, GrooveTempo::original }, { &customTempoButton, GrooveTempo::custom },
    };

    for (auto [button, mode] : tempoButtons)
    {
        styleToggle (*button, 1);
        button->setToggleState (player.tempoMode.load() == mode, juce::dontSendNotification);
        button->onClick = [this, button, mode]
        {
            if (button->getToggleState())
                processor.getGroovePlayer().tempoMode = mode;
        };
        addAndMakeVisible (*button);
    }

    hostTempoButton.setTooltip ("Play at the host's tempo, and in time with it while it plays");
    grooveTempoButton.setTooltip ("Play at the tempo the groove was recorded at");
    customTempoButton.setTooltip ("Play at a tempo of your own: drag the number");
    addAndMakeVisible (*tempoField);

    const std::tuple<juce::TextButton*, const char*, float, const char*> speedButtons[] {
        { &halfButton, "\xc2\xbd\xc3\x97", 0.5f, "Half time" },
        { &normalButton, "1\xc3\x97", 1.0f, "As recorded" },
        { &doubleButton, "2\xc3\x97", 2.0f, "Double time" },
    };

    for (auto [button, text, speed, tip] : speedButtons)
    {
        button->setButtonText (juce::CharPointer_UTF8 (text));
        button->setTooltip (tip);
        styleToggle (*button, 2);
        button->setToggleState (juce::approximatelyEqual (player.speed.load(), speed), juce::dontSendNotification);
        button->onClick = [this, button, speed]
        {
            if (button->getToggleState())
                processor.getGroovePlayer().speed = speed;
        };
        addAndMakeVisible (*button);
    }

    saveButton.setTooltip ("Save the selected tile's MIDI to a file");
    saveButton.onClick = [this] { saveMidi(); };
    addAndMakeVisible (saveButton);

    dragHandle->setTooltip ("Drag the selected tile into a track in your host. The notes follow your MIDI map.");
    addAndMakeVisible (*dragHandle);

    updateTransportButtons();
}

GroovesPage::~GroovesPage()
{
    installer->removeChangeListener (this);

    // The list's model is the list itself; it lets go of it first.
    list->setModel (nullptr);
}

void GroovesPage::visibilityChanged()
{
    if (isVisible())
    {
        if (! scanned)
            rescan();

        // The stage's groove card may have changed it while this page was hidden.
        loopButton.setToggleState (processor.getGroovePlayer().loop.load(), juce::dontSendNotification);
        startTimerHz (30);
    }
    else
    {
        stopTimer();
    }
}

void GroovesPage::rescan()
{
    // What plays keeps playing, but the new library may number its grooves differently.
    scanned = true;
    playingGroove = { -1, -1 };
    library = grooves::scan (Settings::getGroovesFolder());
    chips->setStyles (library);

    // The tempo filter spans every groove installed.
    auto slowest = maxTempo, fastest = minTempo;

    for (const auto& pack : library.packs)
    {
        for (const auto& g : pack.grooves)
        {
            slowest = std::min (slowest, std::floor (g.bpm));
            fastest = std::max (fastest, std::ceil (g.bpm));
        }
    }

    tempoRange.setRange (std::min (minTempo, slowest), std::max (maxTempo, fastest), 1.0);
    tempoRange.setMinAndMaxValues (tempoRange.getMinimum(), tempoRange.getMaximum(), juce::dontSendNotification);

    // Back to what the project had selected, if it's still there.
    const auto packId = processor.getGrooveSetting (settingPack).toString();
    const auto grooveId = processor.getGrooveSetting (settingGroove).toString();
    selected = { -1, -1 };

    for (size_t p = 0; p < library.packs.size(); ++p)
        for (size_t g = 0; g < library.packs[p].grooves.size(); ++g)
            if (library.packs[p].id == packId && library.packs[p].grooves[g].id == grooveId)
                selected = { (int) p, (int) g };

    applyFilter();

    // Reads the selected groove's hits and lays out its tiles.
    const auto part = decode (processor.getGrooveSetting (settingPart).toString());
    const auto ref = selected;
    selected = { -1, -1 };

    if (ref.pack >= 0)
    {
        selectGroove (ref, false);
    }
    else if (! filtered.empty())
    {
        // A first visit starts on the first groove of the biggest style (Rock, in the Groove MIDI Dataset).
        std::map<juce::String, int> counts;

        for (const auto r : filtered)
            ++counts[library.packs[(size_t) r.pack].grooves[(size_t) r.groove].style];

        const auto biggest = std::max_element (counts.begin(), counts.end(), [] (const auto& a, const auto& b) { return a.second < b.second; })->first;
        GrooveRef first { -1, -1 };

        for (const auto r : filtered)
        {
            const auto& g = library.packs[(size_t) r.pack].grooves[(size_t) r.groove];

            if (g.style == biggest && (first.pack < 0 || g.name == g.styleName + " 1"))
                first = r;
        }

        selectGroove (first.pack >= 0 ? first : filtered.front(), false);
    }

    selectPart (part, false);
    resized();
    repaint();
}

const packs::CatalogPack* GroovesPage::missingGroovePack() const
{
    for (const auto& c : packs::catalog())
        if (c.type == packs::CatalogPack::Type::grooves
            && std::none_of (library.packs.begin(), library.packs.end(), [&] (const auto& p) { return p.id == c.id; }))
            return &c;

    return nullptr;
}

void GroovesPage::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The groove pack has finished installing: read it in.
    if (const auto* pack = missingGroovePack())
        if (const auto install = installer->getStatus (pack->id); install.has_value() && install->phase == packs::Installer::Phase::done)
            rescan();

    resized();
    repaint (emptyArea);
}

bool GroovesPage::passesFilter (const grooves::Pack& pack, const grooves::Groove& g) const
{
    if (styleFilter.isNotEmpty() && g.style != styleFilter)
        return false;

    if (g.bpm < tempoRange.getMinValue() - 0.5 || g.bpm > tempoRange.getMaxValue() + 0.5)
        return false;

    if (! beatsButton.getToggleState() && g.fills.empty())
        return false;

    // Every word typed has to match something.
    juce::StringArray words;
    words.addTokens (search.getText(), " ", "\"");
    words.removeEmptyStrings();

    for (const auto& w : words)
        if (! g.name.containsIgnoreCase (w) && ! g.styleName.containsIgnoreCase (w) && ! g.substyle.containsIgnoreCase (w)
            && ! g.drummer.containsIgnoreCase (w) && ! pack.name.containsIgnoreCase (w))
            return false;

    return true;
}

void GroovesPage::applyFilter()
{
    filtered.clear();

    for (size_t p = 0; p < library.packs.size(); ++p)
        for (size_t g = 0; g < library.packs[p].grooves.size(); ++g)
            if (passesFilter (library.packs[p], library.packs[p].grooves[g]))
                filtered.push_back ({ (int) p, (int) g });

    // By name, so "Rock 2" comes before "Rock 10".
    std::stable_sort (filtered.begin(), filtered.end(), [this] (GrooveRef a, GrooveRef b)
    {
        return library.packs[(size_t) a.pack].grooves[(size_t) a.groove].name.compareNatural (
                   library.packs[(size_t) b.pack].grooves[(size_t) b.groove].name) < 0;
    });

    list->updateContent();
    list->repaint();
    repaint (leftArea);

    // A groove the filters now hide gives way to the first one they show.
    const auto stillShown = std::any_of (filtered.begin(), filtered.end(), [this] (GrooveRef r)
    {
        return r.pack == selected.pack && r.groove == selected.groove;
    });

    if (selected.pack >= 0 && ! stillShown && ! filtered.empty())
        selectGroove (filtered.front(), false);
}

const grooves::Pack* GroovesPage::selectedPack() const
{
    return juce::isPositiveAndBelow (selected.pack, (int) library.packs.size()) ? &library.packs[(size_t) selected.pack] : nullptr;
}

const grooves::Groove* GroovesPage::selectedGroove() const
{
    return grooveAt (selected);
}

const grooves::Groove* GroovesPage::grooveAt (GrooveRef ref) const
{
    if (! juce::isPositiveAndBelow (ref.pack, (int) library.packs.size()))
        return nullptr;

    const auto& inPack = library.packs[(size_t) ref.pack].grooves;
    return juce::isPositiveAndBelow (ref.groove, (int) inPack.size()) ? &inPack[(size_t) ref.groove] : nullptr;
}

void GroovesPage::selectGroove (GrooveRef ref, bool keepPlaying)
{
    if (! juce::isPositiveAndBelow (ref.pack, (int) library.packs.size())
        || ! juce::isPositiveAndBelow (ref.groove, (int) library.packs[(size_t) ref.pack].grooves.size()))
        return;

    const auto changed = ref.pack != selected.pack || ref.groove != selected.groove;
    selected = ref;

    const auto& pack = *selectedPack();
    const auto& groove = *selectedGroove();
    processor.setGrooveSetting (settingPack, pack.id);
    processor.setGrooveSetting (settingGroove, groove.id);

    // The hits are read now, for the tiles; the files are small.
    takeHits.clear();
    fillHits.assign (groove.fills.size(), {});
    status.clear();

    if (const auto r = grooves::readHits (groove.file, pack.pedalController, pack.pedalClosedAt, takeHits); r.failed())
        status = r.getErrorMessage();

    for (size_t i = 0; i < groove.fills.size(); ++i)
        grooves::readHits (groove.fills[i].file, pack.pedalController, pack.pedalClosedAt, fillHits[i]);

    tiles->rebuild (tileView.getMaximumVisibleWidth());
    list->repaint();
    repaint (grooveHeaderArea);

    for (size_t row = 0; row < filtered.size(); ++row)
        if (filtered[row].pack == ref.pack && filtered[row].groove == ref.groove)
            list->scrollToEnsureRowIsOnscreen ((int) row);

    if (changed)
    {
        tileView.setViewPosition (0, 0);

        // While a groove plays, choosing another plays it too, so grooves can be auditioned one after another.
        const auto firstShown = beatsButton.getToggleState() || groove.fills.empty() ? Part { Part::Kind::phrase, 0 }
                                                                                       : Part { Part::Kind::fill, 0 };
        selectPart (firstShown, keepPlaying && processor.isGroovePlaying());
    }
}

void GroovesPage::selectPart (Part part, bool play)
{
    const auto* pack = selectedPack();
    const auto* groove = selectedGroove();

    if (pack == nullptr || groove == nullptr)
        return;

    // A part that no longer exists (a re-import changed the groove) falls back to the first phrase.
    const auto valid = part.kind == Part::Kind::whole
                    || (part.kind == Part::Kind::phrase && juce::isPositiveAndBelow (part.index, grooves::numPhrases (*pack, *groove)))
                    || (part.kind == Part::Kind::fill && juce::isPositiveAndBelow (part.index, (int) groove->fills.size()));

    selectedPart = valid ? part : Part { Part::Kind::phrase, 0 };
    processor.setGrooveSetting (settingPart, encode (selectedPart));

    if (play)
    {
        if (auto clip = makeClip (selectedPart))
        {
            playingGroove = selected;
            playingPart = selectedPart;
            playingClipId = ++nextClipId;
            processor.previewGroove (std::move (clip), pack->noteMap, playingClipId);
        }
    }

    tiles->repaint();
    list->repaint();
    updateTransportButtons();
    showDetail();
}

std::array<std::optional<PieceRole>, 128> GroovesPage::roleTable() const
{
    std::array<std::optional<PieceRole>, 128> roles {};

    if (const auto* pack = selectedPack())
    {
        const auto noteMap = MidiMap::factoryMap (pack->noteMap);

        for (int n = 0; n < 128; ++n)
            roles[(size_t) n] = grooves::roleOf (noteMap, n);
    }

    return roles;
}

void GroovesPage::showDetail()
{
    const auto* pack = selectedPack();
    const auto* groove = selectedGroove();

    if (pack == nullptr || groove == nullptr)
        return detail->clear();

    const auto perBar = groove->beatsPerBar();
    const auto roles = roleTable();

    switch (selectedPart.kind)
    {
        case Part::Kind::whole:
            return detail->show ("Whole take", hitsOf (takeHits, 0.0, groove->bars, perBar), groove->bars, perBar, roles);

        case Part::Kind::fill:
            if (juce::isPositiveAndBelow (selectedPart.index, (int) std::min (groove->fills.size(), fillHits.size())))
            {
                const auto& fill = groove->fills[(size_t) selectedPart.index];
                return detail->show (fill.name, hitsOf (fillHits[(size_t) selectedPart.index], 0.0, fill.bars, perBar), fill.bars, perBar, roles);
            }

            return detail->clear();

        case Part::Kind::phrase:
            break;
    }

    const auto bars = grooves::phraseBars (*pack, *groove, selectedPart.index);
    detail->show (phraseTitle (bars),
                  hitsOf (takeHits, bars.getStart() * perBar, bars.getLength(), perBar), bars.getLength(), perBar, roles);
}

void GroovesPage::togglePlay()
{
    if (processor.isGroovePlaying())
        processor.stopGroove();
    else
        selectPart (selectedPart, true);

    updateTransportButtons();
    tiles->repaint();
    list->repaint();
}

std::shared_ptr<const grooves::Clip> GroovesPage::makeClip (Part part) const
{
    const auto* pack = selectedPack();
    const auto* groove = selectedGroove();

    if (pack == nullptr || groove == nullptr)
        return nullptr;

    switch (part.kind)
    {
        case Part::Kind::whole:
            return std::make_shared<grooves::Clip> (grooves::makeClip (*groove, takeHits, 0, groove->bars, groove->name));

        case Part::Kind::fill:
        {
            if (! juce::isPositiveAndBelow (part.index, (int) std::min (groove->fills.size(), fillHits.size())))
                return nullptr;

            // A fill is a short take of its own, at the groove's tempo and meter.
            const auto& fill = groove->fills[(size_t) part.index];
            auto take = *groove;
            take.bars = fill.bars;
            return std::make_shared<grooves::Clip> (grooves::makeClip (take, fillHits[(size_t) part.index], 0, fill.bars,
                                                                       groove->name + " " + fill.name.toLowerCase()));
        }

        case Part::Kind::phrase:
            break;
    }

    const auto bars = grooves::phraseBars (*pack, *groove, part.index);

    if (bars.isEmpty())
        return nullptr;

    return std::make_shared<grooves::Clip> (grooves::makeClip (*groove, takeHits, bars.getStart(), bars.getLength(),
                                                               groove->name + ", bars " + juce::String (bars.getStart() + 1)
                                                                 + "-" + juce::String (bars.getEnd())));
}

juce::File GroovesPage::writeMidi (Part part) const
{
    const auto clip = makeClip (part);
    const auto* pack = selectedPack();

    if (clip == nullptr || pack == nullptr)
        return {};

    // Written to a folder of its own rather than the system's temp folder:
    // some hosts keep referring to a dropped file instead of copying it.
    const auto folder = Settings::getDefaultCacheFolder().getSiblingFile ("Dragged Grooves");
    folder.createDirectory();
    const auto file = folder.getChildFile (grooves::fileNameFor (*clip));

    juce::FileOutputStream out (file);

    if (out.failedToOpen())
        return {};

    out.setPosition (0);
    out.truncate();
    processor.grooveToMidi (*clip, pack->noteMap).writeTo (out);
    return file;
}

void GroovesPage::saveMidi()
{
    const auto clip = makeClip (selectedPart);

    if (clip == nullptr)
        return;

    const auto start = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (grooves::fileNameFor (*clip));
    chooser = std::make_unique<juce::FileChooser> ("Save the groove as MIDI", start, "*.mid");

    // The callback may come after the page is gone (the editor closed), hence the SafePointer.
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer (this), part = selectedPart] (const juce::FileChooser& fc)
                          {
                              const auto dest = fc.getResult();

                              if (safe == nullptr || dest == juce::File())
                                  return;

                              if (const auto file = safe->writeMidi (part); ! file.existsAsFile() || ! file.copyFileTo (dest.withFileExtension ("mid")))
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save MIDI",
                                                                          "Couldn't write " + dest.getFullPathName());
                          });
}

//==============================================================================
// Import: a folder of MIDI files becomes a groove pack (grooves::importMidiFolder).

void GroovesPage::chooseImportFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a folder of MIDI grooves",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory));

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe = juce::Component::SafePointer (this)] (const juce::FileChooser& fc)
                          {
                              if (safe != nullptr && fc.getResult().isDirectory() && safe->onImportFolder)
                                  safe->onImportFolder (fc.getResult());
                          });
}

void GroovesPage::showImportedPack (const juce::File& packFolder)
{
    rescan();

    for (size_t p = 0; p < library.packs.size(); ++p)
    {
        if (library.packs[p].folder == packFolder && ! library.packs[p].grooves.empty())
        {
            search.clear();
            chips->showAll();
            selectGroove ({ (int) p, 0 }, true);
        }
    }
}

void GroovesPage::showPack (const juce::String& packName)
{
    chips->showAll();
    search.setText (packName, true);
}

void GroovesPage::showStyle (const juce::String& styleKey)
{
    search.setText ({}, true);
    chips->showStyle (styleKey);
}

juce::String GroovesPage::describe (Part part) const
{
    if (const auto clip = makeClip (part))
        return clip->name;

    return {};
}

float GroovesPage::tempoFor (const grooves::Groove* groove) const
{
    // What's playing, or what would play.
    auto& player = processor.getGroovePlayer();
    const auto& feedback = player.getFeedback();

    if (processor.isGroovePlaying() && feedback.clipId.load() == playingClipId && feedback.bpm.load() > 0.0f)
        return feedback.bpm.load();

    const auto mode = player.tempoMode.load();
    auto bpm = mode == GrooveTempo::custom ? player.customBpm.load() : feedback.hostBpm.load();

    if (mode == GrooveTempo::original || (mode == GrooveTempo::host && bpm <= 0.0f))
        bpm = groove != nullptr ? (float) groove->bpm : 0.0f;

    return bpm * player.speed.load();
}

std::vector<GroovesPage::Part> GroovesPage::partsShown() const
{
    // In the tiles' order (see TileGrid::rebuild): the whole take and the
    // phrases under Beats, then the fills.
    std::vector<Part> parts;
    const auto* pack = selectedPack();
    const auto* groove = selectedGroove();

    if (pack == nullptr || groove == nullptr)
        return parts;

    if (beatsButton.getToggleState())
    {
        parts.push_back ({ Part::Kind::whole, 0 });

        for (int i = 0; i < grooves::numPhrases (*pack, *groove); ++i)
            parts.push_back ({ Part::Kind::phrase, i });
    }

    if (fillsButton.getToggleState())
        for (int i = 0; i < (int) std::min (groove->fills.size(), fillHits.size()); ++i)
            parts.push_back ({ Part::Kind::fill, i });

    return parts;
}

void GroovesPage::stepPart (int delta)
{
    // From the selected tile; one the filters hide steps to the first.
    const auto parts = partsShown();
    const auto at = std::find (parts.begin(), parts.end(), selectedPart);
    const auto i = at == parts.end() ? 0 : (int) (at - parts.begin()) + delta;

    if (juce::isPositiveAndBelow (i, (int) parts.size()))
        selectPart (parts[(size_t) i], processor.isGroovePlaying());
}

GroovesPage::NowPlaying GroovesPage::nowPlaying() const
{
    NowPlaying now;
    const auto& feedback = processor.getGroovePlayer().getFeedback();
    now.playing = processor.isGroovePlaying();

    // The part playing, if this page started it and the library still has
    // it (a rescan may have renumbered the grooves); else the selected one.
    const auto* playingIt = now.playing && feedback.clipId.load() == playingClipId ? grooveAt (playingGroove) : nullptr;
    const auto ref = playingIt != nullptr ? playingGroove : selected;
    const auto part = playingIt != nullptr ? playingPart : selectedPart;
    const auto* groove = grooveAt (ref);

    if (groove == nullptr)
        return now;

    now.groove = groove->name;
    now.beatsPerBar = groove->beatsPerBar();
    auto bars = groove->bars;

    switch (part.kind)
    {
        case Part::Kind::whole:
            now.part = "Whole take";
            break;

        case Part::Kind::fill:
            if (juce::isPositiveAndBelow (part.index, (int) groove->fills.size()))
            {
                now.part = groove->fills[(size_t) part.index].name;
                bars = groove->fills[(size_t) part.index].bars;
            }
            break;

        case Part::Kind::phrase:
        {
            const auto range = grooves::phraseBars (library.packs[(size_t) ref.pack], *groove, part.index);
            now.part = phraseTitle (range);
            bars = range.getLength();
            break;
        }
    }

    now.lengthBeats = bars * now.beatsPerBar;
    now.bpm = tempoFor (groove);

    if (playingIt != nullptr)
    {
        now.position = feedback.position.load();
        now.hostSynced = feedback.hostSynced.load();
    }

    // Stepping goes from the selected tile (see stepPart).
    const auto parts = partsShown();
    const auto at = std::find (parts.begin(), parts.end(), selectedPart);
    now.hasPrevious = at != parts.end() && at != parts.begin();
    now.hasNext = at == parts.end() ? ! parts.empty() : at + 1 != parts.end();
    return now;
}

void GroovesPage::updateTransportButtons()
{
    const auto playing = processor.isGroovePlaying();
    playButton.setPlaying (playing);

    const auto hasSelection = selectedGroove() != nullptr;
    playButton.setEnabled (hasSelection);
    saveButton.setEnabled (hasSelection);
    dragHandle->setEnabled (hasSelection);
}

void GroovesPage::timerCallback()
{
    auto& player = processor.getGroovePlayer();
    const auto& feedback = player.getFeedback();
    const auto playing = processor.isGroovePlaying();

    if (! emptyArea.isEmpty() && installer->isBusy())
    {
        resized();   // the button says Cancel while it installs
        repaint (emptyArea);
    }

    if (playing != playButton.isPlaying())
    {
        // The preview ended by itself (loop off), or the host project restored a stopped state.
        updateTransportButtons();
        tiles->repaint();
        list->repaint();
    }

    if (playing)
        tiles->repaintPlaying();

    // The detail pane's playhead, while the part it shows is the one playing.
    const auto showsPlaying = playing && feedback.clipId.load() == playingClipId && playingPart == selectedPart
                              && playingGroove.pack == selected.pack && playingGroove.groove == selected.groove;
    detail->setPlayhead (showsPlaying ? feedback.position.load() : -1.0);

    tempoField->setShown (tempoFor (selectedGroove()), player.tempoMode.load() == GrooveTempo::custom);

    // The status line.
    juce::String text;

    if (playing && feedback.clipId.load() == playingClipId)
    {
        text = "Playing " + describe (playingPart);

        if (const auto* other = grooveAt (playingGroove); other != nullptr && playingGroove != selected)
            text = "Playing " + other->name;

        if (feedback.hostSynced.load())
            text << ", in time with the host";
    }
    else if (status.isNotEmpty())
    {
        text = status;
    }
    else
    {
        text = "MIDI map: " + processor.getMidiMap().name;
    }

    if (text != statusText)
    {
        statusText = text;
        repaint (statusArea);
    }
}

//==============================================================================
void GroovesPage::paint (juce::Graphics& g)
{
    using namespace theme;

    g.fillAll (colours::background);

    // The left column: a heading over the list, the filters under it.
    if (! leftArea.isEmpty())
    {
        auto area = leftArea.toFloat();
        g.setColour (colours::textMuted);
        drawTrackedText (g, "GROOVES", label (12.5f), 0.1f, area.removeFromTop (22.0f), juce::Justification::centredLeft);
        g.setFont (mono (12.0f));
        g.drawText (juce::String ((int) filtered.size()) + " / " + juce::String (library.numGrooves()),
                    leftArea.withHeight (22), juce::Justification::centredRight, false);

        // Filters that leave nothing: say so, and how to get grooves back.
        if (filtered.empty() && library.numGrooves() > 0)
        {
            g.setColour (colours::textMuted);
            g.setFont (body (type::body));
            g.drawFittedText ("No grooves match. Clear the search, pick All styles, or widen the tempo range.",
                              list->getBounds().reduced (space::lg, 0).withHeight (60).translated (0, space::xl),
                              juce::Justification::centredTop, 3);
        }

        const auto filters = list->getBounds().getBottom() + 12;
        g.setColour (colours::border);
        g.fillRect (leftArea.getX(), filters - 6, leftArea.getWidth(), 1);

        g.setColour (colours::textMuted);
        drawTrackedText (g, "TYPE", label (12.0f), 0.1f, { (float) leftArea.getX(), (float) beatsButton.getY(), 60.0f, (float) beatsButton.getHeight() },
                         juce::Justification::centredLeft);
        drawTrackedText (g, "TEMPO", label (12.0f), 0.1f, { (float) leftArea.getX(), (float) tempoRange.getY() - 18.0f, 60.0f, 16.0f },
                         juce::Justification::centredLeft);
        g.setFont (mono (12.0f));
        g.drawText (juce::String (juce::roundToInt (tempoRange.getMinValue())) + "-" + juce::String (juce::roundToInt (tempoRange.getMaxValue())) + " BPM",
                    juce::Rectangle<int> (leftArea.getX(), tempoRange.getY() - 18, leftArea.getWidth(), 16), juce::Justification::centredRight, false);
    }

    // The groove's heading.
    if (const auto* groove = selectedGroove(); groove != nullptr && ! grooveHeaderArea.isEmpty())
    {
        auto area = grooveHeaderArea.toFloat();
        g.setColour (colours::text);
        g.setFont (body (21.0f, Weight::semibold));
        g.drawText (groove->name, area.removeFromTop (28.0f), juce::Justification::centredLeft, true);

        auto sub = groove->styleName;

        if (groove->substyle.isNotEmpty() && ! groove->name.containsIgnoreCase (groove->substyle))
            sub << dot << groove->substyle;

        if (groove->drummer.isNotEmpty())
            sub << dot << groove->drummer;

        sub << dot << juce::String (juce::roundToInt (groove->bpm)) << " BPM" << dot << groove->meter()
            << dot << groove->bars << " bars";

        if (const auto* pack = selectedPack())
            sub << dot << pack->name;

        g.setColour (colours::textMuted);
        g.setFont (body (13.5f));
        g.drawText (sub, area, juce::Justification::centredLeft, true);
    }

    // No grooves installed: the catalog's groove pack, greyed, to download.
    if (const auto* pack = missingGroovePack(); pack != nullptr && ! emptyArea.isEmpty())
    {
        auto card = emptyArea.toFloat();
        juce::Path outline;
        outline.addRoundedRectangle (card.reduced (0.5f), cornerRadius + 2.0f);
        const float dashes[] { 4.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (outline, outline, dashes, 2);
        g.setColour (colours::panel);
        g.fillRoundedRectangle (card, cornerRadius + 2.0f);
        g.setColour (colours::borderStrong);
        g.fillPath (outline);

        auto area = emptyArea.reduced (22, 18);
        g.setColour (colours::textMuted);
        drawTrackedText (g, "NO GROOVES INSTALLED", label (12.5f), 0.1f, area.removeFromTop (18).toFloat(), juce::Justification::centredLeft);
        area.removeFromTop (6);
        g.setColour (colours::text.withAlpha (0.6f));
        g.setFont (body (20.0f, Weight::semibold));
        g.drawText (pack->name, area.removeFromTop (28), juce::Justification::centredLeft, false);
        g.setColour (colours::textMuted);
        g.setFont (body (13.5f));
        g.drawFittedText (pack->description + "\nBy " + pack->author + ". " + pack->licenseName + ".", area.removeFromTop (70),
                          juce::Justification::topLeft, 4, 1.0f);

        // Under the button: progress, or what went wrong.
        auto line = downloadButton.getBounds().translated (0, downloadButton.getHeight() + 12).withWidth (area.getWidth()).withHeight (34).toFloat();

        if (const auto install = installer->getStatus (pack->id); install.has_value() && install->phase != packs::Installer::Phase::done)
        {
            const auto progress = install->phase == packs::Installer::Phase::downloading ? 0.8 * install->progress
                                : install->phase == packs::Installer::Phase::unpacking   ? 0.8 + 0.15 * install->progress
                                : install->phase == packs::Installer::Phase::importing   ? 0.95 : 0.0;

            if (install->isActive())
            {
                auto bar = line.removeFromTop (6.0f).withWidth (260.0f);
                g.setColour (colours::raisedHigh);
                g.fillRoundedRectangle (bar, 3.0f);
                g.setColour (colours::accent);
                g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) progress), 3.0f);
                line.removeFromTop (4.0f);
            }

            g.setColour (install->phase == packs::Installer::Phase::failed ? colours::warning : colours::textMuted);
            g.setFont (body (12.5f));
            g.drawFittedText (install->message, line.toNearestInt(), juce::Justification::topLeft, 2, 0.9f);
        }
    }

    // The transport bar.
    g.setColour (colours::panel);
    g.fillRect (transportArea);
    g.setColour (colours::border);
    g.fillRect (transportArea.withHeight (1));

    g.setColour (colours::textMuted);
    drawTrackedText (g, "TEMPO", label (12.0f), 0.1f, { (float) hostTempoButton.getX() - 54.0f, (float) transportArea.getY(), 48.0f, (float) transportArea.getHeight() },
                     juce::Justification::centredRight);
    drawTrackedText (g, "SPEED", label (12.0f), 0.1f, { (float) halfButton.getX() - 54.0f, (float) transportArea.getY(), 48.0f, (float) transportArea.getHeight() },
                     juce::Justification::centredRight);

    g.setColour (processor.isGroovePlaying() ? colours::text : colours::textMuted);
    g.setFont (body (13.0f));
    g.drawFittedText (statusText, statusArea, juce::Justification::centredLeft, 2, 0.9f);
}

void GroovesPage::resized()
{
    auto area = getLocalBounds();

    transportArea = area.removeFromBottom (transportHeight);
    area.reduce (margin, 0);
    area.removeFromTop (12);

    // The chips take what they need; the search box gets the rest.
    constexpr int minSearchWidth = 240;
    auto top = area.removeFromTop (chipRowHeight);
    const auto chipsWidth = chips->layOut (top.getWidth() - minSearchWidth - theme::space::lg);
    chips->setBounds (top.removeFromLeft (chipsWidth));
    top.removeFromLeft (theme::space::lg);
    search.setBounds (top);

    area.removeFromTop (14);
    area.removeFromBottom (12);

    // Left: the list, then the filters.
    leftArea = area.removeFromLeft (leftWidth);
    area.removeFromLeft (20);
    {
        auto left = leftArea;
        left.removeFromTop (26);
        auto filters = left.removeFromBottom (118);
        list->setBounds (left);

        filters.removeFromTop (14);
        auto typeRow = filters.removeFromTop (28);
        typeRow.removeFromLeft (60);
        beatsButton.setBounds (typeRow.removeFromLeft (62));
        typeRow.removeFromLeft (4);
        fillsButton.setBounds (typeRow.removeFromLeft (62));
        rescanButton.setBounds (typeRow.removeFromRight (64));
        typeRow.removeFromRight (4);
        importButton.setBounds (typeRow.removeFromRight (70));

        filters.removeFromTop (30);
        tempoRange.setBounds (filters.removeFromTop (24));
    }

    // Right: the groove's heading, then its tiles.
    rightArea = area;
    const auto hasGrooves = library.numGrooves() > 0;
    emptyArea = scanned && ! hasGrooves && missingGroovePack() != nullptr
                  ? rightArea.withTrimmedTop (40).withWidth (std::min (rightArea.getWidth(), 600)).withHeight (250) : juce::Rectangle<int>();

    if (const auto* pack = missingGroovePack(); pack != nullptr && ! emptyArea.isEmpty())
    {
        const auto install = installer->getStatus (pack->id);
        const auto active = install.has_value() && install->isActive();
        const auto failed = install.has_value() && install->phase == packs::Installer::Phase::failed;
        downloadButton.setButtonText (active ? "Cancel" : (failed ? "Try again" : "Download  " + packs::formatSize (pack->downloadSize())));
        downloadButton.setBounds (emptyArea.getX() + 22, emptyArea.getY() + 156, 200, 34);
    }

    downloadButton.setVisible (! emptyArea.isEmpty());

    grooveHeaderArea = rightArea.removeFromTop (52);
    rightArea.removeFromTop (4);
    detail->setBounds (rightArea.removeFromBottom (detailHeight));
    rightArea.removeFromBottom (theme::space::md);
    tileView.setBounds (rightArea);
    tileView.setVisible (hasGrooves);
    detail->setVisible (hasGrooves);
    tiles->rebuild (tileView.getMaximumVisibleWidth());

    // The transport bar, left to right.
    auto t = transportArea.reduced (margin, 12);
    playButton.setBounds (t.removeFromLeft (92));
    t.removeFromLeft (8);
    loopButton.setBounds (t.removeFromLeft (64));
    t.removeFromLeft (70);
    hostTempoButton.setBounds (t.removeFromLeft (58));
    grooveTempoButton.setBounds (t.removeFromLeft (64));
    customTempoButton.setBounds (t.removeFromLeft (66));
    t.removeFromLeft (8);
    tempoField->setBounds (t.removeFromLeft (100));
    t.removeFromLeft (72);
    halfButton.setBounds (t.removeFromLeft (42));
    normalButton.setBounds (t.removeFromLeft (42));
    doubleButton.setBounds (t.removeFromLeft (42));

    dragHandle->setBounds (t.removeFromRight (150));
    t.removeFromRight (8);
    saveButton.setBounds (t.removeFromRight (112));
    t.removeFromRight (16);
    t.removeFromLeft (20);
    statusArea = t;
}

} // namespace lithium
