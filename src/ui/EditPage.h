#pragma once

#include "ChannelModules.h"
#include "MicsModule.h"
#include "ModelPicker.h"
#include "PluginProcessor.h"
#include "ZoneMap.h"

namespace lithium
{

/** The Edit tab: everything about one drum (or one overhead or room mic
    channel), as a rack of modules.

    On top, a piece card holds a big picture of the drum (in the finish of the
    kit it was recorded in; click it to play the drum, as on the Kit tab) with
    the controls that pick it: the drum, its model, its articulation, and
    Strike, its choke note, mute and solo. Beside it, the Zones
    module shows the articulation's velocity zones, lights up the round-robin
    that just played, and has the crossfade switch. The first row below shapes
    each hit: velocity, pitch, tone, the volume envelope, and the transient
    shaper and saturation. The second row is the drum's mics (how loud it is in
    the close, overhead and room mics) and its compressor. Its EQ, fader, pan,
    sends and output are in the Mixer.

    The drum menu also lists the kit's overhead and room channels. One of
    those shows its mics' picture, mute and solo; its transient shaper and
    saturation where the zones are; and below, the Mics card's whole-kit view
    (how loud every drum is in those mics) beside its compressor.

    It shares the selected piece with the Kit tab (a mic channel is this
    page's own choice), and the channel controls are the same parameters as
    the channel's strip in the Mixer. */
class EditPage final : public juce::Component,
                       private juce::ChangeListener,
                       private SelectionModel::Listener,
                       private juce::Timer
{
public:
    explicit EditPage (LithiumDrumsProcessor&);
    ~EditPage() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    class EnvelopeGraph;
    class PiecePicture;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void selectedPieceChanged (int) override;
    void timerCallback() override;
    void updateKitInfo();
    void showPiece (int);
    void showMic (int slot);
    void showArticulation (int);
    void stepPiece (int delta);
    const KitPieceInfo* currentPiece() const;
    std::vector<int> micChannels() const;   // the kit's overhead and room channels (mic slots), in order

    static constexpr int micMenuId = 1000;  // pieceBox ids: drum p is p + 1, mic slot m is micMenuId + m + 1

    LithiumDrumsProcessor& processor;
    std::shared_ptr<const KitInfo> shownKit;   // keeps the KitInfo the zone map points into alive
    int piece = -1, articulation = 0;
    int micChannel = -1;                       // the mic slot shown instead of a drum, or -1
    std::uint32_t seenHits = 0;
    juce::String lastHitText;

    // Piece card
    ChevronButton previous { "Previous drum", ChevronButton::Direction::left };
    ChevronButton next { "Next drum", ChevronButton::Direction::right };
    juce::ComboBox pieceBox, articulationBox;
    std::unique_ptr<PiecePicture> picture;
    juce::Rectangle<int> pieceCardArea;
    ModelButton model;
    StrikeButton strike;
    ParamToggle crossfade { "Crossfade", "Crossfade between neighbouring velocity zones, so there's no step between them" };
    ParamToggle mute { "M", "Mute this drum" }, solo { "S", "Solo this drum" };
    juce::Label chokeLabel { {}, "CHOKE" };
    ParamCombo choke;

    // Shaping each hit
    Module velocityModule { "Velocity" }, pitchModule { "Pitch" }, toneModule { "Tone" }, envelopeModule { "Envelope" };
    Knob volume { "Volume" }, dynamics { "Dynamics" };
    Knob tune { "Tune" }, pitchEnv { "Bend" }, pitchEnvTime { "Bend time" };
    Knob toneHighPass { "Low cut" }, toneLowPass { "High cut" };
    std::unique_ptr<EnvelopeGraph> envelope;
    Knob attack { "Attack" }, hold { "Hold" }, decay { "Decay" };
    TwoKnobModule transient, saturation;

    // The drum's channel
    MicsModule mics;
    CompModule comp;

    // Zones
    Module zonesModule { "Zones" };
    ZoneMap zoneMap;
};

} // namespace lithium
