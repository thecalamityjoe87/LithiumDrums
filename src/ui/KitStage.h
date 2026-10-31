#pragma once

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>
#include <vector>

namespace lithium
{

/** The pre-rendered drum kit: click a piece to select and audition it.

    Each of the nine stage slots is its own transparent PNG (see
    assets/kit-renders), drawn back to front over the studio room backdrop.
    The loaded kit's pieces are assigned to slots by role; a slot without a
    piece is drawn dimmed and can't be clicked. Clicks are hit-tested against
    the pixels' alpha, front to back, so a click on a transparent part of one
    piece falls through to whatever is behind it. */
class KitStage final : public juce::Component,
                       private SelectionModel::Listener,
                       private juce::Timer
{
public:
    explicit KitStage (LithiumDrumsProcessor&);
    ~KitStage() override;

    void setKitInfo (std::shared_ptr<const KitInfo>);

    /** The front-most slot with a visible pixel at a point in stage
        coordinates, or nothing if the point only hits transparent pixels. */
    std::optional<StageSlot> getSlotAt (juce::Point<float>) const;

    /** The kit piece (index) shown at a point, if any. Dimmed slots don't count. */
    std::optional<int> getPieceAt (juce::Point<float>) const;

    /** Called when the overlay card's "Edit piece" button is clicked. */
    std::function<void()> onEditPiece;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    struct Layer
    {
        std::optional<StageSlot> slot;           // empty for the base layer (stands and floor)
        juce::Image image, glow, flash;          // glow and flash are precomputed at load
        juce::Rectangle<int> renderRect;         // in render pixels (layout.json)
        juce::Rectangle<float> bounds, glowBounds;   // in stage coordinates (1x)
        float flashLevel = 0.0f;
        double flashStartMs = 0.0;
        std::uint32_t lastHitCount = 0;
    };

    class OverlayCard;

    void loadRenders();
    int pieceIn (const Layer&) const noexcept;
    Layer* layerFor (int piece);
    void startFlash (int piece);
    void updateCard();
    void selectedPieceChanged (int piece) override;
    void timerCallback() override;

    LithiumDrumsProcessor& processor;
    juce::Image room;                            // opaque backdrop, the full stage at render scale
    std::vector<Layer> layers;                   // back to front
    float renderScale = 1.5f;
    int glowPadding = 0;
    std::array<int, numStageSlots> slotPiece {};   // the kit piece in each slot, -1 for none
    std::shared_ptr<const KitInfo> kitInfo;
    std::unique_ptr<OverlayCard> card;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KitStage)
};

} // namespace lithium
