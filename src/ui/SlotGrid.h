#pragma once

#include "ModelPicker.h"

namespace lithium
{

/** The Kit tab's Slots view: the same 6 x 4 grid of slots for every kit
    (see engine/KitSlots.h). A slot with a piece shows a picture of it, the
    model it plays (click to swap it), its level, mute and solo; clicking
    the card plays its piece (and selects it), as on the stage, and the
    card lights up briefly when its piece is hit. An empty
    slot shows "Add": clicking it opens the model picker to fill it. The
    selected piece is shared with the stage and the Edit tab. */
class SlotGrid final : public juce::Component, private juce::Timer, private SelectionModel::Listener
{
public:
    explicit SlotGrid (LithiumDrumsProcessor&);
    ~SlotGrid() override;

    /** Builds the cards for a kit, laid out to fill this area. */
    void setKit (std::shared_ptr<const KitInfo>, juce::Rectangle<int> area);

    /** The picture of a piece in the finish of the kit its recordings come
        from; null if there's no render of it. */
    static juce::Image pictureFor (const KitInfo&, int pieceIndex);

    static constexpr int columns = 6, rows = 4;   // kitslots::numSlots of them

private:
    class Card;

    void timerCallback() override;
    void selectedPieceChanged (int) override;
    void layout (juce::Rectangle<int> area);

    LithiumDrumsProcessor& processor;
    std::shared_ptr<const KitInfo> kit;
    std::vector<std::unique_ptr<Card>> cards;
    std::array<std::uint32_t, maxPieces> seenHits {};
};

} // namespace lithium
