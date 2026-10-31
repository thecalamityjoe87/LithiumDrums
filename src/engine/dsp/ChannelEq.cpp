#include "ChannelEq.h"

namespace lithium
{

namespace
{
    constexpr double smoothingSeconds = 0.03;
    constexpr int updateInterval = 16;          // samples between coefficient updates while smoothing

    // Butterworth Q values for the two stages of a 4th-order high-pass.
    constexpr double hpfQ1 = 0.5411961, hpfQ2 = 1.3065630;
    constexpr double shelfQ = 0.7071068;

    dsp::SvfCoefficients blendHighPass (dsp::SvfCoefficients c, float amount) noexcept
    {
        // The high-pass output is x - k*band - low; blending its mix terms
        // towards (1, 0, 0) crossfades it with the dry signal.
        c.m1 *= amount;
        c.m2 *= amount;
        return c;
    }

    dsp::EqCoefficients design (double sr, float hpfHz, float hpfAmount, float lowHz, float lowDb, float mid1Hz,
                                float mid1Db, float mid1Q, float mid2Hz, float mid2Db, float mid2Q, float highHz,
                                float highDb) noexcept
    {
        using dsp::SvfType;
        dsp::EqCoefficients c {};

        if (hpfAmount > 0.0f)
        {
            c[dsp::hpf1] = blendHighPass (dsp::designSvf (SvfType::highPass, hpfHz, 0.0, hpfQ1, sr), hpfAmount);
            c[dsp::hpf2] = blendHighPass (dsp::designSvf (SvfType::highPass, hpfHz, 0.0, hpfQ2, sr), hpfAmount);
        }

        // At 0 dB a shelf or bell designs to exactly (1, 0, 0), so flat bands
        // are recognised as pass-through and skipped.
        c[dsp::lowShelf] = dsp::designSvf (SvfType::lowShelf, lowHz, lowDb, shelfQ, sr);
        c[dsp::mid1] = dsp::designSvf (SvfType::bell, mid1Hz, mid1Db, mid1Q, sr);
        c[dsp::mid2] = dsp::designSvf (SvfType::bell, mid2Hz, mid2Db, mid2Q, sr);
        c[dsp::highShelf] = dsp::designSvf (SvfType::highShelf, highHz, highDb, shelfQ, sr);
        return c;
    }

    bool highPassOn (const EqSettings& s) noexcept   { return s.enabled && s.hpfHz > EqSettings::hpfOffHz + 0.5f; }
    float effectiveDb (const EqSettings& s, float db) noexcept { return s.enabled ? db : 0.0f; }
}

namespace dsp
{
    EqCoefficients designEq (const EqSettings& s, double sr) noexcept
    {
        return design (sr, s.hpfHz, highPassOn (s) ? 1.0f : 0.0f, s.lowHz, effectiveDb (s, s.lowDb), s.mid1Hz,
                       effectiveDb (s, s.mid1Db), s.mid1Q, s.mid2Hz, effectiveDb (s, s.mid2Db), s.mid2Q, s.highHz,
                       effectiveDb (s, s.highDb));
    }

    float magnitudeDb (const EqCoefficients& c, double hz, double sr) noexcept
    {
        std::complex<double> h = 1.0;

        for (const auto& stage : c)
            h *= response (stage, hz, sr);

        return (float) (20.0 * std::log10 (std::max (1.0e-9, std::abs (h))));
    }
}

//==============================================================================
void ChannelEq::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    for (auto* f : { &hpfHz, &lowHz, &mid1Hz, &mid2Hz, &highHz })
        f->reset (sampleRate, smoothingSeconds);

    for (auto* v : { &hpfAmount, &lowDb, &mid1Db, &mid2Db, &highDb, &mid1Q, &mid2Q })
        v->reset (sampleRate, smoothingSeconds);

    reset();
}

void ChannelEq::reset() noexcept
{
    for (auto& stage : state)
        for (auto& s : stage)
            s.reset();

    firstBlock = true;
}

