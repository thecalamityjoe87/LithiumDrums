#include "Compressor.h"

#include <cmath>

namespace lithium
{

namespace
{
    constexpr double switchSeconds = 0.02;
    constexpr float dbPerOctave = 6.0205999f;   // 20 * log10 (2): dB = 6.02 * log2 (gain)

    float coefficientFor (float ms, double sampleRate) noexcept
    {
        return (float) std::exp (-1.0 / (std::max (0.01, (double) ms) * 0.001 * sampleRate));
    }
}

float Compressor::transfer (float in, float threshold, float ratio) noexcept
{
    const auto over = in - threshold;
    const auto slope = 1.0f / std::max (1.0f, ratio) - 1.0f;

    if (2.0f * over <= -kneeDb)
        return in;

    // Inside the knee the curve bends quadratically from 1:1 into the ratio.
    if (2.0f * std::abs (over) < kneeDb)
    {
        const auto x = over + kneeDb * 0.5f;
        return in + slope * x * x / (2.0f * kneeDb);
    }

    return in + slope * over;
}

void Compressor::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    onAmount.reset (sampleRate, switchSeconds);
    makeup.reset (sampleRate, switchSeconds);
    lastAttackMs = lastReleaseMs = -1.0f;
    reset();
}

void Compressor::reset() noexcept
{
    envelopeDb = 0.0f;
    onAmount.setCurrentAndTargetValue (0.0f);
    makeup.setCurrentAndTargetValue (1.0f);
    maxReductionDb = 0.0f;
}

void Compressor::process (float* const* channels, int numChannels, int numSamples, const CompSettings& s) noexcept
{
    onAmount.setTargetValue (s.enabled ? 1.0f : 0.0f);

    if (! onAmount.isSmoothing() && juce::exactlyEqual (onAmount.getCurrentValue(), 0.0f))
    {
        envelopeDb = 0.0f;     // switched on again later, it starts from no reduction
        return;
    }

    if (! juce::exactlyEqual (s.attackMs, lastAttackMs))
        attackCoefficient = coefficientFor (lastAttackMs = s.attackMs, sampleRate);

    if (! juce::exactlyEqual (s.releaseMs, lastReleaseMs))
        releaseCoefficient = coefficientFor (lastReleaseMs = s.releaseMs, sampleRate);

    makeup.setTargetValue (std::exp2 (s.makeupDb / dbPerOctave));
    const auto mix = juce::jlimit (0.0f, 1.0f, s.mix);
    auto* left = channels[0];
    auto* right = channels[numChannels > 1 ? 1 : 0];
    auto env = envelopeDb;
    auto mostReduction = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto peak = std::max (std::abs (left[i]), std::abs (right[i]));
        const auto levelDb = peak > 1.0e-6f ? dbPerOctave * std::log2 (peak) : -120.0f;
        const auto targetDb = transfer (levelDb, s.thresholdDb, s.ratio) - levelDb;

        // More reduction follows the attack time, less follows the release.
        const auto coefficient = targetDb < env ? attackCoefficient : releaseCoefficient;
        env = targetDb + coefficient * (env - targetDb);

        const auto on = onAmount.getNextValue() * mix;
        const auto wet = std::exp2 (env / dbPerOctave) * makeup.getNextValue();
        const auto gain = 1.0f + on * (wet - 1.0f);

        left[i] *= gain;

        if (right != left)
            right[i] *= gain;

        mostReduction = std::min (mostReduction, env * on);
    }

    envelopeDb = env;
    maxReductionDb = std::max (maxReductionDb, -mostReduction);
}

} // namespace lithium
