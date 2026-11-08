#pragma once

#include "engine/EngineParams.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <mutex>

// Parameter IDs and layout. Every user-facing setting is an APVTS parameter,
// so hosts can automate it and it's saved with the project.
//
// Hosts need a fixed list of parameters, but kits differ in how many pieces
// and mics they have. So there is a fixed set of slots (piece 1-24, mic 1-24)
// and the loaded kit fills them in order. Slot parameters take their names
// from the kit ("Snare Volume", "Overheads Pan") so automation lanes stay
// readable.
//
// IDs are part of saved projects: renaming one breaks automation and state
// in existing projects, so treat them as permanent once released.

namespace lithium::params
{

juce::String pieceParamId (int slot, std::string_view name);   // e.g. "piece3_volume" (slot is 0-based)
juce::String micParamId (int slot, std::string_view name);     // e.g. "mic5_pan"

inline constexpr auto masterVolume = "master_volume";
inline constexpr auto velocityCrossfade = "velocity_crossfade";

// Mic emulation (see engine/MicEmulation.h): overheads and a room for a kit
// recorded without them, each switched on by the user, and which built-in
// room the emulated room is in. Like the mic slots' parameters they belong
// to the kit, so a kit change resets them (switched off, and the kit's own
// room choice). Their mixer channels are the mic slots after the kit's own.
inline constexpr auto emulateOverheads = "mic_emulate_overheads";
inline constexpr auto emulateRoom = "mic_emulate_room";
inline constexpr auto emulatedRoomIr = "mic_emulated_room";

/** The on/off parameter of a role's emulated mic (overheads or room). */
inline const char* emulateParam (MicRole r) noexcept   { return r == MicRole::room ? emulateRoom : emulateOverheads; }

// The FX returns. Every mixer channel has a send to each; a channel's
// sends are post-fader unless its "send_pre" is on.
//
//  - Room: convolution with a measured (or synthesized) space ("room_*").
//  - Plate: the algorithmic reverb. It was the mixer's only reverb before
//    the FX page, so it keeps that reverb's IDs ("reverb_*", and each
//    channel's "reverb_send"): projects and presets from then load with
//    their sends and settings as they were, with no migration.
//  - Delay ("delay_*").
inline constexpr auto reverbReturn = "reverb_return";
inline constexpr auto reverbMute = "reverb_mute";
inline constexpr auto reverbOutput = "reverb_output";
inline constexpr auto reverbPan = "reverb_pan";
inline constexpr auto reverbMix = "reverb_mix";
inline constexpr auto reverbSize = "reverb_size";
inline constexpr auto reverbDecay = "reverb_decay";
inline constexpr auto reverbPreDelay = "reverb_predelay";
inline constexpr auto reverbDamping = "reverb_damping";
inline constexpr auto reverbLowCut = "reverb_lowcut";
inline constexpr auto reverbWidth = "reverb_width";

inline constexpr auto roomReturn = "room_return";
inline constexpr auto roomMute = "room_mute";
inline constexpr auto roomOutput = "room_output";
inline constexpr auto roomPan = "room_pan";
inline constexpr auto roomMix = "room_mix";
inline constexpr auto roomIr = "room_ir";
inline constexpr auto roomPreDelay = "room_predelay";
inline constexpr auto roomDecay = "room_decay";
inline constexpr auto roomLowCut = "room_lowcut";
inline constexpr auto roomHighCut = "room_highcut";
inline constexpr auto roomWidth = "room_width";

inline constexpr auto delayReturn = "delay_return";
inline constexpr auto delayMute = "delay_mute";
inline constexpr auto delayOutput = "delay_output";
inline constexpr auto delayPan = "delay_pan";
inline constexpr auto delayMix = "delay_mix";
inline constexpr auto delaySync = "delay_sync";
inline constexpr auto delayNote = "delay_note";
inline constexpr auto delayTime = "delay_time";
inline constexpr auto delayFeedback = "delay_feedback";
inline constexpr auto delayLowCut = "delay_lowcut";
inline constexpr auto delayHighCut = "delay_highcut";
inline constexpr auto delayPingPong = "delay_pingpong";

/** The three returns, in the order the UI shows them. */
enum class FxReturn { room, plate, delay };
inline constexpr int numFxReturns = 3;

/** The master chain. Its EQ, glue compressor and transient shaper use the
    channel parameter names on ChannelId::master() ("master_eq_on",
    "master_comp_ratio", "master_trans_attack"); the rest are here. */
inline constexpr auto fxBypass = "fx_bypass";
inline constexpr auto crushOn = "master_crush_on", crushDrive = "master_crush_drive", crushBlend = "master_crush_blend";
inline constexpr auto crushAttack = "master_crush_attack", crushRelease = "master_crush_release";
inline constexpr auto tapeOn = "master_tape_on", tapeDrive = "master_tape_drive", tapeTone = "master_tape_tone";
inline constexpr auto tapeOutput = "master_tape_output";
inline constexpr auto limitOn = "master_limit_on", limitCeiling = "master_limit_ceiling", limitRelease = "master_limit_release";
inline constexpr auto limitLookahead = "master_limit_lookahead";

// Shaping a piece's hits: append to pieceParamId().
inline constexpr std::string_view dynamics = "dynamics", pitchEnv = "pitch_env", pitchEnvTime = "pitch_env_time";
inline constexpr std::string_view toneHighPass = "tone_hp", toneLowPass = "tone_lp";
inline constexpr std::string_view attack = "attack", hold = "hold";

// A mixer channel (a piece's, or an overhead or room mic's): append to
// ChannelId::param(). A piece's channel fader is "fader" (its "volume" is the
// whole drum); a mic's is its "volume".
inline constexpr std::string_view output = "output", reverbSend = "reverb_send", roomSend = "room_send", delaySend = "delay_send";
inline constexpr std::string_view sendPre = "send_pre";
inline constexpr std::string_view channelWidth = "width";
inline constexpr std::string_view eqOn = "eq_on", eqHighPass = "eq_hpf";
inline constexpr std::string_view eqLowFreq = "eq_low_freq", eqLowGain = "eq_low_gain";
inline constexpr std::string_view eqMid1Freq = "eq_mid1_freq", eqMid1Gain = "eq_mid1_gain", eqMid1Q = "eq_mid1_q";
inline constexpr std::string_view eqMid2Freq = "eq_mid2_freq", eqMid2Gain = "eq_mid2_gain", eqMid2Q = "eq_mid2_q";
inline constexpr std::string_view eqHighFreq = "eq_high_freq", eqHighGain = "eq_high_gain";
inline constexpr std::string_view compOn = "comp_on", compThreshold = "comp_threshold", compRatio = "comp_ratio";
inline constexpr std::string_view compAttack = "comp_attack", compRelease = "comp_release";
inline constexpr std::string_view compMakeup = "comp_makeup", compMix = "comp_mix";
inline constexpr std::string_view satOn = "sat_on", satDrive = "sat_drive", satMix = "sat_mix";
inline constexpr std::string_view transientOn = "trans_on", transientAttack = "trans_attack", transientSustain = "trans_sustain";

/** Which mixer channel a set of channel parameters belongs to: a drum's, an
    overhead or room mic's, or the master bus (whose EQ, compressor and
    transient shaper have the same parameters as a channel's, so the same
    UI modules edit them). */
struct ChannelId
{
    bool isPiece = false;
    int slot = -1;
    bool isMaster = false;

