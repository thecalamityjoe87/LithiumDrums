#pragma once

#include "Controls.h"
#include "KitLibrary.h"
#include "PluginProcessor.h"

// Piece swapping in the UI: every piece shows the model it plays on a
// ModelButton (on the Kit tab's slots and selected-piece card, and in the
// Edit tab's piece bar). Clicking it opens the ModelPicker for the piece's
// slot, which lists every installed kit's pieces that fit there; choosing one
// swaps it in, or fills the slot if it was empty. See engine/PieceSwap.h and
// engine/KitSlots.h.

namespace lithium
{

/** What a piece plays: its kit and piece, e.g. "MuldjordKit · Snare" (the source kit's, for a swapped piece). */
juce::String modelText (const KitInfo&, const KitPieceInfo&);

/** A drop-down-like button with a piece's model. Clicking it opens the
    ModelPicker for that piece. */
class ModelButton final : public juce::Button
{
public:
    explicit ModelButton (LithiumDrumsProcessor&);

    /** The piece it's for (an index into the kit's pieces), or -1 for none. */
    void setPiece (std::shared_ptr<const KitInfo>, int piece);

    void paintButton (juce::Graphics&, bool isMouseOver, bool isDown) override;
    void clicked() override;

private:
    LithiumDrumsProcessor& processor;
    std::shared_ptr<const KitInfo> kit;
    int piece = -1;
};

/** The list of models a slot can hold, shown in a call-out next to its
    ModelButton or empty card: the kit's own piece (if it has one there),
    Empty, then each installed kit's pieces that fit the slot, with arrows
    to step through them, the "Ambience follows piece" switch, and notes on
    what didn't come along. Choosing a model reloads the kit in the
    background (the current one plays until it's ready), then plays the
    piece once so it can be heard. */
class ModelPicker final : public juce::Component,
                          private juce::ListBoxModel,
                          private juce::ChangeListener
{
public:
    ModelPicker (LithiumDrumsProcessor&, int slot);
    ~ModelPicker() override;

    /** Opens a picker for a slot (see KitSlots.h) in a call-out pointing at target. */
    static void show (LithiumDrumsProcessor&, int slot, juce::Component& target);

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int width = 340, height = 460;

private:
    struct Row
    {
        bool heading = false;
        juce::String text;               // the piece's name, or the heading
        juce::String kitId, pieceId;     // empty kitId: the kit's own piece, or with `empty`, nothing
        bool empty = false;              // the "Empty" row
    };

    int getNumRows() override                       { return (int) rows.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void rebuild();
    void choose (int row);
    void step (int delta);
    int currentRow() const;
    const KitPieceInfo* currentPiece (int* index = nullptr) const;

    /** The id swaps for this slot use: the kit's own piece there, or the
        id a piece added to it gets (kitslots::addedPieceId()). */
    juce::String key() const;

    LithiumDrumsProcessor& processor;
    const int slot;
    std::shared_ptr<const KitInfo> shownKit;   // held, so pointers into it stay valid until rebuild() replaces it
    std::vector<Row> rows;
    juce::String title, notes;
    bool notesAreProblems = false, auditionWhenLoaded = false;

    juce::ListBox list;
    ChevronButton previous { "Previous model", ChevronButton::Direction::left };
    ChevronButton next { "Next model", ChevronButton::Direction::right };
    juce::TextButton ambience { "Ambience follows piece" };
};

/** The component call-outs and other popovers go in: the scaled main view,
    so they scale with the rest of the UI. */
juce::Component* findOverlayParent (juce::Component&);

} // namespace lithium
