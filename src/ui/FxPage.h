#pragma once

#include "ChannelModules.h"
#include "FxPresets.h"
#include "PluginProcessor.h"

namespace lithium
{

/** The FX tab, laid out as the signal flows:

     - the three FX returns across the top (Room, Plate, Delay), each a card
       with all of its controls and a preset menu; every mixer channel sends
       to them;
     - the master chain under them, as a row of six modules in their fixed
       order (EQ, glue, crush, tape, transient, limiter) between the bus's
       input and output meters; each has its On switch, its main controls
       and a preset menu;
     - below, the selected module in detail: every control, with its graph.

    The bar at the top has the global FX bypass and the A/B comparison of
    the whole page. Clicking a card or module selects it. */
class FxPage final : public juce::Component, private juce::Timer
{
public:
    explicit FxPage (LithiumDrumsProcessor&);
    ~FxPage() override;

    /** Shows a module in the detail area (fxpresets::Module). */
    void selectModule (int module);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class PresetButton;
    class ReturnCard;
    class ChainTile;
    class Detail;

    void timerCallback() override;
    void visibilityChanged() override;
    void updateAB();

    LithiumDrumsProcessor& processor;
    ParamToggle bypass { "FX Bypass", "Bypass every effect on this page: the returns go silent, the master chain passes the mix through" };
    juce::TextButton slotA { "A" }, slotB { "B" }, copySlot;
    std::array<std::unique_ptr<ReturnCard>, params::numFxReturns> cards;
    std::array<std::unique_ptr<ChainTile>, 6> tiles;
    std::unique_ptr<Detail> detail;
    LevelMeter inMeter { 2 }, outMeter { 2 };
    juce::Rectangle<int> chainArea;
    int selected = -1, refreshCount = 0;
};

} // namespace lithium