    static ChannelId master() noexcept                  { return { false, 0, true }; }

    bool isValid() const noexcept                       { return slot >= 0; }

    juce::String param (std::string_view name) const
    {
        if (isMaster)
            return "master_" + juce::String (name.data(), name.size());

        return isPiece ? pieceParamId (slot, name) : micParamId (slot, name);
    }

    juce::String fader() const                          { return param (isPiece ? "fader" : "volume"); }

    bool operator== (const ChannelId&) const = default;
};

/** The names of the output choices: "Main", then "Aux 1" to "Aux 16". */
juce::StringArray outputNames();

inline constexpr float minVolumeDb = -60.0f;   // the bottom of the fader means silence

/** The display names of the loaded kit's pieces and mics, which the slot
    parameters use as their names. Written on the message thread; read from
    whatever thread the host asks for parameter names on, hence the mutex
    (it's never touched by the audio thread). */
class SlotNames
{
public:
    void set (const KitInfo*);
    juce::String get (bool isMic, int slot) const;

private:
    mutable std::mutex mutex;
    std::array<juce::String, maxPieces> pieces;
    std::array<juce::String, maxMics> mics;
};

juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const SlotNames&);

/** Puts every piece and mic parameter back to its default (0 dB, centred,
    unmuted, no shaping, flat channels, no reverb, the main output), with
    the kit's own choke notes, and the Volume, Tune, Decay and Pan a kit
    preset starts its pieces and mics at (KitPieceInfo::startVolumeDb etc.).
    The reverb bus and master keep their settings. Message thread. */
void resetForKit (juce::AudioProcessorValueTreeState&, const KitInfo&);

/** Saves made before kit presets started the controls (projects before
    state version 2, preset files before format 2) played a kit preset's
    own changes inside the kit, under the controls. This adds them onto the
    saved values in a parameters' state (as the APVTS keeps it), so the save
    plays as it did and its controls show the real values. info is the kit
    the save plays, as loaded. Does nothing for a kit, whose starts are all
    neutral. */
void addKitStarts (juce::ValueTree& parameterState, const juce::AudioProcessorValueTreeState&, const KitInfo& info);

