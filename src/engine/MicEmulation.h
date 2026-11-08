#pragma once

#include "EngineParams.h"
#include "Kit.h"
#include "dsp/ConvolutionReverb.h"
#include "dsp/Svf.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

// Mic emulation: an overhead pair or a room pair for a kit that wasn't
// recorded with one, made from the mics it was.
//
// Each piece is heard through its close mics (or, for a piece no close mic
// recorded, through the overheads): the voices sum those into a mono feed
// per piece (VoiceBuses::emulation), at the kit's own level for each mic.
// MicEmulator then places every piece where it stands on the kit and works
// out how it reaches each emulated mic:
//
//  - Overheads: a spaced pair over the kit. Each piece arrives at each mic
//    after the time its distance takes, quieter with distance, so the pair
//    gets its stereo image from time and level differences, as a real
//    spaced pair does; plus its reflection off the floor. A close mic's low
//    end (proximity effect, a mic inside the kick) isn't what a mic above
//    the kit hears, so each piece's feed is shelved down by its role first.
//  - Room: a pair in front of the kit, a few metres away, through a
//    measured or synthesized room (a built-in IR, convolved).
//
// Only kits missing those mics get them, and only when switched on: each
// emulated mic is a mic slot after the kit's own (Kit::emulatedMic) with a
// mixer channel like any other, so its fader, pan, EQ and sends all work.

namespace lithium
{

namespace emulation
{
    inline constexpr float speedOfSound = 343.0f;   // m/s

    using Position = std::array<float, 3>;          // metres: x right (seen from the front), y toward the drummer, z up

    /** Where a piece stands: its stage slot's place in the Kit tab's scene
        (assets/blender-kit.py), or for a piece with no slot, a place for its
        role (extra is how many pieces of that role without a slot came before it). */
    Position positionOf (std::optional<StageSlot>, PieceRole, int extra) noexcept;

    /** The mics of each emulated pair, left then right. */
    std::array<Position, 2> micPositions (MicRole) noexcept;

    /** Gives a kit just loaded its emulated mics: for each of the overheads
        and room the kit has no mics of (or has, with evenIfRecorded, for
        comparing the two), a stereo mic slot after its own, in both the Kit
        and the KitInfo. Also sets each piece's position and emulation
        source: the loudest of its close mics (or, with none, of the
        overheads), measured on its recordings. One mic, not all of them: a
        drum's mics are at different distances from it, so summing them
        (kick in and kick out, say) cancels some of its low end. Needs
        info.slotPiece and kit.samples. Loader thread. */
    void addEmulatedMics (Kit&, KitInfo&, bool evenIfRecorded = false);

    /** The built-in IR (an index into irs::builtIn()) the emulated room
        starts with for a kit: a room that suits it. */
    int defaultRoomFor (const juce::String& kitId);

    /** The emulated room's IR choices: the built-in IRs, by their names. */
    juce::StringArray roomNames();
}

/** Makes the emulated mics' audio from the voices' emulation feed (see the
    comment at the top). Lives in the Mixer; process() runs on the audio
    thread and never allocates. */
class MicEmulator
{
public:
    /** Allocates the delay lines and the room's convolution. Not real-time safe. */
    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    /** Writes the emulated mics that are switched on into their channels of
        the mic bus (which must be clear there). feed holds one mono channel
        per piece. */
    void process (const Kit&, const EngineParams&, const juce::AudioBuffer<float>& feed,
                  juce::AudioBuffer<float>& micBus, int numSamples) noexcept;

    /** The emulated room's convolution, for its own IrLoader to load into. */
    ConvolutionReverb& getRoomReverb() noexcept      { return room; }

    // The longest a piece takes to reach an emulated mic, with its floor
    // reflection: the delay lines are this long (plus a block).
    static constexpr double maxDelaySeconds = 0.03;

private:
    static constexpr int numPairs = 2;               // overheads, room
    static constexpr int tapsPerSide = 2;            // the direct sound, and its reflection off the floor

    struct Tap
    {
        int delay = 0;                               // samples
        float gain = 0.0f;
    };

    struct PiecePath
    {
        // [pair][side][tap]
        std::array<std::array<std::array<Tap, tapsPerSide>, 2>, numPairs> taps {};
        dsp::SvfCoefficients shelf;                  // the overheads' low shelf for its role
    };

    struct Line
    {
        std::array<std::vector<float>, numPairs> ring;   // the feed, shelved for the overheads; as it is for the room
        dsp::SvfState shelfState;
        int quiet = 0;                               // samples since it last had any signal
        std::array<float, numPairs> lastSend {};     // the piece's send to each pair, as last processed
    };

    void updatePaths (const Kit&) noexcept;
    static int pairIndex (MicRole r) noexcept        { return r == MicRole::room ? 1 : 0; }

    double sampleRate = 44100.0;
    int ringMask = 0;
    int writePosition = 0;
    // The kit the paths were worked out for. A pointer can be reused by the
    // next kit, so its serial is checked too.
    const Kit* pathsFor = nullptr;
    std::uint64_t pathsForSerial = 0;

    std::array<PiecePath, maxPieces> paths;
    std::array<Line, maxPieces> lines;
    std::array<bool, numPairs> wasOn {};

    juce::AudioBuffer<float> roomIn;
    ConvolutionReverb room;
};

} // namespace lithium
