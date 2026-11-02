#pragma once

#include "KitStage.h"
#include "SlotGrid.h"

namespace lithium
{

/** The Kit tab, in one of two views (switched at the top right):
      - Stage: the rendered kit, filling the tab. Clicking a piece selects
        and plays it; a card at the bottom left has the selected piece's model,
        volume, mute, solo and play, and a way to the Edit tab.
      - Slots: a card per piece (see SlotGrid), for building a kit of your
        own by swapping pieces in from other kits. */
class KitPage final : public juce::Component, private juce::ChangeListener
{
public:
    explicit KitPage (LithiumDrumsProcessor&);
    ~KitPage() override;

    std::function<void()> onEditPiece;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class PieceCard;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void updateKitInfo();
    void showSlots (bool);

    LithiumDrumsProcessor& processor;
    std::shared_ptr<const KitInfo> shownKit;

    KitStage stage;
    std::unique_ptr<PieceCard> card;
    juce::TextButton stageButton { "Stage" }, slotsButton { "Slots" };
    juce::Viewport slotView;
    SlotGrid slots;
};

/** Placeholder for the tabs that later milestones fill in. */
class ComingSoonPage final : public juce::Component
{
public:
    ComingSoonPage (juce::String title, juce::String description);
    void paint (juce::Graphics&) override;

private:
    juce::String title, description;
};

} // namespace lithium
