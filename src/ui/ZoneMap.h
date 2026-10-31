#pragma once

#include "engine/Kit.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lithium
{

/** One articulation's velocity zones, laid out along the velocity axis
    (1 to 127). Each zone is a column, taller the louder it is, holding one
    cell per round-robin. The round-robin that played last lights up, and a
    line marks the velocity it was played at.

    Clicking anywhere plays the articulation at the velocity under the mouse. */
class ZoneMap final : public juce::Component, public juce::TooltipClient, private juce::Timer
{
public:
    std::function<void (int velocity)> onAudition;

    /** The articulation must stay alive while it's shown (the page holds the
        KitInfo it lives in). Null shows nothing. */
    void setArticulation (const KitArticulationInfo*, juce::Colour);

    void showHit (int zone, int roundRobin, int velocity);

    /** A thin strip: no axis title and no zone labels. */
    void setCompact (bool c)                 { compact = c; repaint(); }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

private:
    void timerCallback() override;
    juce::Rectangle<float> getPlotArea() const;
    float xForVelocity (float velocity) const;
    int velocityAt (float x) const;
    int zoneAt (int velocity) const;
    juce::Rectangle<float> getColumn (size_t zone) const;

    const KitArticulationInfo* articulation = nullptr;
    juce::Colour colour;
    int hitZone = -1, hitRoundRobin = -1, hitVelocity = -1;
    double hitTime = 0.0;
    int hoverVelocity = -1;
    bool compact = false;
};

} // namespace lithium
