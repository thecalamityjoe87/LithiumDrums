#pragma once

#include "Kit.h"
#include "dsp/Svf.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>

namespace lithium
{

/** Everything needed to start one voice: one hit (one round-robin of one
    velocity zone) played through all of its mics, and how to shape it. */
struct VoiceTrigger
{
    int articulation = -1;
    int hit = -1;
    float gain = 1.0f;           // velocity, zone and crossfade gain combined
    double pitchRatio = 1.0;     // source frames consumed per output frame, before the pitch envelope
    float pitchEnvSemitones = 0.0f;   // the pitch envelope's starting offset
    float pitchEnvDecay = 0.0f;       // per-sample multiplier that takes the offset back to 0
    int attackSamples = 0;
    int holdSamples = 0;

    // A strike position the kit has no recordings of, made by filtering this
    // one hit (see DrumEngine::positionTone): a low shelf and a high shelf.
    // Pass-through by default.
    std::array<dsp::SvfCoefficients, 2> positionTone {};
};

/** Per-block values shared by all voices. */
struct VoiceRenderContext
{
    // Per piece: gain for each mic role on the overhead and room channels.
    std::array<std::array<float, numMicRoles>, maxPieces> roleGains;
    // Per piece and mic: left and right gains into the piece's own channel,
    // for the mics that feed it (close mics). Includes the mic's trim and pan.
    std::array<std::array<std::array<float, 2>, maxMics>, maxPieces> directGains;
    // Per piece: per-sample multiplier for the decay envelope (1 = off).
    std::array<float, maxPieces> decayCoefficient;
    // Per piece: the tone filters (pass-through when off).
    std::array<dsp::SvfCoefficients, maxPieces> toneHighPass, toneLowPass;
    // Mic emulation (see MicEmulation.h): per piece, how loud it goes into
    // the emulated mics' feed (0 while no emulated mic is listening), and
    // per mic, the kit's own level for it.
    std::array<float, maxPieces> emulationLevel {};
    std::array<float, maxMics> emulationMicGain {};
    // Engine-owned scratch: source frames for the resampler (maxSourceFrames
    // per channel), one stream's output before it's mixed, and the envelope.
    std::array<float*, maxChannelsPerMic> scratch {};
    std::array<float*, maxChannelsPerMic> streamOut {};
    float* envelope = nullptr;
};

/** The two buses voices render into: the overhead and room mic channels
    (one bus channel per mic channel, laid out as in Kit::mics), and the
    stereo channel of each piece (channels 2p and 2p + 1). */
struct VoiceBuses
{
    juce::AudioBuffer<float>& mics;
    juce::AudioBuffer<float>& pieces;

    // The emulated mics' feed: one mono channel per piece (channel p), of
    // the mics PieceData::emulationSources names. Null when nothing reads it.
    juce::AudioBuffer<float>* emulation = nullptr;
};

/** One sounding drum hit.

    A voice plays every mic recording of its hit together, from one shared
    playback position. That keeps the mics sample-aligned (phase-coherent),
    which matters as soon as two mics of the same drum are mixed. Close mics
    go into the drum's own channel; overhead and room mics into theirs.

    Voices live in a fixed pool owned by DrumEngine and are reused, never
    allocated. All methods run on the audio thread. */
class Voice
{
public:
    // The resampler uses 32.32 fixed-point positions so that every mic
    // consumes exactly the same number of source frames per block.
    static constexpr int maxPitchRatio = 8;

    // While the pitch envelope moves, the playback rate is updated this often.
    static constexpr int pitchEnvelopeStep = 32;

    void start (Kit&, int piece, std::uint32_t chokeGroupMask, const VoiceTrigger&, std::uint64_t serial) noexcept;

    /** Adds this voice's output into the buses. numSamples is at most the
        engine's slice size. */
    void render (Kit&, VoiceBuses, int startSample, int numSamples, const VoiceRenderContext&) noexcept;

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
        int mic = 0;
        int numChannels = 0;
        int busChannel = 0;
        MicRole role = MicRole::close;
        bool direct = true;
        std::int64_t numFrames = 0;
        // The four-sample window the Hermite interpolator works on, per channel:
        // the frames at x-1, x0, x1, x2 around the current position.
        std::array<std::array<float, 4>, maxChannelsPerMic> window {};
        float sourceGain = 1.0f;                                          // the piece's PieceData::micGain for this mic
        std::array<float, 2> lastGain {};                                 // left and right, as last rendered
        bool feedsEmulation = false;                                      // one of its piece's emulationSources
        float lastEmulationGain = 0.0f;
        std::array<std::array<dsp::SvfState, maxChannelsPerMic>, 2> tone {};   // [high-pass, low-pass][channel]
        std::array<std::array<dsp::SvfState, maxChannelsPerMic>, 2> position {};   // [low shelf, high shelf][channel]
    };

    void closeStreams (Kit&) noexcept;
    std::array<float, 2> targetGains (const MicStream&, const VoiceRenderContext&) const noexcept;
    float emulationGain (const MicStream&, const VoiceRenderContext&) const noexcept;

    std::array<MicStream, maxMics> streams;
    int numStreams = 0;

    bool active = false;
    int piece = 0;
    int articulation = -1;
    std::uint32_t chokeGroups = 0;
    std::uint64_t serial = 0;

    double baseRatio = 1.0;
    std::uint64_t increment = 0;          // pitch ratio in 32.32 fixed point, without the pitch envelope
    std::uint32_t fraction = 0;           // position between window[1] and window[2]
    std::int64_t nextFrame = 0;           // next source frame to read (same for every stream)
    std::int64_t longestStream = 0;
    float pitchOffset = 0.0f, pitchDecay = 0.0f;
    std::array<dsp::SvfCoefficients, 2> positionTone {};

    float gain = 1.0f;
    float decayEnvelope = 1.0f;
    int age = 0;                          // samples played (stops counting once past the hold)
    int attackSamples = 0, holdSamples = 0;
    float fadeGain = 1.0f;
    float fadeStep = 0.0f;
    bool gainsSet = false;                // whether the streams' lastGain are valid yet
};

} // namespace lithium
