#pragma once

#include "SpscQueue.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace lithium
{

/** A groove clip resolved against one kit: which articulation plays when.
    Built on the message thread (see grooves::resolve()), then owned by the
    audio thread, which only reads it. */
struct GroovePattern
{
    struct Event
    {
        double beat;                   // 0 .. lengthBeats
        std::int16_t articulation;     // index into the kit's articulations
        std::uint8_t velocity;
    };

    std::vector<Event> events;         // sorted by beat
    double lengthBeats = 4.0;
    double bpm = 120.0;                // the groove's own tempo
    std::uint64_t kitSerial = 0;       // the kit the articulations are for
    int clipId = 0;                    // which clip this is, for the UI's progress display

    // Start from the top (a newly chosen clip), rather than carry on where
    // the previous pattern was (the same clip, resolved for a new kit).
    bool restart = true;
};

enum class GrooveTempo : int
{
    host,        // the host's tempo (in the standalone app, which has none: the groove's own)
    original,    // the groove's own tempo
    custom       // a tempo set on the Grooves page
};

/** Plays the groove preview on the audio thread.

    The message thread builds a GroovePattern and send()s it. Patterns cross
    threads the way kits do (see KitHandoff), with two lock-free queues:
    new ones go to the audio thread through one, and the ones it's done with
    come back through the other, so the audio thread never frees memory. The
    settings (play, loop, tempo, speed) are plain atomics, and so is what the
    UI reads back (position, tempo).

    With the host playing at its own tempo, the preview locks to the host's
    beat position: a 4-bar phrase lines up with the host's 4-bar grid, as if
    it were on a track. Otherwise it runs from the top at the chosen tempo. */
class GroovePlayer
{
public:
    GroovePlayer() = default;
    ~GroovePlayer();

    //==============================================================================
    // Message thread.

    /** Hands a pattern to the audio thread, and starts playing it if asked.
        Returns false if the queue was full (the caller tries again later). */
    bool send (std::unique_ptr<GroovePattern>&);

    /** Deletes patterns the audio thread has finished with. Call regularly. */
    void collectGarbage();

    std::atomic<bool> playing { false };
    std::atomic<bool> loop { true };
    std::atomic<GrooveTempo> tempoMode { GrooveTempo::host };
    std::atomic<float> customBpm { 120.0f };
    std::atomic<float> speed { 1.0f };          // 0.5 = half time, 2 = double time

    /** Where the preview is, for the UI. clipId is -1 when nothing plays. */
    struct Feedback
    {
        std::atomic<int> clipId { -1 };
        std::atomic<double> position { 0.0 };   // beats into the clip
        std::atomic<float> bpm { 0.0f };        // the tempo it's playing at
        std::atomic<bool> hostSynced { false };
        std::atomic<float> hostBpm { 0.0f };    // the host's tempo, playing or not; 0 = it has none
    };

    const Feedback& getFeedback() const noexcept          { return feedback; }

    //==============================================================================
    // Audio thread.

    struct Transport
    {
        std::optional<double> hostBpm, hostPpq;
        bool hostPlaying = false;
    };

    struct Hit
    {
        int sampleOffset;
        int articulation;
        int velocity;
    };

    /** Advances by one block and fills `out` with the hits that land in it,
        in time order (as many as fit). Plays nothing if the pattern was made
        for another kit than kitSerial. Returns the number of hits. */
    int process (std::uint64_t kitSerial, const Transport&, int numSamples, double sampleRate, std::span<Hit> out) noexcept;

private:
    void takeIncoming() noexcept;
    void retire (GroovePattern*) noexcept;

    // Audio thread.
    GroovePattern* current = nullptr;
    GroovePattern* unretired = nullptr;          // waiting for room in the retired queue
    double position = 0.0;
    bool wasPlaying = false;

    SpscQueue<GroovePattern*, 8> incoming, retired;
    Feedback feedback;

    static_assert (std::atomic<double>::is_always_lock_free, "the audio thread can't wait on a lock");

    JUCE_DECLARE_NON_COPYABLE (GroovePlayer)
};

} // namespace lithium
