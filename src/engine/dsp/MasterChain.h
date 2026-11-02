#pragma once

#include "ChannelChain.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

namespace lithium
{

/** The parallel "crush" compressor: the bus slammed hard, blended in under it. */
struct CrushSettings
{
    bool enabled = false;
    float driveDb = 12.0f;       // how far over the threshold the bus is pushed
    float blend = 0.3f;          // 0..1: how much of the crushed bus is mixed in
    float attackMs = 3.0f;
    float releaseMs = 80.0f;
};

/** Tape-style saturation. */
struct TapeSettings
{
    bool enabled = false;
    float driveDb = 6.0f;
    float tone = 0.0f;           // -1 (dark) .. 1 (bright)
    float outputDb = 0.0f;
};

/** The brickwall limiter at the end of the chain. */
struct LimiterSettings
{
    bool enabled = false;
    float ceilingDb = -0.3f;
    float releaseMs = 60.0f;
    bool lookahead = false;
};

/** Everything the master chain does, in its fixed order. The defaults are
    the parameters' defaults: every stage off. */
struct MasterSettings
{
    EqSettings eq { .enabled = false };
    CompSettings glue { .enabled = false, .thresholdDb = -12.0f, .ratio = 2.0f, .attackMs = 10.0f, .releaseMs = 200.0f };
    CrushSettings crush;
    TapeSettings tape;
    TransientSettings transient;
    LimiterSettings limiter;
};

/** Tape-style saturation, run at four times the sample rate.

    Clipping makes harmonics above the original frequencies; at 48 kHz those
    above 24 kHz would fold back down as inharmonic aliasing. So the signal is
    upsampled four times (juce::dsp::Oversampling, with polyphase IIR
    half-band filters), clipped there, filtered back down and decimated, and
    the aliases are left above the audio band to be filtered out.

    The curve is the channel saturator's, shifted off centre so it rounds one
    side of the wave a little more than the other, which adds the even
    harmonics tape is known for. Tone tilts the result darker or brighter
    around 1 kHz. Switching it on or off crossfades. Audio thread, except prepare().

    The IIR filters shift the phase slightly (well under a millisecond, more
    in the highs). That isn't a fixed delay a host could compensate, so it
    isn't reported as latency. */
class TapeSaturator
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void process (float* const* channels, int numSamples, const TapeSettings&) noexcept;

private:
    juce::dsp::Oversampling<float> oversampling { 2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
    juce::AudioBuffer<float> dry;
    double sampleRate = 44100.0;
    juce::SmoothedValue<float> wet, drive, output;
    std::array<std::array<dsp::SvfState, 2>, 3> tone {};   // [low shelf, high shelf, DC blocker][channel]
    std::array<dsp::SvfCoefficients, 3> toneCoefficients;
    float lastTone = 100.0f;
};

/** A brickwall limiter: nothing leaves above the ceiling.

    Without lookahead, the gain drops the instant a peak would cross the
    ceiling, which can be heard as a little distortion on the hardest hits.
    With lookahead, the audio is delayed by lookaheadMs while the gain looks
    at what's coming: it takes the lowest gain needed anywhere in that
    window, and slides down to it over the window (a moving average), so it
    arrives exactly as the peak does, without distortion. The delay is
    reported to the host as latency (see LithiumDrumsProcessor).

    Release smooths the gain back up afterwards. Audio thread, except prepare(). */
class Limiter
{
public:
    static constexpr double lookaheadMs = 2.0;

    void prepare (double sampleRate);
    void reset() noexcept;
    void process (float* const* channels, int numSamples, const LimiterSettings&) noexcept;

    /** The latency it adds with lookahead on, in samples. */
    int getLookaheadSamples() const noexcept    { return lookahead; }

    float takeMaxGainReductionDb() noexcept     { return std::exchange (maxReductionDb, 0.0f); }

private:
    double sampleRate = 44100.0;
    int lookahead = 0;

    // The delay line for the audio, and the windows for the gain: a sliding
    // minimum (as a monotonic queue of sample indices, kept in a ring) and a
    // moving average (a ring and its running sum).
    std::array<std::vector<float>, 2> delay;
    std::vector<float> minValues, averageRing;
    std::vector<std::int64_t> minIndices;
    int minHead = 0, minTail = 0, averagePosition = 0, delayPosition = 0;
    double averageSum = 0.0;
    std::int64_t sampleIndex = 0;
    bool usedLookahead = false;

    float envelope = 1.0f;
    juce::SmoothedValue<float> wet;
    float releaseCoefficient = 0.0f, lastReleaseMs = -1.0f;
    float maxReductionDb = 0.0f;
};

/** The master bus chain, in a fixed order: EQ, glue compressor, parallel
    crush compressor, tape, transient shaper, limiter. Every stage fades in
    and out when it's switched, so bypassing never clicks. Stereo, in place.
    Audio thread, except prepare(). */
class MasterChain
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void process (float* const* channels, int numSamples, const MasterSettings&) noexcept;

    /** The latency the chain adds with these settings: the limiter's
        lookahead when it's on (whether or not the limiter is bypassed, so
        bypassing it doesn't make the host re-align the track). */
    int getLatencySamples (bool lookahead) const noexcept    { return lookahead ? limiter.getLookaheadSamples() : 0; }

    float takeGlueReductionDb() noexcept         { return glue.takeMaxGainReductionDb(); }
    float takeCrushReductionDb() noexcept        { return crush.takeMaxGainReductionDb(); }
    float takeLimiterReductionDb() noexcept      { return limiter.takeMaxGainReductionDb(); }

    /** The crush stage's compressor settings for its controls. */
    static CompSettings crushCompressor (const CrushSettings&) noexcept;

private:
    ChannelEq eq;
    Compressor glue, crush;
    TapeSaturator tape;
    TransientShaper transient;
    Limiter limiter;
};

} // namespace lithium
