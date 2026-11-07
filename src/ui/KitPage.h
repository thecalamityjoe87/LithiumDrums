#pragma once

#include "KitStage.h"
#include "SlotGrid.h"

namespace lithium
{

class GroovesPage;

/** The Kit tab, in one of two views (switched at the top right):
      - Stage: the rendered kit, filling the tab. Clicking a piece selects
        and plays it; a card at the bottom left has the selected piece's model,
        volume, mute and solo, and a way to the Edit tab. A card at the
        bottom right plays and stops the groove picked on the Grooves tab.
      - Slots: a card per piece (see SlotGrid), for building a kit of your
        own by swapping pieces in from other kits. */
class KitPage final : public juce::Component, private juce::ChangeListener
{
public:
    explicit KitPage (LithiumDrumsProcessor&);
    ~KitPage() override;

    std::function<void()> onEditPiece;
    std::function<void()> onOpenGrooves;

    /** The page whose groove selection the stage's groove card plays. */
    void setGroovesPage (GroovesPage&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class PieceCard;
    class GrooveCard;
    class HihatIndicator;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void updateKitInfo();
    void showSlots (bool);

    LithiumDrumsProcessor& processor;
    std::shared_ptr<const KitInfo> shownKit;

    KitStage stage;
    std::unique_ptr<PieceCard> card;
    std::unique_ptr<GrooveCard> grooveCard;
    std::unique_ptr<HihatIndicator> hihat;
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
