#pragma once

#include "EngineParams.h"
#include "Kit.h"
#include "Voice.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <optional>

namespace lithium
{

/** What played on a piece's latest hit. Packed into one 64-bit atomic in
    EngineFeedback, so the UI always reads a consistent set of values. */
struct LastHit
{
    int articulation = -1;      // index into the kit's articulations
    int zone = 0;               // within the articulation, 0 = the softest
    int roundRobin = 0;         // within the zone
    int velocity = 0;

    std::uint64_t pack() const noexcept
    {
        return (std::uint64_t) (velocity & 0xff)
             | (std::uint64_t) (zone & 0xffff) << 8
             | (std::uint64_t) (roundRobin & 0xffff) << 24
             | (std::uint64_t) (articulation & 0xffff) << 40
             | 1ull << 63;      // "valid": 0 means no hit yet
    }

    static std::optional<LastHit> unpack (std::uint64_t bits) noexcept
    {
        if ((bits >> 63) == 0)
            return std::nullopt;

        return LastHit { (int) ((bits >> 40) & 0xffff), (int) ((bits >> 8) & 0xffff),
                         (int) ((bits >> 24) & 0xffff), (int) (bits & 0xff) };
    }
};

/** Lock-free counters the audio thread publishes for the UI. */
struct EngineFeedback
{
    // Per piece, the latest hit (LastHit::pack). Written before pieceHits is
    // incremented, so a UI that sees a new count also sees its hit.
    std::array<std::atomic<std::uint64_t>, maxPieces> lastHit {};

    // Incremented on every hit of a piece; the UI flashes the piece when it
    // sees the count change. Counters (rather than flags) mean no hit is lost
    // even if the UI polls slower than notes arrive.
    std::array<std::atomic<std::uint32_t>, maxPieces> pieceHits {};   // by piece index in the current kit
    std::atomic<int> activeVoices { 0 };
};

/** The voice pool and everything that decides what plays.

    Owns no kit: the processor passes the current kit in on every call, and
    only swaps kits once all voices are silent (see KitHandoff). Every method
    except prepare() runs on the audio thread and never allocates. */
class DrumEngine
{
public:
    static constexpr int numVoices = 64;
    static constexpr int voiceStealThreshold = numVoices - 8;
    static constexpr int maxVoicesPerPiece = 12;
    static constexpr int maxSliceSize = 256;    // longest stretch rendered in one go

    explicit DrumEngine (EngineFeedback&);

    /** Allocates scratch memory. Not real-time safe. */
    void prepare (double sampleRate);

    void noteOn (Kit&, int note, int velocity, const EngineParams&) noexcept;
    void polyAftertouch (Kit&, int note, int pressure) noexcept;
    void trigger (Kit&, int articulation, int velocity, const EngineParams&) noexcept;

    /** Mixes all voices into the buses: close mics into the pieces' channels,
        overhead and room mics into the mic bus. numSamples may be any
        length; it is rendered in slices of at most maxSliceSize. */
    void render (Kit&, VoiceBuses, int startSample, int numSamples, const EngineParams&) noexcept;

    void fadeOutAll (double seconds) noexcept;
    void stopAll (Kit&) noexcept;
    int getNumActiveVoices() const noexcept;

private:
    int findZone (const Kit&, const Articulation&, int velocity) const noexcept;
    /** Returns the round-robin that was started (within its zone), or -1 if
        the pool had no free voice. */
    int startVoice (Kit&, const Articulation&, int articulationIndex, int zoneIndex, float gain,
                    double pitchRatio, const PieceParams& shaping) noexcept;
    Voice* allocateVoice (int piece) noexcept;
    void chokePiece (int piece, double seconds) noexcept;
    void updateRenderContext (const Kit&, const EngineParams&) noexcept;
    int secondsToSamples (double seconds) const noexcept;

    EngineFeedback& feedback;
    std::array<Voice, numVoices> voices;
    std::uint64_t nextSerial = 0;
    double sampleRate = 44100.0;
    juce::Random random;

    juce::AudioBuffer<float> scratch;      // resampler source frames
    juce::AudioBuffer<float> streamOut;    // one stream's output before it's mixed
    std::vector<float> envelope;           // per-sample envelope for one voice
    VoiceRenderContext context;
};

} // namespace lithium
