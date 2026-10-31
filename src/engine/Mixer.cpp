#include "Mixer.h"

#include <cmath>

namespace lithium
{

namespace
{
    constexpr double smoothingSeconds = 0.02;

    // Mono mics: constant-power pan, scaled so the centre is at unity gain.
    // Stereo mics: a balance control, which keeps the stereo image intact.
    std::pair<float, float> panGains (float pan, int numChannels) noexcept
    {
        pan = juce::jlimit (-1.0f, 1.0f, pan);

        if (numChannels == 1)
        {
            const auto angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
            return { std::cos (angle) * juce::MathConstants<float>::sqrt2,
                     std::sin (angle) * juce::MathConstants<float>::sqrt2 };
        }

        return { pan > 0.0f ? 1.0f - pan : 1.0f,
                 pan < 0.0f ? 1.0f + pan : 1.0f };
    }
}

void Mixer::prepare (double sampleRate, int maxBlockSize)
{
    micBus.setSize (maxMicBusChannels, maxBlockSize);
    micBus.clear();

    for (auto& s : strips)
    {
        s.inserts.prepare (sampleRate, maxBlockSize);
        s.gainLeft.reset (sampleRate, smoothingSeconds);
        s.gainRight.reset (sampleRate, smoothingSeconds);
    }

    masterGain.reset (sampleRate, smoothingSeconds);
}

void Mixer::process (const Kit& kit, int numSamples, const EngineParams& params, std::array<float*, 2> mainOut,
                     const DirectOutputs& directOuts) noexcept
{
    jassert (numSamples <= micBus.getNumSamples());

    bool anySolo = false;

    for (int i = 0; i < kit.numMics; ++i)
        anySolo = anySolo || params.mics[(size_t) i].solo;

    masterGain.setTargetValue (params.masterGain);

    for (int micIndex = 0; micIndex < kit.numMics; ++micIndex)
    {
        const auto i = (size_t) micIndex;
        const auto& mic = kit.mics[i];
        const auto& mp = params.mics[i];
        auto& strip = strips[i];

        // The parameters trim the kit's own default fader and pan.
        const auto audible = ! mp.mute && (! anySolo || mp.solo);
        const auto [panL, panR] = panGains (mic.defaultPan + mp.pan, mic.numChannels);
        const auto level = audible ? mic.defaultGain * mp.volume : 0.0f;
        strip.gainLeft.setTargetValue (level * panL);
        strip.gainRight.setTargetValue (level * panR);

        std::array<float*, 2> in { micBus.getWritePointer (mic.busChannel),
                                   micBus.getWritePointer (mic.busChannel + mic.numChannels - 1) };

        strip.inserts.process (in.data(), mic.numChannels, numSamples);

        // Direct-routed mics skip the master fader, like a direct out on a desk.
        const auto hasDirect = micIndex < numDirectOuts && directOuts[i][0] != nullptr && directOuts[i][1] != nullptr;
        const auto useDirect = mp.output == MicOutput::direct && hasDirect;
        const auto direct = hasDirect ? directOuts[i] : mainOut;
        const auto out = useDirect ? direct : mainOut;

        // Master gain is applied per sample here (rather than once at the end)
        // so the main and direct outputs can share one loop.
        auto master = masterGain;   // copy: each strip walks the same master ramp

        for (int n = 0; n < numSamples; ++n)
        {
            const auto gl = strip.gainLeft.getNextValue();
            const auto gr = strip.gainRight.getNextValue();
            const auto m = useDirect ? 1.0f : master.getNextValue();
            out[0][n] += in[0][n] * gl * m;
            out[1][n] += in[1][n] * gr * m;
        }
    }

    masterGain.skip (numSamples);
}

} // namespace lithium
