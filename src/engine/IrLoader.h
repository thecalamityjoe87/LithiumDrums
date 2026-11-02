#pragma once

#include <juce_dsp/juce_dsp.h>

#include <mutex>
#include <vector>

// The Room return's impulse responses: the ones built into the plugin
// (assets/irs/, listed in irs.json with their credits), or a WAV, FLAC or
// AIFF file of the user's own.

namespace lithium
{

class ConvolutionReverb;

/** One built-in IR, with the credits its licence asks for. */
struct IrInfo
{
    juce::String id, name, file, description, author, source, sourceUrl, licenseName, licenseUrl, changes;
};

namespace irs
{
    /** The built-in IRs, in menu order. Parsed once, on first use. Not for the audio thread. */
    const std::vector<IrInfo>& builtIn();

    /** The extensions a custom IR file may have. */
    inline constexpr auto fileWildcard = "*.wav;*.flac;*.aif;*.aiff";
}

/** Loads impulse responses into the Room reverb, on a thread of its own.

    The message thread asks for an IR (request()); the loader decodes it,
    normalises it, applies the Decay setting (see below), and hands it to
    the ConvolutionReverb's juce::dsp::Convolution with loadImpulseResponse(),
    which is wait-free. The Convolution then prepares the new IR on a
    background thread of its own and swaps it in lock-free at the start of a
    later audio block. The audio thread never waits for any of this.

    Requests made while one is being worked on replace each other: only the
    newest is loaded next. A decoded IR is kept, so moving the Decay knob
    only re-trims it rather than decoding the file again.

    Decay: below 100%, the IR's tail is made to die away faster by an extra
    exponential decay, so its decay time is that percentage of the original,
    and it's cut where it has fallen 80 dB (which also makes it cheaper to
    convolve). */
class IrLoader final : private juce::Thread
{
public:
    explicit IrLoader (ConvolutionReverb&);
    ~IrLoader() override;

    struct Request
    {
        int builtIn = 0;             // an index into irs::builtIn(), or -1 for the file
        juce::File file;
        float decay = 1.0f;          // 0.1..1

        bool operator== (const Request&) const = default;
    };

    /** Message thread. Does nothing if that's what's loaded (or loading) already. */
    void request (const Request&);

    /** What's loaded, for the UI. The thumbnail is the IR's peak envelope,
        in up to 512 points over its length, 0..1. */
    struct Status
    {
        juce::String name;
        juce::String error;          // why a custom file couldn't be loaded (the previous IR stays)
        float seconds = 0.0f;        // its length, after the Decay trim
        float rt60 = 0.0f;           // its decay time (T30) as recorded
        std::vector<float> thumbnail;
        int serial = 0;              // counts up with each load, so the UI can tell
        bool loading = false;
    };

    Status getStatus() const;

    /** Held around ConvolutionReverb::prepare(), so a prepare never overlaps
        this thread handing the Convolution an IR. Never taken on the audio thread. */
    std::mutex& getEngineLock() noexcept         { return engineLock; }

private:
    void run() override;
    bool decode (const Request&, juce::String& error);
    juce::AudioBuffer<float> trimmed (float decay) const;

    ConvolutionReverb& reverb;
    std::mutex engineLock;

    mutable std::mutex mutex;        // guards everything below
    Request wanted, loaded;
    bool pending = false, anyLoaded = false;
    Status status;

    // The thread's own: the decoded IR, and what it was decoded from.
    juce::AudioBuffer<float> source;
    double sourceRate = 48000.0;
    float sourceRt60 = 1.0f;
    Request decodedFrom { -2, {}, 1.0f };
    juce::String sourceName;
};

} // namespace lithium
