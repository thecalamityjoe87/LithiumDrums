#include "StreamingSampleSource.h"

#include <algorithm>

namespace lithium
{

StreamingSampleSource::StreamingSampleSource (std::shared_ptr<const SampleLayout> l, std::shared_ptr<const StreamMap> m,
                                              const std::array<int, maxMics>& micChannels, int numMics, int perMic,
                                              std::shared_ptr<StreamStats> s)
    : InMemorySampleSource (std::move (l)), map (std::move (m)), stats (std::move (s)), streamsPerMic (perMic)
{
    jassert (map->regions.size() == getLayout().samples.size());
    jassert (stats != nullptr);

    numSlots = numMics * streamsPerMic;
    slots = std::make_unique<Slot[]> ((size_t) numSlots);
    ringMemory.assign (bufferBytesFor (micChannels, numMics, streamsPerMic) / sizeof (float), 0.0f);
    freeSlots.resize ((size_t) numSlots);

    auto* ring = ringMemory.data();

    for (int mic = 0; mic < numMics; ++mic)
    {
        for (int k = 0; k < streamsPerMic; ++k)
        {
            const auto index = mic * streamsPerMic + k;
            auto& slot = slots[(size_t) index];
            slot.numChannels = micChannels[(size_t) mic];

            for (int ch = 0; ch < slot.numChannels; ++ch, ring += ringFrames)
                slot.ring[(size_t) ch] = ring;

            freeSlots[(size_t) index] = index;
        }

        numFree[(size_t) mic] = streamsPerMic;
    }

    // Every chunk of every channel goes through this buffer on its way into a ring.
    readBuffer.resize ((size_t) chunkFrames * 3);
    candidates.reserve ((size_t) numSlots);

    for (const auto& dataFile : map->dataFiles)
    {
        auto stream = std::make_unique<juce::FileInputStream> (dataFile);
        valid = valid && stream->openedOk();
        files.push_back (std::move (stream));
    }

    streamer->add (*this);
}

StreamingSampleSource::~StreamingSampleSource()
{
    // After this returns, the disk thread can't be inside service().
    streamer->remove (*this);
}

std::size_t StreamingSampleSource::bufferBytesFor (const std::array<int, maxMics>& micChannels, int numMics,
                                                   int perMic) noexcept
{
    std::size_t channels = 0;

    for (int m = 0; m < numMics; ++m)
        channels += (size_t) micChannels[(size_t) m];

    return channels * (size_t) perMic * (size_t) ringFrames * sizeof (float);
}

std::size_t StreamingSampleSource::getMemoryUsage() const noexcept
{
    return InMemorySampleSource::getMemoryUsage() + ringMemory.size() * sizeof (float);
}

//==============================================================================
void StreamingSampleSource::assignSlot (Slot& slot, SampleId id, std::int64_t readFrame) noexcept
{
    // The writer side of the sequence lock. The release fence keeps the field
    // stores after the odd generation; the final release store keeps them
    // before the even one. A reader that saw any of the new fields therefore
    // also sees the generation change, and throws away what it read.
    const auto g = slot.generation.load (std::memory_order_relaxed);
    slot.generation.store (g + 1, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);

    slot.sample.store (id, std::memory_order_relaxed);
    slot.readFrame.store (readFrame, std::memory_order_relaxed);

    slot.generation.store (g + 2, std::memory_order_release);
}

void StreamingSampleSource::open (SampleId id, SampleCursor& cursor) noexcept
{
    InMemorySampleSource::open (id, cursor);
    cursor.streamSlot = -1;

    const auto& s = getLayout().samples[id];

    // Short samples are entirely in RAM. A muted (unloaded) mic gets no slot
    // either, so it costs no disk reads; it plays silence, as it would in RAM.
    if (s.storedFrames >= s.numFrames || ! isMicLoaded (s.mic))
        return;

    auto& count = numFree[(size_t) s.mic];

    if (count == 0)
    {
        jassertfalse;   // can't happen: there's a slot per voice for every mic
        return;
    }

    const auto index = freeSlots[(size_t) (s.mic * streamsPerMic + --count)];
    assignSlot (slots[(size_t) index], id, s.storedFrames);
    cursor.streamSlot = index;
    stats->activeStreams.fetch_add (1, std::memory_order_relaxed);
}

void StreamingSampleSource::read (SampleCursor& cursor, float* const* dest, int numFrames) noexcept
{
    const auto& s = getLayout().samples[cursor.sample];
    const auto position = cursor.position;
    const auto channels = s.numChannels;

    // 1. The preloaded part, from RAM.
    const auto fromRam = readStored (cursor.sample, position, dest, numFrames);

    // 2. The streamed part: frames from storedFrames on, out of the slot's ring.
    const auto streamStart = std::max (position, s.storedFrames);
    const auto firstStreamed = (int) std::clamp<std::int64_t> (streamStart - position, 0, numFrames);
    const auto wanted = (int) std::clamp<std::int64_t> (s.numFrames - streamStart, 0, numFrames - firstStreamed);
    int got = 0;

    if (wanted > 0 && cursor.streamSlot >= 0)
    {
        auto& slot = slots[(size_t) cursor.streamSlot];
        const auto generation = slot.generation.load (std::memory_order_relaxed);   // our own write
        const auto filled = slot.filled.load (std::memory_order_acquire);
        const auto end = (std::uint32_t) (filled >> frameBits) == (generation & generationMask)
                           ? (std::int64_t) (filled & frameMask)
                           : s.storedFrames;     // nothing read for this use of the slot yet

        got = (int) std::clamp<std::int64_t> (end - streamStart, 0, wanted);

        if (got > 0)
        {
            const auto ringPos = (int) (streamStart % ringFrames);
            const auto first = std::min (got, ringFrames - ringPos);

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto* ring = slot.ring[(size_t) ch];
                std::copy (ring + ringPos, ring + ringPos + first, dest[ch] + firstStreamed);
                std::copy (ring, ring + (got - first), dest[ch] + firstStreamed + first);
            }
        }

        // The release store hands the frames we just copied back to the disk
        // thread, which may now overwrite them. Missing frames are skipped
        // too, so the disk thread catches up with where the voice is.
        slot.readFrame.store (streamStart + wanted, std::memory_order_release);

        if (got < wanted)
        {
            stats->dropouts.fetch_add (1, std::memory_order_relaxed);
            stats->dropoutFrames.fetch_add ((std::uint64_t) (wanted - got), std::memory_order_relaxed);
        }
    }

