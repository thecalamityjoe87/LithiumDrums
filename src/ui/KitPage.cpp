#include "KitPage.h"

#include "Theme.h"

namespace lithium
{

KitPage::KitPage (LithiumDrumsProcessor& p) : processor (p), stage (p)
{
    stage.onEditPiece = [this] { if (onEditPiece) onEditPiece(); };
    addAndMakeVisible (stage);

    // The viewport doesn't own the row (false): it's a member, destroyed with the page.
    stripView.setViewedComponent (&stripRow, false);
    stripView.setScrollBarsShown (false, true);
    stripView.setScrollBarThickness (8);
    stripView.getHorizontalScrollBar().setColour (juce::ScrollBar::thumbColourId, theme::colours::borderStrong);
    addAndMakeVisible (stripView);

    processor.addChangeListener (this);
    updateKitInfo();
}

KitPage::~KitPage()
{
    processor.removeChangeListener (this);
}

void KitPage::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateKitInfo();
}

void KitPage::updateKitInfo()
{
    // Hold our own reference while using it, in case the processor swaps it.
    const auto info = processor.getKitInfo();

    if (info == shownKit)
        return;

    shownKit = info;
    stage.setKitInfo (info);

    // A new kit can have a different number of pieces, so the strips (and
    // their parameter attachments) are rebuilt rather than updated.
    strips.clear();

    if (info != nullptr)
    {
        for (size_t i = 0; i < info->pieces.size(); ++i)
        {
            strips.push_back (std::make_unique<PieceStrip> (processor, (int) i, info->pieces[i]));
            stripRow.addAndMakeVisible (*strips.back());
        }
    }

    resized();
    repaint();
}

void KitPage::paint (juce::Graphics& g)
{
    using namespace theme;

    g.setColour (colours::panel);
    g.fillRect (getLocalBounds().withTrimmedTop (stageHeight));
    g.setColour (colours::border);
    g.fillRect (0, stageHeight, getWidth(), 1);

    if (strips.empty())
    {
        auto area = getLocalBounds().withTrimmedTop (stageHeight).reduced (24).toFloat();
        const auto error = processor.getKitError();
        const auto loading = processor.isKitLoading();

        g.setColour (colours::text);
        g.setFont (body (16.0f, Weight::semibold));
        g.drawText (loading ? "Loading kit..." : error.isNotEmpty() ? "The kit couldn't be loaded" : "No kit loaded",
                    area.removeFromTop (28.0f), juce::Justification::centred, true);

        g.setColour (colours::textMuted);
        g.setFont (body (14.0f));

        if (! loading)
            g.drawFittedText (error.isNotEmpty() ? error
                                                 : "Choose a kit from the menu at the top. Kits go in "
                                                       + processor.getKitsFolder().getFullPathName()
                                                       + ". The README explains how to download and import them.",
                              area.removeFromTop (60.0f).toNearestInt(), juce::Justification::centredTop, 3);
    }
}

void KitPage::resized()
{
    auto area = getLocalBounds();
    stage.setBounds (area.removeFromTop (theme::stageHeight));

    area.reduce (16, 14);
    stripView.setBounds (area);

    constexpr int gap = 8, minStripWidth = 128, scrollBarSpace = 12;
    const auto n = (int) strips.size();

    if (n == 0)
    {
        stripRow.setSize (0, 0);
        return;
    }

    // Equal widths that fill the row, unless that would make strips too
    // narrow; then they keep a minimum width and the row scrolls.
    const auto fitted = (float) (area.getWidth() - gap * (n - 1)) / (float) n;
    const auto scrolls = fitted < (float) minStripWidth;
    const auto stripWidth = scrolls ? (float) minStripWidth : fitted;
    const auto height = area.getHeight() - (scrolls ? scrollBarSpace : 0);

    stripRow.setSize (juce::roundToInt ((float) n * stripWidth + (float) (gap * (n - 1))), height);

    for (int i = 0; i < n; ++i)
    {
        const auto x = juce::roundToInt ((float) i * (stripWidth + (float) gap));
        const auto right = juce::roundToInt ((float) i * (stripWidth + (float) gap) + stripWidth);
        strips[(size_t) i]->setBounds (x, 0, right - x, height);
    }
}

//==============================================================================
ComingSoonPage::ComingSoonPage (juce::String t, juce::String d) : title (std::move (t)), description (std::move (d)) {}

void ComingSoonPage::paint (juce::Graphics& g)
{
    using namespace theme;

    g.fillAll (colours::background);

    auto area = getLocalBounds().toFloat().withSizeKeepingCentre (600.0f, 110.0f);

    g.setColour (colours::text);
    drawTrackedText (g, title.toUpperCase(), label (34.0f, Weight::bold), 0.12f,
                     area.removeFromTop (44.0f), juce::Justification::centred);

    g.setColour (colours::accent);
    drawTrackedText (g, "COMING SOON", label (14.0f), 0.2f, area.removeFromTop (26.0f), juce::Justification::centred);

    g.setColour (colours::textMuted);
    g.setFont (body (15.0f));
    g.drawText (description, area.removeFromTop (30.0f), juce::Justification::centred, true);
}

} // namespace lithium
