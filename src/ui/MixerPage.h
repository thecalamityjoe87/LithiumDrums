#pragma once

#include "ChannelModules.h"
#include "PluginProcessor.h"

namespace lithium
{

/** One fader strip in the Mixer page's bank: a drum's channel, an overhead
    or room mic's channel, a close mic's trim, an FX return or the master.
    Clicking a channel shows it in the inspector; clicking a return opens
    its effect on the FX page.

    Every strip is the same fixed stack, top to bottom: name and colour dot;
    the insert switches (EQ, compressor, transient, saturation); the three
    sends (Room, Plate, Delay), whose names open their effect; the Pre
    switch and pan; the fader beside its meter, with a dB scale and readout;
    then mute, solo and the output. Strips without some of these leave
    their rows empty, so every fader in the bank lines up. The fader's
    travel is fixed by the theme (theme::size::faderTravel), never stretched. */
class MixerStrip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Kind { piece, mic, trim, fxReturn, master };

    /** slot: the piece or mic, or for a return its params::FxReturn. */
    MixerStrip (LithiumDrumsProcessor&, Kind, int slot = -1, const KitInfo* = nullptr);

    std::function<void()> onSelect;
    std::function<void (params::FxReturn)> onOpenReturn;

    const Kind kind;
    const int slot;

    void setSelected (bool);
    void pushLevels (float left, float right);

    /** The height the stack needs: the bank is this tall. */
    static int getContentHeight();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> getReadoutArea() const;
    juce::Rectangle<int> getSendCaption (int index) const;
    bool hasInserts() const noexcept     { return kind == Kind::piece || kind == Kind::mic; }
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    juce::String name, tag;
    juce::Colour dot;
    bool selected = false, silent = false;
    int hoveredCaption = -1;

    ControlSlider fader;
    LevelMeter meter;
    std::array<Knob, params::numFxReturns> sends { Knob { {}, Knob::Style::compact }, Knob { {}, Knob::Style::compact },
                                                    Knob { {}, Knob::Style::compact } };
    Knob pan { {}, Knob::Style::compact };
    ParamToggle pre { "Pre", "Sends before the fader" };
    ParamToggle eq { "EQ", "EQ on/off" }, comp { "Cmp", "Compressor on/off" };
    ParamToggle transient { "Trn", "Transient shaper on/off" }, saturation { "Sat", "Saturation on/off" };
    ParamToggle mute { "M", "Mute" }, solo { "S", "Solo" };
    ParamCombo output;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> faderAttachment;   // after the fader it uses
};

/** The top of the Mixer page: the kit's master EQ, which stays put, and
    everything else about the selected channel (its transient shaper,
    compressor, saturation, sends and output). Those modules are re-attached
    to whichever channel is selected. A channel's own EQ is on the Edit page
    and the FX returns' own effects are on the FX page. */
class MixerInspector final : public juce::Component
{
public:
    explicit MixerInspector (LithiumDrumsProcessor&);
    ~MixerInspector() override;

    std::function<void (params::FxReturn)> onOpenReturn;

    void showChannel (params::ChannelId);
    void showNothing();

    void pushReduction (float db)            { comp.pushReduction (db); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Mode { none, channel };
    void setMode (Mode);

    LithiumDrumsProcessor& processor;
    Mode mode = Mode::none;

    EqModule eq;
    CompModule comp;
    TwoKnobModule transient, saturation;
    RoutingModule routing;
};

/** The Mixer tab: the inspector on top; below, a fader bank with a channel
    per drum, then the overhead and room mics, the three FX returns and the
    master. A second view of the bank trims the close mics that feed the
    drum channels. */
class MixerPage final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit MixerPage (LithiumDrumsProcessor&);
    ~MixerPage() override;

    /** Asks for an FX return's effect to be shown on the FX page. */
    std::function<void (params::FxReturn)> onOpenReturn;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateKitInfo();
    void rebuildStrips();
    void select (params::ChannelId);
    void updateSelection();
    void openReturn (params::FxReturn r)         { if (onOpenReturn) onOpenReturn (r); }

    LithiumDrumsProcessor& processor;
    std::shared_ptr<const KitInfo> shownKit;
    MixerInspector inspector;
    juce::TextButton channelsView { "Channels" }, trimsView { "Close mics" };
    bool showingTrims = false;
    juce::Viewport stripView;
    juce::Component stripRow;
    std::vector<std::unique_ptr<MixerStrip>> strips;
    std::vector<int> dividers;   // x positions in the row, between groups of strips
    std::array<std::unique_ptr<MixerStrip>, params::numFxReturns> returnStrips;
    MixerStrip masterStrip;
    juce::Rectangle<int> bankCard, bankHeader;   // the card holding the view switch and the whole bank; its title row
};

} // namespace lithium
