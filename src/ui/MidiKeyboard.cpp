#include "MidiKeyboard.h"

#include "Theme.h"
#include "engine/MidiMapping.h"

namespace lithium
{

namespace
{
    constexpr int overviewWidth = 44;          // octave numbers plus the mini keyboard
    constexpr int overviewKeysWidth = 22;
    constexpr int badgeWidth = 30;
    constexpr double flashMs = 180.0;
    constexpr int minVelocity = 20;

    bool isBlack (int note)
    {
        switch (note % 12)
        {
            case 1: case 3: case 6: case 8: case 10: return true;
            default:                                  return false;
        }
    }

    // White keys are pale, black keys dark, as on a keyboard; a mapped key
    // takes on some of its drum's colour.
    juce::Colour keyColour (int note, const MidiKeyboard::Key& key)
    {
        const auto base = isBlack (note) ? theme::colours::raisedHigh : theme::colours::text.darker (0.12f);
        return key.label.isEmpty() ? base : base.interpolatedWith (key.colour, isBlack (note) ? 0.35f : 0.28f);
    }
}

MidiKeyboard::MidiKeyboard()
{
    setRepaintsOnMouseActivity (false);
}

void MidiKeyboard::setKeys (const std::array<Key, 128>& k)
{
    keys = k;
    repaint();
}

void MidiKeyboard::setHighlighted (std::vector<int> notes)
{
    highlighted = std::move (notes);
    repaint();
}

void MidiKeyboard::flash (int note)
{
    if (juce::isPositiveAndBelow (note, 128))
    {
        flashUntil[(size_t) note] = juce::Time::getMillisecondCounterHiRes() + flashMs;
        repaint();
    }
}

void MidiKeyboard::tick()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    bool changed = false;

    for (auto& f : flashUntil)
    {
        if (f > 0.0 && now > f)
        {
            f = 0.0;
            changed = true;
        }
    }

    if (changed)
        repaint();
}

int MidiKeyboard::getVisibleNotes() const
{
    return std::max (1, keysArea.getHeight() / keyHeight);
}

void MidiKeyboard::setLowest (int note)
{
    const auto clamped = juce::jlimit (0, 128 - getVisibleNotes(), note);

    if (clamped != lowest)
    {
        lowest = clamped;
        repaint();
    }
}

void MidiKeyboard::scrollToShow (int note)
{
    if (note < lowest)
        setLowest (note - 2);
    else if (note >= lowest + getVisibleNotes())
        setLowest (note - getVisibleNotes() + 3);
}

void MidiKeyboard::scrollToMapped()
{
    for (int n = 0; n < 128; ++n)
    {
        if (keys[(size_t) n].label.isNotEmpty())
        {
            setLowest (n - 1);
            return;
        }
    }

    setLowest (36 - 1);
}

//==============================================================================
juce::Rectangle<int> MidiKeyboard::getKeyBounds (int note) const
{
    // Bottom up: the lowest note in view sits on the bottom edge.
    const auto row = note - lowest;
    auto r = juce::Rectangle<int> (keysArea.getX(), keysArea.getBottom() - (row + 1) * keyHeight, keysArea.getWidth(), keyHeight);
    return isBlack (note) ? r.withTrimmedRight (keysArea.getWidth() / 6) : r;
}

int MidiKeyboard::noteAt (juce::Point<int> p) const
{
    if (! keysArea.contains (p))
        return -1;

    const auto note = lowest + (keysArea.getBottom() - 1 - p.y) / keyHeight;
    return note < lowest + getVisibleNotes() && note < 128 && getKeyBounds (note).contains (p) ? note : -1;
}

float MidiKeyboard::overviewY (int note) const
{
    // Note 0 at the bottom, 127 at the top.
    return (float) overviewArea.getBottom() - (float) overviewArea.getHeight() * (float) note / 128.0f;
}

int MidiKeyboard::overviewNoteAt (int y) const
{
    return juce::jlimit (0, 127, (int) ((float) (overviewArea.getBottom() - y) * 128.0f / (float) overviewArea.getHeight()));
}

//==============================================================================
void MidiKeyboard::paint (juce::Graphics& g)
{
    for (int n = lowest; n < std::min (128, lowest + getVisibleNotes()); ++n)
        paintKey (g, n);

    paintOverview (g);
}

void MidiKeyboard::paintKey (juce::Graphics& g, int note)
{
    using namespace theme;

    const auto& key = keys[(size_t) note];
    const auto black = isBlack (note);
    const auto r = getKeyBounds (note).reduced (0, 1).toFloat();
    const auto lit = flashUntil[(size_t) note] > 0.0;
    const auto mapped = key.label.isNotEmpty();

    auto fill = keyColour (note, key);

    if (lit)
        fill = fill.interpolatedWith (colours::accent, 0.6f);
    else if (note == hovered)
        fill = black ? fill.brighter (0.25f) : fill.brighter (0.15f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, 4.0f);

    // The note number in a badge of the drum's colour; a spare note's badge
    // is only outlined, as it isn't part of the map.
    const auto badge = r.withWidth ((float) badgeWidth).reduced (3.0f, 3.0f);
    g.setColour (mapped ? key.colour : (black ? colours::panel : colours::textMuted.withAlpha (0.35f)));

    if (key.spare)
        g.drawRoundedRectangle (badge.reduced (0.75f), 3.0f, 1.5f);
    else
        g.fillRoundedRectangle (badge, 3.0f);

    g.setColour (key.spare ? (black ? colours::text : colours::background)
                           : mapped ? colours::background : (black ? colours::textMuted : colours::background));
    g.setFont (mono (11.5f, Weight::medium));
    g.drawText (juce::String (note), badge, juce::Justification::centred, false);

    const auto textColour = black ? colours::text : colours::background;
    auto textArea = r.withTrimmedLeft ((float) badgeWidth + 4.0f).withTrimmedRight (6.0f);

    if (note % 12 == 0)
    {
        // Octave names on the Cs, as on a DAW's piano roll.
        g.setColour (textColour.withAlpha (0.5f));
        g.setFont (mono (10.5f));
        g.drawText (noteName (note), textArea.removeFromRight (30.0f), juce::Justification::centredRight, false);
    }

    if (mapped)
    {
        g.setColour (textColour);
        g.setFont (body (13.0f, Weight::medium));
        g.drawText (key.label, textArea, juce::Justification::centredLeft, true);
    }

    if (std::find (highlighted.begin(), highlighted.end(), note) != highlighted.end())
    {
        g.setColour (colours::accent);
        g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 2.0f);
    }
}

