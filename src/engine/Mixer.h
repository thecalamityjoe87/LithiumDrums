#pragma once

#include "EngineParams.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

namespace lithium
{

/** Where per-channel processing (EQ, compression) will go in a later milestone.
    It sits between the voices and the fader, so it sees each mic on its own. */
class InsertChain
{
public:
    void prepare (double /*sampleRate*/, int /*maxBlockSize*/) {}
    void process (float* const* /*channels*/, int /*numChannels*/, int /*numSamples*/) noexcept {}
};

/** The mic mixer: one strip per mic channel of the loaded kit (inserts,
    volume, pan, mute, solo), summed into the main stereo output, or sent to
    that mic's own direct output when it is routed there.

    The voices render into getMicBus(), one channel per mic channel (stereo
    mics take two), and process() then mixes that bus down. */
class Mixer
{
public:
    // Direct outputs exist for the first numDirectOuts mics; any further mics
    // always go to the main mix.
    static constexpr int numDirectOuts = 16;
    using DirectOutputs = std::array<std::array<float*, 2>, numDirectOuts>;   // null where not available

    /** Allocates the mic bus. Not real-time safe. */
    void prepare (double sampleRate, int maxBlockSize);

    int getMaxBlockSize() const noexcept                  { return micBus.getNumSamples(); }
    juce::AudioBuffer<float>& getMicBus() noexcept       { return micBus; }
    void clearMicBus (int numSamples) noexcept            { micBus.clear (0, numSamples); }

    /** Mixes the first numSamples of the mic bus into the outputs (adding to
        whatever is there), using the kit's mic layout and default levels.
        numSamples must not exceed getMaxBlockSize(). */
    void process (const Kit&, int numSamples, const EngineParams&, std::array<float*, 2> mainOut,
                  const DirectOutputs& directOuts) noexcept;

private:
    struct Strip
    {
        InsertChain inserts;
        juce::SmoothedValue<float> gainLeft, gainRight;
    };

    std::array<Strip, maxMics> strips;
    juce::SmoothedValue<float> masterGain;
    juce::AudioBuffer<float> micBus;
};

} // namespace lithium
