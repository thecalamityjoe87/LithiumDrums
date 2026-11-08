#include "Mixer.h"

#include "dsp/Pan.h"

#include <cmath>

namespace lithium
{

namespace
{
    constexpr double smoothingSeconds = 0.02;

    bool silentAndSettled (const juce::SmoothedValue<float>& v) noexcept
    {
        return ! v.isSmoothing() && juce::exactlyEqual (v.getCurrentValue(), 0.0f);
    }

    enum ReturnIndex { roomReturn, plateReturn, delayReturn };

    float peakOf (const float* x, int n) noexcept
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (x, n);
        return std::max (range.getEnd(), -range.getStart());
    }
}

void Mixer::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    micBus.setSize (maxMicBusChannels, maxBlockSize);
    micBus.clear();
    pieceBus.setSize (maxPieces * 2, maxBlockSize);
    pieceBus.clear();
    masterBus.setSize (2, maxBlockSize);
    wet.setSize (2, maxBlockSize);

    for (auto& b : sendBuses)
        b.setSize (2, maxBlockSize);

    auto prepareStrip = [newSampleRate] (Strip& s)
    {
        s.chain.prepare (newSampleRate);
        s.gainLeft.reset (newSampleRate, smoothingSeconds);
        s.gainRight.reset (newSampleRate, smoothingSeconds);

        for (auto& level : s.sends)
            level.reset (newSampleRate, smoothingSeconds);
    };

    for (auto& s : pieceStrips)
        prepareStrip (s);

    for (auto& s : micStrips)
        prepareStrip (s);

    for (auto& r : returns)
    {
        r.gainLeft.reset (sampleRate, smoothingSeconds);
        r.gainRight.reset (sampleRate, smoothingSeconds);
        r.mix.reset (sampleRate, smoothingSeconds);
        r.mix.setCurrentAndTargetValue (1.0f);
        r.resting = true;
    }

    room.prepare (sampleRate, maxBlockSize);
    plate.prepare (sampleRate);
    delay.prepare (sampleRate);
    chain.prepare (sampleRate, maxBlockSize);

    for (auto& aux : auxDelay)
        for (auto& line : aux)
            line.assign ((size_t) std::max (1, chain.getLatencySamples (true)), 0.0f);

    auxDelayPosition = 0;
    masterGain.reset (sampleRate, smoothingSeconds);
}

std::array<float*, 2> Mixer::route (int output, const AuxOutputs& auxOuts) noexcept
{
    if (juce::isPositiveAndNotGreaterThan (output, numAuxOuts) && output != mainOutput)
        if (const auto& aux = auxOuts[(size_t) output - 1]; aux[0] != nullptr && aux[1] != nullptr)
            return aux;

    return master;
}

float Mixer::processChannel (Strip& strip, int spectrumKey, std::array<float*, 2> in, int numChannels, int numSamples,
                             const ChannelParams& params, float gainLeft, float gainRight,
                             const std::array<float, numReturns>& sendGains, const AuxOutputs& auxOuts) noexcept
{
    strip.gainLeft.setTargetValue (gainLeft);
    strip.gainRight.setTargetValue (gainRight);
    auto anySend = false;

    for (size_t r = 0; r < (size_t) numReturns; ++r)
    {
        strip.sends[r].setTargetValue (sendGains[r]);
        anySend = anySend || ! silentAndSettled (strip.sends[r]);
    }

    // A silent channel (muted, or faded right down, with no send) skips its processing.
    if (silentAndSettled (strip.gainLeft) && silentAndSettled (strip.gainRight) && ! anySend)
        return 0.0f;

    // The channel the Mixer's EQ shows also feeds its spectrum display.
    auto* tap = meters != nullptr ? meters->channelSpectrum.ifTapping (spectrumKey) : nullptr;
    strip.chain.process (in.data(), numChannels, numSamples, params.chain, tap);

    // Routed to an aux out, a channel skips the master fader and chain, like
    // a direct out on a desk.
    const auto out = route (params.output, auxOuts);
    const auto toMaster = out == master;

    // Master gain is applied per sample here (rather than once at the end)
    // so the master bus and aux outputs can share one loop.
    auto masterRamp = masterGain;   // a copy: each channel walks the same master ramp
    float peak = 0.0f;

    for (int n = 0; n < numSamples; ++n)
    {
        const auto left = in[0][n] * strip.gainLeft.getNextValue();
        const auto right = in[1][n] * strip.gainRight.getNextValue();
        const auto m = toMaster ? masterRamp.getNextValue() : 1.0f;
        out[0][n] += left * m;
        out[1][n] += right * m;
        peak = std::max (peak, std::max (std::abs (left), std::abs (right)));
    }

    // The sends take the channel after its chain, unpanned, so a stereo send
    // keeps the channel's own image. Their gains include the fader when
    // they're post-fader (see process()).
    for (size_t r = 0; r < (size_t) numReturns; ++r)
    {
        auto& s = strip.sends[r];

        if (silentAndSettled (s))
            continue;

        for (int n = 0; n < numSamples; ++n)
        {
            const auto g = s.getNextValue();
            send[r][0][n] += in[0][n] * g;
            send[r][1][n] += in[1][n] * g;
        }
    }

    return peak;
}

