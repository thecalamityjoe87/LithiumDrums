#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace lithium
{

/** The MIDI panel's keyboard: every MIDI note as a key, labelled with what
    it plays in the current map and tinted with that drum's colour.

    It stands upright, high notes at the top, with a few octaves showing at
    a time. Beside it, a mini keyboard of all 128 notes shows where the
    mapped notes are and which part is in view; click or drag it (or use the
    mouse wheel) to move. Clicking a key plays its note, harder the further
    right the click; notes that come in light their keys. */
class MidiKeyboard final : public juce::Component,
                           public juce::TooltipClient
{
public:
    struct Key
    {
        juce::String label;          // what the note plays, empty if nothing
        juce::String detail;         // for the tooltip
        juce::Colour colour;         // the drum's group colour
        bool spare = false;          // a spare note, given automatically to something the map doesn't reach
    };

    MidiKeyboard();

    void setKeys (const std::array<Key, 128>&);

    /** Outlines these notes: the keys of the articulation selected (or
        learning) in the table. */
    void setHighlighted (std::vector<int> notes);

    /** Lights a key, for a note that just came in. */
    void flash (int note);

    /** Scrolls so a note is in view, if it isn't already. */
    void scrollToShow (int note);

    /** Scrolls so the lowest mapped note sits near the bottom. */
    void scrollToMapped();

    /** Fades out flashes. Call regularly (the panel's timer). */
    void tick();

    /** A key was clicked: the note, and a velocity from where on the key. */
    std::function<void (int note, int velocity)> onPlay;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::String getTooltip() override;

    static constexpr int keyHeight = 22;

private:
    int getVisibleNotes() const;
    void setLowest (int);
    juce::Rectangle<int> getKeyBounds (int note) const;
    int noteAt (juce::Point<int>) const;
    int overviewNoteAt (int y) const;
    float overviewY (int note) const;
    void paintKey (juce::Graphics&, int note);
    void paintOverview (juce::Graphics&);

    std::array<Key, 128> keys;
    std::array<double, 128> flashUntil {};
    std::vector<int> highlighted;
    int lowest = 24;                 // the bottom note in view
    int hovered = -1;
    bool draggingOverview = false;
    juce::Rectangle<int> keysArea, overviewArea;
};

} // namespace lithium
