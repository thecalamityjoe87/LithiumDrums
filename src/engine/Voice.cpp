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

    std::int64_t toFixed (double ratio) noexcept
    {
        return std::llround (std::clamp (ratio, 1.0 / 256.0, (double) Voice::maxPitchRatio) * (double) one);
    }
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

    baseRatio = trigger.pitchRatio;
    increment = (std::uint64_t) toFixed (baseRatio);
    fraction = 0;
    pitchOffset = trigger.pitchEnvSemitones;
    pitchDecay = trigger.pitchEnvDecay;

    gain = trigger.gain;
    decayEnvelope = 1.0f;
    age = 0;
    attackSamples = std::max (0, trigger.attackSamples);
    holdSamples = std::max (0, trigger.holdSamples);
    fadeGain = 1.0f;
    fadeStep = 0.0f;
    gainsSet = false;    // the first render jumps straight to the target gains

    numStreams = 0;
    longestStream = 0;

    for (int m = 0; m < kit.numMics; ++m)
    {
        const auto id = kit.getSample (trigger.hit, m);

        if (id == invalidSampleId)
            continue;

        const auto& mic = kit.mics[(size_t) m];
        auto& s = streams[(size_t) numStreams++];
        s.mic = m;
        s.numChannels = std::min ({ kit.samples->getNumChannels (id), mic.numChannels, maxChannelsPerMic });
        s.busChannel = mic.busChannel;
        s.role = mic.role;
        s.direct = mic.direct;
        s.numFrames = kit.samples->getNumFrames (id);
        s.tone = {};
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

std::array<float, 2> Voice::targetGains (const MicStream& s, const VoiceRenderContext& ctx) const noexcept
{
    if (s.direct)
        return ctx.directGains[(size_t) piece][(size_t) s.mic];

    const auto g = ctx.roleGains[(size_t) piece][(size_t) s.role];
    return { g, g };
}

void Voice::render (Kit& kit, VoiceBuses buses, int startSample, int numSamples, const VoiceRenderContext& ctx) noexcept
{
    if (! active || numSamples <= 0)
        return;

    const auto p = (size_t) piece;

    // 1. The amplitude envelope for the whole slice: velocity/zone gain x
    //    attack x decay (after the hold) x fade.
    const auto decayCoefficient = ctx.decayCoefficient[p];
    auto* env = ctx.envelope;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto attack = age < attackSamples ? (float) (age + 1) / (float) attackSamples : 1.0f;

        if (age >= holdSamples)
            decayEnvelope *= decayCoefficient;

        if (age < std::max (attackSamples, holdSamples))
            ++age;

        if (fadeStep > 0.0f)
            fadeGain = std::max (0.0f, fadeGain - fadeStep);

        env[i] = gain * attack * decayEnvelope * fadeGain;
    }

    if (! gainsSet)
    {
        for (int si = 0; si < numStreams; ++si)
            streams[(size_t) si].lastGain = targetGains (streams[(size_t) si], ctx);

        gainsSet = true;
    }

    const auto& highPass = ctx.toneHighPass[p];
    const auto& lowPass = ctx.toneLowPass[p];
    const auto highPassOn = ! highPass.isIdentity(), lowPassOn = ! lowPass.isIdentity();

    // 2. Resample every stream, in chunks. All streams share one position,
    //    so each consumes exactly the same number of source frames. While the
    //    pitch envelope moves, the rate changes linearly within each short
    //    chunk; otherwise the whole slice is one chunk at a fixed rate.
    const auto pitchMoving = std::abs (pitchOffset) > 0.001f;
    const auto chunkLength = pitchMoving ? pitchEnvelopeStep : numSamples;

    for (int offset = 0; offset < numSamples; offset += chunkLength)
    {
        const auto n = std::min (chunkLength, numSamples - offset);
        auto inc0 = (std::int64_t) increment;
        std::int64_t step = 0;

        if (std::abs (pitchOffset) > 0.001f)
        {
            const auto endOffset = pitchOffset * std::pow (pitchDecay, (float) n);
            inc0 = toFixed (baseRatio * std::exp2 (pitchOffset / 12.0));
            const auto inc1 = toFixed (baseRatio * std::exp2 (endOffset / 12.0));
            step = (inc1 - inc0) / n;
            pitchOffset = std::abs (endOffset) > 0.001f ? endOffset : 0.0f;
        }

        // Where the position ends up: the sum of n increments growing by `step`.
        const auto end = (std::int64_t) fraction + (std::int64_t) n * inc0 + step * (std::int64_t) n * (n - 1) / 2;
        const auto framesNeeded = (int) (end >> 32);

        for (int si = 0; si < numStreams; ++si)
        {
            auto& s = streams[(size_t) si];

            if (nextFrame >= s.numFrames + 4)
                continue;     // this mic's recording has fully played out; its window is all zeros

            kit.samples->read (s.cursor, ctx.scratch.data(), framesNeeded);

            for (int ch = 0; ch < s.numChannels; ++ch)
            {
                auto* out = ctx.streamOut[(size_t) ch];
                const auto* src = ctx.scratch[(size_t) ch];
                auto& w = s.window[(size_t) ch];
                auto xm1 = w[0], x0 = w[1], x1 = w[2], x2 = w[3];
                auto pos = (std::uint64_t) fraction;
                auto inc = inc0;
                int k = 0;

                for (int i = 0; i < n; ++i)
                {
                    out[i] = hermite (xm1, x0, x1, x2, (float) (std::uint32_t) pos * fractionScale) * env[offset + i];
                    pos += (std::uint64_t) inc;
                    inc += step;

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

                // The piece's tone filters, per stream (each mic of the hit).
                // Copies of the state, so it stays in registers (see ChannelEq).
                for (const auto [on, c, which] : { std::tuple { highPassOn, &highPass, 0 }, { lowPassOn, &lowPass, 1 } })
                {
                    if (! on)
                        continue;

                    auto state = s.tone[(size_t) which][(size_t) ch];

                    for (int i = 0; i < n; ++i)
                        out[i] = state.process (*c, out[i]);

                    s.tone[(size_t) which][(size_t) ch] = state;
                }
            }

            // 3. Mix into the buses, ramping each stream's gain across the
            //    whole slice so fader moves don't click.
            const auto target = targetGains (s, ctx);
            const auto stepL = (target[0] - s.lastGain[0]) / (float) numSamples;
            const auto stepR = (target[1] - s.lastGain[1]) / (float) numSamples;
            auto gl = s.lastGain[0] + stepL * (float) offset;
            auto gr = s.lastGain[1] + stepR * (float) offset;
            const auto* left = ctx.streamOut[0];
            const auto* right = ctx.streamOut[(size_t) s.numChannels - 1];

            if (s.direct)
            {
                // Into the piece's stereo channel; a mono mic feeds both sides.
                auto* outL = buses.pieces.getWritePointer ((int) p * 2, startSample + offset);
                auto* outR = buses.pieces.getWritePointer ((int) p * 2 + 1, startSample + offset);

                for (int i = 0; i < n; ++i)
                {
                    gl += stepL;
                    gr += stepR;
                    outL[i] += left[i] * gl;
                    outR[i] += right[i] * gr;
                }
            }
            else
            {
                for (int ch = 0; ch < s.numChannels; ++ch)
                {
                    auto* out = buses.mics.getWritePointer (s.busChannel + ch, startSample + offset);
                    const auto* x = ctx.streamOut[(size_t) ch];
                    auto g = gl;

                    for (int i = 0; i < n; ++i)
                    {
                        g += stepL;
                        out[i] += x[i] * g;
                    }
                }
            }
        }

        fraction = (std::uint32_t) (end & (std::int64_t) (one - 1));
        nextFrame += framesNeeded;
    }

    for (int si = 0; si < numStreams; ++si)
        streams[(size_t) si].lastGain = targetGains (streams[(size_t) si], ctx);

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
