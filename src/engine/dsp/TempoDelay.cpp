#include "TempoDelay.h"

#include "ChannelChain.h"

#include <cmath>

namespace lithium
{

float TempoDelay::syncedMs (int note, double bpm) noexcept
{
    const auto& n = delaynotes::all[(size_t) juce::jlimit (0, (int) delaynotes::all.size() - 1, note)];
    const auto ms = n.beats * 60000.0 / juce::jlimit (20.0, 400.0, bpm);
    return (float) juce::jlimit (1.0, maxSeconds * 1000.0, ms);
}

void TempoDelay::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    const auto length = juce::nextPowerOfTwo ((int) std::ceil (maxSeconds * sampleRate) + 8);

    for (auto& l : lines)
        l.assign ((size_t) length, 0.0f);

    mask = length - 1;
    delaySamples.reset (sampleRate, 0.08);
    lastLowCutHz = lastHighCutHz = -1.0f;
    reset();
}

void TempoDelay::reset() noexcept
{
    for (auto& l : lines)
        std::fill (l.begin(), l.end(), 0.0f);

    for (auto* s : { &lowCut, &highCut })
        for (auto& c : *s)
            c.reset();

    write = 0;
    silentSamples = 0;
    idle = true;
}

void TempoDelay::process (const float* inL, const float* inR, float* outL, float* outR, int numSamples,
                          const DelaySettings& s) noexcept
{
    if (! juce::exactlyEqual (s.lowCutHz, lastLowCutHz))
        lowCutCoefficients = dsp::designSvf (dsp::SvfType::highPass, lastLowCutHz = s.lowCutHz, 0.0, 0.7071, sampleRate);

    if (! juce::exactlyEqual (s.highCutHz, lastHighCutHz))
        highCutCoefficients = dsp::designSvf (dsp::SvfType::lowPass, lastHighCutHz = s.highCutHz, 0.0, 0.7071, sampleRate);

    const auto target = (float) juce::jlimit (1.0, maxSeconds * sampleRate - 4.0, (double) s.timeMs * 0.001 * sampleRate);

    // The first echo after a pause starts at the right time, not gliding to it.
    if (idle)
        delaySamples.setCurrentAndTargetValue (target);
    else
        delaySamples.setTargetValue (target);

    float inputPeak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
        inputPeak = std::max ({ inputPeak, std::abs (inL[i]), std::abs (inR[i]) });

    // Silent for as long as 60 dB of repeats take (feedback^n = 0.001), and
    // then some: the loop has nothing left in it.
    const auto feedback = juce::jlimit (0.0f, 0.95f, s.feedback);
    const auto repeats = feedback > 0.001f ? std::log (0.001f) / std::log (feedback) : 1.0f;
    const auto tail = (int) ((repeats + 1.0f) * target) + (int) (0.1 * sampleRate);

    if (inputPeak > 1.0e-7f)
    {
        silentSamples = 0;
        idle = false;
    }
    else if (! idle && (silentSamples += numSamples) > tail)
    {
        reset();
    }

    if (idle)
    {
        juce::FloatVectorOperations::clear (outL, numSamples);
        juce::FloatVectorOperations::clear (outR, numSamples);
        return;
    }

    auto lc = lowCut, hc = highCut;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto d = delaySamples.getNextValue();
        const auto position = (float) write - d;
        const auto whole = (int) std::floor (position);
        const auto frac = position - (float) whole;
        std::array<float, 2> echo {};

        for (size_t ch = 0; ch < 2; ++ch)
        {
            const auto a = lines[ch][(size_t) (whole & mask)];
            const auto b = lines[ch][(size_t) ((whole + 1) & mask)];
            echo[ch] = a + frac * (b - a);
        }

        // What goes into each line: the send and the repeat coming back round
        // (gently clipped), through the filters. So the first echo is
        // filtered once, the second twice, and so on.
        std::array<float, 2> into {};

        if (s.pingPong)
        {
            // The send enters on the left; each side feeds the other.
            into[0] = 0.5f * (inL[i] + inR[i]) + Saturator::shape (echo[1] * feedback);
            into[1] = Saturator::shape (echo[0] * feedback);
        }
        else
        {
            into[0] = inL[i] + Saturator::shape (echo[0] * feedback);
            into[1] = inR[i] + Saturator::shape (echo[1] * feedback);
        }

        for (size_t ch = 0; ch < 2; ++ch)
            lines[ch][(size_t) write] = hc[ch].process (highCutCoefficients, lc[ch].process (lowCutCoefficients, into[ch]));

        write = (write + 1) & mask;
        outL[i] = echo[0];
        outR[i] = echo[1];
    }

    lowCut = lc;
    highCut = hc;
}

} // namespace lithium
