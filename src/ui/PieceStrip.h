#pragma once

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lithium
{

/** One kit piece's strip under the stage: name, model, volume, mute, solo
    and audition. The controls are bound straight to the parameters of the
    piece's slot. KitPage builds a new set of strips whenever the kit changes. */
class PieceStrip final : public juce::Component, private SelectionModel::Listener
{
public:
    PieceStrip (LithiumDrumsProcessor&, int piece, const KitPieceInfo&);
    ~PieceStrip() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    class AuditionButton;

    void selectedPieceChanged (int) override;
    juce::Rectangle<int> getReadoutArea() const;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    LithiumDrumsProcessor& processor;
    const int piece;
    const juce::String name;
    const PieceGroup group;
    bool selected = false;

    juce::ComboBox model;
    juce::Slider volume;
    juce::TextButton mute { "M" }, solo { "S" };
    std::unique_ptr<AuditionButton> audition;

    // Attachments keep a control and a parameter in sync in both directions.
    // They must be destroyed before the controls they refer to, so they are
    // declared after them (members are destroyed in reverse order).
    SliderAttachment volumeAttachment;
    ButtonAttachment muteAttachment, soloAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PieceStrip)
};

} // namespace lithium
