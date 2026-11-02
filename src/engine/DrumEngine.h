#pragma once

#include "EngineParams.h"
#include "Kit.h"
#include "SpscQueue.h"
#include "Voice.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <optional>

namespace lithium
{

/** What played on a piece's latest hit. Packed into one 64-bit atomic in
    EngineFeedback, so the UI always reads a consistent set of values. */
struct LastHit
{
    int articulation = -1;      // index into the kit's articulations
    int zone = 0;               // within the articulation, 0 = the softest
    int roundRobin = 0;         // within the zone
    int velocity = 0;

    std::uint64_t pack() const noexcept
    {
        return (std::uint64_t) (velocity & 0xff)
             | (std::uint64_t) (zone & 0xffff) << 8
             | (std::uint64_t) (roundRobin & 0xffff) << 24
             | (std::uint64_t) (articulation & 0xffff) << 40
             | 1ull << 63;      // "valid": 0 means no hit yet
    }

    static std::optional<LastHit> unpack (std::uint64_t bits) noexcept
    {
        if ((bits >> 63) == 0)
            return std::nullopt;

        return LastHit { (int) ((bits >> 40) & 0xffff), (int) ((bits >> 8) & 0xffff),
                         (int) ((bits >> 24) & 0xffff), (int) (bits & 0xff) };
    }
};

/** Lock-free counters the audio thread publishes for the UI. */
struct EngineFeedback
{
    // Per piece, the latest hit (LastHit::pack). Written before pieceHits is
    // incremented, so a UI that sees a new count also sees its hit.
    std::array<std::atomic<std::uint64_t>, maxPieces> lastHit {};

    // Incremented on every hit of a piece; the UI flashes the piece when it
    // sees the count change. Counters (rather than flags) mean no hit is lost
    // even if the UI polls slower than notes arrive.
    std::array<std::atomic<std::uint32_t>, maxPieces> pieceHits {};   // by piece index in the current kit
    std::atomic<int> activeVoices { 0 };

    // MIDI input, for the MIDI panel and MIDI learn. The latest note-on,
    // packed as (count << 16) | (velocity << 8) | note, so the UI can tell a
    // new hit of the same note from the last one. 0 = none yet.
    std::atomic<std::uint32_t> lastNote { 0 };

    // The hi-hat pedal's openness (0 = closed, 1 = open) and the strike
    // positions (0 = centre, 1 = edge), by PositionSource. -1 = not received yet.
    std::atomic<float> hihatOpenness { -1.0f };
    std::array<std::atomic<float>, numPositionSources> position { -1.0f, -1.0f, -1.0f, -1.0f };

    // Counts the hi-hat pedal's controller messages, so the Kit page can
    // flash when they arrive.
    std::atomic<std::uint32_t> pedalMessages { 0 };

    // The latest controller message of any number, for learning a
    // controller and for calibration, packed as
    // (count << 24) | (channel << 16) | (controller << 8) | value. 0 = none yet.
    std::atomic<std::uint32_t> lastController { 0 };

    struct Controller
    {
        int channel, controller, value;
    };

    static std::optional<Controller> unpackController (std::uint32_t bits) noexcept
    {
        if (bits == 0)
            return std::nullopt;

        return Controller { (int) ((bits >> 16) & 0x1f), (int) ((bits >> 8) & 0x7f), (int) (bits & 0x7f) };
    }

    static std::optional<std::pair<int, int>> unpackNote (std::uint32_t bits) noexcept
    {
        if (bits == 0)
            return std::nullopt;

        return std::pair { (int) (bits & 0x7f), (int) ((bits >> 8) & 0x7f) };   // note, velocity
    }
};

/** One incoming MIDI message and what it did, for the MIDI monitor. Plain
    data, sent from the audio thread through a lock-free queue. */
struct MidiMonitorEvent
{
    enum class Kind : std::uint8_t
    {
        note,
        controller,
        aftertouch,
        pedalChick,      // played by the pedal's movement alone
        footSplash
    };

