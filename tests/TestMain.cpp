// Headless tests for the engine, loader, processor and state.
//
//   LithiumDrumsTests                 run all tests
//   LithiumDrumsTests --snapshot f.png [--scale 1.5] [--select <piece id>] [--kit <folder>] [--click <button>]
//                                     [--mixer-channel piece:<n> | mic:<n> | reverb]
//                                     render the editor to an image (needs a display, e.g. xvfb-run)
//   LithiumDrumsTests --measure-kit <kit folder> [--stream] [--preload <frames>] [--stress <seconds> [--full-mix]] [--cold]
//                                     load a real kit, report its RAM use and play every mapped note.
//                                     --stream streams it from the stream cache (building the cache if needed),
//                                     --stress then plays a dense groove in real time and counts dropouts,
//                                     --full-mix runs it with EQ, compression and a reverb send on every mic,
//                                     --cold first drops the cache file from the OS page cache (Linux)

#include "AllocationCounter.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "engine/DrumEngine.h"
#include "engine/KitLoader.h"
#include "engine/Mixer.h"
#include "engine/StreamCache.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <new>
#include <thread>

#if JUCE_LINUX
 #include <fcntl.h>
 #include <unistd.h>
#endif

//==============================================================================
// Replacing the global operator new lets the tests count heap allocations made
// on a thread while it's inside a "no allocations allowed" scope, which is how
// the real-time safety test checks processBlock().

namespace lithium::test
{
    thread_local bool countingAllocations = false;
    std::atomic<int> allocationCount { 0 };
}

void* operator new (std::size_t size)
{
    if (lithium::test::countingAllocations)
        ++lithium::test::allocationCount;

    if (auto* p = std::malloc (size == 0 ? 1 : size))
        return p;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)                  { return operator new (size); }
