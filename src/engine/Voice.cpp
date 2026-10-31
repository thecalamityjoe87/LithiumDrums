#include "Voice.h"

#include <algorithm>
#include <cmath>

namespace lithium
{

namespace
{
    constexpr std::uint64_t one = 1ull << 32;     // 1.0 in 32.32 fixed point
    constexpr float fractionScale = 1.0f / 4294967296.0f;

    // 4-point, 3rd-order Hermite interpolation between x0 and x1 at t in [0, 1).
    // Much cleaner than linear interpolation when pitching, and still cheap.
    inline float hermite (float xm1, float x0, float x1, float x2, float t) noexcept
    {
        const auto c1 = 0.5f * (x1 - xm1);
        const auto c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const auto c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    // Voices end once their envelope falls below -80 dB.
    constexpr float silenceThreshold = 1.0e-4f;
}

void Voice::start (Kit& kit, int p, std::uint32_t chokeGroupMask, const VoiceTrigger& trigger,
                   std::uint64_t newSerial) noexcept
{
    if (active)
        closeStreams (kit);

    piece = p;
    articulation = trigger.articulation;
    chokeGroups = chokeGroupMask;
    serial = newSerial;

    const auto ratio = std::clamp (trigger.pitchRatio, 1.0 / 256.0, (double) maxPitchRatio);
    increment = (std::uint64_t) std::llround (ratio * (double) one);
    fraction = 0;

    gain = trigger.gain;
    decayEnvelope = 1.0f;
    samplesPlayed = 0;
    fadeGain = 1.0f;
    fadeStep = 0.0f;
    lastRoleGains.fill (-1.0f);     // "not set yet": the first render jumps straight to the target

    numStreams = 0;
    longestStream = 0;

    for (int m = 0; m < kit.numMics; ++m)
    {
        const auto id = kit.getSample (trigger.hit, m);

        if (id == invalidSampleId)
            continue;

        const auto& mic = kit.mics[(size_t) m];
        auto& s = streams[(size_t) numStreams++];
        s.numChannels = std::min ({ kit.samples->getNumChannels (id), mic.numChannels, maxChannelsPerMic });
        s.busChannel = mic.busChannel;
        s.role = mic.role;
        s.numFrames = kit.samples->getNumFrames (id);
        longestStream = std::max (longestStream, s.numFrames);

        kit.samples->open (id, s.cursor);

        // Prime the interpolation window with the first three frames, so the
        // first output sample is exactly the first sample of the recording.
        std::array<std::array<float, 3>, maxChannelsPerMic> first {};
        std::array<float*, maxChannelsPerMic> firstPtrs { first[0].data(), first[1].data() };
        kit.samples->read (s.cursor, firstPtrs.data(), 3);

        for (int ch = 0; ch < s.numChannels; ++ch)
            s.window[(size_t) ch] = { 0.0f, first[(size_t) ch][0], first[(size_t) ch][1], first[(size_t) ch][2] };
    }

    nextFrame = 3;
    active = numStreams > 0;
}

void Voice::render (Kit& kit, juce::AudioBuffer<float>& micBus, int startSample, int numSamples,
                    const VoiceRenderContext& ctx) noexcept
{
    if (! active || numSamples <= 0)
        return;

    const auto p = (size_t) piece;

    // 1. The amplitude envelope for this block: velocity/layer gain x decay x fade.
    const auto decayCoefficient = ctx.decayCoefficient[p];
    auto* env = ctx.envelope;

    for (int i = 0; i < numSamples; ++i)
    {
        if (samplesPlayed >= ctx.decayHoldSamples)
            decayEnvelope *= decayCoefficient;
        else
            ++samplesPlayed;

        if (fadeStep > 0.0f)
            fadeGain = std::max (0.0f, fadeGain - fadeStep);

        env[i] = gain * decayEnvelope * fadeGain;
    }

    // 2. Mixer gains per mic role, ramped across the block so fader moves don't click.
    const auto& targetRoleGains = ctx.roleGains[p];

    if (lastRoleGains[0] < 0.0f)
        lastRoleGains = targetRoleGains;

    // 3. Resample every mic stream. All streams share the same position, so
    //    each one consumes exactly `framesNeeded` new source frames.
    const auto end = (std::uint64_t) fraction + (std::uint64_t) numSamples * increment;
    const auto framesNeeded = (int) (end >> 32);

    for (int si = 0; si < numStreams; ++si)
    {
        auto& s = streams[(size_t) si];

        if (nextFrame >= s.numFrames + 4)
            continue;     // this mic's recording has fully played out; its window is all zeros

        kit.samples->read (s.cursor, ctx.scratch.data(), framesNeeded);

        const auto role = (size_t) s.role;
        const auto g0 = lastRoleGains[role];
        const auto gStep = (targetRoleGains[role] - g0) / (float) numSamples;

        for (int ch = 0; ch < s.numChannels; ++ch)
        {
            auto* out = micBus.getWritePointer (s.busChannel + ch, startSample);
            const auto* src = ctx.scratch[(size_t) ch];
            auto& w = s.window[(size_t) ch];
            auto xm1 = w[0], x0 = w[1], x1 = w[2], x2 = w[3];
            auto pos = (std::uint64_t) fraction;
            auto g = g0;
            int k = 0;

            for (int i = 0; i < numSamples; ++i)
            {
                out[i] += hermite (xm1, x0, x1, x2, (float) (std::uint32_t) pos * fractionScale) * env[i] * g;
                g += gStep;
                pos += increment;

                while (pos >= one)
                {
                    pos -= one;
                    xm1 = x0;
                    x0 = x1;
                    x1 = x2;
                    x2 = src[k++];
                }
            }

            jassert (k == framesNeeded);
            w = { xm1, x0, x1, x2 };
        }
    }

    lastRoleGains = targetRoleGains;
    fraction = (std::uint32_t) (end & (one - 1));
    nextFrame += framesNeeded;

    // 4. Finished? Faded out, decayed to silence, or every recording played out.
    const auto envelopeNow = decayEnvelope * fadeGain;

    if (envelopeNow < silenceThreshold || nextFrame >= longestStream + 4)
        stop (kit);
}

void Voice::fadeOut (int numSamples) noexcept
{
    // Never slow down a fade that is already running faster.
    fadeStep = std::max (fadeStep, fadeGain / (float) std::max (1, numSamples));
}

void Voice::stop (Kit& kit) noexcept
{
    closeStreams (kit);
    active = false;
}

float Voice::getStealScore() const noexcept
{
    const auto remaining = longestStream > 0 ? 1.0f - (float) nextFrame / (float) longestStream : 0.0f;
    return gain * decayEnvelope * fadeGain * remaining * remaining;
}

void Voice::closeStreams (Kit& kit) noexcept
{
    for (int i = 0; i < numStreams; ++i)
        kit.samples->close (streams[(size_t) i].cursor);

    numStreams = 0;
}

} // namespace lithium
