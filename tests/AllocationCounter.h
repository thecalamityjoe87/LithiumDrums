#pragma once

#include <atomic>

namespace lithium::test
{
    extern thread_local bool countingAllocations;
    extern std::atomic<int> allocationCount;

    /** Counts heap allocations made by the current thread while in scope. */
    struct ScopedAllocationCounter
    {
        ScopedAllocationCounter()     { allocationCount = 0; countingAllocations = true; }
        ~ScopedAllocationCounter()    { countingAllocations = false; }

        int count() const             { return allocationCount.load(); }
    };
}
