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

// The reverb bus.
inline constexpr auto reverbReturn = "reverb_return";
inline constexpr auto reverbMute = "reverb_mute";
inline constexpr auto reverbOutput = "reverb_output";
inline constexpr auto reverbSize = "reverb_size";
inline constexpr auto reverbDecay = "reverb_decay";
inline constexpr auto reverbPreDelay = "reverb_predelay";
inline constexpr auto reverbDamping = "reverb_damping";
inline constexpr auto reverbLowCut = "reverb_lowcut";
inline constexpr auto reverbWidth = "reverb_width";

// Shaping a piece's hits: append to pieceParamId().
inline constexpr std::string_view dynamics = "dynamics", pitchEnv = "pitch_env", pitchEnvTime = "pitch_env_time";
inline constexpr std::string_view toneHighPass = "tone_hp", toneLowPass = "tone_lp";
inline constexpr std::string_view attack = "attack", hold = "hold";

// A mixer channel (a piece's, or an overhead or room mic's): append to
// ChannelId::param(). A piece's channel fader is "fader" (its "volume" is the
// whole drum); a mic's is its "volume".
inline constexpr std::string_view output = "output", reverbSend = "reverb_send";
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

/** Which mixer channel a set of channel parameters belongs to. */
struct ChannelId
{
    bool isPiece = false;
    int slot = -1;

    bool isValid() const noexcept                       { return slot >= 0; }
    juce::String param (std::string_view name) const    { return isPiece ? pieceParamId (slot, name) : micParamId (slot, name); }
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
    the kit's own choke notes. The reverb bus and master keep their
    settings. Message thread. */
void resetForKit (juce::AudioProcessorValueTreeState&, const KitInfo&);

/** After a reload that added or removed pieces (filling or emptying a Kit
    tab slot), moves each piece's settings to its new place: from[n] is the
    index piece n had before, or -1 for a new piece, which starts at the
    defaults (with the kit's choke note). Places past the kit's last piece
    are reset too. Message thread. */
void movePieces (juce::AudioProcessorValueTreeState&, const KitInfo&, const std::vector<int>& from);

/** Puts one part of a channel back to its defaults. Message thread. */
void resetEq (juce::AudioProcessorValueTreeState&, ChannelId);
void resetCompressor (juce::AudioProcessorValueTreeState&, ChannelId);

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

private:
    using Ref = std::atomic<float>*;

    struct ChannelRefs
    {
        Ref output, reverbSend;
        Ref eqOn, hpf, lowFreq, lowGain, mid1Freq, mid1Gain, mid1Q, mid2Freq, mid2Gain, mid2Q, highFreq, highGain;
        Ref compOn, threshold, ratio, attack, release, makeup, mix;
        Ref satOn, satDrive, satMix, transientOn, transientAttack, transientSustain;
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

    struct ReverbRefs
    {
        Ref returnLevel, mute, output, size, decay, preDelay, damping, lowCut, width;
    };

    static ChannelRefs findChannel (juce::AudioProcessorValueTreeState&, ChannelId);
    static void readChannel (const ChannelRefs&, ChannelParams&) noexcept;

    std::array<PieceRefs, maxPieces> pieces {};
    std::array<MicRefs, maxMics> mics {};
    ReverbRefs reverb {};
    Ref master = nullptr;
    Ref crossfade = nullptr;
};

} // namespace lithium::params
