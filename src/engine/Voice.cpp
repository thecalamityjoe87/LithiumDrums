#include "Voice.h"

#include <algorithm>
#include <cmath>

#if defined (__SSE2__) || defined (_M_X64)
 #include <emmintrin.h>
 #include <xmmintrin.h>
#endif

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

   #if defined (__SSE2__) || defined (_M_X64)
    #define LITHIUM_VOICE_SSE2 1

    /** Four outputs of Voice's resampler at once, with SSE2 (always there on
        64-bit x86). `src` is this chunk's frames, where whole position m
        reads src[m - 4 .. m - 1] (every m here is at least 4), and
        pos/inc/step are the 32.32 position, increment and increment step,
        advanced past the four.

        Each lane's four frames sit next to each other in memory, so
        one unaligned load per lane fetches them as a row; a 4x4 transpose
        then turns the four rows into four columns: every lane's x-1, every
        lane's x0, and so on. (Letting the compiler vectorise a plain loop over
        four lanes was no faster: it gathered the 16 frames one by one.)

        The arithmetic is the same as hermite(), operation for operation, and
        SSE's packed float operations round exactly like the scalar ones, so
        the output is bit-identical to the scalar loop. */
    inline void hermite4 (const float* src, std::uint64_t& pos, std::int64_t& inc, std::int64_t step,
                          const float* env, float* out) noexcept
    {
        std::array<std::uint64_t, 4> p;

        for (auto& lane : p)
        {
            lane = pos;
            pos += (std::uint64_t) inc;
            inc += step;
        }

        auto row = [src] (std::uint64_t lane) noexcept { return _mm_loadu_ps (src + ((std::ptrdiff_t) (lane >> 32) - 4)); };
        auto xm1 = row (p[0]), x0 = row (p[1]), x1 = row (p[2]), x2 = row (p[3]);
        _MM_TRANSPOSE4_PS (xm1, x0, x1, x2);

        // The fractions (the low 32 bits) as floats. SSE2 only converts
        // signed integers, so convert the top and bottom 16 bits separately:
        // both convert exactly, and hi x 65536 + lo then rounds once, to the
        // same float as converting the unsigned 32-bit value directly.
        const auto frac = _mm_set_epi32 ((int) (std::uint32_t) p[3], (int) (std::uint32_t) p[2],
                                         (int) (std::uint32_t) p[1], (int) (std::uint32_t) p[0]);
        const auto hi = _mm_cvtepi32_ps (_mm_srli_epi32 (frac, 16));
        const auto lo = _mm_cvtepi32_ps (_mm_and_si128 (frac, _mm_set1_epi32 (0xffff)));
        const auto t = _mm_mul_ps (_mm_add_ps (_mm_mul_ps (hi, _mm_set1_ps (65536.0f)), lo), _mm_set1_ps (fractionScale));

        // hermite(), four lanes at a time.
        const auto half = _mm_set1_ps (0.5f);
        const auto c1 = _mm_mul_ps (half, _mm_sub_ps (x1, xm1));
        const auto c2 = _mm_sub_ps (_mm_add_ps (_mm_sub_ps (xm1, _mm_mul_ps (_mm_set1_ps (2.5f), x0)),
                                                _mm_mul_ps (_mm_set1_ps (2.0f), x1)),
                                    _mm_mul_ps (half, x2));
        const auto c3 = _mm_add_ps (_mm_mul_ps (half, _mm_sub_ps (x2, xm1)),
                                    _mm_mul_ps (_mm_set1_ps (1.5f), _mm_sub_ps (x0, x1)));
        const auto h = _mm_add_ps (_mm_mul_ps (_mm_add_ps (_mm_mul_ps (_mm_add_ps (_mm_mul_ps (c3, t), c2), t), c1), t), x0);

        _mm_storeu_ps (out, _mm_mul_ps (h, _mm_loadu_ps (env)));
    }
   #endif

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
    positionTone = trigger.positionTone;

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
        s.sourceGain = kit.pieces[(size_t) p].micGain[(size_t) m];
        s.feedsEmulation = ((kit.pieces[(size_t) p].emulationSources >> m) & 1u) != 0;
        s.numFrames = kit.samples->getNumFrames (id);
        s.tone = {};
        s.position = {};
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
    {
        const auto& g = ctx.directGains[(size_t) piece][(size_t) s.mic];
        return { g[0] * s.sourceGain, g[1] * s.sourceGain };
    }

    const auto g = ctx.roleGains[(size_t) piece][(size_t) s.role] * s.sourceGain;
    return { g, g };
}

