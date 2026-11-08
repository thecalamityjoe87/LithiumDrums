#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace lithium
{

/** Audio for a spectrum display: one mixer channel's signal (the one the
    UI picks with setTarget()) just before and just after its EQ, in mono (the
    two channels averaged), kept in two ring buffers that the UI reads the
    latest samples from.

    One writer (the audio thread) and one reader (the message thread), and no
    locks: the writer stores the samples, then publishes how many it has
    written so far with a release store; the reader loads that count with an
    acquire load (so it sees every sample stored before it) and copies the
    samples just before it. If the writer laps the reader mid-copy, part of a
    block shows newer samples, which an analyser can't tell apart; the ring
    holds four reads' worth, so it's rare. The samples are atomics (relaxed,
    which costs nothing on the usual CPUs) so that this race isn't undefined
    behaviour.

    The audio thread only writes while someone is looking (setWanted()), and
    only for the target channel, so the analyser costs nothing otherwise. */
struct SpectrumTap
{
    static constexpr int ringSize = 8192;   // a power of two
    enum Point { beforeEq, afterEq, numPoints };

    void setWanted (bool w) noexcept       { wanted.store (w, std::memory_order_relaxed); }
    bool isWanted() const noexcept         { return wanted.load (std::memory_order_relaxed); }

    /** Which channel to tap: a drum's (its piece slot) or a mic's (see keyFor()). */
    static constexpr int keyFor (bool isPiece, int slot) noexcept   { return isPiece ? slot : 1000 + slot; }
    void setTarget (int key) noexcept      { target.store (key, std::memory_order_relaxed); }

    /** Audio thread: this tap, if it's wanted for this channel; else null. */
    SpectrumTap* ifTapping (int key) noexcept
    {
        return isWanted() && target.load (std::memory_order_relaxed) == key ? this : nullptr;
    }

    /** Message thread: how many samples have been written so far (it only
        changes while the channel is processed, so a stalled count means silence). */
    std::uint64_t getWrittenCount (Point point) const noexcept   { return rings[(size_t) point].written.load (std::memory_order_acquire); }

    /** Audio thread: appends a stereo block. */
    void push (Point point, const float* left, const float* right, int numSamples) noexcept
    {
        auto& ring = rings[(size_t) point];
        auto written = ring.written.load (std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i, ++written)
            ring.samples[(size_t) (written & (ringSize - 1))].store (0.5f * (left[i] + right[i]), std::memory_order_relaxed);

        ring.written.store (written, std::memory_order_release);
    }

    /** Message thread: copies the latest `count` samples (oldest first, at
        most ringSize) into `out`. False until that many have been written. */
    bool read (Point point, float* out, int count) const noexcept
    {
        const auto& ring = rings[(size_t) point];
        const auto written = ring.written.load (std::memory_order_acquire);

        if (count > ringSize || written < (std::uint64_t) count)
            return false;

        const auto first = written - (std::uint64_t) count;

        for (int i = 0; i < count; ++i)
            out[i] = ring.samples[(size_t) ((first + (std::uint64_t) i) & (ringSize - 1))].load (std::memory_order_relaxed);

        return true;
    }

private:
    struct Ring
    {
        std::array<std::atomic<float>, ringSize> samples {};
        std::atomic<std::uint64_t> written { 0 };   // samples written so far, ever
    };

    std::array<Ring, numPoints> rings;
    std::atomic<bool> wanted { false };
    std::atomic<int> target { -1 };
};

} // namespace lithium
