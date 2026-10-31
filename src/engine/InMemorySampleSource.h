#pragma once

#include "KitSchema.h"
#include "SampleSource.h"

#include <array>
#include <cstddef>
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
        std::size_t offset = 0;    // bytes into the mic's store
    };

    explicit SampleLayout (int bitsPerSample) : bytesPerSample (bitsPerSample <= 16 ? 2 : 3) {}

    SampleId add (int mic, int numChannels, std::int64_t numFrames);

    /** Allocates (but doesn't fill) the store for one mic. */
    std::unique_ptr<MicStore> createStore (int mic) const;

    /** Where a sample's audio goes inside its mic's store. Different samples
        may be written from different threads at once. */
    std::byte* getWritePointer (MicStore& store, SampleId id) const noexcept   { return store.data.data() + samples[id].offset; }

    /** Packs one sample value (already scaled to the bit depth) at dest. */
    static void pack (std::byte* dest, int bytesPerSample, int value) noexcept;

    int bytesPerSample;
    std::vector<Sample> samples;
    std::array<std::size_t, maxMics> storeSizes {};
};

/** Every sample held in RAM, as integers at the source's bit depth, and
    converted to float as voices read it. 24-bit audio takes 3 bytes per
    sample instead of float's 4, and 16-bit takes 2.

    The loader attaches the stores before the kit is published. After that,
    only the audio thread touches this object; it calls swapStore() to unload
    or reload a mic. */
class InMemorySampleSource final : public SampleSource
{
public:
    explicit InMemorySampleSource (std::shared_ptr<const SampleLayout>);

    const SampleLayout& getLayout() const noexcept   { return *layout; }

    void attachStore (int mic, std::unique_ptr<MicStore> store)   { stores[(size_t) mic] = std::move (store); }

    /** Bytes held by the stores attached right now. Only call this from the
        thread that currently owns the kit. */
    std::size_t getMemoryUsage() const noexcept override;

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

private:
    // shared_ptr only so the loader can keep the layout too. It is copied and
    // released only on the loader thread (kits are built and deleted there),
    // never on the audio thread.
    std::shared_ptr<const SampleLayout> layout;
    std::array<std::unique_ptr<MicStore>, maxMics> stores;
};

} // namespace lithium
