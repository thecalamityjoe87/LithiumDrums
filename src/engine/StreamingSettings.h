#pragma once

#include <juce_core/juce_core.h>

#include <cstddef>

namespace lithium
{

enum class StreamingMode { automatic, always, never };

/** Whether, and how, kits stream from disk. */
struct StreamingSettings
{
    // automatic streams kits that would take more than autoThresholdBytes in RAM.
    StreamingMode mode = StreamingMode::never;
    int preloadFrames = 16384;                       // per sample, per mic, kept in RAM
    juce::File cacheFolder;                          // required for streaming
    std::size_t autoThresholdBytes = (std::size_t) 512 << 20;
};

} // namespace lithium