void ChannelEq::setTargets (const EqSettings& s) noexcept
{
    // A switched-off high-pass keeps its last frequency while it fades out,
    // rather than sweeping down to 20 Hz on the way.
    if (highPassOn (s) || firstBlock)
        hpfHz.setTargetValue (std::max (s.hpfHz, EqSettings::hpfOffHz));

    hpfAmount.setTargetValue (highPassOn (s) ? 1.0f : 0.0f);
    lowHz.setTargetValue (s.lowHz);
    lowDb.setTargetValue (effectiveDb (s, s.lowDb));
    mid1Hz.setTargetValue (s.mid1Hz);
    mid1Db.setTargetValue (effectiveDb (s, s.mid1Db));
    mid1Q.setTargetValue (s.mid1Q);
    mid2Hz.setTargetValue (s.mid2Hz);
    mid2Db.setTargetValue (effectiveDb (s, s.mid2Db));
    mid2Q.setTargetValue (s.mid2Q);
    highHz.setTargetValue (s.highHz);
    highDb.setTargetValue (effectiveDb (s, s.highDb));

    if (firstBlock)
    {
        // A fresh channel starts at its settings rather than gliding to them.
        for (auto* f : { &hpfHz, &lowHz, &mid1Hz, &mid2Hz, &highHz })
            f->setCurrentAndTargetValue (f->getTargetValue());

        for (auto* v : { &hpfAmount, &lowDb, &mid1Db, &mid2Db, &highDb, &mid1Q, &mid2Q })
            v->setCurrentAndTargetValue (v->getTargetValue());

        firstBlock = false;
        updateCoefficients();
    }
}

void ChannelEq::advance (int numSamples) noexcept
{
    for (auto* f : { &hpfHz, &lowHz, &mid1Hz, &mid2Hz, &highHz })
        f->skip (numSamples);

    for (auto* v : { &hpfAmount, &lowDb, &mid1Db, &mid2Db, &highDb, &mid1Q, &mid2Q })
        v->skip (numSamples);
}

void ChannelEq::updateCoefficients() noexcept
{
    coefficients = design (sampleRate, hpfHz.getCurrentValue(), hpfAmount.getCurrentValue(), lowHz.getCurrentValue(),
                           lowDb.getCurrentValue(), mid1Hz.getCurrentValue(), mid1Db.getCurrentValue(),
                           mid1Q.getCurrentValue(), mid2Hz.getCurrentValue(), mid2Db.getCurrentValue(),
                           mid2Q.getCurrentValue(), highHz.getCurrentValue(), highDb.getCurrentValue());

    for (size_t i = 0; i < coefficients.size(); ++i)
    {
        const auto active = ! coefficients[i].isIdentity();

        // A stage that was skipped has stale state; it starts again from silence.
        if (active && ! stageActive[i])
            for (auto& s : state[i])
                s.reset();

        stageActive[i] = active;
    }
}

void ChannelEq::process (float* const* channels, int numChannels, int numSamples, const EqSettings& settings) noexcept
{
    setTargets (settings);

    for (int done = 0; done < numSamples;)
    {
        if (untilUpdate <= 0)
        {
            const auto smoothing = hpfHz.isSmoothing() || lowHz.isSmoothing() || mid1Hz.isSmoothing()
                                || mid2Hz.isSmoothing() || highHz.isSmoothing() || hpfAmount.isSmoothing()
                                || lowDb.isSmoothing() || mid1Db.isSmoothing() || mid2Db.isSmoothing()
                                || highDb.isSmoothing() || mid1Q.isSmoothing() || mid2Q.isSmoothing();

            if (smoothing)
            {
                advance (updateInterval);
                updateCoefficients();
            }

            untilUpdate = updateInterval;
        }

        const auto n = std::min (numSamples - done, untilUpdate);

        // The active stages, gathered into locals. Local copies rather than
        // references, because the loop writes through a float* that, as far as
        // the compiler knows, could point into the state; with references it
        // would reload the state on every sample.
        std::array<dsp::SvfCoefficients, dsp::numEqStages> c;
        std::array<size_t, dsp::numEqStages> active;
        size_t numActive = 0;

        for (size_t stage = 0; stage < coefficients.size(); ++stage)
            if (stageActive[stage])
            {
                c[numActive] = coefficients[stage];
                active[numActive++] = stage;
            }

        for (int ch = 0; numActive > 0 && ch < numChannels; ++ch)
        {
            std::array<dsp::SvfState, dsp::numEqStages> s;

            for (size_t k = 0; k < numActive; ++k)
                s[k] = state[active[k]][(size_t) ch];

            // Every stage for one sample, then the next sample: each stage's
            // recursion only waits on its own previous sample, so the CPU can
            // work on several stages at once.
            auto* x = channels[ch] + done;

            for (int i = 0; i < n; ++i)
            {
                auto y = x[i];

                for (size_t k = 0; k < numActive; ++k)
                    y = s[k].process (c[k], y);

                x[i] = y;
            }

            for (size_t k = 0; k < numActive; ++k)
                state[active[k]][(size_t) ch] = s[k];
        }

        done += n;
        untilUpdate -= n;
    }
}

} // namespace lithium
