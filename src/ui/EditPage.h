#pragma once

#include "ChannelModules.h"
#include "ModelPicker.h"
#include "PluginProcessor.h"
#include "ZoneMap.h"

namespace lithium
{

/** The Edit tab: everything about one drum, as a rack of modules.

    A piece bar on top picks the drum and its articulation. The first row
    shapes each hit: velocity, pitch, tone, the volume envelope and how much
    of the drum each kind of mic hears. The second row is the drum's own
    mixer channel: EQ, transient shaper, compressor, saturation, and its
    fader, pan, reverb send and output. A thin strip at the bottom shows the
    articulation's velocity zones and lights up the round-robin that just played.

    It shares the selected piece with the Kit tab, and the channel controls
    are the same parameters as the drum's strip in the Mixer. */
class EditPage final : public juce::Component,
                       private juce::ChangeListener,
                       private SelectionModel::Listener,
                       private juce::Timer
{
public:
    explicit EditPage (LithiumDrumsProcessor&);
    ~EditPage() override;

    /** Asks for an FX return's effect to be shown on the FX page (a send's name was clicked). */
    std::function<void (params::FxReturn)> onOpenReturn;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    class SendRow;
    class EnvelopeGraph;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void selectedPieceChanged (int) override;
    void timerCallback() override;
    void updateKitInfo();
    void showPiece (int);
    void showArticulation (int);
    void stepPiece (int delta);
    const KitPieceInfo* currentPiece() const;

    LithiumDrumsProcessor& processor;
    std::shared_ptr<const KitInfo> shownKit;   // keeps the KitInfo the zone map points into alive
    int piece = -1, articulation = 0;
    std::uint32_t seenHits = 0;
    juce::String lastHitText;

    // Piece bar
    ChevronButton previous { "Previous drum", ChevronButton::Direction::left };
    ChevronButton next { "Next drum", ChevronButton::Direction::right };
    juce::ComboBox pieceBox, articulationBox;
    ModelButton model;
    std::vector<std::unique_ptr<juce::TextButton>> tabs;
    juce::TextButton play { "Play" };
    ParamToggle crossfade { "Crossfade", "Crossfade between neighbouring velocity zones, so there's no step between them" };
    ParamToggle mute { "M", "Mute this drum" }, solo { "S", "Solo this drum" };

    // Shaping each hit
    Module velocityModule { "Velocity" }, pitchModule { "Pitch" }, toneModule { "Tone" }, envelopeModule { "Envelope" },
        micsModule { "Mics" };
    Knob volume { "Volume" }, dynamics { "Dynamics" };
    Knob tune { "Tune" }, pitchEnv { "Bend" }, pitchEnvTime { "Bend time" };
    Knob toneHighPass { "Low cut" }, toneLowPass { "High cut" };
    std::unique_ptr<EnvelopeGraph> envelope;
    Knob attack { "Attack" }, hold { "Hold" }, decay { "Decay" };
    std::array<std::unique_ptr<SendRow>, numMicRoles> sends;
    juce::Label chokeLabel { {}, "CHOKE NOTE" };
    ParamCombo choke;

    // The drum's channel
    EqModule eq;
    CompModule comp;
    TwoKnobModule transient, saturation;
    RoutingModule routing;

    // Zones
    Module zonesModule { "Zones" };
    ZoneMap zoneMap;
};

} // namespace lithium
