#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace lithium
{

/** A mixer channel's compressor, in the units its parameters use. The
    defaults are the parameters' defaults (see Parameters.cpp). */
struct CompSettings
{
    bool enabled = false;
    float thresholdDb = -18.0f;
    float ratio = 4.0f;
    float attackMs = 10.0f;
    float releaseMs = 120.0f;
    float makeupDb = 0.0f;
    float mix = 1.0f;            // 0..1; below 1 is parallel ("New York") compression
};

/** A feed-forward compressor with a soft knee, stereo-linked for stereo mics
    (both sides get the same gain, so the image doesn't shift).

    The detector follows the peak level in dB with separate attack and
    release times. Compression is only ever a gain, so the dry/wet mix is a
    blend of gains rather than of two signals, and switching the compressor
    on or off fades that blend instead of cutting. Audio thread, except prepare(). */
class Compressor
{
public:
    static constexpr float kneeDb = 6.0f;

    void prepare (double sampleRate);
    void reset() noexcept;

    void process (float* const* channels, int numChannels, int numSamples, const CompSettings&) noexcept;

    /** The most gain reduction (a positive number of dB) since the last call. */
    float takeMaxGainReductionDb() noexcept   { return std::exchange (maxReductionDb, 0.0f); }

    /** The static curve: output level for an input level, both in dB. */
    static float transfer (float inputDb, float thresholdDb, float ratio) noexcept;

private:
    double sampleRate = 44100.0;
    juce::SmoothedValue<float> onAmount;    // 0 = off, 1 = on
    juce::SmoothedValue<float> makeup;      // linear
    float envelopeDb = 0.0f;                // current gain change, <= 0
    float attackCoefficient = 0.0f, releaseCoefficient = 0.0f;
    float lastAttackMs = -1.0f, lastReleaseMs = -1.0f;
    float maxReductionDb = 0.0f;
};

} // namespace lithium
