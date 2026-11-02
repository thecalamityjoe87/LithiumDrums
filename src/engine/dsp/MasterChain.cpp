#include "MasterChain.h"

#include <cmath>

namespace lithium
{

namespace
{
    constexpr double switchSeconds = 0.02;
    constexpr float tapeBias = 0.18f;            // how far off centre the tape curve sits

    float dbToGain (float db) noexcept           { return std::exp2 (db / 6.0205999f); }
}

//==============================================================================
void TapeSaturator::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    oversampling.initProcessing ((size_t) maxBlockSize);
    dry.setSize (2, maxBlockSize);
    wet.reset (sampleRate, switchSeconds);
    drive.reset (sampleRate, switchSeconds);
    output.reset (sampleRate, switchSeconds);
    lastTone = 100.0f;
    reset();
}

void TapeSaturator::reset() noexcept
{
    oversampling.reset();
    wet.setCurrentAndTargetValue (0.0f);

    for (auto& stage : tone)
        for (auto& s : stage)
            s.reset();
}

void TapeSaturator::process (float* const* channels, int numSamples, const TapeSettings& s) noexcept
{
    wet.setTargetValue (s.enabled ? 1.0f : 0.0f);
    drive.setTargetValue (dbToGain (s.driveDb));
    output.setTargetValue (dbToGain (s.outputDb));

    if (! wet.isSmoothing() && juce::exactlyEqual (wet.getCurrentValue(), 0.0f))
    {
        drive.skip (numSamples);
        output.skip (numSamples);
        return;
    }

    if (! juce::exactlyEqual (s.tone, lastTone))
    {
        lastTone = s.tone;
        const auto t = juce::jlimit (-1.0f, 1.0f, s.tone);

        // A tilt around 1 kHz: one shelf goes up as the other goes down.
        toneCoefficients[0] = dsp::designSvf (dsp::SvfType::lowShelf, 250.0, -3.0 * t, 0.7071, sampleRate);
        toneCoefficients[1] = dsp::designSvf (dsp::SvfType::highShelf, 4000.0, 4.0 * t, 0.7071, sampleRate);
        // The off-centre curve adds a little DC; a 10 Hz high-pass takes it out.
        toneCoefficients[2] = dsp::designSvf (dsp::SvfType::highPass, 10.0, 0.0, 0.7071, sampleRate);
    }

    for (int ch = 0; ch < 2; ++ch)
        dry.copyFrom (ch, 0, channels[ch], numSamples);

    // Drive in, per sample at the base rate (the smoothing is cheap there).
    auto driveNow = drive;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto d = driveNow.getNextValue();
        channels[0][i] *= d;
        channels[1][i] *= d;
    }

    // Clip at four times the rate. AudioBlock just wraps the channel pointers.
    juce::dsp::AudioBlock<float> block (channels, 2, (size_t) numSamples);
    auto up = oversampling.processSamplesUp (block);
    const auto shapeZero = Saturator::shape (tapeBias);

    for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
    {
        auto* x = up.getChannelPointer (ch);

        for (size_t i = 0; i < up.getNumSamples(); ++i)
            x[i] = Saturator::shape (x[i] + tapeBias) - shapeZero;
    }

    oversampling.processSamplesDown (block);

    // Back to the input's level (quiet signals pass unchanged), the tone tilt and the output.
    for (int ch = 0; ch < 2; ++ch)
    {
        auto d = drive, o = output, w = wet;
        auto low = tone[0][(size_t) ch], high = tone[1][(size_t) ch], dc = tone[2][(size_t) ch];
        const auto* in = dry.getReadPointer (ch);
        auto* x = channels[ch];

        for (int i = 0; i < numSamples; ++i)
        {
            auto y = x[i] / d.getNextValue();
            y = dc.process (toneCoefficients[2], high.process (toneCoefficients[1], low.process (toneCoefficients[0], y)));
            y *= o.getNextValue();
            x[i] = in[i] + w.getNextValue() * (y - in[i]);
        }

        tone[0][(size_t) ch] = low;
        tone[1][(size_t) ch] = high;
        tone[2][(size_t) ch] = dc;
    }

    drive.skip (numSamples);
    output.skip (numSamples);
    wet.skip (numSamples);
}

//==============================================================================
void Limiter::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    lookahead = std::max (1, (int) std::lround (lookaheadMs * 0.001 * sampleRate));

    // The windows hold lookahead + 1 values (see process()). The queue's
    // ring has a spare slot, as head == tail means it's empty.
    const auto window = (size_t) lookahead + 1;

    for (auto& d : delay)
        d.assign ((size_t) lookahead, 0.0f);

    minValues.assign (window + 1, 1.0f);
    minIndices.assign (window + 1, 0);
    averageRing.assign (window, 1.0f);
    wet.reset (sampleRate, switchSeconds);
    lastReleaseMs = -1.0f;
    reset();
}

void Limiter::reset() noexcept
{
    for (auto& d : delay)
        std::fill (d.begin(), d.end(), 0.0f);

    std::fill (averageRing.begin(), averageRing.end(), 1.0f);
    averageSum = (double) averageRing.size();
    minHead = minTail = averagePosition = delayPosition = 0;
    sampleIndex = 0;
    envelope = 1.0f;
    wet.setCurrentAndTargetValue (0.0f);
}

