#pragma once

#include "KitStage.h"
#include "PieceStrip.h"

namespace lithium
{

/** The Kit tab: the stage on top, one strip per piece of the loaded kit
    below, scrolling sideways when there are more than fit. */
class KitPage final : public juce::Component, private juce::ChangeListener
{
public:
    explicit KitPage (LithiumDrumsProcessor&);
    ~KitPage() override;

    std::function<void()> onEditPiece;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void updateKitInfo();

    LithiumDrumsProcessor& processor;
    KitStage stage;
    juce::Viewport stripView;
    juce::Component stripRow;                         // the strips, inside the viewport
    std::vector<std::unique_ptr<PieceStrip>> strips;
    std::shared_ptr<const KitInfo> shownKit;
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
