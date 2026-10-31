#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

// A state-variable filter in the "trapezoidal integrator" form described by
// Andrew Simper (Cytomic, "Linear Trapezoidal Integrated SVF"). One structure
// gives high-pass, shelves and bells, depending only on how its three outputs
// are mixed (m0, m1, m2).
//
// It's used instead of the textbook biquad because its coefficients can
// change while audio runs through it without clicks or blow-ups, so EQ knobs
// can be swept (and automated) freely.

namespace lithium::dsp
{

struct SvfCoefficients
{
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    float m0 = 1.0f, m1 = 0.0f, m2 = 0.0f;   // the default passes audio through unchanged
    double g = 0.0, k = 1.0;                 // kept (in double) for response()

    // Exact on purpose: a flat band designs to exactly these values.
    bool isIdentity() const noexcept
    {
        return juce::exactlyEqual (m0, 1.0f) && juce::exactlyEqual (m1, 0.0f) && juce::exactlyEqual (m2, 0.0f);
    }
};

enum class SvfType { highPass, lowPass, lowShelf, bell, highShelf };

/** Designs one band. gainDb is ignored for the high- and low-pass. */
inline SvfCoefficients designSvf (SvfType type, double hz, double gainDb, double q, double sampleRate) noexcept
{
    hz = std::clamp (hz, 10.0, sampleRate * 0.49);
    const auto a = std::pow (10.0, gainDb / 40.0);
    auto g = std::tan (std::numbers::pi * hz / sampleRate);
    auto k = 1.0 / q;
    double m0 = 1.0, m1 = 0.0, m2 = 0.0;

    switch (type)
    {
        case SvfType::highPass:  m1 = -k; m2 = -1.0; break;
        case SvfType::lowPass:   m0 = 0.0; m2 = 1.0; break;
        case SvfType::bell:      k = 1.0 / (q * a); m1 = k * (a * a - 1.0); break;
        case SvfType::lowShelf:  g /= std::sqrt (a); m1 = k * (a - 1.0); m2 = a * a - 1.0; break;
        case SvfType::highShelf: g *= std::sqrt (a); m0 = a * a; m1 = k * (1.0 - a) * a; m2 = 1.0 - a * a; break;
    }

    SvfCoefficients c;
    const auto a1 = 1.0 / (1.0 + g * (g + k));
    c.a1 = (float) a1;
    c.a2 = (float) (g * a1);
    c.a3 = (float) (g * g * a1);
    c.m0 = (float) m0;
    c.m1 = (float) m1;
    c.m2 = (float) m2;
    c.g = g;
    c.k = k;
    return c;
}

/** The band's exact frequency response at hz. The trapezoidal SVF is the
    bilinear transform of the analogue prototype, so the response at a
    digital frequency is the prototype's at s = j tan(pi f / fs) / g. The EQ
    display uses this, so the curve on screen is the one you hear. */
inline std::complex<double> response (const SvfCoefficients& c, double hz, double sampleRate) noexcept
{
    if (c.isIdentity() || c.g <= 0.0)
        return 1.0;

    const std::complex<double> s (0.0, std::tan (std::numbers::pi * std::min (hz, sampleRate * 0.4999) / sampleRate) / c.g);
    const auto denominator = s * s + c.k * s + 1.0;
    return (double) c.m0 + (double) c.m1 * (s / denominator) + (double) c.m2 / denominator;
}

/** One channel's filter state. */
struct SvfState
{
    float ic1 = 0.0f, ic2 = 0.0f;

    float process (const SvfCoefficients& c, float x) noexcept
    {
        const auto v3 = x - ic2;
        const auto v1 = c.a1 * ic1 + c.a2 * v3;
        const auto v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return c.m0 * x + c.m1 * v1 + c.m2 * v2;
    }

    void reset() noexcept   { ic1 = ic2 = 0.0f; }
};

} // namespace lithium::dsp