    // 3. Silence for everything else: an unloaded mic's preload, frames the
    //    disk hasn't delivered, and anything past the end.
    for (int ch = 0; ch < channels; ++ch)
    {
        std::fill (dest[ch] + fromRam, dest[ch] + firstStreamed, 0.0f);
        std::fill (dest[ch] + firstStreamed + got, dest[ch] + numFrames, 0.0f);
    }

    cursor.position += numFrames;
}

void StreamingSampleSource::close (SampleCursor& cursor) noexcept
{
    if (cursor.streamSlot >= 0)
    {
        const auto mic = cursor.streamSlot / streamsPerMic;
        assignSlot (slots[(size_t) cursor.streamSlot], invalidSampleId, 0);

        // The slot can be reused straight away: the new generation tells the
        // disk thread to start over, even if it's in the middle of a read for
        // the old one.
        freeSlots[(size_t) (mic * streamsPerMic + numFree[(size_t) mic]++)] = cursor.streamSlot;
        stats->activeStreams.fetch_sub (1, std::memory_order_relaxed);
    }

    cursor = {};
}

//==============================================================================
bool StreamingSampleSource::service()
{
    if (! valid)
        return false;

    const auto& samples = getLayout().samples;
    candidates.clear();   // keeps its capacity: no allocation here either, though this isn't the audio thread

    for (int i = 0; i < numSlots; ++i)
    {
        auto& slot = slots[(size_t) i];

        // The reader side of the sequence lock: read, then check nothing changed.
        const auto g1 = slot.generation.load (std::memory_order_acquire);

        if ((g1 & 1) != 0)
            continue;     // being changed right now; next pass

        const auto id = slot.sample.load (std::memory_order_relaxed);
        const auto readFrame = slot.readFrame.load (std::memory_order_acquire);
        std::atomic_thread_fence (std::memory_order_acquire);

        if (slot.generation.load (std::memory_order_relaxed) != g1 || id == invalidSampleId)
            continue;

        const auto& s = samples[id];

        if (slot.diskGeneration != g1)
        {
            // A new hit in this slot: start reading where the preload ends.
            slot.diskGeneration = g1;
            slot.diskFrame = s.storedFrames;
        }

        slot.diskFrame = std::max (slot.diskFrame, readFrame);   // the voice got ahead of us (a dropout)

        const auto remaining = s.numFrames - slot.diskFrame;
        const auto space = ringFrames - (slot.diskFrame - readFrame);

        // Only whole chunks (or the sample's last one): fewer, larger reads.
        if (remaining > 0 && space >= std::min<std::int64_t> (chunkFrames, remaining))
            candidates.push_back ({ slot.diskFrame - readFrame, i, g1 });
    }

    // The voices closest to running out go first.
    std::sort (candidates.begin(), candidates.end(),
               [] (const Candidate& a, const Candidate& b) { return a.buffered < b.buffered; });

    for (const auto& c : candidates)
        fill (slots[(size_t) c.slot], c);

    return ! candidates.empty();
}

