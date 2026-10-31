#include "ChannelChain.h"

#include <cmath>

namespace lithium
{

namespace
{
    constexpr double switchSeconds = 0.02;
    constexpr float dbPerOctave = 6.0205999f;
    constexpr float maxShapeDb = 12.0f;          // the most the transient shaper adds or takes away

    float coefficientFor (double ms, double sampleRate) noexcept
    {
        return (float) std::exp (-1.0 / (ms * 0.001 * sampleRate));
    }

    // A peak follower: rises with one coefficient, falls with another.
    inline float follow (float state, float x, float rise, float fall) noexcept
    {
        return x + (x > state ? rise : fall) * (state - x);
    }
}

//==============================================================================
void Saturator::prepare (double sampleRate)
{
    wet.reset (sampleRate, switchSeconds);
    drive.reset (sampleRate, switchSeconds);
    wet.setCurrentAndTargetValue (0.0f);
    drive.setCurrentAndTargetValue (1.0f);
}

void Saturator::process (float* const* channels, int numChannels, int numSamples, const SaturationSettings& s) noexcept
{
    wet.setTargetValue (s.enabled ? juce::jlimit (0.0f, 1.0f, s.mix) : 0.0f);
    drive.setTargetValue (std::exp2 (s.driveDb / dbPerOctave));

    if (! wet.isSmoothing() && juce::exactlyEqual (wet.getCurrentValue(), 0.0f))
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto d = drive.getNextValue();
        const auto w = wet.getNextValue();

        // Divided by the drive again, so quiet signals pass at their own
        // level and only the peaks are rounded off.
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto x = channels[ch][i];
            channels[ch][i] = x + w * (shape (x * d) / d - x);
        }
    }
}

//==============================================================================
void TransientShaper::prepare (double sampleRate)
{
    attackFast = coefficientFor (0.2, sampleRate);
    attackSlow = coefficientFor (25.0, sampleRate);
    attackPairRelease = coefficientFor (60.0, sampleRate);
    releaseFast = coefficientFor (20.0, sampleRate);
    releaseSlow = coefficientFor (400.0, sampleRate);
    onAmount.reset (sampleRate, switchSeconds);
    reset();
}

void TransientShaper::reset() noexcept
{
    fast = slow = ringFast = ringSlow = 0.0f;
    onAmount.setCurrentAndTargetValue (0.0f);
}

void TransientShaper::process (float* const* channels, int numChannels, int numSamples, const TransientSettings& s) noexcept
{
    onAmount.setTargetValue (s.enabled ? 1.0f : 0.0f);

    if (! onAmount.isSmoothing() && juce::exactlyEqual (onAmount.getCurrentValue(), 0.0f))
    {
        fast = slow = ringFast = ringSlow = 0.0f;
        return;
    }

    const auto attack = juce::jlimit (-1.0f, 1.0f, s.attack);
    const auto sustain = juce::jlimit (-1.0f, 1.0f, s.sustain);
    auto* left = channels[0];
    auto* right = channels[numChannels > 1 ? 1 : 0];
    constexpr float floor = 1.0e-5f;   // -100 dB: silence compares as equal, so it isn't boosted

    // Locals, so the state stays in registers (the loop writes through float pointers).
    auto f = fast, sl = slow, rf = ringFast, rs = ringSlow;
    for (int i = 0; i < numSamples; ++i)
    {
        const auto x = std::max (std::abs (left[i]), std::abs (right[i]));
        f = follow (f, x, attackFast, attackPairRelease);
        sl = follow (sl, x, attackSlow, attackPairRelease);
        rf = follow (rf, x, 0.0f, releaseFast);
        rs = follow (rs, x, 0.0f, releaseSlow);

        const auto attackDb = dbPerOctave * std::log2 ((f + floor) / (sl + floor));
        const auto ringDb = dbPerOctave * std::log2 ((rs + floor) / (rf + floor));
        const auto db = juce::jlimit (-maxShapeDb, maxShapeDb, attack * attackDb + sustain * ringDb);
        const auto gain = 1.0f + onAmount.getNextValue() * (std::exp2 (db / dbPerOctave) - 1.0f);

        left[i] *= gain;

        if (right != left)
            right[i] *= gain;
    }

    fast = f;
    slow = sl;
    ringFast = rf;
    ringSlow = rs;
}

//==============================================================================
void ChannelChain::prepare (double sampleRate)
{
    eq.prepare (sampleRate);
    transient.prepare (sampleRate);
    compressor.prepare (sampleRate);
    saturator.prepare (sampleRate);
}

void ChannelChain::process (float* const* channels, int numChannels, int numSamples, const ChannelSettings& s) noexcept
{
    eq.process (channels, numChannels, numSamples, s.eq);
    transient.process (channels, numChannels, numSamples, s.transient);
    compressor.process (channels, numChannels, numSamples, s.comp);
    saturator.process (channels, numChannels, numSamples, s.saturation);
}

} // namespace lithium
