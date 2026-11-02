#include "ConvolutionReverb.h"

#include <cmath>

namespace lithium
{

ConvolutionReverb::ConvolutionReverb()
    : convolution (juce::dsp::Convolution::NonUniform { headSize })
{
}

void ConvolutionReverb::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;

    // Convolution resamples the IR to this rate itself, and re-prepares the
    // last IR it was given.
    convolution.prepare ({ sampleRate, (juce::uint32) maxBlockSize, 2 });
    wet.setSize (2, maxBlockSize);

    const auto length = juce::nextPowerOfTwo ((int) std::ceil (maxPreDelayMs * 0.001 * sampleRate) + 4);

    for (auto& d : preDelay)
        d.assign ((size_t) length, 0.0f);

    preDelayMask = length - 1;
    preDelaySamples.reset (sampleRate, 0.1);
    width.reset (sampleRate, 0.05);
    lastLowCutHz = lastHighCutHz = -1.0f;
    reset();
}

void ConvolutionReverb::reset() noexcept
{
    convolution.reset();

    for (auto& d : preDelay)
        std::fill (d.begin(), d.end(), 0.0f);

    for (auto* s : { &lowCut, &highCut })
        for (auto& c : *s)
            c.reset();

    preDelayWrite = 0;
    silentSamples = 0;
    idle = true;
}

void ConvolutionReverb::process (const float* inL, const float* inR, float* outL, float* outR, int numSamples,
                                 const ConvolutionSettings& s) noexcept
{
    jassert (numSamples <= wet.getNumSamples());

    if (! juce::exactlyEqual (s.lowCutHz, lastLowCutHz))
        lowCutCoefficients = dsp::designSvf (dsp::SvfType::highPass, lastLowCutHz = s.lowCutHz, 0.0, 0.7071, sampleRate);

    if (! juce::exactlyEqual (s.highCutHz, lastHighCutHz))
        highCutCoefficients = dsp::designSvf (dsp::SvfType::lowPass, lastHighCutHz = s.highCutHz, 0.0, 0.7071, sampleRate);

    preDelaySamples.setTargetValue ((float) (juce::jlimit (0.0, maxPreDelayMs, (double) s.preDelayMs) * 0.001 * sampleRate));
    width.setTargetValue (juce::jlimit (0.0f, 1.0f, s.width));

    // Idle until something is sent; then until the IR has rung out.
    float inputPeak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
        inputPeak = std::max ({ inputPeak, std::abs (inL[i]), std::abs (inR[i]) });

    const auto tail = (int) ((tailSeconds.load (std::memory_order_relaxed) + s.preDelayMs * 0.001f + 0.1f) * (float) sampleRate);

    if (inputPeak > 1.0e-7f)
    {
        silentSamples = 0;
        idle = false;
    }
    else if (! idle && (silentSamples += numSamples) > tail)
    {
        idle = true;
    }

    if (idle)
    {
        juce::FloatVectorOperations::clear (outL, numSamples);
        juce::FloatVectorOperations::clear (outR, numSamples);
        preDelaySamples.skip (numSamples);
        width.skip (numSamples);
        return;
    }

    // Filter and pre-delay the send into the wet buffer.
    auto* wl = wet.getWritePointer (0);
    auto* wr = wet.getWritePointer (1);
    auto lc = lowCut, hc = highCut;   // copies, so the state stays in registers

    for (int i = 0; i < numSamples; ++i)
    {
        const auto l = hc[0].process (highCutCoefficients, lc[0].process (lowCutCoefficients, inL[i]));
        const auto r = hc[1].process (highCutCoefficients, lc[1].process (lowCutCoefficients, inR[i]));
        preDelay[0][(size_t) preDelayWrite] = l;
        preDelay[1][(size_t) preDelayWrite] = r;

        // Read between two samples, so a moving pre-delay glides rather than clicks.
        const auto position = (float) preDelayWrite - preDelaySamples.getNextValue();
        const auto whole = (int) std::floor (position);
        const auto frac = position - (float) whole;

        for (size_t ch = 0; ch < 2; ++ch)
        {
            const auto a = preDelay[ch][(size_t) (whole & preDelayMask)];
            const auto b = preDelay[ch][(size_t) ((whole + 1) & preDelayMask)];
            (ch == 0 ? wl : wr)[i] = a + frac * (b - a);
        }

        preDelayWrite = (preDelayWrite + 1) & preDelayMask;
    }

    lowCut = lc;
    highCut = hc;

    // AudioBlock only wraps the buffer's channel pointers; nothing is allocated.
    juce::dsp::AudioBlock<float> block (wet.getArrayOfWritePointers(), 2, (size_t) numSamples);
    convolution.process (juce::dsp::ProcessContextReplacing<float> (block));

    for (int i = 0; i < numSamples; ++i)
    {
        const auto mid = 0.5f * (wl[i] + wr[i]);
        const auto side = 0.5f * (wl[i] - wr[i]) * width.getNextValue();
        outL[i] = mid + side;
        outR[i] = mid - side;
    }
}

} // namespace lithium
