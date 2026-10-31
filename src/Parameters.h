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

/** Puts a kit's pieces and mics back to their neutral settings: 0 dB,
    centred, unmuted, natural decay, full sends, and the kit's default choke
    notes. Message thread. */
void resetForKit (juce::AudioProcessorValueTreeState&, const KitInfo&);

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
    struct PieceRefs
    {
        std::atomic<float>* volume;
        std::atomic<float>* tune;
        std::atomic<float>* decay;
        std::atomic<float>* mute;
        std::atomic<float>* solo;
        std::array<std::atomic<float>*, numMicRoles> sends;
        std::atomic<float>* chokeNote;
    };

    struct MicRefs
    {
        std::atomic<float>* volume;
        std::atomic<float>* pan;
        std::atomic<float>* mute;
        std::atomic<float>* solo;
        std::atomic<float>* output;
    };

    std::array<PieceRefs, maxPieces> pieces {};
    std::array<MicRefs, maxMics> mics {};
    std::atomic<float>* master = nullptr;
    std::atomic<float>* crossfade = nullptr;
};

} // namespace lithium::params
