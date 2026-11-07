#include "InMemorySampleSource.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <bit>
#include <cstring>

namespace lithium
{

SampleId SampleLayout::add (int mic, int numChannels, std::int64_t numFrames, std::int64_t maxStoredFrames)
{
    jassert (juce::isPositiveAndBelow (mic, maxMics) && numChannels >= 1 && numChannels <= maxChannelsPerMic);

    auto& size = storeSizes[(size_t) mic];
    const auto stored = std::min (numFrames, maxStoredFrames);
    samples.push_back ({ mic, numChannels, numFrames, stored, size });
    size += (size_t) stored * (size_t) numChannels * (size_t) bytesPerSample;
    return (SampleId) (samples.size() - 1);
}

std::unique_ptr<MicStore> SampleLayout::createStore (int mic) const
{
    auto store = std::make_unique<MicStore>();
    store->data.resize (storeSizes[(size_t) mic]);
    return store;
}

void SampleLayout::pack (std::byte* dest, int numBytes, int value) noexcept
{
    // Little-endian, written byte by byte so it's the same on every platform.
    const auto v = (std::uint32_t) value;
    dest[0] = (std::byte) (v & 0xff);
    dest[1] = (std::byte) ((v >> 8) & 0xff);

    if (numBytes == 3)
        dest[2] = (std::byte) ((v >> 16) & 0xff);
}

//==============================================================================
InMemorySampleSource::InMemorySampleSource (std::shared_ptr<const SampleLayout> l) : layout (std::move (l)) {}

std::size_t InMemorySampleSource::getMemoryUsage() const noexcept
{
    std::size_t bytes = 0;

    for (const auto& s : stores)
        bytes += s != nullptr ? s->data.size() : 0;

    return bytes;
}

std::unique_ptr<MicStore> InMemorySampleSource::swapStore (int mic, std::unique_ptr<MicStore> store) noexcept
{
    // Moving unique_ptrs only shuffles pointers: no allocation, no freeing.
    return std::exchange (stores[(size_t) mic], std::move (store));
}

void InMemorySampleSource::open (SampleId id, SampleCursor& cursor) noexcept
{
    cursor.sample = id;
    cursor.position = 0;
}

void SampleLayout::unpack (const std::byte* bytes, int bytesPerSample, int channels,
                           float* const* dest, int destOffset, int numFrames) noexcept
{
    const auto* src = reinterpret_cast<const unsigned char*> (bytes);

    if (bytesPerSample == 2)
    {
        constexpr float scale = 1.0f / 32768.0f;

        for (int i = destOffset; i < destOffset + numFrames; ++i)
        {
            for (int ch = 0; ch < channels; ++ch, src += 2)
            {
                // Assemble the bytes into an unsigned value, then reinterpret
                // as signed 16-bit: well defined, and endian-independent.
                const auto v = (std::int16_t) (std::uint16_t) (src[0] | (src[1] << 8));
                dest[ch][i] = (float) v * scale;
            }
        }
    }
    else
    {
        constexpr float scale = 1.0f / 8388608.0f;

        // Mono (each disk-streaming ring is one channel): the disk streamer
        // spends most of its time here. One unaligned 4-byte load per sample
        // instead of three byte loads: on a little-endian CPU the load puts the
        // sample's bytes in the low 24 bits, so shifting left by 8 gives the
        // same word as below (the fourth byte, the next sample's, falls off the
        // top). The load reads one byte past the sample, so the buffer's last
        // sample is done byte by byte, to stay inside it.
        if constexpr (std::endian::native == std::endian::little)
        {
            if (channels == 1 && numFrames > 0)
            {
                auto* d = dest[0] + destOffset;
                int i = 0;

                for (; i < numFrames - 1; ++i, src += 3)
                {
                    std::uint32_t word;
                    std::memcpy (&word, src, sizeof (word));   // compiles to one load
                    d[i] = (float) ((std::int32_t) (word << 8) >> 8) * scale;
                }

                const auto word = (std::uint32_t) src[0] << 8 | (std::uint32_t) src[1] << 16 | (std::uint32_t) src[2] << 24;
                d[i] = (float) ((std::int32_t) word >> 8) * scale;
                return;
            }
        }

        for (int i = destOffset; i < destOffset + numFrames; ++i)
        {
            for (int ch = 0; ch < channels; ++ch, src += 3)
            {
                // Put the 24 bits at the top of a 32-bit word, then shift back
                // down as signed: the arithmetic shift sign-extends.
                const auto word = (std::uint32_t) src[0] << 8 | (std::uint32_t) src[1] << 16 | (std::uint32_t) src[2] << 24;
                dest[ch][i] = (float) ((std::int32_t) word >> 8) * scale;
            }
        }
    }
}

int InMemorySampleSource::readStored (SampleId id, std::int64_t position, float* const* dest, int numFrames) const noexcept
{
    const auto& s = layout->samples[id];
    const auto* store = stores[(size_t) s.mic].get();

    // An unloaded (muted) mic reads as silence. Voices keep running either
    // way, so the mic stays in step if it's reloaded mid-hit.
    if (store == nullptr)
        return 0;

    const auto available = (int) std::clamp<std::int64_t> (s.storedFrames - position, 0, numFrames);

    if (available > 0)
        SampleLayout::unpack (store->data.data() + s.offset + (size_t) position * (size_t) s.numChannels * (size_t) layout->bytesPerSample,
                              layout->bytesPerSample, s.numChannels, dest, 0, available);

    return available;
}

void InMemorySampleSource::read (SampleCursor& cursor, float* const* dest, int numFrames) noexcept
{
    const auto available = readStored (cursor.sample, cursor.position, dest, numFrames);

    for (int ch = 0; ch < layout->samples[cursor.sample].numChannels; ++ch)
        std::fill (dest[ch] + available, dest[ch] + numFrames, 0.0f);

    cursor.position += numFrames;
}

void InMemorySampleSource::close (SampleCursor& cursor) noexcept
{
    cursor = {};
}

} // namespace lithium
