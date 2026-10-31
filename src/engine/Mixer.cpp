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
}

void Mixer::prepare (double sampleRate, int maxBlockSize)
{
    micBus.setSize (maxMicBusChannels, maxBlockSize);
    micBus.clear();
    pieceBus.setSize (maxPieces * 2, maxBlockSize);
    pieceBus.clear();
    reverbSend.setSize (2, maxBlockSize);
    reverbReturn.setSize (2, maxBlockSize);

    auto prepareStrip = [sampleRate] (Strip& s)
    {
        s.chain.prepare (sampleRate);
        s.gainLeft.reset (sampleRate, smoothingSeconds);
        s.gainRight.reset (sampleRate, smoothingSeconds);
        s.send.reset (sampleRate, smoothingSeconds);
    };

    for (auto& s : pieceStrips)
        prepareStrip (s);

    for (auto& s : micStrips)
        prepareStrip (s);

    reverb.prepare (sampleRate);
    masterGain.reset (sampleRate, smoothingSeconds);
    returnGain.reset (sampleRate, smoothingSeconds);
}

std::array<float*, 2> Mixer::route (int output, std::array<float*, 2> mainOut, const AuxOutputs& auxOuts) const noexcept
{
    if (juce::isPositiveAndNotGreaterThan (output, numAuxOuts) && output != mainOutput)
        if (const auto& aux = auxOuts[(size_t) output - 1]; aux[0] != nullptr && aux[1] != nullptr)
            return aux;

    return mainOut;
}

float Mixer::processChannel (Strip& strip, std::array<float*, 2> in, int numChannels, int numSamples,
                             const ChannelParams& params, float gainLeft, float gainRight, float sendGain,
                             std::array<float*, 2> mainOut, const AuxOutputs& auxOuts) noexcept
{
    strip.gainLeft.setTargetValue (gainLeft);
    strip.gainRight.setTargetValue (gainRight);
    strip.send.setTargetValue (sendGain);

    // A silent channel (muted, or faded right down) skips its processing.
    if (silentAndSettled (strip.gainLeft) && silentAndSettled (strip.gainRight) && silentAndSettled (strip.send))
        return 0.0f;

    strip.chain.process (in.data(), numChannels, numSamples, params.chain);

    // Routed to an aux out, a channel skips the master fader, like a direct out on a desk.
    const auto out = route (params.output, mainOut, auxOuts);
    const auto toMain = out == mainOut;

    // Master gain is applied per sample here (rather than once at the end)
    // so the main and aux outputs can share one loop.
    auto master = masterGain;   // a copy: each channel walks the same master ramp
    float peak = 0.0f;

    for (int n = 0; n < numSamples; ++n)
    {
        const auto left = in[0][n] * strip.gainLeft.getNextValue();
        const auto right = in[1][n] * strip.gainRight.getNextValue();
        const auto s = strip.send.getNextValue();
        const auto m = toMain ? master.getNextValue() : 1.0f;
        out[0][n] += left * m;
        out[1][n] += right * m;
        send[0][n] += left * s;
        send[1][n] += right * s;
        peak = std::max (peak, std::max (std::abs (left), std::abs (right)));
    }

    return peak;
}

void Mixer::process (const Kit& kit, int numSamples, const EngineParams& params, std::array<float*, 2> mainOut,
                     const AuxOutputs& auxOuts) noexcept
{
    jassert (numSamples <= micBus.getNumSamples());

    masterGain.setTargetValue (params.masterGain);
    reverbSend.clear (0, numSamples);
    send = { reverbSend.getWritePointer (0), reverbSend.getWritePointer (1) };

    // The drums' channels. Mute and solo of a drum already silenced its hits
    // in the voices, so here it's just the channel's fader, pan and send.
    for (int p = 0; p < (int) kit.pieces.size(); ++p)
    {
        const auto& pp = params.pieces[(size_t) p];
        const auto [l, r] = dsp::panGains (pp.pan, 2);
        std::array<float*, 2> in { pieceBus.getWritePointer (p * 2), pieceBus.getWritePointer (p * 2 + 1) };
        auto& strip = pieceStrips[(size_t) p];

        // The send is post-fader, but not panned: the stereo send keeps the drum's own image.
        const auto peak = processChannel (strip, in, 2, numSamples, pp.channel, pp.faderGain * l, pp.faderGain * r,
                                          pp.faderGain * pp.channel.reverbSend, mainOut, auxOuts);

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

        const auto peak = processChannel (strip, in, mic.numChannels, numSamples, mp.channel, level * l, level * r,
                                          std::abs (level) * mp.channel.reverbSend, mainOut, auxOuts);

        if (meters != nullptr)
        {
            MixerFeedback::raise (meters->micPeak[(size_t) m], peak);
            MixerFeedback::raise (meters->micReductionDb[(size_t) m], strip.chain.takeMaxGainReductionDb());
        }
    }

    // The reverb return.
    std::array<float*, 2> wet { reverbReturn.getWritePointer (0), reverbReturn.getWritePointer (1) };
    reverb.process (send[0], send[1], wet[0], wet[1], numSamples, params.reverb.room);
    returnGain.setTargetValue (params.reverb.mute ? 0.0f : params.reverb.returnGain);

    {
        const auto out = route (params.reverb.output, mainOut, auxOuts);
        const auto toMain = out == mainOut;
        auto master = masterGain;
        float peak = 0.0f;

        for (int n = 0; n < numSamples; ++n)
        {
            const auto g = returnGain.getNextValue() * (toMain ? master.getNextValue() : 1.0f);
            out[0][n] += wet[0][n] * g;
            out[1][n] += wet[1][n] * g;
            peak = std::max (peak, std::max (std::abs (wet[0][n] * g), std::abs (wet[1][n] * g)));
        }

        if (meters != nullptr)
            MixerFeedback::raise (meters->reverbPeak, peak);
    }

    masterGain.skip (numSamples);

    if (meters != nullptr)
    {
        for (size_t ch = 0; ch < 2; ++ch)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax (mainOut[ch], numSamples);
            MixerFeedback::raise (meters->masterPeak[ch], std::max (range.getEnd(), -range.getStart()));
        }
    }
}

} // namespace lithium
