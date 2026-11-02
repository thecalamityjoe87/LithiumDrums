#pragma once

#include "InMemorySampleSource.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace lithium
{

class DiskStreamer;

/** Streaming counters, for the UI. The processor creates one and every kit it
    loads writes into it, so the numbers carry on across kit changes. Shared
    with shared_ptr because kits are deleted on the loader thread at times the
    processor doesn't control; the audio and disk threads only ever use the
    raw pointer, never the reference count. */
struct StreamStats
{
    std::atomic<std::uint64_t> bytesRead { 0 };        // disk thread
    std::atomic<std::uint64_t> dropouts { 0 };         // audio thread: reads the disk hadn't caught up with
    std::atomic<std::uint64_t> dropoutFrames { 0 };    // audio thread: frames played as silence because of them
    std::atomic<std::uint64_t> readErrors { 0 };       // disk thread
    std::atomic<int> activeStreams { 0 };              // audio thread
};

/** Where one sample's audio is in a stream cache pack's audio.bin. */
struct StreamRegion
{
    std::array<std::int64_t, maxChannelsPerMic> offset {};   // byte offset of the sample's first frame, per channel
    int bytesPerSample = 3;
    int pack = 0;                                            // index into StreamMap::dataFiles
};

/** Where every sample of a kit streams from. Immutable once built, and
    shared by the kit and the loader (which fills preloads from it). A kit
    streams from one cache pack per audio folder: its own, plus one for each
    kit that a swapped-in piece comes from. */
struct StreamMap
{
    std::vector<juce::File> dataFiles;
    std::vector<StreamRegion> regions;   // by SampleId
};

//==============================================================================
/** Plays the first part of each sample from RAM (the preload, kept in the
    same per-mic stores as InMemorySampleSource) and the rest from the stream
    cache on disk.

    Every voice that plays a long sample gets a "stream slot" per mic: a small
    ring buffer that the disk thread (DiskStreamer) keeps topped up ahead of
    the voice. The preload covers the time it takes the disk thread to notice
    a new hit and read its first chunk, so with a sensible preload size the
    voice never waits.

    Real-time safety: the audio thread never blocks, allocates or touches the
    file. Slots, rings and free lists are all allocated in the constructor.
    The two threads share each slot through atomics only:

      - The audio thread owns `generation`, `sample` and `readFrame`. Opening
        or closing a stream changes the slot under a sequence lock: the
        generation is odd while it's being changed and moves to the next even
        number when it's done. The disk thread reads the fields, then checks
        the generation didn't change meanwhile; if it did, it skips the slot
        until the next pass.
      - The disk thread owns `filled`: the generation it filled the ring for,
        packed with the frame up to which the ring holds valid audio. The
        audio thread only trusts it when the generation matches its own, so
        audio left over from the slot's previous use is never played.
      - The ring is written by the disk thread and read by the audio thread,
        never in the same place at once: the disk thread only writes the part
        the audio thread has finished with (behind `readFrame`), and only
        publishes `filled` after writing (release/acquire ordering).

    If the disk falls behind, the voice plays silence for the missing frames
    (counted as a dropout) and carries on in step; the disk thread skips ahead
    to where the voice is. */
class StreamingSampleSource final : public InMemorySampleSource
{
public:
    static constexpr int chunkFrames = 4096;           // frames per disk read, per channel
    static constexpr int ringFrames = 3 * chunkFrames;   // per channel of each slot

    /** micChannels[m] is mic m's channel count. Each mic gets streamsPerMic
        slots: one per voice, since a voice plays each mic at most once. */
    StreamingSampleSource (std::shared_ptr<const SampleLayout>, std::shared_ptr<const StreamMap>,
                           const std::array<int, maxMics>& micChannels, int numMics, int streamsPerMic,
                           std::shared_ptr<StreamStats>);

    /** Stops the disk thread using this source. Loader thread (or any thread
        that isn't the audio thread, once the audio thread has let go of the kit). */
    ~StreamingSampleSource() override;

    /** Memory taken by the ring buffers of every slot. */
    static std::size_t bufferBytesFor (const std::array<int, maxMics>& micChannels, int numMics, int streamsPerMic) noexcept;

    /** False if a cache file couldn't be opened. */
    bool isValid() const noexcept                           { return valid; }
    const std::vector<juce::File>& getDataFiles() const noexcept   { return map->dataFiles; }

    bool isStreaming() const noexcept override              { return true; }
    std::size_t getMemoryUsage() const noexcept override;

    //==============================================================================
    // SampleSource (audio thread)

    void open (SampleId, SampleCursor&) noexcept override;
    void read (SampleCursor&, float* const* dest, int numFrames) noexcept override;
    void close (SampleCursor&) noexcept override;

    //==============================================================================
    /** Disk thread: reads the next chunk for every slot that has room, most
        urgent first. Returns true if it read anything. */
    bool service();

private:
    struct Slot
    {
        // Audio thread writes (under the sequence lock), disk thread reads.
        std::atomic<std::uint32_t> generation { 0 };
        std::atomic<SampleId> sample { invalidSampleId };
        std::atomic<std::int64_t> readFrame { 0 };      // the voice has read everything before this frame

        // Disk thread writes, audio thread reads.
        std::atomic<std::uint64_t> filled { 0 };        // generation << 40 | end of valid audio in the ring

        // Set up in the constructor, then constant.
        std::array<float*, maxChannelsPerMic> ring {};
        int numChannels = 1;

        // Disk thread only.
        std::uint32_t diskGeneration = 0xffffffffu;
        std::int64_t diskFrame = 0;                     // next frame to read from disk
    };

    struct Candidate
    {
        std::int64_t buffered;
        int slot;
        std::uint32_t generation;
    };

    static constexpr int frameBits = 40;
    static constexpr std::uint64_t frameMask = (1ull << frameBits) - 1;
    static constexpr std::uint32_t generationMask = (1u << 24) - 1;

    static std::uint64_t packFilled (std::uint32_t generation, std::int64_t frame) noexcept
    {
        return ((std::uint64_t) (generation & generationMask) << frameBits) | ((std::uint64_t) frame & frameMask);
    }

    void assignSlot (Slot&, SampleId, std::int64_t readFrame) noexcept;
    void fill (Slot&, const Candidate&);

    std::shared_ptr<const StreamMap> map;
    std::shared_ptr<StreamStats> stats;
    const int streamsPerMic;

    std::unique_ptr<Slot[]> slots;            // atomics can't be moved, so not a vector
    int numSlots = 0;
    std::vector<float> ringMemory;

    // Audio thread only: free slots, per mic, as stacks.
    std::vector<int> freeSlots;
    std::array<int, maxMics> numFree {};

    // Disk thread only (after the constructor opens them).
    std::vector<std::unique_ptr<juce::FileInputStream>> files;   // by StreamRegion::pack
    bool valid = true;
    std::vector<std::byte> readBuffer;
    std::vector<Candidate> candidates;

    juce::SharedResourcePointer<DiskStreamer> streamer;
};

//==============================================================================
/** The disk thread. One per process, shared by every streaming kit of every
    plugin instance (get it through juce::SharedResourcePointer), so several
    instances don't fight over the disk with a thread each.

    It loops over the registered sources and lets each read ahead, then
    sleeps for a couple of milliseconds when there's nothing to do. The audio
    thread never wakes it (signalling a thread isn't real-time safe on every
    platform); polling every 2 ms is far quicker than any preload runs out. */
class DiskStreamer final : private juce::Thread
{
public:
    DiskStreamer();
    ~DiskStreamer() override;

    void add (StreamingSampleSource&);

    /** Returns once the disk thread is no longer using the source. */
    void remove (StreamingSampleSource&);

    /** The cache packs being streamed from right now, so that clearing the
        cache can leave them alone. */
    juce::Array<juce::File> getPacksInUse() const;

private:
    void run() override;

    mutable std::mutex mutex;    // held by the disk thread for each pass; never by the audio thread
    std::vector<StreamingSampleSource*> sources;
};

} // namespace lithium