/** After a reload that added or removed pieces (filling or emptying a Kit
    tab slot), moves each piece's settings to its new place: from[n] is the
    index piece n had before, or -1 for a new piece, which starts at the
    defaults (with the kit's choke note and a kit preset's starts). Places
    past the kit's last piece are reset too. Message thread. */
void movePieces (juce::AudioProcessorValueTreeState&, const KitInfo&, const std::vector<int>& from);

/** Puts one part of a channel back to its defaults. Message thread. */
void resetEq (juce::AudioProcessorValueTreeState&, ChannelId);
void resetCompressor (juce::AudioProcessorValueTreeState&, ChannelId);

/** Whether a parameter belongs to the FX page (the returns and the master
    chain), for its A/B comparison and presets. */
bool isFxParameter (const juce::String& id);

/** Whether a role's emulated mic (overheads or room) is switched on. Any thread but the audio thread. */
bool isEmulating (const juce::AudioProcessorValueTreeState&, MicRole);

/** Switches a role's emulated mic on or off, as one gesture for the host. Message thread. */
void setEmulating (juce::AudioProcessorValueTreeState&, MicRole, bool);

/** Whether a mic slot has a channel to show: one of the kit's own mics, or
    an emulated one that's switched on. */
bool hasMicChannel (const juce::AudioProcessorValueTreeState&, const KitInfo&, int slot);

/** The names of the Room reverb's IR choices: the built-in IRs, then "Custom file". */
juce::StringArray roomIrNames();

/** Reads every parameter the audio thread needs into an EngineParams.

    The constructor looks parameters up by ID once (string lookups allocate),
    and keeps the raw std::atomic<float> pointers that APVTS exposes, so read()
    is a series of atomic loads: safe on the audio thread. */
class Snapshot
{
public:
    explicit Snapshot (juce::AudioProcessorValueTreeState&);

    void read (EngineParams&) const noexcept;

    /** Whether each mic slot is muted right now. */
    std::array<bool, maxMics> readMicMutes() const noexcept;

    /** Whether the limiter's lookahead is on (which sets the latency). */
    bool readLookahead() const noexcept;

private:
    using Ref = std::atomic<float>*;

    struct ChannelRefs
    {
        Ref output, reverbSend, roomSend, delaySend, sendPre;
        Ref eqOn, hpf, lowFreq, lowGain, mid1Freq, mid1Gain, mid1Q, mid2Freq, mid2Gain, mid2Q, highFreq, highGain;
        Ref compOn, threshold, ratio, attack, release, makeup, mix;
        Ref satOn, satDrive, satMix, transientOn, transientAttack, transientSustain, width;
    };

    struct PieceRefs
    {
        Ref volume, tune, decay, mute, solo, chokeNote;
        std::array<Ref, numMicRoles> sends;
        Ref dynamics, pitchEnv, pitchEnvTime, toneHighPass, toneLowPass, attack, hold;
        Ref fader, pan;
        ChannelRefs channel;
    };

    struct MicRefs
    {
        Ref volume, pan, mute, solo;
        ChannelRefs channel;
    };

    struct ReturnRefs
    {
        Ref returnLevel, mute, output, pan, mix;
    };

    struct ReverbRefs
    {
        ReturnRefs bus;
        Ref size, decay, preDelay, damping, lowCut, width;
    };

    struct RoomRefs
    {
        ReturnRefs bus;
        Ref preDelay, lowCut, highCut, width;
    };

    struct DelayRefs
    {
        ReturnRefs bus;
        Ref sync, note, time, feedback, lowCut, highCut, pingPong;
    };

    struct MasterRefs
    {
        Ref bypass;
        Ref eqOn, hpf, lowFreq, lowGain, mid1Freq, mid1Gain, mid1Q, mid2Freq, mid2Gain, mid2Q, highFreq, highGain;
        Ref compOn, threshold, ratio, attack, release, makeup, mix;
        Ref transientOn, transientAttack, transientSustain;
        Ref crushOn, crushDrive, crushBlend, crushAttack, crushRelease;
        Ref tapeOn, tapeDrive, tapeTone, tapeOutput;
        Ref limitOn, limitCeiling, limitRelease, limitLookahead;
    };

    static ChannelRefs findChannel (juce::AudioProcessorValueTreeState&, ChannelId);
    static ReturnRefs findReturn (juce::AudioProcessorValueTreeState&, const char* levelId, const char* muteId,
                                  const char* outputId, const char* panId, const char* mixId);
    static void readChannel (const ChannelRefs&, ChannelParams&) noexcept;
    static void readReturn (const ReturnRefs&, ReturnParams&) noexcept;

    std::array<PieceRefs, maxPieces> pieces {};
    std::array<MicRefs, maxMics> mics {};
    ReverbRefs reverb {};
    RoomRefs room {};
    DelayRefs delay {};
    MasterRefs masterChain {};
    Ref master = nullptr;
    Ref crossfade = nullptr;
    std::array<Ref, numMicRoles> emulate {};   // close is null
};

} // namespace lithium::params
