#include "GroovePlayer.h"

#include <algorithm>
#include <cmath>

namespace lithium
{

GroovePlayer::~GroovePlayer()
{
    // By now no other thread uses the player, so everything it holds can go.
    delete current;
    delete unretired;

    for (auto* queue : { &incoming, &retired })
        for (GroovePattern* p = nullptr; queue->pop (p);)
            delete p;
}

bool GroovePlayer::send (std::unique_ptr<GroovePattern>& pattern)
{
    collectGarbage();

    if (! incoming.push (pattern.get()))
        return false;

    // The queue holds a plain pointer; ownership now rides with it to the audio thread.
    pattern.release();
    return true;
}

void GroovePlayer::collectGarbage()
{
    for (GroovePattern* p = nullptr; retired.pop (p);)
        delete p;
}

void GroovePlayer::retire (GroovePattern* p) noexcept
{
    jassert (unretired == nullptr);

    if (! retired.push (p))
        unretired = p;   // the message thread hasn't collected lately; try again next block
}

void GroovePlayer::takeIncoming() noexcept
{
    if (unretired != nullptr)
    {
        if (! retired.push (unretired))
            return;   // still no room: leave new patterns waiting, so nothing is lost

        unretired = nullptr;
    }

    for (GroovePattern* next = nullptr; unretired == nullptr && incoming.pop (next);)
    {
        if (next->restart || current == nullptr)
            position = 0.0;
        else
            position = std::fmod (position, next->lengthBeats);

        if (current != nullptr)
            retire (current);

        current = next;
    }
}

int GroovePlayer::process (std::uint64_t kitSerial, const Transport& transport, int numSamples, double sampleRate,
                           std::span<Hit> out) noexcept
{
    takeIncoming();
    feedback.hostBpm.store ((float) transport.hostBpm.value_or (0.0), std::memory_order_relaxed);

    if (! playing.load (std::memory_order_relaxed) || current == nullptr || current->lengthBeats <= 0.0
        || numSamples <= 0 || sampleRate <= 0.0)
    {
        wasPlaying = false;
        feedback.clipId.store (-1, std::memory_order_relaxed);
        return 0;
    }

    if (! wasPlaying)
        position = 0.0;   // Play after Stop starts from the top

    wasPlaying = true;

    const auto& pattern = *current;
    const auto length = pattern.lengthBeats;
    const auto mode = tempoMode.load (std::memory_order_relaxed);
    const auto rate = (double) juce::jlimit (0.25f, 4.0f, speed.load (std::memory_order_relaxed));

    auto bpm = pattern.bpm;

    if (mode == GrooveTempo::host && transport.hostBpm.has_value() && *transport.hostBpm > 0.0)
        bpm = *transport.hostBpm;
    else if (mode == GrooveTempo::custom)
        bpm = (double) juce::jlimit (20.0f, 400.0f, customBpm.load (std::memory_order_relaxed));

    // Beats of the pattern per sample. Half time plays the pattern's beats at
    // half the rate, so its 4 bars take 8 of the host's.
    const auto beatsPerSample = bpm * rate / (60.0 * sampleRate);

    // Locked to the host, the position comes from the host's own beat
    // position every block, so it follows loops and jumps there too.
    const auto synced = mode == GrooveTempo::host && transport.hostPlaying && transport.hostPpq.has_value();

    if (synced)
    {
        position = std::fmod (*transport.hostPpq * rate, length);

        if (position < 0.0)
            position += length;
    }

    // Plays nothing (but keeps time) while the pattern waits to be resolved for a new kit.
    const auto audible = pattern.kitSerial == kitSerial;
    const auto shouldLoop = loop.load (std::memory_order_relaxed);
    const auto maxHits = (int) out.size();
    int numHits = 0;

    auto from = position;
    auto remaining = numSamples * beatsPerSample;   // beats left in this block
    auto samplesDone = 0.0;                        // samples of the block before `from`
    auto stopped = false;

    while (remaining > 0.0)
    {
        const auto to = std::min (length, from + remaining);

        // What's left can be too small to move the position at all (it
        // rounds away against it), which would loop here forever.
        if (to <= from && from < length)
            break;

        if (audible)
        {
            auto it = std::lower_bound (pattern.events.begin(), pattern.events.end(), from,
                                        [] (const GroovePattern::Event& e, double beat) { return e.beat < beat; });

            for (; it != pattern.events.end() && it->beat < to && numHits < maxHits; ++it)
            {
                const auto offset = (int) (samplesDone + (it->beat - from) / beatsPerSample);
                out[(size_t) numHits++] = { juce::jlimit (0, numSamples - 1, offset), it->articulation, it->velocity };
            }
        }

        samplesDone += (to - from) / beatsPerSample;
        remaining -= to - from;
        from = to;

        if (from >= length)
        {
            if (! shouldLoop)
            {
                stopped = true;
                break;
            }

            from = 0.0;
        }
    }

    position = from;

    if (stopped)
    {
        playing.store (false, std::memory_order_relaxed);
        wasPlaying = false;
        feedback.clipId.store (-1, std::memory_order_relaxed);
        return numHits;
    }

    feedback.position.store (position, std::memory_order_relaxed);
    feedback.bpm.store ((float) (bpm * rate), std::memory_order_relaxed);
    feedback.hostSynced.store (synced, std::memory_order_relaxed);
    feedback.clipId.store (pattern.clipId, std::memory_order_relaxed);
    return numHits;
}

} // namespace lithium
