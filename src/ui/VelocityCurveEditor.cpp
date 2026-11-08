#include "VelocityCurveEditor.h"

#include "Theme.h"

namespace lithium
{

namespace
{
    constexpr float nodeRadius = 5.0f;
    constexpr float grabDistance = 9.0f;
    const juce::String arrow (juce::CharPointer_UTF8 (" \xe2\x86\x92 "));

    std::vector<VelocityNode> withEnds (const VelocityCurve& curve)
    {
        return curve.nodes.size() >= 2 ? curve.nodes : std::vector<VelocityNode> { { 0, 0 }, { 127, 127 } };
    }
}

VelocityCurveEditor::VelocityCurveEditor()
{
    nodes = withEnds ({});
    setTooltip ("Velocity in (across) and what it plays at (up). Drag a node to change the response, "
                "click to add a node, double-click a node to remove it.");
}

void VelocityCurveEditor::setCurve (const VelocityCurve& curve, float g)
{
    if (dragging >= 0)
        return;

    nodes = withEnds (curve);
    global = g;
    hovering = -1;
    repaint();
}

void VelocityCurveEditor::showHit (int in, int out)
{
    hitIn = in;
    hitOut = out;
    repaint();
}

//==============================================================================
juce::Rectangle<float> VelocityCurveEditor::plot() const
{
    // Room for the node circles all round, and the axis labels below and left.
    return getLocalBounds().toFloat().withTrimmedLeft (26.0f).withTrimmedBottom (16.0f).reduced (nodeRadius + 1.0f);
}

juce::Point<float> VelocityCurveEditor::toScreen (float in, float out) const
{
    const auto p = plot();
    return { p.getX() + p.getWidth() * in / 127.0f, p.getBottom() - p.getHeight() * out / 127.0f };
}

VelocityNode VelocityCurveEditor::fromScreen (juce::Point<float> pos) const
{
    const auto p = plot();
    return { juce::jlimit (0.0f, 127.0f, std::round ((pos.x - p.getX()) / p.getWidth() * 127.0f)),
             juce::jlimit (0.0f, 127.0f, (p.getBottom() - pos.y) / p.getHeight() * 127.0f) };
}

int VelocityCurveEditor::nodeAt (juce::Point<float> pos) const
{
    auto best = -1;
    auto bestDistance = grabDistance;

    for (size_t i = 0; i < nodes.size(); ++i)
    {
        if (const auto d = toScreen (nodes[i].in, nodes[i].out).getDistanceFrom (pos); d <= bestDistance)
        {
            best = (int) i;
            bestDistance = d;
        }
    }

    return best;
}

//==============================================================================
void VelocityCurveEditor::paint (juce::Graphics& g)
{
    using namespace theme;
    const auto p = plot();

    g.setColour (colours::panel);
    g.fillRoundedRectangle (p.expanded (nodeRadius + 1.0f), 5.0f);

    // Quarters, and the straight line for reference.
    g.setColour (colours::border);

    for (const auto v : { 32.0f, 64.0f, 96.0f })
    {
        g.drawVerticalLine (juce::roundToInt (toScreen (v, 0).x), p.getY(), p.getBottom());
        g.drawHorizontalLine (juce::roundToInt (toScreen (0, v).y), p.getX(), p.getRight());
    }

    {
        juce::Path diagonal;
        diagonal.startNewSubPath (toScreen (0, 0));
        diagonal.lineTo (toScreen (127, 127));
        const std::array<float, 2> dashes { 4.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (diagonal, diagonal, dashes.data(), (int) dashes.size());
        g.setColour (colours::borderStrong);
        g.fillPath (diagonal);
    }

    g.setColour (colours::textMuted);
    g.setFont (mono (10.5f));
    g.drawText ("127", juce::Rectangle<float> (0.0f, p.getY() - 6.0f, 24.0f, 12.0f), juce::Justification::centredRight, false);
    g.drawText ("0", juce::Rectangle<float> (0.0f, p.getBottom() - 6.0f, 24.0f, 12.0f), juce::Justification::centredRight, false);
    g.drawText ("127", juce::Rectangle<float> (p.getRight() - 24.0f, p.getBottom() + 6.0f, 30.0f, 12.0f), juce::Justification::centredRight, false);
    g.setFont (body (10.5f));
    g.drawText ("in", juce::Rectangle<float> (p.getX(), p.getBottom() + 6.0f, p.getWidth(), 12.0f), juce::Justification::centred, false);
    g.drawText ("out", juce::Rectangle<float> (0.0f, p.getCentreY() - 6.0f, 22.0f, 12.0f), juce::Justification::centredRight, false);

    const VelocityCurve curve { nodes };
    const auto steps = std::max (2, juce::roundToInt (p.getWidth() / 2.0f));

    auto trace = [&] (bool withGlobal)
    {
        juce::Path path;

        for (int i = 0; i <= steps; ++i)
        {
            const auto in = 127.0f * (float) i / (float) steps;
            const auto out = withGlobal ? applyGlobalVelocity (curve (in), global) : curve (in);
            const auto point = toScreen (in, out);

            if (i == 0)
                path.startNewSubPath (point);
            else
                path.lineTo (point);
        }

        return path;
    };

    // What really plays, with the global velocity: shown only when it differs.
    if (! juce::exactlyEqual (global, 0.0f))
    {
        g.setColour (colours::textMuted.withAlpha (0.55f));
        g.strokePath (trace (true), juce::PathStrokeType (1.5f));
    }

    g.setColour (colours::accent);
    g.strokePath (trace (false), juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const auto c = toScreen (nodes[i].in, nodes[i].out);
        const auto active = (int) i == dragging || (int) i == hovering;
        const auto circle = juce::Rectangle<float> (nodeRadius * 2.0f, nodeRadius * 2.0f).withCentre (c);
        g.setColour (active ? colours::accent : colours::raised);
        g.fillEllipse (circle);
        g.setColour (colours::accent);
        g.drawEllipse (circle.reduced (0.75f), 1.5f);
    }

    // The node being moved, as numbers.
    if (dragging >= 0 || hovering >= 0)
    {
        const auto& n = nodes[(size_t) (dragging >= 0 ? dragging : hovering)];
        g.setColour (colours::text);
        g.setFont (mono (11.0f));
        g.drawText (juce::String (juce::roundToInt (n.in)) + arrow + juce::String (juce::roundToInt (n.out)),
                    p.reduced (6.0f).removeFromTop (14.0f), juce::Justification::topLeft, false);
    }
    else if (hitIn >= 0)
    {
        const auto dot = toScreen ((float) hitIn, (float) hitOut);
        g.setColour (colours::text);
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (dot));
        g.setFont (mono (11.0f));
        g.drawText ("hit " + juce::String (hitIn) + arrow + juce::String (hitOut), p.reduced (6.0f).removeFromTop (14.0f),
                    juce::Justification::topLeft, false);
    }
}

//==============================================================================
void VelocityCurveEditor::mouseMove (const juce::MouseEvent& e)
{
    if (const auto h = nodeAt (e.position); h != hovering)
    {
        hovering = h;
        setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void VelocityCurveEditor::mouseExit (const juce::MouseEvent&)
{
    hovering = -1;
    repaint();
}

void VelocityCurveEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled())
        return;

    dragging = nodeAt (e.position);

    if (dragging >= 0 || (int) nodes.size() >= VelocityCurve::maxNodes || ! plot().expanded (nodeRadius).contains (e.position))
        return;

    // A new node, between the two it falls between, at least a velocity from each.
    const auto node = fromScreen (e.position);
    const auto after = std::find_if (nodes.begin(), nodes.end(), [&] (const auto& n) { return n.in > node.in; });

    if (after == nodes.begin() || after == nodes.end() || node.in < (after - 1)->in + 1.0f || node.in > after->in - 1.0f)
        return;

    dragging = (int) (nodes.insert (after, node) - nodes.begin());
    changed();
}

void VelocityCurveEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0)
        return;

    moveNode (dragging, e.position);
    changed();
}

void VelocityCurveEditor::mouseUp (const juce::MouseEvent& e)
{
    dragging = -1;
    hovering = nodeAt (e.position);
    repaint();
}

void VelocityCurveEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto i = nodeAt (e.position);

    // The two ends always stay.
    if (i <= 0 || i >= (int) nodes.size() - 1)
        return;

    nodes.erase (nodes.begin() + i);
    dragging = hovering = -1;
    changed();
}

void VelocityCurveEditor::moveNode (int index, juce::Point<float> pos)
{
    auto& n = nodes[(size_t) index];
    const auto moved = fromScreen (pos);
    n.out = moved.out;

    // The ends stay at 0 and 127; the others between their neighbours, so
    // the order never changes under the mouse.
    if (index > 0 && index < (int) nodes.size() - 1)
        n.in = juce::jlimit (nodes[(size_t) index - 1].in + 1.0f, nodes[(size_t) index + 1].in - 1.0f, moved.in);
}

void VelocityCurveEditor::changed()
{
    repaint();

    if (onChange)
        onChange (VelocityCurve { nodes });
}

} // namespace lithium
