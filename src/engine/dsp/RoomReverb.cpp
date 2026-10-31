#include "RoomReverb.h"

#include <cmath>

namespace lithium
{

namespace
{
    // Line lengths at size 1.0, in ms. Mutually unrelated, so their echoes
    // never line up into a pitch.
    constexpr std::array<double, RoomReverb::numLines> lineMs { 31.3, 36.7, 41.9, 47.3, 53.9, 59.1, 66.7, 73.3 };
    constexpr double minSizeScale = 0.3, maxSizeScale = 1.6;

    constexpr std::array<std::array<double, RoomReverb::numDiffusers>, 2> diffuserMs {{ { 3.1, 4.7, 7.3, 11.3 },
                                                                                        { 3.4, 5.1, 6.9, 10.7 } }};
    constexpr float diffusion = 0.62f;

    constexpr double maxPreDelayMs = 250.0;
    constexpr double modulationMs = 0.12;        // depth of the line-length wobble
    constexpr double modulationHz = 0.37;        // the slowest line; the others are a little faster
    constexpr int blockLength = 32;              // settings are updated this often
    constexpr float inputGain = 0.5f, outputGain = 0.55f;
    constexpr float earlyGain = 0.25f;           // the diffused send, heard before the first trip round the lines

    // A fast in-place Hadamard transform, scaled to keep energy constant.
    inline void hadamard (std::array<float, RoomReverb::numLines>& v) noexcept
    {
        for (size_t len = 1; len < v.size(); len <<= 1)
        {
            for (size_t i = 0; i < v.size(); i += 2 * len)
            {
                for (size_t j = i; j < i + len; ++j)
                {
                    const auto a = v[j], b = v[j + len];
                    v[j] = a + b;
                    v[j + len] = a - b;
                }
            }
        }

        for (auto& x : v)
            x *= 0.35355339f;   // 1 / sqrt (8)
    }
}

void RoomReverb::DelayLine::allocate (int minimumLength)
{
    const auto size = juce::nextPowerOfTwo (std::max (2, minimumLength));
    buffer.assign ((size_t) size, 0.0f);
    mask = size - 1;
    write = 0;
}

float RoomReverb::DelayLine::readFractional (float delay) const noexcept
{
    const auto position = (float) write - delay;
    const auto whole = (int) std::floor (position);
    const auto fraction = position - (float) whole;
    const auto a = buffer[(size_t) (whole & mask)];
    const auto b = buffer[(size_t) ((whole + 1) & mask)];
    return a + fraction * (b - a);
}

//==============================================================================
void RoomReverb::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    const auto samples = [this] (double ms) { return (int) std::ceil (ms * 0.001 * sampleRate); };

    for (auto& d : preDelay)
        d.allocate (samples (maxPreDelayMs) + 2);

    for (size_t ch = 0; ch < 2; ++ch)
    {
        for (size_t i = 0; i < (size_t) numDiffusers; ++i)
        {
            auto& ap = diffusers[ch][i];
            ap.delay = std::max (1, samples (diffuserMs[ch][i]));
            ap.line.allocate (ap.delay + 1);
        }
    }

    for (size_t i = 0; i < lines.size(); ++i)
        lines[i].allocate (samples (lineMs[i] * maxSizeScale + 2.0 * modulationMs) + 4);

    preDelaySamples.reset (sampleRate, 0.1);
    sizeScale.reset (sampleRate, 0.25);
    width.reset (sampleRate, 0.05);
    lastLowCutHz = lastDampingHz = -1.0f;
    reset();
}

void RoomReverb::reset() noexcept
{
    for (auto& d : preDelay)
        d.clear();

    for (auto& channel : diffusers)
        for (auto& ap : channel)
            ap.line.clear();

    for (auto& l : lines)
        l.clear();

    for (auto& s : lowCut)
        s.reset();

    damping.fill (0.0f);
    silentSamples = 0;
    idle = true;
}

