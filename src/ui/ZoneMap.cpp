#include "ZoneMap.h"

#include "Theme.h"

namespace lithium
{

namespace
{
    constexpr double flashSeconds = 0.6;
    constexpr float columnGap = 4.0f;
}

void ZoneMap::setArticulation (const KitArticulationInfo* a, juce::Colour c)
{
    articulation = a;
    colour = c;
    hitZone = hitRoundRobin = hitVelocity = -1;
    repaint();
}

void ZoneMap::showHit (int zone, int roundRobin, int velocity)
{
    hitZone = zone;
    hitRoundRobin = roundRobin;
    hitVelocity = velocity;
    hitTime = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    startTimerHz (60);   // for the flash; stops itself once it has faded
    repaint();
}

void ZoneMap::timerCallback()
{
    if (juce::Time::getMillisecondCounterHiRes() / 1000.0 - hitTime > flashSeconds)
        stopTimer();

    repaint();
}

juce::Rectangle<float> ZoneMap::getPlotArea() const
{
    return compact ? getLocalBounds().toFloat().reduced (8.0f, 6.0f).withTrimmedBottom (14.0f)
                   : getLocalBounds().toFloat().reduced (12.0f, 10.0f).withTrimmedBottom (34.0f);
}

float ZoneMap::xForVelocity (float velocity) const
{
    const auto plot = getPlotArea();
    return plot.getX() + (velocity - 1.0f) / 127.0f * plot.getWidth();
}

int ZoneMap::velocityAt (float x) const
{
    const auto plot = getPlotArea();
    return juce::jlimit (1, 127, 1 + (int) ((x - plot.getX()) / plot.getWidth() * 127.0f));
}

int ZoneMap::zoneAt (int velocity) const
{
    if (articulation == nullptr)
        return -1;

    for (size_t z = 0; z < articulation->zones.size(); ++z)
        if (velocity >= articulation->zones[z].velocityLow && velocity <= articulation->zones[z].velocityHigh)
            return (int) z;

    return -1;
}

juce::Rectangle<float> ZoneMap::getColumn (size_t z) const
{
    const auto plot = getPlotArea();
    const auto& zone = articulation->zones[z];
    const auto left = xForVelocity ((float) zone.velocityLow);
    const auto right = xForVelocity ((float) zone.velocityHigh + 1.0f);

    // Louder zones stand taller, so the map reads like a staircase.
    const auto height = plot.getHeight() * (0.3f + 0.7f * (float) zone.velocityHigh / 127.0f);
    return { left + columnGap * 0.5f, plot.getBottom() - height, std::max (2.0f, right - left - columnGap), height };
}

void ZoneMap::paint (juce::Graphics& g)
{
    using namespace theme;

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (colours::background);
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (colours::border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);

    const auto plot = getPlotArea();

    // The velocity axis.
    g.setFont (mono (10.0f));

    for (const auto v : { 1, 32, 64, 96, 127 })
    {
        const auto x = xForVelocity ((float) v + (v == 127 ? 1.0f : 0.0f));
        g.setColour (colours::border);
        g.fillRect (juce::Rectangle<float> (x - 0.5f, plot.getBottom() + (compact ? 1.0f : 4.0f), 1.0f, compact ? 3.0f : 5.0f));
        g.setColour (colours::textMuted);
        g.drawText (juce::String (v), juce::Rectangle<float> (x - 20.0f, plot.getBottom() + (compact ? 3.0f : 10.0f), 40.0f, 11.0f),
                    juce::Justification::centred);
    }

    if (! compact)
    {
        g.setColour (colours::textMuted);
        drawTrackedText (g, "VELOCITY", label (10.0f), 0.15f,
                         juce::Rectangle<float> (plot.getX(), plot.getBottom() + 22.0f, plot.getWidth(), 12.0f),
                         juce::Justification::centred);
    }

    if (articulation == nullptr || articulation->zones.empty())
        return;

    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const auto flash = (float) std::exp (-(now - hitTime) / (flashSeconds * 0.35));
    const auto hoverZone = zoneAt (hoverVelocity);

    for (size_t z = 0; z < articulation->zones.size(); ++z)
    {
        const auto column = getColumn (z);
        const auto& zone = articulation->zones[z];
        const auto isHit = (int) z == hitZone;

        g.setColour (colour.withAlpha ((int) z == hoverZone ? 0.16f : 0.08f));
        g.fillRoundedRectangle (column, 5.0f);
        g.setColour (colour.withAlpha (isHit ? 0.75f : 0.35f));
        g.drawRoundedRectangle (column.reduced (0.5f), 5.0f, isHit ? 1.5f : 1.0f);

        // The round-robin cells: the biggest size that fits them all.
        const auto inner = compact ? column.reduced (4.0f) : column.reduced (6.0f).withTrimmedTop (16.0f).withTrimmedBottom (18.0f);
        const auto n = zone.numRoundRobins;
        float cell = 4.0f;
        int perRow = 1;

        for (auto size = compact ? 10.0f : 16.0f; size >= 3.0f; size -= 1.0f)
        {
            const auto gap = std::max (2.0f, size * 0.25f);
            perRow = std::max (1, (int) ((inner.getWidth() + gap) / (size + gap)));
            const auto rows = (n + perRow - 1) / perRow;

            if ((float) rows * (size + gap) - gap <= inner.getHeight())
            {
                cell = size;
                break;
            }
        }

        const auto gap = std::max (2.0f, cell * 0.25f);
        const auto rowWidth = (float) std::min (n, perRow) * (cell + gap) - gap;
        const auto x0 = inner.getCentreX() - rowWidth * 0.5f;

        for (int i = 0; i < n; ++i)
        {
            const auto r = juce::Rectangle<float> (x0 + (float) (i % perRow) * (cell + gap),
                                                   inner.getY() + (float) (i / perRow) * (cell + gap), cell, cell);

            if (r.getBottom() > inner.getBottom() + 0.5f)
                break;   // too many to draw; the tooltip still gives the count

            const auto lit = isHit && i == hitRoundRobin;
            g.setColour (lit ? colour.interpolatedWith (colours::raisedHigh, 1.0f - std::max (0.35f, flash)) : colours::raisedHigh);
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (lit ? colour : colours::borderStrong);
            g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
        }

        // Zone number on top, velocity range at the bottom, where there's room.
        g.setColour (colours::textMuted);
        g.setFont (label (11.0f));

        if (compact)
            continue;

        if (column.getWidth() >= 18.0f)
            g.drawText (juce::String ((int) z + 1), column.withHeight (16.0f).translated (0.0f, 3.0f), juce::Justification::centred, false);

        if (column.getWidth() >= 40.0f)
        {
            g.setFont (mono (10.0f));
            g.drawText (juce::String (zone.velocityLow) + "-" + juce::String (zone.velocityHigh),
                        column.withTop (column.getBottom() - 17.0f), juce::Justification::centred, false);
        }
    }

    // The last hit's velocity.
    if (hitVelocity > 0)
    {
        const auto x = xForVelocity ((float) hitVelocity + 0.5f);
        g.setColour (colours::text.withAlpha (0.35f + 0.6f * flash));
        g.fillRect (juce::Rectangle<float> (x - 1.0f, plot.getY(), 2.0f, plot.getHeight() + 4.0f));
    }

    // And where a click would play.
    if (hoverVelocity > 0)
    {
        const auto x = xForVelocity ((float) hoverVelocity + 0.5f);
        g.setColour (colours::accent.withAlpha (0.5f));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, plot.getY(), 1.0f, plot.getHeight() + 4.0f));
    }
}

void ZoneMap::mouseDown (const juce::MouseEvent& e)
{
    if (articulation != nullptr && onAudition)
        onAudition (velocityAt (e.position.x));
}

void ZoneMap::mouseMove (const juce::MouseEvent& e)
{
    hoverVelocity = velocityAt (e.position.x);
    repaint();
}

void ZoneMap::mouseExit (const juce::MouseEvent&)
{
    hoverVelocity = -1;
    repaint();
}

juce::String ZoneMap::getTooltip()
{
    const auto z = zoneAt (hoverVelocity);

    if (z < 0)
        return {};

    const auto& zone = articulation->zones[(size_t) z];
    return "Zone " + juce::String (z + 1) + ": velocity " + juce::String (zone.velocityLow) + " to " + juce::String (zone.velocityHigh)
         + ", " + juce::String (zone.numRoundRobins) + (zone.numRoundRobins == 1 ? " round-robin" : " round-robins")
         + "\nClick to play at velocity " + juce::String (hoverVelocity);
}

} // namespace lithium
