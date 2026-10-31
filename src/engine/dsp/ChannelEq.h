#pragma once

#include "Svf.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

namespace lithium
{

/** A mixer channel's EQ, in the units its parameters use. The defaults here
    are the parameters' defaults too (see Parameters.cpp): flat, high-pass off. */
struct EqSettings
{
    static constexpr float hpfOffHz = 20.0f;   // the high-pass is off at the bottom of its range

    bool enabled = true;
    float hpfHz = hpfOffHz;
    float lowHz = 100.0f, lowDb = 0.0f;
    float mid1Hz = 400.0f, mid1Db = 0.0f, mid1Q = 1.0f;
    float mid2Hz = 2500.0f, mid2Db = 0.0f, mid2Q = 1.0f;
    float highHz = 8000.0f, highDb = 0.0f;
};

namespace dsp
{
    // A 24 dB/octave high-pass (two Butterworth stages), a low shelf, two
    // bells and a high shelf, in series.
    enum EqStage { hpf1, hpf2, lowShelf, mid1, mid2, highShelf, numEqStages };
    using EqCoefficients = std::array<SvfCoefficients, numEqStages>;

    /** The EQ as heard: a disabled EQ, or a high-pass at its Off position,
        comes out as pass-through stages. */
    EqCoefficients designEq (const EqSettings&, double sampleRate) noexcept;

    /** The EQ's gain at hz, in dB. For drawing the curve. */
    float magnitudeDb (const EqCoefficients&, double hz, double sampleRate) noexcept;
}

/** The EQ in one mixer channel (mono or stereo). Parameter changes are
    smoothed and the coefficients recomputed every few samples while they
    move, so sweeps and automation are click-free; switching the EQ (or the
    high-pass) off fades it to flat rather than cutting over. Stages that are
    flat cost nothing. Audio thread, except prepare(). */
class ChannelEq
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;

    void process (float* const* channels, int numChannels, int numSamples, const EqSettings&) noexcept;

private:
    void setTargets (const EqSettings&) noexcept;
    void advance (int numSamples) noexcept;
    void updateCoefficients() noexcept;

    using Frequency = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>;
    using Linear = juce::SmoothedValue<float>;

    double sampleRate = 44100.0;
    Frequency hpfHz, lowHz, mid1Hz, mid2Hz, highHz;
    Linear hpfAmount, lowDb, mid1Db, mid2Db, highDb, mid1Q, mid2Q;

    dsp::EqCoefficients coefficients;
    std::array<std::array<dsp::SvfState, 2>, dsp::numEqStages> state;
    std::array<bool, dsp::numEqStages> stageActive {};
    int untilUpdate = 0;
    bool firstBlock = true;
};

} // namespace lithium
