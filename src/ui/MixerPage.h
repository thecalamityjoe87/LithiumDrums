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
    the insert switches (EQ, compressor, transient, saturation), where a trim
    or an FX return has its pan (a drum's or mic's is in the inspector's
    Channel card); the fader beside its meter, with a dB scale and readout;
    then mute and solo at the bottom. Outputs aren't on the strips: a drum's
    or mic's is in the inspector's Channel card, a return's on its card on
    the FX page. Strips without some of these leave their rows empty, so
    every fader in the bank lines up. The fader's travel is fixed (the
    theme's, plus the rows the strips no longer need), never stretched. */
class MixerStrip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Kind { piece, mic, trim, fxReturn, master };

    /** slot: the piece or mic, or for a return its params::FxReturn. */
    MixerStrip (LithiumDrumsProcessor&, Kind, int slot = -1, const KitInfo* = nullptr);

    std::function<void()> onSelect;

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
    bool hasInserts() const noexcept     { return kind == Kind::piece || kind == Kind::mic; }

    juce::String name, tag;
    juce::Colour dot;
    bool selected = false, silent = false;

    ControlSlider fader;
    LevelMeter meter;
    Knob pan { {}, Knob::Style::compact };
    ParamToggle eq { "EQ", "EQ on/off" }, comp { "Cmp", "Compressor on/off" };
    ParamToggle transient { "Trn", "Transient shaper on/off" }, saturation { "Sat", "Saturation on/off" };
    ParamToggle mute { "M", "Mute" }, solo { "S", "Solo" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> faderAttachment;   // after the fader it uses
};

/** The top of the Mixer page: the selected channel's EQ, and its fader, pan,
    sends and output. The modules are re-attached to whichever channel is
    selected. A drum's compressor, transient shaper, saturation and mics are
    on the Edit page; the master EQ and the FX returns' effects are on the
    FX page. */
class MixerInspector final : public juce::Component
{
public:
    explicit MixerInspector (LithiumDrumsProcessor&);
    ~MixerInspector() override;

    std::function<void (params::FxReturn)> onOpenReturn;

    /** name: the channel's, for the cards' titles. */
    void showChannel (params::ChannelId, const juce::String& name);
    void showNothing();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Mode { none, channel };
    void setMode (Mode);

    Mode mode = Mode::none;

    LithiumDrumsProcessor& processor;
    EqModule eq;
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
    HelpButton stripsHelp { "Channel strips", {
        "One strip per drum, then the overhead and room mics, the Room, Plate and Delay returns, and the master. "
        "Click a drum's or mic's strip to show its EQ and Channel cards above; click a return to open its effect "
        "on the FX page.",
        "The fader sets the channel's level; M mutes it and S solos it. EQ, CMP, TRN and SAT switch its EQ, "
        "compressor, transient shaper and saturation on and off. The EQ is set above, the others on the Edit page.",
        "Close mics shows the close mic trims instead: how loud each close mic is, and where it sits, in the drum "
        "channels it feeds." } };
    bool showingTrims = false;
    std::array<bool, 2> shownEmulation {};   // overheads, room: whether their emulated strips were shown
    juce::Viewport stripView;
    juce::Component stripRow;
    std::vector<std::unique_ptr<MixerStrip>> strips;
    std::vector<int> dividers;   // x positions in the row, between groups of strips
    std::array<std::unique_ptr<MixerStrip>, params::numFxReturns> returnStrips;
    MixerStrip masterStrip;
    juce::Rectangle<int> bankCard, bankHeader;   // the card holding the view switch and the whole bank; its title row
};

} // namespace lithium