float Voice::emulationGain (const MicStream& s, const VoiceRenderContext& ctx) const noexcept
{
    if (! s.feedsEmulation)
        return 0.0f;

    // A stereo mic goes into the mono feed as the average of its sides.
    return ctx.emulationLevel[(size_t) piece] * ctx.emulationMicGain[(size_t) s.mic] * s.sourceGain / (float) s.numChannels;
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
        {
            auto& s = streams[(size_t) si];
            s.lastGain = targetGains (s, ctx);
            s.lastEmulationGain = emulationGain (s, ctx);
        }

        gainsSet = true;
    }

    const auto& highPass = ctx.toneHighPass[p];
    const auto& lowPass = ctx.toneLowPass[p];
    const auto highPassOn = ! highPass.isIdentity(), lowPassOn = ! lowPass.isIdentity();
    const auto positionOn = ! positionTone[0].isIdentity() || ! positionTone[1].isIdentity();

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

        // Unity pitch, sitting exactly on a source frame: the common case when
        // the session runs at the kit's sample rate and the drum isn't tuned.
        // Hermite at t = 0 returns x0 exactly, so every output sample is just a
        // source frame; skipping the interpolator gives identical output.
        const auto unity = inc0 == (std::int64_t) one && step == 0 && fraction == 0;

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

                // Think of the window followed by this chunk's frames as one
                // sequence c = w[0..3], src[0..framesNeeded-1]. Before output i
                // the position is fraction + (the increments so far); its whole
                // part m says how many frames the window has moved on, so the
                // four frames to interpolate are c[m..m+3], and its low 32 bits
                // are the fraction between c[m+1] and c[m+2].
                auto frame = [&] (int index) noexcept { return index < 4 ? w[(size_t) index] : src[index - 4]; };

                if (unity)
                {
                    // m = i and the fraction is 0, where Hermite returns c[m+1]
                    // exactly: the output is the frames themselves. This loop has
                    // no branches or loop-carried state, so the compiler can
                    // vectorise it.
                    const auto fromWindow = std::min (n, 3);

                    for (int i = 0; i < fromWindow; ++i)
                        out[i] = w[(size_t) i + 1] * env[offset + i];

                    for (int i = 3; i < n; ++i)
                        out[i] = src[i - 3] * env[offset + i];
                }
                else
                {
                    // The position is kept absolute (never wrapped back below 1),
                    // so each output finds its frames by index instead of
                    // shifting the window frame by frame in a while loop: that
                    // loop's branch went either way in an irregular pattern at
                    // ratios like 44.1/48 kHz, and mispredicted often. Same
                    // frames and same fraction, so the same output bit for bit.
                    auto pos = (std::uint64_t) fraction;
                    auto inc = inc0;
                    int i = 0;

                    // The first few outputs still reach back into the window (m < 4).
                    for (; i < n && (pos >> 32) < 4; ++i)
                    {
                        const auto m = (int) (pos >> 32);
                        out[i] = hermite (frame (m), frame (m + 1), frame (m + 2), frame (m + 3),
                                          (float) (std::uint32_t) pos * fractionScale) * env[offset + i];
                        pos += (std::uint64_t) inc;
                        inc += step;
                    }

                    // The rest read only this chunk's frames: c[m] is src[m - 4].
                    // On x86, four outputs at a time (see hermite4).
                   #if LITHIUM_VOICE_SSE2
                    for (; i + 4 <= n; i += 4)
                        hermite4 (src, pos, inc, step, env + offset + i, out + i);
                   #endif

                    // One at a time: the last few, or all of them without SSE2.
                    for (; i < n; ++i)
                    {
                        const auto* x = src + ((int) (pos >> 32) - 4);
                        out[i] = hermite (x[0], x[1], x[2], x[3], (float) (std::uint32_t) pos * fractionScale) * env[offset + i];
                        pos += (std::uint64_t) inc;
                        inc += step;
                    }

                    jassert ((int) (pos >> 32) == framesNeeded);
                }

                // The window moves on by framesNeeded: the new one is
                // c[framesNeeded..framesNeeded+3]. While framesNeeded < 4 part of it
                // is still old window frames, so build it in a copy, not in place.
                std::array<float, 4> next;

                for (int j = 0; j < 4; ++j)
                    next[(size_t) j] = frame (framesNeeded + j);

                w = next;

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

                // The emulated strike position, on this hit alone.
                if (positionOn)
                {
                    auto low = s.position[0][(size_t) ch], high = s.position[1][(size_t) ch];

                    for (int i = 0; i < n; ++i)
                        out[i] = high.process (positionTone[1], low.process (positionTone[0], out[i]));

                    s.position[0][(size_t) ch] = low;
                    s.position[1][(size_t) ch] = high;
                }
            }

            // 3. Mix into the buses, ramping each stream's gain across the
            //    whole slice so fader moves don't click.
            const auto target = targetGains (s, ctx);
            const auto stepL = (target[0] - s.lastGain[0]) / (float) numSamples;
            const auto stepR = (target[1] - s.lastGain[1]) / (float) numSamples;
            // The gain at output i is start + (i + 1) x step, worked out from i
            // rather than accumulated (g += step). An accumulated ramp makes
            // every sample wait for the previous sample's add, which stops the
            // compiler vectorising these loops; the two differ only by float rounding.
            const auto startL = s.lastGain[0] + stepL * (float) offset;
            const auto startR = s.lastGain[1] + stepR * (float) offset;
            const auto* left = ctx.streamOut[0];
            const auto* right = ctx.streamOut[(size_t) s.numChannels - 1];

            if (s.direct)
            {
                // Into the piece's stereo channel; a mono mic feeds both sides.
                auto* outL = buses.pieces.getWritePointer ((int) p * 2, startSample + offset);
                auto* outR = buses.pieces.getWritePointer ((int) p * 2 + 1, startSample + offset);

                for (int i = 0; i < n; ++i)
                {
                    const auto ramp = (float) (i + 1);
                    outL[i] += left[i] * (startL + ramp * stepL);
                    outR[i] += right[i] * (startR + ramp * stepR);
                }
            }
            else
            {
                for (int ch = 0; ch < s.numChannels; ++ch)
                {
                    auto* out = buses.mics.getWritePointer (s.busChannel + ch, startSample + offset);
                    const auto* x = ctx.streamOut[(size_t) ch];

                    for (int i = 0; i < n; ++i)
                        out[i] += x[i] * (startL + (float) (i + 1) * stepL);
                }
            }

            // 4. The emulated mics' feed: the piece's mono channel in it,
            //    ramped the same way. Skipped while it's silent and settled.
            if (s.feedsEmulation && buses.emulation != nullptr)
            {
                const auto targetE = emulationGain (s, ctx);

                if (! juce::exactlyEqual (targetE, 0.0f) || ! juce::exactlyEqual (s.lastEmulationGain, 0.0f))
                {
                    const auto stepE = (targetE - s.lastEmulationGain) / (float) numSamples;
                    const auto startE = s.lastEmulationGain + stepE * (float) offset;
                    auto* out = buses.emulation->getWritePointer ((int) p, startSample + offset);

                    for (int ch = 0; ch < s.numChannels; ++ch)
                    {
                        const auto* x = ctx.streamOut[(size_t) ch];

                        for (int i = 0; i < n; ++i)
                            out[i] += x[i] * (startE + (float) (i + 1) * stepE);
                    }
                }
            }
        }

        fraction = (std::uint32_t) (end & (std::int64_t) (one - 1));
        nextFrame += framesNeeded;
    }

    for (int si = 0; si < numStreams; ++si)
    {
        auto& s = streams[(size_t) si];
        s.lastGain = targetGains (s, ctx);
        s.lastEmulationGain = emulationGain (s, ctx);
    }

    // 5. Finished? Faded out, decayed to silence, or every recording played out.
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