void RoomReverb::updateBlock (const ReverbSettings& s, int numSamples) noexcept
{
    if (! juce::exactlyEqual (s.lowCutHz, lastLowCutHz))
        lowCutCoefficients = dsp::designSvf (dsp::SvfType::highPass, lastLowCutHz = s.lowCutHz, 0.0, 0.7071, sampleRate);

    if (! juce::exactlyEqual (s.dampingHz, lastDampingHz))
        dampingCoefficient = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * (lastDampingHz = s.dampingHz) / sampleRate));

    preDelaySamples.setTargetValue ((float) (juce::jlimit (0.0, maxPreDelayMs, (double) s.preDelayMs) * 0.001 * sampleRate));
    sizeScale.setTargetValue ((float) juce::jmap ((double) juce::jlimit (0.0f, 1.0f, s.size), minSizeScale, maxSizeScale));
    width.setTargetValue (juce::jlimit (0.0f, 1.0f, s.width));

    const auto scale = (double) sizeScale.skip (numSamples);
    const auto rt60 = std::max (0.05, (double) s.decaySeconds);
    const auto depth = modulationMs * 0.001 * sampleRate;

    for (size_t i = 0; i < lines.size(); ++i)
    {
        const auto base = lineMs[i] * scale * 0.001 * sampleRate;
        lfoPhase[i] += juce::MathConstants<double>::twoPi * modulationHz * (1.0 + 0.13 * (double) i) * numSamples / sampleRate;

        if (lfoPhase[i] > juce::MathConstants<double>::twoPi)
            lfoPhase[i] -= juce::MathConstants<double>::twoPi;

        lineLength[i] = (float) (base + depth * (1.0 + std::sin (lfoPhase[i])));

        // The gain that makes one trip round this line lose its share of 60 dB.
        feedback[i] = (float) std::pow (10.0, -3.0 * (double) lineLength[i] / (rt60 * sampleRate));
    }
}

void RoomReverb::process (const float* inL, const float* inR, float* outL, float* outR, int numSamples,
                          const ReverbSettings& s) noexcept
{
    // Idle until something is sent; then until the tail has died away.
    float inputPeak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
        inputPeak = std::max ({ inputPeak, std::abs (inL[i]), std::abs (inR[i]) });

    const auto tailSamples = (int) ((s.decaySeconds * 1.2 + s.preDelayMs * 0.001 + 0.2) * sampleRate);

    if (inputPeak > 1.0e-7f)
    {
        silentSamples = 0;
        idle = false;
    }
    else if (! idle && (silentSamples += numSamples) > tailSamples)
    {
        reset();   // clears the buffers (no allocation), so nothing stale comes back later
    }

    if (idle)
    {
        juce::FloatVectorOperations::clear (outL, numSamples);
        juce::FloatVectorOperations::clear (outR, numSamples);
        // Keep the smoothed settings moving, so a hit after a pause doesn't glide in.
        updateBlock (s, numSamples);
        preDelaySamples.skip (numSamples);
        width.skip (numSamples);
        return;
    }

    for (int start = 0; start < numSamples; start += blockLength)
    {
        const auto n = std::min (blockLength, numSamples - start);
        updateBlock (s, n);

        for (int i = start; i < start + n; ++i)
        {
            auto left = lowCut[0].process (lowCutCoefficients, inL[i]);
            auto right = lowCut[1].process (lowCutCoefficients, inR[i]);

            preDelay[0].push (left);
            preDelay[1].push (right);
            // +1 because the sample just pushed is one behind the write position.
            const auto delay = preDelaySamples.getNextValue() + 1.0f;
            left = preDelay[0].readFractional (delay);
            right = preDelay[1].readFractional (delay);

            for (auto& ap : diffusers[0])
                left = ap.process (left, diffusion);

            for (auto& ap : diffusers[1])
                right = ap.process (right, diffusion);

            std::array<float, numLines> taps, v;

            for (size_t l = 0; l < lines.size(); ++l)
            {
                taps[l] = lines[l].readFractional (lineLength[l]);
                damping[l] += dampingCoefficient * (taps[l] - damping[l]);
                v[l] = damping[l] * feedback[l];
            }

            hadamard (v);

            for (size_t l = 0; l < lines.size(); ++l)
                lines[l].push (v[l] + inputGain * ((l & 1) == 0 ? left : right));

            // Even lines make the left output and odd lines the right, so the
            // two sides are decorrelated; width blends them towards mono.
            const auto wetL = (taps[0] + taps[2] + taps[4] + taps[6]) * outputGain + left * earlyGain;
            const auto wetR = (taps[1] + taps[3] + taps[5] + taps[7]) * outputGain + right * earlyGain;
            const auto mid = 0.5f * (wetL + wetR);
            const auto side = 0.5f * (wetL - wetR) * width.getNextValue();
            outL[i] = mid + side;
            outR[i] = mid - side;
        }
    }
}

} // namespace lithium
