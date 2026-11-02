#pragma once

#include "Svf.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <vector>

namespace lithium
{

/** The Room return's settings, in the units its parameters use. Which IR
    plays, and how much of its decay, aren't here: changing those means
    loading a new IR, which IrLoader does on its own thread. */
struct ConvolutionSettings
{
    float preDelayMs = 0.0f;
    float lowCutHz = 100.0f;     // filters the send, so kicks don't turn the room to mud
    float highCutHz = 12000.0f;
    float width = 1.0f;          // 0 = mono, 1 = as recorded
};

/** The Room return: a measured (or synthesized) space, by convolution.

    The send is filtered and pre-delayed, then convolved with the impulse
    response by juce::dsp::Convolution, in its non-uniform partitioned mode:
    the first 512 samples of the IR are convolved in small blocks (zero
    latency), the rest in larger ones, which is much cheaper for a reverb's
    long tail.

    The IR itself is loaded elsewhere (IrLoader): Convolution's
    loadImpulseResponse() is wait-free, and the Convolution builds the new
    engine on a background thread of its own and swaps it in lock-free at
    the start of a later process() call, so the audio thread never waits.

    process() is the audio thread's; everything else is called while it isn't
    running (prepare) or is lock-free. When nothing has been sent for longer
    than the IR lasts, the convolution is skipped: its output would be exact
    silence anyway. */
class ConvolutionReverb
{
public:
    static constexpr int headSize = 512;
    static constexpr double maxPreDelayMs = 250.0;

    ConvolutionReverb();

    /** Allocates everything. Not real-time safe. */
    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    /** Writes 100% wet output (replacing what's in outL and outR). */
    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples,
                  const ConvolutionSettings&) noexcept;

    /** For IrLoader: the engine to load IRs into. */
    juce::dsp::Convolution& getEngine() noexcept         { return convolution; }

    /** The current IR's length, so the reverb knows when it has rung out.
        Set by IrLoader when it sends an IR. */
    void setTailSeconds (float s) noexcept               { tailSeconds.store (s, std::memory_order_relaxed); }

    /** Samples of latency: always 0 (see the class comment), checked by the tests. */
    int getLatency() const                               { return convolution.getLatency(); }

private:
    juce::dsp::Convolution convolution;
    double sampleRate = 44100.0;

    std::array<dsp::SvfState, 2> lowCut, highCut;
    dsp::SvfCoefficients lowCutCoefficients, highCutCoefficients;
    float lastLowCutHz = -1.0f, lastHighCutHz = -1.0f;

    std::array<std::vector<float>, 2> preDelay;
    int preDelayMask = 0, preDelayWrite = 0;
    juce::SmoothedValue<float> preDelaySamples, width;

    juce::AudioBuffer<float> wet;
    std::atomic<float> tailSeconds { 3.0f };
    int silentSamples = 0;
    bool idle = true;
};

} // namespace lithium