    enum class Detail : std::uint8_t
    {
        none,
        choke,           // the note or aftertouch choked a cymbal
        pedal,           // a hit that followed the pedal (amount = openness), or the pedal controller itself
        position,        // a hit that followed the position (amount = position), or a position controller
        emulated,        // a position the kit has no recordings of, made by filtering the hit
        rideBlend,       // the ride's bow blended towards its edge
        ignored          // a controller on another MIDI channel than the one set
    };

    Kind kind = Kind::note;
    Detail detail = Detail::none;
    std::uint8_t channel = 0;               // 1-16, 0 = the on-screen keyboard
    std::uint8_t number = 0, value = 0;     // note and velocity, or controller and value
    std::int8_t source = -1;                // controllers: -1 = the pedal, else the PositionSource
    std::int16_t articulation = -1;         // what played (the louder of a blend), -1 = nothing
    std::int16_t blendedWith = -1;          // the other step of a blend
    float blend = 0.0f;                     // how much of blendedWith, 0..1
    float amount = -1.0f;                   // the openness or position, 0..1; -1 = none
    float position = -1.0f;                 // a hi-hat hit: its bow position as well as the pedal's openness
};

/** The voice pool and everything that decides what plays.

    Owns no kit: the processor passes the current kit in on every call, and
    only swaps kits once all voices are silent (see KitHandoff). Every method
    except prepare() runs on the audio thread and never allocates. */
class DrumEngine
{
public:
    static constexpr int numVoices = 64;
    static constexpr int voiceStealThreshold = numVoices - 8;
    static constexpr int maxVoicesPerPiece = 12;
    static constexpr int maxSliceSize = 256;    // longest stretch rendered in one go

    explicit DrumEngine (EngineFeedback&);

    /** Allocates scratch memory. Not real-time safe. */
    void prepare (double sampleRate);

    /** A note from MIDI (channel 1-16) or the on-screen keyboard (channel 0). */
    void noteOn (Kit&, int note, int velocity, const EngineParams&, int channel = 0) noexcept;
    void polyAftertouch (Kit&, int note, int pressure, int channel = 0) noexcept;

    /** The hi-hat pedal and strike position controllers, as the kit's note
        table's ControllerSettings set them. The pedal can play a chick or a
        foot splash by itself (see ChickMode). Other controllers only reach
        the MIDI monitor and MIDI learn. */
    void controlChange (Kit&, int channel, int controller, int value, const EngineParams&) noexcept;

    /** Where the next MIDI message happens, in samples since the start: the
        pedal's speed is worked out from it. */
    void setTime (std::int64_t sample) noexcept         { now = sample; }

    /** Plays one articulation. gainScale scales the whole hit (for blends
        between openness or position steps); with chokeOthers false the hit
        doesn't apply its chokes (the caller already has). positionTone
        filters it (an emulated strike position). */
    void trigger (Kit&, int articulation, int velocity, const EngineParams&,
                  float gainScale = 1.0f, bool chokeOthers = true,
                  const std::array<dsp::SvfCoefficients, 2>& positionTone = {}) noexcept;

    /** The MIDI monitor's events. One consumer (the MIDI panel). */
    bool popMonitorEvent (MidiMonitorEvent& e) noexcept  { return monitor.pop (e); }

    /** Pedal openness (after calibration and the curve) as the hi-hat's
        openness ladder sees it: the thresholds map the pedal's travel onto
        the standard steps (closed 0, quarter 0.25, half 0.5, three quarters
        0.75, open 1), so a kit with other steps is spread over the same curve. */
    static float hatLadderValue (float pedalOpenness, const ControllerSettings&) noexcept;

    /** A controller value (0-127) as pedal openness or position (0..1):
        calibrated to the range the hardware really sends, inverted if
        asked, and bent by the curve. */
    static float pedalOpenness (int value, const ControllerSettings&) noexcept;
    static float padPosition (int source, int value, const ControllerSettings&) noexcept;

