#pragma once

#include <cstddef>
#include <cstdint>

// How voices get at sample audio, without knowing where it lives.
//
// Small kits are held fully in RAM (InMemorySampleSource). Large ones keep the
// first part of each sample in RAM and stream the rest from disk
// (StreamingSampleSource). Voices only ever talk to this interface, so they
// don't know which they're playing from.
//
// The contract is shaped around streaming: a voice opens a cursor at note-on
// and then reads strictly forwards, block by block. A streaming source can
// keep per-cursor state (e.g. which disk buffer the cursor is using) inside
// SampleCursor::streamSlot, and it is allowed to return silence for frames that
// haven't arrived from disk in time, rather than blocking.

namespace lithium
{

using SampleId = std::uint32_t;
inline constexpr SampleId invalidSampleId = 0xffffffffu;

struct SampleCursor
{
    SampleId sample = invalidSampleId;
    std::int64_t position = 0;     // next frame to be read
    int streamSlot = -1;           // the streaming source's ring buffer for this cursor
};

class SampleSource
{
public:
    virtual ~SampleSource() = default;

    // Thread-safe, constant data (may be called from any thread).
    virtual int getNumChannels (SampleId) const noexcept = 0;
    virtual std::int64_t getNumFrames (SampleId) const noexcept = 0;
    virtual std::size_t getMemoryUsage() const noexcept = 0;

    // Audio thread only. Must not allocate, lock, or do I/O.

    /** Points the cursor at the start of a sample. */
    virtual void open (SampleId, SampleCursor&) noexcept = 0;

    /** Copies the next numFrames frames into dest (one pointer per sample
        channel) and advances the cursor. Frames past the end, or not yet
        available, are written as zeros. */
    virtual void read (SampleCursor&, float* const* dest, int numFrames) noexcept = 0;

    /** Releases whatever the cursor holds. */
    virtual void close (SampleCursor&) noexcept = 0;
};

} // namespace lithium
