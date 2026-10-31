#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lithium
{

/** Runs job(i, formats, error) for every i in [0, numJobs) on a pool of
    worker threads. Workers pull the next index from an atomic counter, so
    the load balances itself. Each worker has its own AudioFormatManager,
    because format readers aren't shared between threads. Stops at the
    first failure, or when shouldAbort() says so. */
template <typename Job>
inline bool runParallel (size_t numJobs, const std::function<bool()>& shouldAbort, juce::String& error, Job&& job)
{
    std::atomic<size_t> next { 0 };
    std::atomic<bool> failed { false };
    std::mutex errorMutex;

    const auto numWorkers = (size_t) juce::jlimit (1, 8, (int) std::thread::hardware_concurrency() - 1);

    {
        // std::jthread joins in its destructor, so leaving this scope waits for every worker.
        std::vector<std::jthread> workers;

        for (size_t w = 0; w < std::min (numWorkers, std::max<size_t> (numJobs, 1)); ++w)
        {
            workers.emplace_back ([&]
            {
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();

                for (auto i = next++; i < numJobs && ! failed; i = next++)
                {
                    if (shouldAbort && shouldAbort())
                    {
                        failed = true;
                        break;
                    }

                    juce::String jobError;

                    if (! job (i, formats, jobError))
                    {
                        std::scoped_lock lock (errorMutex);

                        if (error.isEmpty())
                            error = jobError;

                        failed = true;
                    }
                }
            });
        }
    }

    if (failed && error.isEmpty())
        error = "Loading was cancelled";

    return ! failed;
}

} // namespace lithium
