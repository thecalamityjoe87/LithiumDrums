#pragma once

#include "SpectrumTap.h"
#include "EngineParams.h"
#include "MicEmulation.h"
#include "Voice.h"
#include "dsp/ChannelChain.h"
#include "dsp/ConvolutionReverb.h"
#include "dsp/FdnReverb.h"
#include "dsp/MasterChain.h"
#include "dsp/TempoDelay.h"

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
    std::array<std::atomic<float>, 3> returnPeak {};                // Room, Plate, Delay (params::FxReturn order), for the Mixer page
    std::array<std::atomic<float>, 3> fxReturnPeak {};              // the same, for the FX page (each page takes its own)
    std::array<std::atomic<float>, 2> chainInPeak {}, chainOutPeak {};   // the master chain's input and output
    std::atomic<float> glueReductionDb { 0.0f }, crushReductionDb { 0.0f }, limiterReductionDb { 0.0f };
    std::array<std::atomic<float>, 2> masterPeak {};                // the main out
    SpectrumTap channelSpectrum;                                    // the Mixer EQ's selected channel, before and after its EQ

    static void raise (std::atomic<float>& a, float value) noexcept
    {
        if (value > a.load (std::memory_order_relaxed))
            a.store (value, std::memory_order_relaxed);
    }

    static float take (std::atomic<float>& a) noexcept   { return a.exchange (0.0f, std::memory_order_relaxed); }
};

/** The mixer: a channel per drum, a channel per overhead and room mic
    (recorded, or emulated: see MicEmulation.h), three FX returns, and the
    master bus with its chain.

    The voices render each hit's close mics into its drum's stereo channel
    (getPieceBus(), channels 2p and 2p + 1) and its overhead and room mics
    into those mics' channels (getMicBus()). Every channel then runs the same
    chain (EQ, transient shaper, compressor, saturation), its fader and pan,
    and goes to the master bus (through the master fader) or straight to one
    of the aux outs. Each channel also sends to the three returns (Room,
    Plate and Delay), before or after its fader; each return's output goes
    to the master bus or an aux out too. The master bus then runs the master
    chain (EQ, glue, crush, tape, transient shaper, limiter) into the main out.

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
    VoiceBuses getBuses() noexcept                        { return { micBus, pieceBus, &emulationFeed }; }

    void clearBuses (int numSamples) noexcept
    {
        micBus.clear (0, numSamples);
        pieceBus.clear (0, numSamples);
        emulationFeed.clear (0, numSamples);
    }

    /** Mixes the first numSamples of the buses into the outputs (adding to
        whatever is there). numSamples must not exceed getMaxBlockSize(). */
    void process (const Kit&, int numSamples, const EngineParams&, std::array<float*, 2> mainOut,
                  const AuxOutputs& auxOuts) noexcept;

    /** The Room return, for IrLoader to load IRs into. */
    ConvolutionReverb& getRoomReverb() noexcept           { return room; }

    /** The emulated room mics' room, for their own IrLoader. */
    ConvolutionReverb& getEmulatedRoom() noexcept         { return emulator.getRoomReverb(); }

    /** The latency the mixer adds: the limiter's lookahead, when it's on. */
    int getLatencySamples (bool lookahead) const noexcept { return chain.getLatencySamples (lookahead); }

private:
    static constexpr int numReturns = 3;

    struct Strip
    {
        ChannelChain chain;
        juce::SmoothedValue<float> gainLeft, gainRight;
        std::array<juce::SmoothedValue<float>, numReturns> sends;
    };

    struct Return
    {
        juce::SmoothedValue<float> gainLeft, gainRight, mix;
        bool resting = true;   // silent and settled: its effect isn't running
    };

    /** Runs one channel: its chain, fader and pan (as left and right gains),
        then into its output and the three sends. Returns its peak level. */
    float processChannel (Strip&, int spectrumKey, std::array<float*, 2> in, int numChannels, int numSamples, const ChannelParams&,
                          float gainLeft, float gainRight, const std::array<float, numReturns>& sendGains,
                          const AuxOutputs&) noexcept;

    /** Runs one return: its effect on its send bus, then its mix, fader and
        pan into its output. Returns its peak level. */
    float processReturn (int index, const ReturnParams&, bool bypassed, int numSamples, const AuxOutputs&) noexcept;

    /** Where a channel or return goes: the master bus, or an aux out. */
    std::array<float*, 2> route (int output, const AuxOutputs&) noexcept;

    /** With the limiter's lookahead on, the main out is late by its latency;
        the aux outs are delayed to match, so they stay in time with it once
        the host compensates. */
    void delayAuxOuts (const AuxOutputs&, int numSamples, bool lookahead) noexcept;

    MixerFeedback* meters;
    std::array<Strip, maxPieces> pieceStrips;
    std::array<Strip, maxMics> micStrips;
    std::array<Return, numReturns> returns;
    juce::SmoothedValue<float> masterGain;
    juce::AudioBuffer<float> micBus, pieceBus, masterBus, wet;
    juce::AudioBuffer<float> emulationFeed;   // one mono channel per piece (see VoiceBuses::emulation)
    MicEmulator emulator;
    std::array<juce::AudioBuffer<float>, numReturns> sendBuses;
    std::array<std::array<float*, 2>, numReturns> send {};
    std::array<float*, 2> master {};
    double sampleRate = 44100.0;

    std::array<std::array<std::vector<float>, 2>, numAuxOuts> auxDelay;
    int auxDelayPosition = 0;
    bool auxDelaying = false;

    ConvolutionReverb room;
    FdnReverb plate;
    TempoDelay delay;
    MasterChain chain;

    // The settings of the effects for this block, kept here so processReturn() can reach them.
    const EngineParams* current = nullptr;
};

} // namespace lithium