float Mixer::processReturn (int index, const ReturnParams& params, bool bypassed, int numSamples, const AuxOutputs& auxOuts) noexcept
{
    auto& r = returns[(size_t) index];
    const auto level = bypassed || params.mute ? 0.0f : params.returnGain;
    const auto [l, rgt] = dsp::panGains (params.pan, 2);
    r.gainLeft.setTargetValue (level * l);
    r.gainRight.setTargetValue (level * rgt);
    r.mix.setTargetValue (juce::jlimit (0.0f, 1.0f, params.mix));

    // A return that's silent doesn't run its effect at all. Its tail would be
    // stale by the time it comes back, so the effect is cleared once.
    if (silentAndSettled (r.gainLeft) && silentAndSettled (r.gainRight))
    {
        if (! r.resting)
        {
            r.resting = true;

            switch (index)
            {
                case roomReturn:  room.reset(); break;
                case plateReturn: plate.reset(); break;
                default:          delay.reset(); break;
            }
        }

        r.mix.skip (numSamples);
        return 0.0f;
    }

    r.resting = false;
    const auto* in = send[(size_t) index].data();
    auto* w = wet.getArrayOfWritePointers();

    switch (index)
    {
        case roomReturn:  room.process (in[0], in[1], w[0], w[1], numSamples, current->room.room); break;
        case plateReturn: plate.process (in[0], in[1], w[0], w[1], numSamples, current->reverb.room); break;
        default:
        {
            auto settings = current->delay.delay;
            settings.timeMs = current->delay.sync ? TempoDelay::syncedMs (current->delay.note, current->hostBpm)
                                                  : current->delay.freeTimeMs;
            delay.process (in[0], in[1], w[0], w[1], numSamples, settings);
            break;
        }
    }

    const auto out = route (params.output, auxOuts);
    const auto toMaster = out == master;
    auto masterRamp = masterGain;
    float peak = 0.0f;

    for (int n = 0; n < numSamples; ++n)
    {
        // Below 100% mix, some of the send comes through as it is, as an
        // effect's mix knob does on an insert.
        const auto mix = r.mix.getNextValue();
        const auto left = (w[0][n] + (1.0f - mix) * (in[0][n] - w[0][n])) * r.gainLeft.getNextValue();
        const auto right = (w[1][n] + (1.0f - mix) * (in[1][n] - w[1][n])) * r.gainRight.getNextValue();
        const auto m = toMaster ? masterRamp.getNextValue() : 1.0f;
        out[0][n] += left * m;
        out[1][n] += right * m;
        peak = std::max (peak, std::max (std::abs (left), std::abs (right)));
    }

    return peak;
}

void Mixer::delayAuxOuts (const AuxOutputs& auxOuts, int numSamples, bool lookahead) noexcept
{
    if (lookahead != auxDelaying)
    {
        auxDelaying = lookahead;

        for (auto& aux : auxDelay)
            for (auto& line : aux)
                std::fill (line.begin(), line.end(), 0.0f);

        auxDelayPosition = 0;
    }

    if (! lookahead)
        return;

    const auto length = (int) auxDelay[0][0].size();
    auto position = auxDelayPosition;

    for (size_t a = 0; a < auxOuts.size(); ++a)
    {
        if (auxOuts[a][0] == nullptr)
            continue;

        for (size_t ch = 0; ch < 2; ++ch)
        {
            auto* x = auxOuts[a][ch];
            auto* line = auxDelay[a][ch].data();
            position = auxDelayPosition;

            for (int n = 0; n < numSamples; ++n)
            {
                std::swap (x[n], line[position]);
                position = position + 1 == length ? 0 : position + 1;
            }
        }
    }

    // Every line moved on by the same amount, whether or not its aux out is in use.
    auxDelayPosition = (auxDelayPosition + numSamples) % length;
}

