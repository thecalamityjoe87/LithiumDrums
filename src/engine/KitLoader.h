#pragma once

#include "Kit.h"
#include "KitHandoff.h"
#include "MidiMap.h"
#include "StreamingSampleSource.h"
#include "StreamingSettings.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace lithium
{

/** Everything needed to decode a kit's audio again later: which files feed
    which mic samples. Kept by the loader so a mic that was unloaded while
    muted can be rebuilt when it's unmuted. Immutable once built. */
struct LoadPlan
{
    struct Target
    {
        int mic = 0;
        SampleId sample = invalidSampleId;
        std::array<int, 2> fileChannels {};   // 0-based channels in the file (both equal for a mono file on a stereo mic)
        std::int64_t start = 0;               // first frame of the file that belongs to the sample
    };

    struct File
    {
        juce::String path;                    // relative to the audio folder
        int numChannels = 0;
        std::vector<Target> targets;
    };

    juce::File folder;                        // the audio folder: the kit's own, or a preset's base kit
    std::shared_ptr<const SampleLayout> layout;
    std::vector<File> files;
    int numMics = 0;

    // Streaming kits: where the samples are in the stream cache. Preloads
    // are read from there (no decoding) rather than from the source files.
    std::shared_ptr<const StreamMap> stream;
    std::size_t streamBufferBytes = 0;        // allocated whichever mics are loaded

    /** Memory used with the given mics loaded: their stores, plus the stream buffers. */
    std::size_t bytesFor (std::uint32_t micMask) const noexcept;
};

using MicMask = std::uint32_t;   // bit n = mic n
inline constexpr MicMask allMics = 0xffffffffu;
static_assert (maxMics <= 32, "MicMask needs a bit per mic");

enum class LoadPhase { reading, caching, loading };

/** How KitLoader::loadKit() and decode() run. (Declared outside KitLoader
    because a nested struct with default member values can't be used as a
    default argument inside its own class.) */
struct KitLoadOptions
{
    MicMask mics = allMics;                     // mics to decode into RAM
    std::function<bool()> shouldAbort;          // polled between files
    std::atomic<float>* progress = nullptr;     // set to 0..1 as loading goes on
    std::atomic<LoadPhase>* phase = nullptr;    // what loading is busy with
    float progressFrom = 0.1f;                  // decode() moves progress from here to 1

    StreamingSettings streaming;
    std::shared_ptr<StreamStats> streamStats;   // where a streaming kit counts its reads and dropouts
};

/** Loads kits on a background thread and publishes them through a KitHandoff.
    The same thread unloads and reloads mics as they're muted and unmuted, and
    deletes the kits and mic stores that the audio thread retires.

    Requests come from the message thread. A newer request cancels a load that
    is still in progress. Decoding is spread over several worker threads,
    because large libraries are mostly decode-bound. */
class KitLoader : private juce::Thread
{
public:
    struct Request
    {
        juce::File folder;
        std::uint64_t resetToken = 0;   // copied into the Kit, see Kit::resetToken
        MicMask mics = allMics;         // mics to load into RAM straight away
        StreamingSettings streaming;
        std::shared_ptr<StreamStats> streamStats;
        MidiMap midiMap = MidiMap::lithiumStandard();   // the map the kit starts with
    };

    struct Result
    {
        juce::File folder;
        std::shared_ptr<const KitInfo> info;   // null on failure
        juce::String error;
        std::uint64_t resetToken = 0;
    };

    explicit KitLoader (KitHandoff&);
    ~KitLoader() override;

    /** Starts loading a kit. Any thread except the audio thread. */
    void requestLoad (Request);

    /** The most recent finished load, if there is one that hasn't been taken yet. */
    std::optional<Result> takeResult();

    bool isLoading() const noexcept           { return loading.load(); }
    float getProgress() const noexcept        { return progress.load(); }
    LoadPhase getPhase() const noexcept       { return phase.load(); }

    /** Which mics of the current kit should be in RAM. The loader thread
        loads and unloads mics until it matches. Any thread. */
    void setWantedMics (MicMask);

    /** Bytes of sample data in RAM for the current kit. Any thread. */
    std::size_t getMemoryUsage() const noexcept   { return memoryUsage.load(); }

    /** Called on the loader thread after each finished load. Set it before the
        first request and don't change it afterwards. */
    std::function<void()> onLoadFinished;

    //==============================================================================
    using Options = KitLoadOptions;

    /** Loads a kit synchronously on the calling thread. Returns null and fills
        error on failure. planOut, if given, receives the decode plan. */
    static std::unique_ptr<Kit> loadKit (const juce::File& folder, const MidiMap&, KitInfo& info,
                                         juce::String& error, const Options& = {},
                                         std::shared_ptr<const LoadPlan>* planOut = nullptr);

    /** Decodes the audio of the mics in micMask into new stores. */
    static bool decode (const LoadPlan&, MicMask, std::array<std::unique_ptr<MicStore>, maxMics>& stores,
                        juce::String& error, const Options& = {});

private:
    void run() override;
    void loadRequested (const Request&, std::uint64_t myGeneration);
    bool updateMics();

    KitHandoff& handoff;

    std::mutex mutex;                       // guards request and result (never used by the audio thread)
    std::optional<Request> request;
    std::optional<Result> result;

    std::atomic<std::uint64_t> generation { 0 };
    std::atomic<bool> loading { false };
    std::atomic<float> progress { 0.0f };
    std::atomic<LoadPhase> phase { LoadPhase::reading };
    std::atomic<MicMask> wantedMics { allMics };
    std::atomic<std::size_t> memoryUsage { 0 };
    juce::WaitableEvent wakeUp;

    // Loader thread only: the kit published last, and which of its mics are in RAM.
    std::shared_ptr<const LoadPlan> currentPlan;
    std::uint64_t currentSerial = 0;
    MicMask loadedMics = 0;
};

} // namespace lithium
