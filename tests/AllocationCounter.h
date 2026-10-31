#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <stdlib.h>   // setenv / _putenv_s

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

    inline void setEnvironment (const char* name, const juce::String& value)
    {
       #if JUCE_WINDOWS
        _putenv_s (name, value.toRawUTF8());
       #else
        setenv (name, value.toRawUTF8(), 1);
       #endif
    }
}
