#pragma once

#include "Svf.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

namespace lithium
{

/** Note values the delay can sync to, in the order of the delay_note
    parameter's choices. */
namespace delaynotes
{
    struct Note
    {
        const char* name;
        double beats;        // in quarter notes
    };

    inline constexpr std::array<Note, 15> all {{
        { "1/1", 4.0 },       { "1/2", 2.0 },        { "1/2 dotted", 3.0 },  { "1/2 triplet", 4.0 / 3.0 },
        { "1/4", 1.0 },       { "1/4 dotted", 1.5 }, { "1/4 triplet", 2.0 / 3.0 },
        { "1/8", 0.5 },       { "1/8 dotted", 0.75 }, { "1/8 triplet", 1.0 / 3.0 },
        { "1/16", 0.25 },     { "1/16 dotted", 0.375 }, { "1/16 triplet", 1.0 / 6.0 },
        { "1/32", 0.125 },    { "1/32 triplet", 1.0 / 12.0 },
    }};

    inline constexpr int defaultNote = 8;   // 1/8 dotted
}

/** The Delay return's settings, in the units its parameters use. */
struct DelaySettings
{
    float timeMs = 375.0f;       // the echo time, already worked out from the tempo when synced
    float feedback = 0.35f;      // 0..0.95
    float lowCutHz = 150.0f;     // filters every repeat, so they thin out
    float highCutHz = 6000.0f;   // and darken, as on tape
    bool pingPong = false;
};

/** A stereo delay for the Delay return.

    The send and every repeat go through a low cut and a high cut on their
    way into the delay line, so each repeat is thinner and darker than the
    last; what comes back round is soft-clipped, so a feedback near 100%
    saturates instead of running away. In ping-pong mode the send is summed
    to mono and the repeats bounce between left and right; otherwise each
    side repeats on itself.

    Changing the time glides to the new one over 80 ms, like turning a tape
    delay's speed, rather than jumping (which would click). Audio thread,
    except prepare(). Goes idle when the repeats have died away. */
class TempoDelay
{
public:
    static constexpr double maxSeconds = 4.0;

    void prepare (double sampleRate);
    void reset() noexcept;

    /** Writes 100% wet output (replacing what's in outL and outR). */
    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples,
                  const DelaySettings&) noexcept;

    /** The echo time for a note value at a tempo, clamped to what the delay can do. */
    static float syncedMs (int note, double bpm) noexcept;

private:
    double sampleRate = 44100.0;
    std::array<std::vector<float>, 2> lines;
    int mask = 0, write = 0;
    juce::SmoothedValue<float> delaySamples;
    std::array<dsp::SvfState, 2> lowCut, highCut;
    dsp::SvfCoefficients lowCutCoefficients, highCutCoefficients;
    float lastLowCutHz = -1.0f, lastHighCutHz = -1.0f;
    int silentSamples = 0;
    bool idle = true;
};

} // namespace lithium