void MidiKeyboard::paintOverview (juce::Graphics& g)
{
    using namespace theme;

    const auto strip = overviewArea.withTrimmedLeft (overviewArea.getWidth() - overviewKeysWidth).toFloat();
    const auto rowHeight = strip.getHeight() / 128.0f;

    g.setColour (colours::panel);
    g.fillRoundedRectangle (strip.expanded (2.0f), 4.0f);

    for (int n = 0; n < 128; ++n)
    {
        const auto& key = keys[(size_t) n];
        auto row = juce::Rectangle<float> (strip.getX(), overviewY (n) - rowHeight, strip.getWidth(), rowHeight);

        if (isBlack (n))
            row = row.withTrimmedRight (strip.getWidth() * 0.35f);

        g.setColour (key.label.isNotEmpty() ? key.colour : keyColour (n, key).withMultipliedAlpha (0.55f));
        g.fillRect (row.reduced (0.0f, rowHeight > 3.0f ? 0.5f : 0.0f));
    }

    // Octave numbers beside each C (C-2 is note 0, C3 middle C).
    g.setFont (mono (10.0f));
    g.setColour (colours::textMuted);

    for (int octave = 0; octave < 11; ++octave)
    {
        const auto y = overviewY (octave * 12);
        g.drawText (juce::String (octave - 2), juce::Rectangle<float> (strip.getX() - 22.0f, y - 12.0f, 18.0f, 12.0f),
                    juce::Justification::centredRight, false);
    }

    // The part in view.
    const auto top = overviewY (std::min (128, lowest + getVisibleNotes()));
    const auto view = juce::Rectangle<float> (strip.getX() - 3.0f, top, strip.getWidth() + 6.0f, overviewY (lowest) - top);
    g.setColour (colours::accent.withAlpha (0.12f));
    g.fillRoundedRectangle (view, 3.0f);
    g.setColour (colours::accent);
    g.drawRoundedRectangle (view, 3.0f, 1.5f);
}

void MidiKeyboard::resized()
{
    auto area = getLocalBounds();
    overviewArea = area.removeFromRight (overviewWidth).reduced (0, 4);
    area.removeFromRight (10);

    // Whole keys only, aligned to the bottom.
    keysArea = area.withTrimmedTop (area.getHeight() % keyHeight);
    setLowest (lowest);
}

//==============================================================================
void MidiKeyboard::mouseDown (const juce::MouseEvent& e)
{
    draggingOverview = overviewArea.expanded (4, 0).contains (e.getPosition());

    if (draggingOverview)
    {
        mouseDrag (e);
        return;
    }

    if (const auto note = noteAt (e.getPosition()); note >= 0 && onPlay)
    {
        // Further right is harder, so the velocity zones can be heard.
        const auto key = getKeyBounds (note);
        const auto t = (float) (e.x - key.getX()) / (float) std::max (1, key.getWidth());
        onPlay (note, juce::jlimit (minVelocity, 127, minVelocity + juce::roundToInt (t * (127 - minVelocity))));
    }
}

void MidiKeyboard::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingOverview)
        setLowest (overviewNoteAt (e.y) - getVisibleNotes() / 2);   // centre the view on the mouse
}

void MidiKeyboard::mouseMove (const juce::MouseEvent& e)
{
    if (const auto note = noteAt (e.getPosition()); note != hovered)
    {
        hovered = note;
        repaint (keysArea);
    }
}

void MidiKeyboard::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    repaint (keysArea);
}

void MidiKeyboard::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    // Up shows higher notes. Small trackpad scrolls still move at least one key.
    const auto steps = wheel.deltaY * 12.0f;
    setLowest (lowest + (std::abs (steps) < 1.0f ? (steps > 0.0f ? 1 : -1) : juce::roundToInt (steps)));
}

juce::String MidiKeyboard::getTooltip()
{
    const auto mouse = getMouseXYRelative();

    if (overviewArea.expanded (4, 0).contains (mouse))
        return "All 128 notes. Click or drag to move the keyboard there.";

    const auto note = noteAt (mouse);

    if (note < 0)
        return {};

    const auto& key = keys[(size_t) note];
    const auto head = juce::String (note) + " " + noteName (note);
    return (key.detail.isNotEmpty() ? head + juce::String (juce::CharPointer_UTF8 (" \xe2\x86\x92 ")) + key.detail : head + ": not mapped")
           + "\nClick to play it (further right is harder).";
}

} // namespace lithium