void Mixer::process (const Kit& kit, int numSamples, const EngineParams& params, std::array<float*, 2> mainOut,
                     const AuxOutputs& auxOuts) noexcept
{
    jassert (numSamples <= micBus.getNumSamples());

    current = &params;
    masterGain.setTargetValue (params.masterGain);
    masterBus.clear (0, numSamples);
    master = { masterBus.getWritePointer (0), masterBus.getWritePointer (1) };

    for (size_t r = 0; r < sendBuses.size(); ++r)
    {
        sendBuses[r].clear (0, numSamples);
        send[r] = { sendBuses[r].getWritePointer (0), sendBuses[r].getWritePointer (1) };
    }

    // The send gains: before or after the fader, as each channel has them.
    auto sendGains = [] (const ChannelParams& c, float fader)
    {
        const auto g = c.sendsPreFader ? 1.0f : fader;
        return std::array<float, numReturns> { c.roomSend * g, c.reverbSend * g, c.delaySend * g };
    };

    // The drums' channels. Mute and solo of a drum already silenced its hits
    // in the voices, so here it's just the channel's fader, pan and sends.
    for (int p = 0; p < (int) kit.pieces.size(); ++p)
    {
        const auto& pp = params.pieces[(size_t) p];
        const auto [l, r] = dsp::panGains (pp.pan, 2);
        std::array<float*, 2> in { pieceBus.getWritePointer (p * 2), pieceBus.getWritePointer (p * 2 + 1) };
        auto& strip = pieceStrips[(size_t) p];

        const auto peak = processChannel (strip, SpectrumTap::keyFor (true, p), in, 2, numSamples, pp.channel, pp.faderGain * l, pp.faderGain * r,
                                          sendGains (pp.channel, pp.faderGain), auxOuts);

        if (meters != nullptr)
        {
            MixerFeedback::raise (meters->piecePeak[(size_t) p], peak);
            MixerFeedback::raise (meters->pieceReductionDb[(size_t) p], strip.chain.takeMaxGainReductionDb());
        }
    }

    // The overhead and room mics' channels. (Close mics have none: they feed the drums' channels.)
    const auto anySolo = params.anyMicSoloed (kit.numMics);

    for (int m = 0; m < kit.numMics; ++m)
    {
        const auto& mic = kit.mics[(size_t) m];

        if (mic.direct)
            continue;

        const auto& mp = params.mics[(size_t) m];
        const auto audible = ! mp.mute && (! anySolo || mp.solo);

        // The parameters trim the kit's own default fader and pan.
        const auto [l, r] = dsp::panGains (mic.defaultPan + mp.pan, mic.numChannels);
        const auto level = audible ? mic.defaultGain * mp.volume : 0.0f;
        std::array<float*, 2> in { micBus.getWritePointer (mic.busChannel),
                                   micBus.getWritePointer (mic.busChannel + mic.numChannels - 1) };
        auto& strip = micStrips[(size_t) m];

        // A muted mic sends nothing, even before its fader.
        auto gains = sendGains (mp.channel, std::abs (level));

        if (! audible)
            gains = {};

        const auto peak = processChannel (strip, SpectrumTap::keyFor (false, m), in, mic.numChannels, numSamples, mp.channel, level * l, level * r, gains, auxOuts);

        if (meters != nullptr)
        {
            MixerFeedback::raise (meters->micPeak[(size_t) m], peak);
            MixerFeedback::raise (meters->micReductionDb[(size_t) m], strip.chain.takeMaxGainReductionDb());
        }
    }

    // The returns.
    const std::array<const ReturnParams*, numReturns> returnParams { &params.room, &params.reverb, &params.delay };

    for (int r = 0; r < numReturns; ++r)
    {
        const auto peak = processReturn (r, *returnParams[(size_t) r], params.fxBypass, numSamples, auxOuts);

        if (meters != nullptr)
        {
            MixerFeedback::raise (meters->returnPeak[(size_t) r], peak);
            MixerFeedback::raise (meters->fxReturnPeak[(size_t) r], peak);
        }
    }

    masterGain.skip (numSamples);

    // The master chain, bypassed as a whole by the FX page's bypass button.
    if (meters != nullptr)
        for (size_t ch = 0; ch < 2; ++ch)
            MixerFeedback::raise (meters->chainInPeak[ch], peakOf (master[ch], numSamples));

    if (params.fxBypass)
    {
        auto bypassed = params.master;
        bypassed.eq.enabled = bypassed.glue.enabled = bypassed.crush.enabled = false;
        bypassed.tape.enabled = bypassed.transient.enabled = bypassed.limiter.enabled = false;
        chain.process (master.data(), numSamples, bypassed);
    }
    else
    {
        chain.process (master.data(), numSamples, params.master);
    }

    for (size_t ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::add (mainOut[ch], master[ch], numSamples);

    delayAuxOuts (auxOuts, numSamples, params.master.limiter.lookahead);

    if (meters != nullptr)
    {
        for (size_t ch = 0; ch < 2; ++ch)
        {
            MixerFeedback::raise (meters->chainOutPeak[ch], peakOf (master[ch], numSamples));
            MixerFeedback::raise (meters->masterPeak[ch], peakOf (mainOut[ch], numSamples));
        }

        MixerFeedback::raise (meters->glueReductionDb, chain.takeGlueReductionDb());
        MixerFeedback::raise (meters->crushReductionDb, chain.takeCrushReductionDb());
        MixerFeedback::raise (meters->limiterReductionDb, chain.takeLimiterReductionDb());
    }

    current = nullptr;
}

} // namespace lithium
