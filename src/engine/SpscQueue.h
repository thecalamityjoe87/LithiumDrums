#pragma once

#include <juce_core/juce_core.h>

#include <array>

namespace lithium
{

/** A fixed-size, lock-free queue for exactly one producer thread and one
    consumer thread. Storage is allocated up front, so push and pop never
    allocate and are safe on the audio thread.

    juce::AbstractFifo does the hard part: it hands out index ranges using
    atomics, so the two threads never touch the same slot at the same time. */
template <typename T, int Capacity>
class SpscQueue
{
public:
    /** Returns false (and drops the item) if the queue is full. */
    bool push (const T& item) noexcept
    {
        const auto scope = fifo.write (1);

        if (scope.blockSize1 == 0)
            return false;

        items[(size_t) scope.startIndex1] = item;
        return true;
    }

    bool pop (T& item) noexcept
    {
        const auto scope = fifo.read (1);

        if (scope.blockSize1 == 0)
            return false;

        item = items[(size_t) scope.startIndex1];
        return true;
    }

    int getFreeSpace() const noexcept      { return fifo.getFreeSpace(); }
    int getNumReady() const noexcept       { return fifo.getNumReady(); }

private:
    // AbstractFifo keeps one slot free to tell "full" from "empty".
    juce::AbstractFifo fifo { Capacity + 1 };
    std::array<T, (size_t) Capacity + 1> items {};
};

} // namespace lithium
