#pragma once

#include "Kit.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>

namespace lithium
{

/** Everything needed to start one voice: one hit (one round-robin of one
    velocity zone) played through all of its mics. */
struct VoiceTrigger
{
    int articulation = -1;
    int hit = -1;
    float gain = 1.0f;           // velocity, zone and crossfade gain combined
    double pitchRatio = 1.0;     // source frames consumed per output frame
};

/** Per-block values shared by all voices. */
struct VoiceRenderContext
{
    // Per piece: mix gain for each mic role (volume x mute/solo x send).
    std::array<std::array<float, numMicRoles>, maxPieces> roleGains;
    // Per piece: per-sample multiplier for the decay envelope (1 = off).
    std::array<float, maxPieces> decayCoefficient;
    int decayHoldSamples = 0;
    // Engine-owned scratch, at least maxSourceFrames long per channel.
    std::array<float*, maxChannelsPerMic> scratch {};
    float* envelope = nullptr;
};

/** One sounding drum hit.

    A voice plays every mic recording of its hit together, from one shared
    playback position. That keeps the mics sample-aligned (phase-coherent),
    which matters as soon as two mics of the same drum are mixed.

    Voices live in a fixed pool owned by DrumEngine and are reused, never
    allocated. All methods run on the audio thread. */
class Voice
{
public:
    // The resampler uses 32.32 fixed-point positions so that every mic
    // consumes exactly the same number of source frames per block.
    static constexpr int maxPitchRatio = 8;

    void start (Kit&, int piece, std::uint32_t chokeGroupMask, const VoiceTrigger&, std::uint64_t serial) noexcept;

    /** Adds this voice's output into the mic bus. */
    void render (Kit&, juce::AudioBuffer<float>& micBus, int startSample, int numSamples,
                 const VoiceRenderContext&) noexcept;

    /** Starts a short fade to silence, used for chokes and voice stealing. */
    void fadeOut (int numSamples) noexcept;

    /** Stops immediately. Only for when the output is already silent or muted. */
    void stop (Kit&) noexcept;

    bool isActive() const noexcept                   { return active; }
    bool isFadingOut() const noexcept                { return fadeStep > 0.0f; }
    int getPiece() const noexcept                    { return piece; }
    int getArticulation() const noexcept             { return articulation; }
    std::uint32_t getChokeGroups() const noexcept    { return chokeGroups; }
    std::uint64_t getSerial() const noexcept         { return serial; }

    /** A rough loudness estimate for picking which voice to steal: the
        current envelope gain, weighted by how far through the sample we are
        (sample tails are quiet). */
    float getStealScore() const noexcept;

private:
    struct MicStream
    {
        SampleCursor cursor;
        int numChannels = 0;
        int busChannel = 0;
        MicRole role = MicRole::close;
        std::int64_t numFrames = 0;
        // The four-sample window the Hermite interpolator works on, per channel:
        // the frames at x-1, x0, x1, x2 around the current position.
        std::array<std::array<float, 4>, maxChannelsPerMic> window {};
    };

    void closeStreams (Kit&) noexcept;

    std::array<MicStream, maxMics> streams;
    int numStreams = 0;

    bool active = false;
    int piece = 0;
    int articulation = -1;
    std::uint32_t chokeGroups = 0;
    std::uint64_t serial = 0;

    std::uint64_t increment = 0;          // pitch ratio in 32.32 fixed point
    std::uint32_t fraction = 0;           // position between window[1] and window[2]
    std::int64_t nextFrame = 0;           // next source frame to read (same for every stream)
    std::int64_t longestStream = 0;

    float gain = 1.0f;
    float decayEnvelope = 1.0f;
    int samplesPlayed = 0;
    float fadeGain = 1.0f;
    float fadeStep = 0.0f;
    std::array<float, numMicRoles> lastRoleGains {};
};

} // namespace lithium
