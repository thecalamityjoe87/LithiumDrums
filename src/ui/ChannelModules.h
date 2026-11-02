#pragma once

#include "Controls.h"
#include "EqGraph.h"

namespace lithium
{

/** A titled card holding one group of controls. The Edit page and the
    Mixer's inspector are both built from these. A module can carry an On
    button and a Reset button in its header. */
class Module : public juce::Component
{
public:
    explicit Module (juce::String title);

    void setTitle (juce::String t)           { title = std::move (t); repaint(); }
    void paint (juce::Graphics&) override;

protected:
    /** The area under the header. */
    juce::Rectangle<int> getContentArea() const;

    /** Puts the On (and Reset) buttons at the right of the header. */
    void layoutHeaderButtons (juce::Component* on, juce::Component* reset);

    juce::String title;
};

/** The channel's EQ: the curve with its band nodes, and a knob per setting. */
class EqModule final : public Module
{
public:
    explicit EqModule (juce::AudioProcessorValueTreeState&);

    void attach (params::ChannelId);
    void setSampleRate (double rate)         { graph.setSampleRate (rate); }
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    juce::AudioProcessorValueTreeState& state;
    params::ChannelId channel;
    EqGraph graph;
    ParamToggle on { "On", "EQ on/off" };
    juce::TextButton reset { "Reset" };
    Knob hpf { "HPF" }, lowFreq { "Low" }, lowGain { "Gain" }, mid1Freq { "Mid 1" }, mid1Gain { "Gain" }, mid1Q { "Q" };
    Knob mid2Freq { "Mid 2" }, mid2Gain { "Gain" }, mid2Q { "Q" }, highFreq { "High" }, highGain { "Gain" };
    std::vector<juce::Rectangle<int>> dividers;
};

/** The channel's compressor: its curve, six knobs and a gain-reduction meter. */
class CompModule final : public Module
{
public:
    explicit CompModule (juce::AudioProcessorValueTreeState&);
    ~CompModule() override;

    void attach (params::ChannelId);
    void pushReduction (float db)            { meter.push (db); }
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class TransferGraph;

    juce::AudioProcessorValueTreeState& state;
    params::ChannelId channel;
    std::unique_ptr<TransferGraph> graph;
    ParamToggle on { "On", "Compressor on/off" };
    juce::TextButton reset { "Reset" };
    Knob threshold { "Threshold" }, ratio { "Ratio" }, attack { "Attack" }, release { "Release" }, makeup { "Makeup" }, mix { "Mix" };
    ReductionMeter meter;
};

/** Two knobs and an On button: the transient shaper, or the saturation. */
class TwoKnobModule final : public Module
{
public:
    enum class Kind { transient, saturation };

    TwoKnobModule (juce::AudioProcessorValueTreeState&, Kind);

    void attach (params::ChannelId);
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& state;
    const Kind kind;
    ParamToggle on;
    Knob first, second;
};

/** Where the channel goes: its sends to the three FX returns (before or
    after the fader) and its output, and optionally (on the Edit page) its
    fader and pan. A send's name opens its effect on the FX page. */
class RoutingModule final : public Module
{
public:
    RoutingModule (juce::AudioProcessorValueTreeState&, bool withFaderAndPan);

    /** Called with a params::FxReturn when a send's name is clicked. */
    std::function<void (params::FxReturn)> onOpenReturn;

    void attach (params::ChannelId);
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    juce::AudioProcessorValueTreeState& state;
    const bool withFader;
    Knob fader { "Channel", Knob::Style::labelled, Knob::Size::large }, pan { "Pan", Knob::Style::labelled, Knob::Size::large };
    std::array<Knob, params::numFxReturns> sends { Knob { "Room" }, Knob { "Plate" }, Knob { "Delay" } };
    ParamToggle pre { "Pre", "Sends before the fader: they don't follow it" };
    ParamCombo output;
};

} // namespace lithium
