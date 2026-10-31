#pragma once

#include "EngineParams.h"
#include "Kit.h"
#include "Voice.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace lithium
{

/** Lock-free counters the audio thread publishes for the UI. */
struct EngineFeedback
{
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

    /** Mixes all voices into micBus (the kit's mic bus channels). numSamples
        may be any length; it is rendered in slices of at most maxSliceSize. */
    void render (Kit&, juce::AudioBuffer<float>& micBus, int startSample, int numSamples, const EngineParams&) noexcept;

    void fadeOutAll (double seconds) noexcept;
    void stopAll (Kit&) noexcept;
    int getNumActiveVoices() const noexcept;

private:
    int findZone (const Kit&, const Articulation&, int velocity) const noexcept;
    void startVoice (Kit&, const Articulation&, int articulationIndex, int zoneIndex, float gain,
                     double pitchRatio) noexcept;
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
    std::vector<float> envelope;           // per-sample envelope for one voice
    VoiceRenderContext context;
};

} // namespace lithium
