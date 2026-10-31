// Headless tests for the engine, loader, processor and state.
//
//   LithiumDrumsTests                 run all tests
//   LithiumDrumsTests --snapshot f.png [--scale 1.5] [--select <piece id>] [--kit <folder>] [--click <button>]
//                                     render the editor to an image (needs a display, e.g. xvfb-run)
//   LithiumDrumsTests --measure-kit <kit folder>
//                                     load a real kit, report its RAM use and play every mapped note

#include "AllocationCounter.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "engine/DrumEngine.h"
#include "engine/KitLoader.h"
#include "engine/Mixer.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdlib>
#include <stdlib.h>   // setenv / _putenv_s
#include <iostream>
#include <optional>
#include <new>

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
    SnapshotWriter (juce::File f, float s, juce::String select, const juce::File& kit, juce::String click)
        : file (std::move (f)), scale (s), selectId (std::move (select)), clickName (std::move (click))
    {
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

            if (auto* button = findButton (*editor, clickName))
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

    SnapshotWriter writer (file, scale, select, kit, click);
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

static int measureKit (const juce::File& folder)
{
    using namespace lithium;

    const auto rssBefore = residentKb();
    KitInfo info;
    juce::String error;
    auto kit = KitLoader::loadKit (folder, MidiMap::generalMidi(), info, error);

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

    for (const auto& w : info.warnings)
        std::cout << "  warning: " << w << std::endl;

    // Play every mapped note at three velocities through the engine and mixer.
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
            mixer.clearMicBus (512);
            engine.render (*kit, mixer.getMicBus(), 0, 512, params);

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
                  << info.pieces[(size_t) art.piece].articulations[target - kit->pieces[(size_t) art.piece].firstArticulation]
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

    return silent == 0 ? 0 : 1;
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
        return measureKit (juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--measure-kit") + 1]));

    // The tests use a kits folder holding only the synthetic test kit, so a
    // new processor loads that rather than whatever real kits are installed.
   #if JUCE_WINDOWS
    _putenv_s ("LITHIUM_KITS_FOLDER", LITHIUM_TEST_KITS_FOLDER);
   #else
    setenv ("LITHIUM_KITS_FOLDER", LITHIUM_TEST_KITS_FOLDER, 1);
   #endif

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runTestsInCategory ("Lithium");

    int failures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult (i)->failures;

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
