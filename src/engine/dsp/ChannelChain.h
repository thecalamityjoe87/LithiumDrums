#pragma once

#include "engine/SpectrumTap.h"
#include "ChannelEq.h"
#include "Compressor.h"

namespace lithium
{

/** Soft saturation, in the units its parameters use. */
struct SaturationSettings
{
    bool enabled = false;
    float driveDb = 6.0f;        // how hard the signal hits the curve
    float mix = 1.0f;            // 0..1
};

/** A transient shaper: more or less attack, more or less ring. */
struct TransientSettings
{
    bool enabled = false;
    float attack = 0.0f;         // -1..1: soften or sharpen the hit
    float sustain = 0.0f;        // -1..1: shorten or lengthen the ring
};

/** Everything a mixer channel does to its sound before the fader. The
    defaults are the parameters' defaults (see Parameters.cpp). */
struct ChannelSettings
{
    EqSettings eq;
    CompSettings comp;
    SaturationSettings saturation;
    TransientSettings transient;
    float width = 1.0f;   // 0..2: 1 leaves the stereo image alone, 0 is mono, 2 is twice as wide
};

/** Soft clipping through a smooth curve that leaves quiet signals alone and
    rounds off peaks; more drive means more of the signal is rounded. The
    curve is a rational approximation of tanh, which is much cheaper and
    indistinguishable here. Audio thread, except prepare(). */
class Saturator
{
public:
    void prepare (double sampleRate);
    void process (float* const* channels, int numChannels, int numSamples, const SaturationSettings&) noexcept;

    /** The curve, for drawing. */
    static float shape (float x) noexcept
    {
        x = std::clamp (x, -3.0f, 3.0f);
        return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
    }

private:
    juce::SmoothedValue<float> wet, drive;
};

/** Two pairs of envelope followers, compared in dB. A fast follower racing
    ahead of a slow one marks an attack; a slow release lingering above a
    fast one marks the ring. Each difference, scaled by its amount, becomes
    a gain change. Level-independent, so it needs no threshold. Stereo
    channels are linked. Audio thread, except prepare(). */
class TransientShaper
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void process (float* const* channels, int numChannels, int numSamples, const TransientSettings&) noexcept;

private:
    // Envelope coefficients. The attack pair rises at different speeds and
    // falls together; the ring pair rises together and falls at different speeds.
    float attackFast = 0.0f, attackSlow = 0.0f, attackPairRelease = 0.0f, releaseFast = 0.0f, releaseSlow = 0.0f;
    float fast = 0.0f, slow = 0.0f, ringFast = 0.0f, ringSlow = 0.0f;                     // follower states (linear)
    juce::SmoothedValue<float> onAmount;
};

/** Stereo width: the sides (left minus right) scaled around the middle (left
    plus right), so 0 is mono and 2 is twice as wide. Leaves a mono channel
    alone. Audio thread, except prepare(). */
class StereoWidth
{
public:
    void prepare (double sampleRate);
    void process (float* const* channels, int numChannels, int numSamples, float width) noexcept;

private:
    juce::SmoothedValue<float> amount;
};

/** One mixer channel's processing, in order: EQ, transient shaper,
    compressor, saturation, stereo width. Works in place on one or two channels. */
class ChannelChain
{
public:
    void prepare (double sampleRate);
    /** With a tap, the signal just before and just after the EQ is copied to
        it, for the Mixer's spectrum display. */
    void process (float* const* channels, int numChannels, int numSamples, const ChannelSettings&, SpectrumTap* tap = nullptr) noexcept;

    float takeMaxGainReductionDb() noexcept         { return compressor.takeMaxGainReductionDb(); }

private:
    ChannelEq eq;
    TransientShaper transient;
    Compressor compressor;
    Saturator saturator;
    StereoWidth stereoWidth;
};

} // namespace lithium