    /** The filters that make a centre hit sound as if struck nearer the edge
        (0 = as recorded .. 1 = at the edge): less body, more ring and
        overtones. The level drop is returned separately (as a gain). */
    static std::pair<std::array<dsp::SvfCoefficients, 2>, float> positionTone (PieceRole, float edge, double sampleRate) noexcept;

    /** Mixes all voices into the buses: close mics into the pieces' channels,
        overhead and room mics into the mic bus. numSamples may be any
        length; it is rendered in slices of at most maxSliceSize. */
    void render (Kit&, VoiceBuses, int startSample, int numSamples, const EngineParams&) noexcept;

    void fadeOutAll (double seconds) noexcept;
    void stopAll (Kit&) noexcept;
    int getNumActiveVoices() const noexcept;

private:
    /** What a ladder played: one step, or two blended. */
    struct Played
    {
        int articulation = -1, other = -1;
        float blend = 0.0f;
    };

    int findZone (const Kit&, const Articulation&, int velocity) const noexcept;
    Played playLadder (Kit&, const Ladder&, float value, int velocity, const EngineParams&,
                       float gainScale = 1.0f, const std::array<dsp::SvfCoefficients, 2>& positionTone = {}) noexcept;
    Played playPosition (Kit&, int articulation, float strike, float emulation, int velocity, const EngineParams&,
                         MidiMonitorEvent::Detail&) noexcept;
    void applyChokes (const Kit&, const Articulation&) noexcept;
    void dampHats (const Kit&, float ladderOpenness) noexcept;
    void pedalMoved (Kit&, float previous, float openness, int channel, const EngineParams&) noexcept;
    void playPedalHit (Kit&, bool splash, float speed, int channel, const EngineParams&) noexcept;
    void toMonitor (const MidiMonitorEvent& e) noexcept  { monitor.push (e); }   // dropped if the monitor is behind
    /** Returns the round-robin that was started (within its zone), or -1 if
        the pool had no free voice. */
    int startVoice (Kit&, const Articulation&, int articulationIndex, int zoneIndex, float gain,
                    double pitchRatio, const PieceParams& shaping, const std::array<dsp::SvfCoefficients, 2>& positionTone) noexcept;
    Voice* allocateVoice (int piece) noexcept;
    void chokePiece (int piece, double seconds) noexcept;
    void updateRenderContext (const Kit&, const EngineParams&) noexcept;
    int secondsToSamples (double seconds) const noexcept;

    EngineFeedback& feedback;
    std::array<Voice, numVoices> voices;
    std::uint64_t nextSerial = 0;
    double sampleRate = 44100.0;
    juce::Random random;

    // Controller state, kept across kit changes: it's where the pedal is,
    // not something about the kit. Latched per MIDI channel, so a note
    // always plays with the value that came on its own channel just before
    // it ([0]: the latest on any channel). -1 = not received yet.
    using PerChannel = std::array<float, 17>;
    static PerChannel notYet() noexcept                 { PerChannel a; a.fill (-1.0f); return a; }
    static float latched (const PerChannel&, int channel) noexcept;

    PerChannel hatOpenness = notYet();
    std::array<PerChannel, numPositionSources> position { notYet(), notYet(), notYet(), notYet() };
    std::uint32_t noteCount = 0, controllerCount = 0, pedalCount = 0;

    // The pedal's recent movement, for chicks and foot splashes: where it
    // was when (a small ring of the latest values), and the last fast close.
    struct PedalPoint
    {
        std::int64_t time = 0;
        float openness = 0.0f;
    };

    std::array<PedalPoint, 16> pedalHistory {};
    int pedalHistoryNext = 0;
    std::int64_t lastClose = 0;
    bool splashArmed = false;
    bool pedalNoteSeen = false;      // the module sends pedal notes itself (ChickMode::automatic)
    std::int64_t now = 0;

    SpscQueue<MidiMonitorEvent, 512> monitor;

    juce::AudioBuffer<float> scratch;      // resampler source frames
    juce::AudioBuffer<float> streamOut;    // one stream's output before it's mixed
    std::vector<float> envelope;           // per-sample envelope for one voice
    VoiceRenderContext context;
};

} // namespace lithium
