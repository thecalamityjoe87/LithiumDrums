#pragma once

#include "ChannelModules.h"
#include "PluginProcessor.h"

namespace lithium
{

/** One fader strip in the Mixer page's bank: a drum's channel, an overhead
    or room mic's channel, a close mic's trim, the reverb return or the
    master. Clicking a channel (or the reverb) shows it in the inspector. */
class MixerStrip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Kind { piece, mic, trim, reverb, master };

    MixerStrip (LithiumDrumsProcessor&, Kind, int slot = -1, const KitInfo* = nullptr);

    std::function<void()> onSelect;

    const Kind kind;
    const int slot;

    void setSelected (bool);
    void pushLevels (float left, float right);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> getReadoutArea() const;

    juce::String name, tag;
    juce::Colour dot;
    bool selected = false, silent = false;

    juce::Slider fader;
    LevelMeter meter;
    Knob pan { {}, Knob::Style::compact };
    ParamToggle eq { "EQ", "EQ on/off" }, comp { "Comp", "Compressor on/off" };
    ParamToggle mute { "M", "Mute" }, solo { "S", "Solo" };
    ParamCombo output;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> faderAttachment;   // after the fader it uses
};

/** The top of the Mixer page: everything about the selected channel (its
    EQ, transient shaper, compressor, saturation, send and output), or the
    reverb's room. The same modules are re-attached to whichever channel is
    selected. */
class MixerInspector final : public juce::Component
{
public:
    explicit MixerInspector (LithiumDrumsProcessor&);
    ~MixerInspector() override;

    void showChannel (params::ChannelId, juce::String name, juce::String description);
    void showReverb();
    void showNothing();

    void pushReduction (float db)            { comp.pushReduction (db); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class DecayGraph;

    enum class Mode { none, channel, reverb };
    void setMode (Mode);

    LithiumDrumsProcessor& processor;
    Mode mode = Mode::none;
    juce::String title, subtitle;

    EqModule eq;
    CompModule comp;
    TwoKnobModule transient, saturation;
    RoutingModule routing;

    Module room { "Room" }, returnModule { "Return" };
    std::unique_ptr<DecayGraph> decay;
    Knob size { "Size" }, decayTime { "Decay" }, preDelay { "Pre-delay" }, damping { "Damping" }, lowCut { "Low cut" }, width { "Width" };
    Knob returnLevel { "Return" };
    ParamToggle returnMute { "Mute", "Mute the reverb return" };
    juce::Label outputLabel { {}, "OUTPUT" };
    ParamCombo returnOutput;
};

/** The Mixer tab: the inspector on top; below, a fader bank with a channel
    per drum, then the overhead and room mics, the reverb return and the
    master. A second view of the bank trims the close mics that feed the
    drum channels. */
class MixerPage final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit MixerPage (LithiumDrumsProcessor&);
    ~MixerPage() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateKitInfo();
    void rebuildStrips();
    void select (params::ChannelId);
    void selectReverb();
    void updateSelection();

    LithiumDrumsProcessor& processor;
    std::shared_ptr<const KitInfo> shownKit;
    MixerInspector inspector;
    juce::TextButton channelsView { "Channels" }, trimsView { "Close mics" };
    bool showingTrims = false;
    juce::Viewport stripView;
    juce::Component stripRow;
    std::vector<std::unique_ptr<MixerStrip>> strips;
    std::vector<int> dividers;   // x positions in the row, between groups of strips
    MixerStrip reverbStrip, masterStrip;
};

} // namespace lithium
