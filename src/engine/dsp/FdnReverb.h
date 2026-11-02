#pragma once

#include "Svf.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

namespace lithium
{

/** The reverb bus's settings, in the units its parameters use. */
struct ReverbSettings
{
    float size = 0.5f;           // 0..1, the room's dimensions
    float decaySeconds = 1.4f;   // RT60: time to fall by 60 dB
    float preDelayMs = 10.0f;
    float dampingHz = 7000.0f;   // highs above this die away faster, like soft furnishings
    float lowCutHz = 120.0f;     // filters the send, so kicks don't turn the room to mud
    float width = 1.0f;          // 0 = mono, 1 = full stereo
};

/** An algorithmic stereo room for the mixer's reverb bus.

    Built as a feedback delay network: the send is filtered, pre-delayed and
    smeared by a few allpass diffusers (heard directly as the early
    reflections), then circulates through eight delay lines of unrelated
    lengths. Every pass goes through an 8x8 Hadamard
    matrix (which mixes the lines without adding or losing energy), a gentle
    low-pass (damping) and a gain chosen per line so the tail falls 60 dB in
    exactly the decay time. Slow, tiny modulation of the line lengths keeps
    cymbal tails from ringing metallically.

    All buffers are allocated in prepare(); process() runs on the audio
    thread. When nothing has been sent for longer than the tail lasts, the
    reverb goes idle and costs nothing. */
class FdnReverb
{
public:
    static constexpr int numLines = 8;
    static constexpr int numDiffusers = 4;

    void prepare (double sampleRate);
    void reset() noexcept;

    /** Writes 100% wet output (replacing what's in outL and outR). */
    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples,
                  const ReverbSettings&) noexcept;

private:
    /** A circular buffer whose size is a power of two, so wrapping is a mask. */
    struct DelayLine
    {
        std::vector<float> buffer;
        int mask = 0, write = 0;

        void allocate (int minimumLength);
        void clear() noexcept                       { std::fill (buffer.begin(), buffer.end(), 0.0f); }
        void push (float x) noexcept                { buffer[(size_t) write] = x; write = (write + 1) & mask; }
        float read (int delay) const noexcept       { return buffer[(size_t) ((write - delay) & mask)]; }
        float readFractional (float delay) const noexcept;
    };

    struct Allpass
    {
        DelayLine line;
        int delay = 1;

        float process (float x, float g) noexcept
        {
            const auto delayed = line.read (delay);
            const auto v = x + g * delayed;
            line.push (v);
            return delayed - g * v;
        }
    };

    void updateBlock (const ReverbSettings&, int numSamples) noexcept;

    double sampleRate = 44100.0;

    std::array<dsp::SvfState, 2> lowCut;
    dsp::SvfCoefficients lowCutCoefficients;
    float lastLowCutHz = -1.0f;

    std::array<DelayLine, 2> preDelay;
    juce::SmoothedValue<float> preDelaySamples;
    std::array<std::array<Allpass, numDiffusers>, 2> diffusers;

    std::array<DelayLine, numLines> lines;
    std::array<float, numLines> lineLength {};      // in samples, including modulation, for this block
    std::array<float, numLines> feedback {};
    std::array<float, numLines> damping {};         // one-pole low-pass state
    std::array<double, numLines> lfoPhase {};
    juce::SmoothedValue<float> sizeScale;
    float dampingCoefficient = 1.0f, lastDampingHz = -1.0f;
    juce::SmoothedValue<float> width;

    int silentSamples = 0;
    bool idle = true;
};

} // namespace lithium
