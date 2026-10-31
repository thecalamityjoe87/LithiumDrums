#pragma once

#include "EngineParams.h"
#include "Voice.h"
#include "dsp/ChannelChain.h"
#include "dsp/RoomReverb.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace lithium
{

/** Levels the mixer publishes for the meters, lock-free.

    Each value is the highest since the UI last looked: the audio thread
    raises it (raise), and the UI reads and clears it in one step (take).
    The race between the two can lose at most one block's peak, which a
    meter can't show anyway. */
struct MixerFeedback
{
    std::array<std::atomic<float>, maxPieces> piecePeak {};         // post-fader, linear
    std::array<std::atomic<float>, maxPieces> pieceReductionDb {};  // compressor, positive dB
    std::array<std::atomic<float>, maxMics> micPeak {};
    std::array<std::atomic<float>, maxMics> micReductionDb {};
    std::atomic<float> reverbPeak { 0.0f };
    std::array<std::atomic<float>, 2> masterPeak {};

    static void raise (std::atomic<float>& a, float value) noexcept
    {
        if (value > a.load (std::memory_order_relaxed))
            a.store (value, std::memory_order_relaxed);
    }

    static float take (std::atomic<float>& a) noexcept   { return a.exchange (0.0f, std::memory_order_relaxed); }
};

/** The mixer: a channel per drum, a channel per overhead and room mic, a
    reverb bus and the master.

    The voices render each hit's close mics into its drum's stereo channel
    (getPieceBus(), channels 2p and 2p + 1) and its overhead and room mics
    into those mics' channels (getMicBus()). Every channel then runs the same
    chain (EQ, transient shaper, compressor, saturation), its fader and pan,
    and goes to the main mix (through the master fader) or one of the aux
    outs, with a post-fader send to the shared reverb, whose return can go
    to the main mix or an aux out as well.

    process() runs on the audio thread and never allocates; the channels'
    processing works in place on the buses. */
class Mixer
{
public:
    // Aux outs the host hasn't enabled are null; anything routed to one
    // plays through the main out instead.
    using AuxOutputs = std::array<std::array<float*, 2>, numAuxOuts>;

    /** feedback may be null (the tests don't always need meters). */
    explicit Mixer (MixerFeedback* feedback = nullptr) : meters (feedback) {}

    /** Allocates the buses and the effects' buffers. Not real-time safe. */
    void prepare (double sampleRate, int maxBlockSize);

    int getMaxBlockSize() const noexcept                  { return micBus.getNumSamples(); }
    juce::AudioBuffer<float>& getMicBus() noexcept       { return micBus; }
    juce::AudioBuffer<float>& getPieceBus() noexcept     { return pieceBus; }
    VoiceBuses getBuses() noexcept                        { return { micBus, pieceBus }; }

    void clearBuses (int numSamples) noexcept
    {
        micBus.clear (0, numSamples);
        pieceBus.clear (0, numSamples);
    }

    /** Mixes the first numSamples of the buses into the outputs (adding to
        whatever is there). numSamples must not exceed getMaxBlockSize(). */
    void process (const Kit&, int numSamples, const EngineParams&, std::array<float*, 2> mainOut,
                  const AuxOutputs& auxOuts) noexcept;

private:
    struct Strip
    {
        ChannelChain chain;
        juce::SmoothedValue<float> gainLeft, gainRight, send;
    };

    /** Runs one channel: its chain, fader and pan (as left and right gains),
        then into its output and the reverb send. Returns its peak level. */
    float processChannel (Strip&, std::array<float*, 2> in, int numChannels, int numSamples, const ChannelParams&,
                          float gainLeft, float gainRight, float sendGain, std::array<float*, 2> mainOut,
                          const AuxOutputs&) noexcept;

    std::array<float*, 2> route (int output, std::array<float*, 2> mainOut, const AuxOutputs&) const noexcept;

    MixerFeedback* meters;
    std::array<Strip, maxPieces> pieceStrips;
    std::array<Strip, maxMics> micStrips;
    juce::SmoothedValue<float> masterGain, returnGain;
    juce::AudioBuffer<float> micBus, pieceBus, reverbSend, reverbReturn;
    std::array<float*, 2> send {};
    RoomReverb reverb;
};

} // namespace lithium