void operator delete (void* p) noexcept                   { std::free (p); }
void operator delete[] (void* p) noexcept                 { std::free (p); }
void operator delete (void* p, std::size_t) noexcept      { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept    { std::free (p); }

//==============================================================================
// Runs the real message loop (so the kit loader's result reaches the UI),
// then renders the editor into a PNG and quits.
class SnapshotWriter final : private juce::Timer
{
public:
    SnapshotWriter (juce::File f, float s, juce::String select, const juce::File& kit, juce::String click, const juce::String& mixerChannel)
        : file (std::move (f)), scale (s), selectId (std::move (select)), clickName (std::move (click))
    {
        processor.reverbSelected = mixerChannel.isEmpty() ? false : mixerChannel == "reverb";

        if (mixerChannel.startsWith ("piece:") || mixerChannel.startsWith ("mic:"))
            processor.selectedMixerChannel = { mixerChannel.startsWith ("piece:"), mixerChannel.fromFirstOccurrenceOf (":", false, false).getIntValue() };

        if (kit != juce::File())
            processor.loadKit (kit, true);

        startTimer (50);
    }

    int result = 1;

private:
    void timerCallback() override
    {
        ++ticks;

        if (editor == nullptr && (! processor.isKitLoading() || ticks > 600))
        {
            if (const auto info = processor.getKitInfo())
                for (size_t i = 0; i < info->pieces.size(); ++i)
                    if (info->pieces[i].id == selectId)
                        processor.getSelection().select ((int) i);

            editor.reset (processor.createEditor());
            editor->setSize (juce::roundToInt (1280 * scale), juce::roundToInt (800 * scale));
            readyTick = ticks;

            // Several buttons may be given, comma-separated, and are clicked in order.
            for (const auto& name : juce::StringArray::fromTokens (clickName, ",", {}))
                if (auto* button = findButton (*editor, name))
                    button->triggerClick();

        }
        else if (editor != nullptr && ticks > readyTick + 4)
        {
            stopTimer();
            const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            file.deleteFile();
            juce::FileOutputStream out (file);

            if (! out.failedToOpen() && png.writeImageToStream (image, out))
            {
                std::cout << "Wrote " << file.getFullPathName() << std::endl;
                result = 0;
            }

            editor = nullptr;
            juce::MessageManager::getInstance()->stopDispatchLoop();
        }
    }

    static juce::Button* findButton (juce::Component& c, const juce::String& name)
    {
        if (name.isEmpty())
            return nullptr;

        if (auto* b = dynamic_cast<juce::Button*> (&c); b != nullptr && b->getName() == name)
            return b;

        for (auto* child : c.getChildren())
            if (auto* found = findButton (*child, name))
                return found;

        return nullptr;
    }

    lithium::LithiumDrumsProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::File file;
    float scale;
    juce::String selectId, clickName;
    int ticks = 0, readyTick = 0;
};

static int writeSnapshot (const juce::StringArray& args)
{
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--snapshot") + 1]);
    const auto scale = args.contains ("--scale") ? args[args.indexOf ("--scale") + 1].getFloatValue() : 1.0f;
    const auto select = args.contains ("--select") ? args[args.indexOf ("--select") + 1] : juce::String();

    const auto kit = args.contains ("--kit") ? juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--kit") + 1])
                                             : juce::File();
    const auto click = args.contains ("--click") ? args[args.indexOf ("--click") + 1] : juce::String();

    const auto mixerChannel = args.contains ("--mixer-channel") ? args[args.indexOf ("--mixer-channel") + 1] : juce::String();
    SnapshotWriter writer (file, scale, select, kit, click, mixerChannel);
    juce::MessageManager::getInstance()->runDispatchLoop();
    return writer.result;
}

//==============================================================================
// Resident memory of this process, in KB (Linux only; -1 elsewhere).
static long residentKb()
{
    const auto status = juce::File ("/proc/self/status").loadFileAsString();
    const auto line = status.fromFirstOccurrenceOf ("VmRSS:", false, false).upToFirstOccurrenceOf ("\n", false, false);
    return line.isNotEmpty() ? line.trim().getLargeIntValue() : -1;
}

/** Plays a dense groove through the engine in real time (each block waits
    for its deadline, as an audio callback would) and reports what the disk
    streaming did. Every hit is at least velocity 90, there's a crash or china
    on every beat and a tom fill every other bar, so dozens of long cymbal
    tails stream at once. */
static int stressKit (lithium::Kit& kit, const lithium::KitInfo& info, lithium::StreamStats& stats, double seconds, bool fullMix)
{
    using namespace lithium;

    // The first (or first "open") articulation of the nth piece of a role.
    auto articulation = [&] (PieceRole role, int nth, const char* named = nullptr)
    {
        for (size_t p = 0; p < info.pieces.size(); ++p)
        {
            if (info.pieces[p].role != role || nth-- > 0)
                continue;

            const auto& pd = kit.pieces[p];

            for (int a = 0; named != nullptr && a < pd.numArticulations; ++a)
                if (info.pieces[p].articulations[(size_t) a].name.containsIgnoreCase (named))
                    return pd.firstArticulation + a;

            return pd.firstArticulation;
        }

        return -1;
    };

    const auto kick = articulation (PieceRole::kick, 0), snare = articulation (PieceRole::snare, 0);
    const auto hatClosed = articulation (PieceRole::hihat, 0), hatOpen = articulation (PieceRole::hihat, 0, "open");
    const auto ride = articulation (PieceRole::ride, 0), china = articulation (PieceRole::china, 0);
    const std::array<int, 2> crashes { articulation (PieceRole::crash, 0), articulation (PieceRole::crash, 1) };
    const std::array<int, 4> toms { articulation (PieceRole::tom, 0), articulation (PieceRole::tom, 1),
                                    articulation (PieceRole::floorTom, 0), articulation (PieceRole::floorTom, 1) };

    EngineFeedback feedback;
    DrumEngine engine (feedback);
    Mixer mixer;
    EngineParams params;
    params.resetForKit (kit);
    engine.prepare (kit.sampleRate);

    if (fullMix)
    {
        // A busy mix: every channel (drums, overheads, room) high-passed,
        // EQ'd, transient-shaped, compressed, saturated and sent to the
        // reverb, and every hit pitch-bent and filtered.
        auto busy = [] (ChannelParams& c)
        {
            auto& eq = c.chain.eq;
            eq.hpfHz = 60.0f;
            eq.lowDb = 2.0f;
            eq.mid1Db = -3.0f;
            eq.mid2Db = 2.5f;
            eq.highDb = 1.5f;
            c.chain.comp.enabled = true;
            c.chain.comp.thresholdDb = -24.0f;
            c.chain.comp.mix = 0.7f;
            c.chain.transient.enabled = true;
            c.chain.transient.attack = 0.4f;
            c.chain.saturation.enabled = true;
            c.reverbSend = 0.25f;
        };

        for (size_t p = 0; p < kit.pieces.size(); ++p)
        {
            busy (params.pieces[p].channel);
            params.pieces[p].pitchEnvSemitones = 3.0f;
            params.pieces[p].toneLowPassHz = 9000.0f;
        }

        for (int m = 0; m < kit.numMics; ++m)
            busy (params.mics[(size_t) m].channel);
    }

    constexpr int blockSize = 256;
    mixer.prepare (kit.sampleRate, blockSize);
    juce::AudioBuffer<float> out (2, blockSize);
    juce::Random random (1);

    const auto stepFrames = kit.sampleRate * 60.0 / 160.0 / 4.0;   // 16th notes at 160 BPM
    const auto totalBlocks = (int) (seconds * kit.sampleRate / blockSize);
    const auto blockDuration = std::chrono::duration<double> (blockSize / kit.sampleRate);
    const auto start = std::chrono::steady_clock::now();

    const auto dropoutsBefore = stats.dropouts.load(), framesBefore = stats.dropoutFrames.load();
    const auto bytesBefore = stats.bytesRead.load();
    int peakVoices = 0, peakStreams = 0, nextStep = 0, lateBlocks = 0;
    double renderSeconds = 0.0, mixSeconds = 0.0;

    auto hit = [&] (int art, int low = 90)
    {
        if (art >= 0)
            engine.trigger (kit, art, random.nextInt ({ low, 128 }), params);
    };

    // As in processBlock: denormals flushed to zero, so decaying filter and
    // reverb tails don't fall into the CPU's slow path.
    const juce::ScopedNoDenormals noDenormals;

    for (int b = 0; b < totalBlocks; ++b)
    {
        const auto t0 = std::chrono::steady_clock::now();

        while (nextStep * stepFrames < (double) (b + 1) * blockSize)
        {
            const auto step = nextStep % 16, bar = nextStep / 16;
            const auto fill = bar % 2 == 1 && step >= 8;

            if (step % 4 == 0)
                hit (step == 0 && china >= 0 ? china : crashes[(size_t) (step / 4) % 2] >= 0 ? crashes[(size_t) (step / 4) % 2] : crashes[0], 100);

            if (step % 2 == 0)
                hit (kick);

            if (step == 4 || step == 12)
                hit (snare, 110);

            if (fill)
                hit (toms[(size_t) (step % 4)]);
            else
                hit (step == 14 ? hatOpen : hatClosed);

            if (step % 2 == 1)
                hit (ride);

            ++nextStep;
        }

        mixer.clearBuses (blockSize);
        engine.render (kit, mixer.getBuses(), 0, blockSize, params);
        const auto tMix = std::chrono::steady_clock::now();
        mixer.process (kit, blockSize, params, { out.getWritePointer (0), out.getWritePointer (1) }, {});
        mixSeconds += std::chrono::duration<double> (std::chrono::steady_clock::now() - tMix).count();

        peakVoices = std::max (peakVoices, engine.getNumActiveVoices());
        peakStreams = std::max (peakStreams, stats.activeStreams.load());

        const auto t1 = std::chrono::steady_clock::now();
        renderSeconds += std::chrono::duration<double> (t1 - t0).count();

        const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration> (blockDuration * (b + 1));

        if (t1 > deadline)
            ++lateBlocks;
        else
            std::this_thread::sleep_until (deadline);
    }

    engine.stopAll (kit);

    const auto dropouts = stats.dropouts.load() - dropoutsBefore;
    const auto mbRead = (double) (stats.bytesRead.load() - bytesBefore) / (1024.0 * 1024.0);

    std::cout << "  stress, " << juce::String (seconds, 0) << " s real time: peak " << peakVoices << " voices, "
              << peakStreams << " streams; read " << juce::String (mbRead, 0) << " MB ("
              << juce::String (mbRead / seconds, 1) << " MB/s); engine load "
              << juce::String (100.0 * renderSeconds / seconds, 1) << "% (mixer " << juce::String (100.0 * mixSeconds / seconds, 1)
              << "%" << (fullMix ? ", full mix" : "") << "); "
              << (juce::int64) dropouts << " dropouts (" << (juce::int64) (stats.dropoutFrames.load() - framesBefore)
              << " frames), " << lateBlocks << " late blocks, " << (juce::int64) stats.readErrors.load() << " read errors" << std::endl;

    return dropouts == 0 ? 0 : 1;
}

static int measureKit (const juce::File& folder, const juce::StringArray& args)
{
    using namespace lithium;

    auto option = [&] (const char* name) { return args.contains (name) ? args[args.indexOf (name) + 1] : juce::String(); };

    KitLoader::Options options;
    options.streamStats = std::make_shared<StreamStats>();

    if (args.contains ("--stream"))
    {
        options.streaming.mode = StreamingMode::always;
        options.streaming.cacheFolder = juce::SharedResourcePointer<Settings>()->getStreaming().cacheFolder;
        options.streaming.preloadFrames = option ("--preload").isNotEmpty() ? option ("--preload").getIntValue()
                                                                             : Settings::defaultPreloadFrames;
    }

    const auto rssBefore = residentKb();
    KitInfo info;
    juce::String error;
    auto kit = KitLoader::loadKit (folder, MidiMap::generalMidi(), info, error, options);

    if (kit == nullptr)
    {
        std::cerr << "Failed: " << error << std::endl;
        return 1;
    }

    const auto rssAfter = residentKb();
    const auto mb = [] (double bytes) { return juce::String (bytes / (1024.0 * 1024.0), 0) + " MB"; };

    std::cout << info.credits.name << ": " << info.pieces.size() << " pieces, " << info.mics.size() << " mics, "
              << info.numSampleFiles << " files, " << kit->getNumHits() << " hits\n"
              << "  sample data in RAM: " << mb ((double) info.memoryBytes) << " (" << info.bitsPerSample << "-bit)\n"
              << "  process RAM grew by: " << mb ((double) (rssAfter - rssBefore) * 1024.0) << "\n"
              << "  load time: " << juce::String (info.loadSeconds, 1) << " s" << std::endl;

    if (info.streaming)
        std::cout << "  streaming: preload " << info.preloadFrames << " frames; "
                  << mb ((double) (info.memoryBytes - info.streamBufferBytes)) << " of preloads + "
                  << mb ((double) info.streamBufferBytes) << " of stream buffers, instead of "
                  << mb ((double) info.fullMemoryBytes) << " fully in RAM\n"
                  << "  stream cache: " << mb ((double) streamcache::getCacheSize (info.streamCache)) << " in "
                  << info.streamCache.getFullPathName() << std::endl;

    for (const auto& w : info.warnings)
        std::cout << "  warning: " << w << std::endl;

    // Play every mapped note at three velocities through the engine and mixer.
    // For this report every mic gets a bus channel of its own (rather than
    // the close mics feeding the drums' channels), so each can be checked.
    for (int m = 0; m < kit->numMics; ++m)
        kit->mics[(size_t) m].direct = false;

    EngineFeedback feedback;
    DrumEngine engine (feedback);
    Mixer mixer;
    EngineParams params;
    params.resetForKit (*kit);
    engine.prepare (kit->sampleRate);
    mixer.prepare (kit->sampleRate, 512);

    juce::AudioBuffer<float> out (2, 512);
    std::vector<float> micPeaks ((size_t) kit->numBusChannels, 0.0f);

    auto render = [&] (int blocks)
    {
        float peak = 0.0f;

        for (int b = 0; b < blocks; ++b)
        {
            out.clear();
            mixer.clearBuses (512);
            engine.render (*kit, mixer.getBuses(), 0, 512, params);

            for (int ch = 0; ch < kit->numBusChannels; ++ch)
                micPeaks[(size_t) ch] = std::max (micPeaks[(size_t) ch], mixer.getMicBus().getMagnitude (ch, 0, 512));

            mixer.process (*kit, 512, params, { out.getWritePointer (0), out.getWritePointer (1) }, {});
            peak = std::max (peak, out.getMagnitude (0, 512));
        }

        return peak;
    };

    int mapped = 0, silent = 0;

    for (int note = 0; note < 128; ++note)
    {
        const auto target = kit->noteMap[(size_t) note].articulation;

        if (target < 0)
            continue;

        ++mapped;
        juce::String levels;

        for (const auto velocity : { 20, 70, 127 })
        {
            engine.noteOn (*kit, note, velocity, params);
            const auto peak = render (12);
            engine.stopAll (*kit);
            levels << " " << juce::String (juce::Decibels::gainToDecibels (peak), 1);

            if (peak < 1.0e-4f)
                ++silent;
        }

        const auto& art = kit->articulations[(size_t) target];
        std::cout << "  note " << note << " -> " << info.pieces[(size_t) art.piece].name << " / "
                  << info.pieces[(size_t) art.piece].articulations[(size_t) (target - kit->pieces[(size_t) art.piece].firstArticulation)].name
                  << ": peak dB at velocity 20/70/127:" << levels << std::endl;
    }

    std::cout << "  " << mapped << " mapped notes, " << silent << " silent plays" << std::endl;

    for (size_t m = 0; m < info.mics.size(); ++m)
    {
        const auto& mic = kit->mics[m];
        float p = 0.0f;

        for (int ch = 0; ch < mic.numChannels; ++ch)
            p = std::max (p, micPeaks[(size_t) (mic.busChannel + ch)]);

        if (p < 1.0e-4f)
        {
            std::cout << "  mic " << info.mics[m].name << " was silent" << std::endl;
            ++silent;
        }
    }

    // Hi-hat choke: an articulation that's choked, then one that chokes it.
    // Crossfading is off here, so each hit is exactly one voice.
    params.velocityCrossfade = false;

    for (int open = 0; open < (int) kit->articulations.size(); ++open)
    {
        const auto group = kit->articulations[(size_t) open].chokeGroupMask;

        if (group == 0)
            continue;

        for (int closer = 0; closer < (int) kit->articulations.size(); ++closer)
        {
            if ((kit->articulations[(size_t) closer].chokesMask & group) == 0)
                continue;

            engine.trigger (*kit, open, 110, params);
            render (20);
            const auto ringing = engine.getNumActiveVoices();
            engine.trigger (*kit, closer, 60, params);
            render (8);    // ~90 ms: the 30 ms choke fade has finished
            const auto after = engine.getNumActiveVoices();
            engine.stopAll (*kit);

            std::cout << "  choke: " << ringing << " ringing voice(s), " << after << " after the closing hit"
                      << (ringing >= 1 && after <= 1 ? " (choked)" : " (NOT choked)") << std::endl;

            if (! (ringing >= 1 && after <= 1))
                ++silent;

            open = (int) kit->articulations.size();
            break;
        }
    }

    auto result = silent == 0 ? 0 : 1;

    if (const auto seconds = option ("--stress").getDoubleValue(); seconds > 0.0)
    {
       #if JUCE_LINUX
        // Ask the kernel to forget the cache file's pages, so every read
        // really goes to the disk. No root needed: it only drops clean pages.
        if (args.contains ("--cold") && info.streaming)
        {
            const auto data = info.streamCache.getChildFile ("audio.bin").getFullPathName();

            if (const auto fd = ::open (data.toRawUTF8(), O_RDONLY); fd >= 0)
            {
                ::posix_fadvise (fd, 0, 0, POSIX_FADV_DONTNEED);
                ::close (fd);
                std::cout << "  dropped the stream cache from the page cache" << std::endl;
            }
        }
       #endif

        result |= stressKit (*kit, info, *options.streamStats, seconds, args.contains ("--full-mix"));
    }

    return result;
}

int main (int argc, char* argv[])
{
    // Sets up JUCE's message manager and everything the GUI classes need,
    // and shuts it down again at the end of main.
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    if (args.contains ("--snapshot"))
        return writeSnapshot (args);

    if (args.contains ("--measure-kit"))
        return measureKit (juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--measure-kit") + 1]), args);

    // The tests use a kits folder holding only the synthetic test kit, so a
    // new processor loads that rather than whatever real kits are installed.
    // Likewise the stream cache goes in the build folder, starting empty so
    // the tests build it, and kits stay in RAM unless a test asks otherwise,
    // whatever the user's own settings say.
    const auto testCache = juce::File (LITHIUM_TEST_KITS_FOLDER).getSiblingFile ("test-cache");
    testCache.deleteRecursively();

    lithium::test::setEnvironment ("LITHIUM_KITS_FOLDER", LITHIUM_TEST_KITS_FOLDER);
    lithium::test::setEnvironment ("LITHIUM_CACHE_FOLDER", testCache.getFullPathName());
    lithium::test::setEnvironment ("LITHIUM_STREAMING", "never");

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runTestsInCategory ("Lithium");

    int failures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult (i)->failures;

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
