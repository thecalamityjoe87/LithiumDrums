#pragma once

#include "engine/MidiMap.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lithium
{

/** One piece's velocity curve in the MIDI panel: the velocity coming in
    across, the velocity that plays up.

    Drag a node to move it (the end nodes only up and down), click anywhere
    else to add one, double-click a node to remove it. With a global velocity
    set, the faint second line is what the piece really plays at. The last
    hit on the piece shows as a dot.

    The editor only shows and edits nodes; whoever owns it saves the curve
    (in the MIDI map) from onChange. Message thread only. */
class VelocityCurveEditor final : public juce::Component,
                                  public juce::SettableTooltipClient
{
public:
    VelocityCurveEditor();

    /** While a node is dragged, and when one is added or removed. The curve
        isn't sanitised yet (a node dragged onto the straight line still
        counts), so the drag isn't interrupted. */
    std::function<void (const VelocityCurve&)> onChange;

    /** Shows a curve. Ignored mid-drag: the drag's own nodes win. */
    void setCurve (const VelocityCurve&, float global);

    /** The dot for the last hit: what came in and what it played at. -1 hides it. */
    void showHit (int in, int out);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> plot() const;
    juce::Point<float> toScreen (float in, float out) const;
    VelocityNode fromScreen (juce::Point<float>) const;
    int nodeAt (juce::Point<float>) const;
    void moveNode (int index, juce::Point<float>);
    void changed();

    std::vector<VelocityNode> nodes;   // always at least the two ends
    float global = 0.0f;
    int dragging = -1, hovering = -1;
    int hitIn = -1, hitOut = -1;
};

} // namespace lithium