void Limiter::process (float* const* channels, int numSamples, const LimiterSettings& s) noexcept
{
    if (s.lookahead != usedLookahead)
    {
        // The delay changes length (the host is told of the new latency on
        // the message thread); start the windows afresh.
        usedLookahead = s.lookahead;
        const auto w = wet.getTargetValue();
        reset();
        wet.setCurrentAndTargetValue (w);
    }

    wet.setTargetValue (s.enabled ? 1.0f : 0.0f);

    if (! juce::exactlyEqual (s.releaseMs, lastReleaseMs))
        releaseCoefficient = (float) std::exp (-1.0 / (std::max (1.0f, lastReleaseMs = s.releaseMs) * 0.001 * sampleRate));

    const auto ceiling = dbToGain (juce::jlimit (-24.0f, 0.0f, s.ceilingDb));
    const auto window = lookahead + 1;
    const auto ringSize = (int) minValues.size();
    auto* left = channels[0];
    auto* right = channels[1];
    auto lowest = 1.0f;

    // Bypassed without lookahead: nothing to do, and no delay to keep.
    if (! s.lookahead && ! wet.isSmoothing() && juce::exactlyEqual (wet.getCurrentValue(), 0.0f))
    {
        envelope = 1.0f;
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const auto peak = std::max (std::abs (left[i]), std::abs (right[i]));
        const auto needed = peak > ceiling ? ceiling / peak : 1.0f;
        float gain = needed;
        auto l = left[i], r = right[i];

        if (s.lookahead)
        {
            // The lowest gain needed over the last `window` samples. The
            // queue keeps candidates in increasing order of gain: a new value
            // knocks out every larger one behind it, and the front drops off
            // once it's older than the window.
            while (minTail != minHead && minValues[(size_t) ((minTail - 1 + ringSize) % ringSize)] >= needed)
                minTail = (minTail - 1 + ringSize) % ringSize;

            minValues[(size_t) minTail] = needed;
            minIndices[(size_t) minTail] = sampleIndex;
            minTail = (minTail + 1) % ringSize;

            if (minIndices[(size_t) minHead] <= sampleIndex - window)
                minHead = (minHead + 1) % ringSize;

            const auto held = minValues[(size_t) minHead];

            // The moving average over the same window. Every value averaged
            // here covers the sample leaving the delay line now, and each is
            // at most the gain it needs, so the average is too: the ceiling
            // holds, and the gain arrives at it smoothly.
            averageSum += held - averageRing[(size_t) averagePosition];
            averageRing[(size_t) averagePosition] = held;
            averagePosition = (averagePosition + 1) % window;
            gain = (float) (averageSum / window);

            l = delay[0][(size_t) delayPosition];
            r = delay[1][(size_t) delayPosition];
            delay[0][(size_t) delayPosition] = left[i];
            delay[1][(size_t) delayPosition] = right[i];
            delayPosition = (delayPosition + 1) % lookahead;
            ++sampleIndex;
        }

        // Down at once, back up at the release rate (never above what's needed).
        envelope = gain < envelope ? gain : gain + releaseCoefficient * (envelope - gain);

        const auto w = wet.getNextValue();
        const auto g = 1.0f + w * (envelope - 1.0f);
        left[i] = l * g;
        right[i] = r * g;

        // Rounding in the running sum could let a peak overshoot by a hair.
        if (w >= 1.0f)
        {
            left[i] = juce::jlimit (-ceiling, ceiling, left[i]);
            right[i] = juce::jlimit (-ceiling, ceiling, right[i]);
        }

        lowest = std::min (lowest, g);
    }

    maxReductionDb = std::max (maxReductionDb, -juce::Decibels::gainToDecibels (lowest, -60.0f));
}

//==============================================================================
CompSettings MasterChain::crushCompressor (const CrushSettings& c) noexcept
{
    // Hard and fast: a 20:1 ratio with the threshold pulled down by the
    // drive, and most of the lost level made up, so the crushed bus sits
    // under the clean one at a similar loudness. The blend is the mix.
    CompSettings s;
    s.enabled = c.enabled;
    s.thresholdDb = -c.driveDb;
    s.ratio = 20.0f;
    s.attackMs = c.attackMs;
    s.releaseMs = c.releaseMs;
    s.makeupDb = c.driveDb * 0.8f;
    s.mix = c.blend;
    return s;
}

void MasterChain::prepare (double sampleRate, int maxBlockSize)
{
    eq.prepare (sampleRate);
    glue.prepare (sampleRate);
    crush.prepare (sampleRate);
    tape.prepare (sampleRate, maxBlockSize);
    transient.prepare (sampleRate);
    limiter.prepare (sampleRate);
}

void MasterChain::reset() noexcept
{
    eq.reset();
    glue.reset();
    crush.reset();
    tape.reset();
    transient.reset();
    limiter.reset();
}

void MasterChain::process (float* const* channels, int numSamples, const MasterSettings& s) noexcept
{
    eq.process (channels, 2, numSamples, s.eq);
    glue.process (channels, 2, numSamples, s.glue);
    crush.process (channels, 2, numSamples, crushCompressor (s.crush));
    tape.process (channels, numSamples, s.tape);
    transient.process (channels, 2, numSamples, s.transient);
    limiter.process (channels, numSamples, s.limiter);
}

} // namespace lithium
