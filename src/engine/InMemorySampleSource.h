#pragma once

#include "KitSchema.h"
#include "SampleSource.h"

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

namespace lithium
{

/** One mic channel's audio for every hit in a kit, packed as little-endian
    16- or 24-bit integers (interleaved for stereo mics).

    Keeping each mic in its own block is what makes unloading a muted mic
    cheap: the whole block is swapped out in one pointer exchange. */
struct MicStore
{
    std::vector<std::byte> data;
};

/** Where every sample lives: which mic's store, at what offset, how long.

    A sample may keep only its first frames in RAM (storedFrames): the
    streaming source serves the rest from disk. Fully in RAM, storedFrames
    equals numFrames.

    Built once by the loader and never changed afterwards, so it can be
    shared (read-only) between the kit on the audio thread and the loader,
    which needs it to rebuild a mic's store when the mic is unmuted. */
struct SampleLayout
{
    struct Sample
    {
        int mic = 0;
        int numChannels = 1;
        std::int64_t numFrames = 0;
        std::int64_t storedFrames = 0;   // frames held in the mic's store, from the start
        std::size_t offset = 0;          // bytes into the mic's store
    };

    explicit SampleLayout (int bitsPerSample) : bytesPerSample (bitsPerSample <= 16 ? 2 : 3) {}

    /** Adds a sample, keeping at most maxStoredFrames of it in RAM. */
    SampleId add (int mic, int numChannels, std::int64_t numFrames,
                  std::int64_t maxStoredFrames = std::numeric_limits<std::int64_t>::max());

    /** Allocates (but doesn't fill) the store for one mic. */
    std::unique_ptr<MicStore> createStore (int mic) const;

    /** Where a sample's audio goes inside its mic's store. Different samples
        may be written from different threads at once. */
    std::byte* getWritePointer (MicStore& store, SampleId id) const noexcept   { return store.data.data() + samples[id].offset; }

    /** Packs one sample value (already scaled to the bit depth) at dest. */
    static void pack (std::byte* dest, int bytesPerSample, int value) noexcept;

    /** The reverse of pack(), for interleaved data: reads numFrames frames of
        numChannels channels from src into dest[0..numChannels), starting at
        dest[ch][destOffset]. */
    static void unpack (const std::byte* src, int bytesPerSample, int numChannels,
                        float* const* dest, int destOffset, int numFrames) noexcept;

    int bytesPerSample;
    std::vector<Sample> samples;
    std::array<std::size_t, maxMics> storeSizes {};
};

/** Every sample held in RAM, as integers at the source's bit depth, and
    converted to float as voices read it. 24-bit audio takes 3 bytes per
    sample instead of float's 4, and 16-bit takes 2.

    The loader attaches the stores before the kit is published. After that,
    only the audio thread touches this object; it calls swapStore() to unload
    or reload a mic.

    StreamingSampleSource builds on this class: it keeps the first part of
    each sample in these same stores and streams the rest. */
class InMemorySampleSource : public SampleSource
{
public:
    explicit InMemorySampleSource (std::shared_ptr<const SampleLayout>);

    const SampleLayout& getLayout() const noexcept   { return *layout; }

    void attachStore (int mic, std::unique_ptr<MicStore> store)   { stores[(size_t) mic] = std::move (store); }

    /** Bytes held by the stores attached right now. Only call this from the
        thread that currently owns the kit. */
    std::size_t getMemoryUsage() const noexcept override;

    /** True when samples have more frames than are held in RAM. */
    virtual bool isStreaming() const noexcept      { return false; }

    /** Replaces a mic's store (null = unloaded: its samples play as silence)
        and returns the old one. The caller must hand the old store to another
        thread for deletion, since freeing memory isn't real-time safe. */
    std::unique_ptr<MicStore> swapStore (int mic, std::unique_ptr<MicStore>) noexcept;

    bool isMicLoaded (int mic) const noexcept      { return stores[(size_t) mic] != nullptr; }

    //==============================================================================
    // SampleSource

    int getNumChannels (SampleId id) const noexcept override        { return layout->samples[id].numChannels; }
    std::int64_t getNumFrames (SampleId id) const noexcept override { return layout->samples[id].numFrames; }

    void open (SampleId, SampleCursor&) noexcept override;
    void read (SampleCursor&, float* const* dest, int numFrames) noexcept override;
    void close (SampleCursor&) noexcept override;

    /** The start of a sample, as far as it's in RAM (for a streaming kit,
        its preload), without opening a stream. Returns how many frames it
        copied (0 for a mic that isn't loaded). Not for the audio thread's
        playback; the loader uses it to look at a kit's recordings. */
    int peekStart (SampleId id, float* const* dest, int numFrames) const noexcept   { return readStored (id, 0, dest, numFrames); }

protected:
    /** Copies the frames of [position, position + numFrames) that are held in
        RAM into dest, from dest[ch][0]. Returns how many it copied: frames
        past storedFrames, or of an unloaded mic, are left untouched. */
    int readStored (SampleId, std::int64_t position, float* const* dest, int numFrames) const noexcept;

private:
    // shared_ptr only so the loader can keep the layout too. It is copied and
    // released only on the loader thread (kits are built and deleted there),
    // never on the audio thread.
    std::shared_ptr<const SampleLayout> layout;
    std::array<std::unique_ptr<MicStore>, maxMics> stores;
};

} // namespace lithium