void StreamingSampleSource::fill (Slot& slot, const Candidate& c)
{
    // Sequence-lock read again: the slot may have been reused since service() looked.
    const auto id = slot.sample.load (std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_acquire);

    if (slot.generation.load (std::memory_order_relaxed) != c.generation)
        return;   // the next pass starts it afresh

    const auto& s = getLayout().samples[id];
    const auto& region = map->regions[id];
    auto& file = files[(size_t) region.pack];
    const auto bytes = region.bytesPerSample;
    const auto start = slot.diskFrame;
    const auto n = (int) std::min<std::int64_t> (chunkFrames, s.numFrames - start);
    const auto ringPos = (int) (start % ringFrames);
    const auto first = std::min (n, ringFrames - ringPos);

    for (int ch = 0; ch < slot.numChannels; ++ch)
    {
        const auto numBytes = (size_t) n * (size_t) bytes;
        auto ok = file->setPosition (region.offset[(size_t) ch] + start * bytes);
        ok = ok && file->read (readBuffer.data(), numBytes) == (int) numBytes;

        if (! ok)
        {
            // The cache file was changed or truncated underneath us. Play
            // silence rather than retrying forever.
            std::fill (readBuffer.begin(), readBuffer.begin() + (std::ptrdiff_t) numBytes, std::byte {});
            stats->readErrors.fetch_add (1, std::memory_order_relaxed);
        }

        auto* ring = slot.ring[(size_t) ch];
        SampleLayout::unpack (readBuffer.data(), bytes, 1, &ring, ringPos, first);
        SampleLayout::unpack (readBuffer.data() + (size_t) first * (size_t) bytes, bytes, 1, &ring, 0, n - first);
        stats->bytesRead.fetch_add (numBytes, std::memory_order_relaxed);
    }

    slot.diskFrame = start + n;

    // Publish the new audio, but only if the slot still plays the same hit.
    // (If it was reused meanwhile, the stale `filled` would be ignored by the
    // audio thread anyway; skipping it just keeps things tidy.)
    if (slot.generation.load (std::memory_order_acquire) == c.generation)
        slot.filled.store (packFilled (c.generation, slot.diskFrame), std::memory_order_release);
}

//==============================================================================
DiskStreamer::DiskStreamer() : juce::Thread ("Lithium disk streamer")
{
    // Above normal priority, so a busy UI can't starve it, but not a
    // real-time priority: it does file I/O.
    startThread (juce::Thread::Priority::high);
}

DiskStreamer::~DiskStreamer()
{
    stopThread (5000);
}

void DiskStreamer::add (StreamingSampleSource& source)
{
    {
        std::scoped_lock lock (mutex);
        sources.push_back (&source);
    }

    notify();   // wake it from an idle wait
}

void DiskStreamer::remove (StreamingSampleSource& source)
{
    // Waits for the pass in progress (if any) to finish.
    std::scoped_lock lock (mutex);
    sources.erase (std::remove (sources.begin(), sources.end(), &source), sources.end());
}

juce::Array<juce::File> DiskStreamer::getPacksInUse() const
{
    std::scoped_lock lock (mutex);
    juce::Array<juce::File> packs;

    for (const auto* s : sources)
        for (const auto& dataFile : s->getDataFiles())
            packs.addIfNotAlreadyThere (dataFile.getParentDirectory());

    return packs;
}

void DiskStreamer::run()
{
    while (! threadShouldExit())
    {
        bool busy = false, streaming = false, idle = false;

        {
            std::scoped_lock lock (mutex);
            idle = sources.empty();

            for (auto* s : sources)
            {
                busy = s->service() || busy;
                streaming = streaming || s->hasActiveStreams();
            }
        }

        // Straight round again while there's reading to do; a short nap
        // while voices are streaming. With a kit loaded but nothing playing,
        // a longer one: a new hit plays from its preload meanwhile, and the
        // smallest (8192 frames) lasts over 40 ms even for a 96 kHz kit
        // pitched an octave up. With no streaming kit at all, a long one
        // (add() wakes it).
        if (! busy)
            wait (idle ? 1000 : streaming ? 2 : 10);
    }
}

} // namespace lithium
