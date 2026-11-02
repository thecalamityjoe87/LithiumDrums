// Headless tests for the engine, loader, processor and state.
//
//   LithiumDrumsTests                 run all tests
//   LithiumDrumsTests --only <words>  run the tests whose name contains them, e.g. --only Groove
//   LithiumDrumsTests --snapshot f.png [--scale 1.5] [--select <piece id>] [--kit <folder>] [--click <button>]
//                                     [--mixer-channel piece:<n> | mic:<n> | reverb]
//                                     [--swap <piece or slot-id>=<kit id>/<piece id>,...]
//                                     render the editor to an image (needs a display, e.g. xvfb-run)
//   LithiumDrumsTests --sfz-dump <file.sfz> [n]
//                                     parse an SFZ instrument and print its first n regions (default 5),
//                                     its controller defaults and warnings: for writing import mappings
//   LithiumDrumsTests --midi-audit [--no-play]
//                                     check every installed kit and preset against every built-in MIDI map:
//                                     what each map reaches, what only spare notes reach, and (unless
//                                     --no-play) that every articulation makes a sound
//   LithiumDrumsTests --swap-audit
//                                     swap every installed kit's pieces into every other kit and report where the
//                                     mics go and anything left silent; then play real swaps (needs DRSKit Full,
//                                     MuldjordKit and CrocellKit)
//   LithiumDrumsTests --groove-audit
//                                     resolve every installed groove against every installed kit and preset:
//                                     hits a kit can't play, how the hi-hat spreads over its openness steps,
//                                     and that dragged MIDI plays the same hits in each built-in map
//   LithiumDrumsTests --measure-kit <kit folder> [--map <map id>] [--stream] [--preload <frames>] [--stress <seconds> [--full-mix]] [--cold]
//                                     load a real kit, report its RAM use and play every mapped note.
//                                     --stream streams it from the stream cache (building the cache if needed),
//                                     --stress then plays a dense groove in real time and counts dropouts,
//                                     --full-mix runs it with EQ, compression and a reverb send on every mic,
//                                     --cold first drops the cache file from the OS page cache (Linux),
//                                     --map plays it through another MIDI map (roland-vdrums, alesis, ...),
//                                     and then sweeps the hi-hat pedal and snare position
//   LithiumDrumsTests --idle-check [seconds] [--replay <aseqdump log>] [--kit <folder>]
//                                     restore the standalone app's saved state (with its editor open; needs a
//                                     display), play four kicks (or replay MIDI captured with
//                                     `aseqdump -p <port> | ts`-style timestamped lines, as fast as it renders),
//                                     then run in real time with no MIDI and report every hit the engine plays
//                                     and every sound that starts on its own

#include "AllocationCounter.h"
#include "KitLibrary.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "engine/DrumEngine.h"
#include "importer/SfzParser.h"
#include "engine/KitLoader.h"
#include "engine/Mixer.h"
#include "engine/StreamCache.h"
#include "engine/MidiMapping.h"
#include "grooves/GrooveMidi.h"

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

// operator new above allocates with malloc, so free is the right match. GCC
// can't see that through the replacement and warns that it's mismatched.
#if defined (__GNUC__) && ! defined (__clang__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete (void* p) noexcept                   { std::free (p); }
void operator delete[] (void* p) noexcept                 { std::free (p); }
void operator delete (void* p, std::size_t) noexcept      { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept    { std::free (p); }
#if defined (__GNUC__) && ! defined (__clang__)
 #pragma GCC diagnostic pop
#endif

//==============================================================================
// Runs the real message loop (so the kit loader's result reaches the UI),
// then renders the editor into a PNG and quits.
class SnapshotWriter final : private juce::Timer
{
public:
    SnapshotWriter (juce::File f, float s, juce::String select, const juce::File& kit, juce::String click, const juce::String& mixerChannel,
                    const juce::String& swaps)
        : file (std::move (f)), scale (s), selectId (std::move (select)), clickName (std::move (click))
    {
        processor.reverbSelected = mixerChannel.isEmpty() ? false : mixerChannel == "reverb";

        if (mixerChannel.startsWith ("piece:") || mixerChannel.startsWith ("mic:"))
            processor.selectedMixerChannel = { mixerChannel.startsWith ("piece:"), mixerChannel.fromFirstOccurrenceOf (":", false, false).getIntValue() };

        if (kit != juce::File())
            processor.loadKit (kit, true);

        // Pieces from other kits: "snare=muldjordkit/snare,slot-tom3=crocellkit-full/tom1".
        for (const auto& swap : juce::StringArray::fromTokens (swaps, ",", {}))
            processor.setPieceSwap ({ swap.upToFirstOccurrenceOf ("=", false, false),
                                      swap.fromFirstOccurrenceOf ("=", false, false).upToFirstOccurrenceOf ("/", false, false),
                                      swap.fromFirstOccurrenceOf ("/", false, false) });

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

//==============================================================================
/** --swap-audit: every installed kit's pieces swapped into every other kit
    (on the manifests: which mics go where, and whether anything is left to
    hear), then a few real swaps loaded and played through the processor. */
static int swapAudit()
{
    using namespace lithium;

    const auto library = kitlibrary::scan (juce::SharedResourcePointer<Settings>()->getKitsFolder());
    std::vector<std::pair<juce::String, KitManifest>> kits;

    for (const auto& e : library.kits)
    {
        if (e.summary.isPreset)
            continue;

        KitManifest m;

        if (KitManifest::parse (e.folder, m).wasOk())
            kits.emplace_back (e.summary.id, std::move (m));
    }

    int swapsTried = 0, silent = 0;

    for (const auto& [hostId, host] : kits)
    {
        for (const auto& [sourceId, source] : kits)
        {
            if (hostId == sourceId)
                continue;

            for (size_t t = 0; t < host.pieces.size(); ++t)
            {
                for (size_t s = 0; s < source.pieces.size(); ++s)
                {
                    if (! swaps::canReplace (host.pieces[t].role, source.pieces[s].role))
                        continue;

                    ++swapsTried;
                    const auto map = swaps::mapMics (source, (int) s, host, (int) t, true);
                    const auto kept = std::count_if (map.begin(), map.end(), [] (int m) { return m >= 0; });

                    if (kept == 0)
                    {
                        ++silent;
                        std::cout << "  silent: " << source.pieces[s].name << " (" << sourceId << ") in " << hostId << "'s "
                                  << host.pieces[t].name << std::endl;
                    }
                }
            }

            // One line per pair: where the snare's mics go.
            const auto ts = host.findPiece ("snare"), ss = source.findPiece ("snare");

            if (ts >= 0 && ss >= 0)
            {
                juce::StringArray parts;
                const auto map = swaps::mapMics (source, ss, host, ts, true);

                for (size_t m = 0; m < map.size(); ++m)
                    parts.add (source.mics[m].id + (map[m] >= 0 ? "->" + host.mics[(size_t) map[m]].id : juce::String ("->(dropped)")));

                std::cout << sourceId << " snare in " << hostId << ": " << parts.joinIntoString (", ") << std::endl;
            }
        }
    }

    std::cout << swapsTried << " possible swaps, " << silent << " with nothing left to hear\n" << std::endl;

    // Real swaps, played: MuldjordKit's snare and CrocellKit's (48 kHz) kick in DRSKit Full.
    int failures = 0;
    const auto* drs = library.findById ("drskit-full");

    if (drs == nullptr || library.findById ("muldjordkit") == nullptr || library.findById ("crocellkit-full") == nullptr)
    {
        std::cout << "DRSKit Full, MuldjordKit and CrocellKit are needed for the playback check; skipped." << std::endl;
        return 0;
    }

    LithiumDrumsProcessor processor;
    processor.loadKit (drs->folder, true);
    processor.setPieceSwap ({ "snare", "muldjordkit", "snare", true });
    processor.setPieceSwap ({ "kick", "crocellkit-full", "kick", true });

    juce::AudioBuffer<float> buffer (processor.getTotalNumOutputChannels(), 512);
    processor.prepareToPlay (44100.0, 512);

    for (int i = 0; i < 6000 && (processor.isKitLoading() || processor.getKitInfo() == nullptr); ++i)
    {
        processor.applyPendingKitLoad();
        juce::MidiBuffer none;
        processor.processBlock (buffer, none);
        juce::Thread::sleep (10);
    }

    const auto info = processor.getKitInfo();

    if (info == nullptr)
    {
        std::cout << "The kit didn't load: " << processor.getKitError() << std::endl;
        return 1;
    }

    for (const auto* id : { "snare", "kick" })
    {
        for (size_t p = 0; p < info->pieces.size(); ++p)
        {
            if (info->pieces[p].id != id)
                continue;

            std::cout << id << ": from " << info->pieces[p].swappedFrom << " (" << info->pieces[p].swappedPiece << ")";

            for (const auto& note : info->pieces[p].swapNotes)
                std::cout << "; " << note;

            // Play it a few times, and measure the output.
            float peak = 0.0f;

            for (int block = 0; block < 40; ++block)
            {
                juce::MidiBuffer midi;

                if (block % 10 == 0)
                    processor.audition ((int) p, 110);

                processor.processBlock (buffer, midi);
                peak = std::max (peak, buffer.getMagnitude (0, 0, 512));
            }

            std::cout << ", peak " << juce::Decibels::gainToDecibels (peak) << " dB" << std::endl;
            failures += peak > 0.001f ? 0 : 1;
        }
    }

    return failures == 0 ? 0 : 1;
}

//==============================================================================
static int grooveAudit()
{
    using namespace lithium;

    const juce::SharedResourcePointer<Settings> settings;
    const auto kits = kitlibrary::scan (settings->getKitsFolder());
    const auto grooveLibrary = grooves::scan (Settings::getGroovesFolder());
    int problems = 0;

    // Every take, read once.
    struct Take
    {
        grooves::Clip clip;
        juce::String noteMap;
    };

    std::vector<Take> takes;

    for (const auto& pack : grooveLibrary.packs)
    {
        for (const auto& g : pack.grooves)
        {
            std::vector<grooves::Hit> hits;

            if (const auto r = grooves::readHits (g.file, pack.pedalController, pack.pedalClosedAt, hits); r.failed())
            {
                std::cout << "Can't read " << g.name << ": " << r.getErrorMessage() << std::endl;
                ++problems;
                continue;
            }

            takes.push_back ({ grooves::makeClip (g, hits, 0, g.bars, g.name), pack.noteMap });

            for (const auto& f : g.fills)
                if (grooves::readHits (f.file, pack.pedalController, pack.pedalClosedAt, hits).wasOk())
                    takes.push_back ({ grooves::makeClip (g, hits, 0, f.bars, g.name + " " + f.name), pack.noteMap });
        }
    }

    std::cout << "Groove audit: " << takes.size() << " takes against " << kits.kits.size() << " kits and presets" << std::endl;

    for (const auto& entry : kits.kits)
    {
        KitLoader::Options options;
        options.streaming = settings->getStreaming();
        options.streamStats = std::make_shared<StreamStats>();

        KitInfo info;
        juce::String error;

        if (KitLoader::loadKit (entry.folder, MidiMap::kitDefault(), info, error, options) == nullptr)
        {
            std::cout << entry.credits().name << ": FAILED TO LOAD: " << error << std::endl;
            ++problems;
            continue;
        }

        std::size_t recorded = 0, played = 0, mismatched = 0;
        std::map<juce::String, int> hats;
        std::vector<std::pair<MidiMap, NoteTable>> maps;

        for (const auto& m : MidiMap::factoryMaps())
            maps.push_back ({ m, resolveMidiMap (m, info) });

        for (const auto& take : takes)
        {
            const auto packMap = MidiMap::factoryMap (take.noteMap);
            const auto resolved = grooves::resolve (take.clip, packMap, info);
            recorded += take.clip.hits.size();
            played += resolved.size();

            for (const auto& h : resolved)
            {
                const auto [p, a] = pieceAndArticulation (info, h.articulation);

                if (info.pieces[(size_t) p].role == PieceRole::hihat)
                    ++hats[info.pieces[(size_t) p].articulations[(size_t) a].id];
            }

            // Dragged MIDI, in each map, plays what the preview played.
            for (const auto& [map, table] : maps)
            {
                const auto midi = grooves::toMidiFile (take.clip, packMap, &info, table);
                size_t i = 0;

                for (const auto* e : *midi.getTrack (0))
                {
                    if (! e->message.isNoteOn())
                        continue;

                    if (i >= resolved.size() || table.targets[(size_t) e->message.getNoteNumber()].articulation != resolved[i].articulation)
                        ++mismatched;

                    ++i;
                }

                mismatched += i < resolved.size() ? resolved.size() - i : 0;
            }
        }

        juce::String hatText;

        for (const auto& [id, n] : hats)
            hatText << id << " " << juce::String (100.0 * n / std::max<std::size_t> (1, played), 1) << "%  ";

        std::cout << "  " << entry.credits().name.paddedRight (' ', 28) << " plays " << juce::String (100.0 * (double) played / (double) std::max<std::size_t> (1, recorded), 2)
                  << "% of hits" << (mismatched > 0 ? "  EXPORT MISMATCHES: " + juce::String ((int) mismatched) : juce::String())
                  << "\n      hats: " << hatText << std::endl;

        problems += mismatched > 0 ? 1 : 0;
    }

    std::cout << (problems == 0 ? "GROOVE AUDIT PASSED" : "GROOVE AUDIT FOUND PROBLEMS") << std::endl;
    return problems == 0 ? 0 : 1;
}

static int sfzDump (const juce::StringArray& args)
{
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--sfz-dump") + 1]);
    const auto count = juce::jmax (1, args[args.indexOf ("--sfz-dump") + 2].getIntValue() > 0 ? args[args.indexOf ("--sfz-dump") + 2].getIntValue() : 5);

    lithium::sfz::Instrument instrument;

    if (const auto r = lithium::sfz::parse (file, instrument); r.failed())
    {
        std::cerr << r.getErrorMessage() << std::endl;
        return 1;
    }

    std::cout << instrument.regions.size() << " regions" << std::endl;

    for (const auto& [cc, value] : instrument.controllerDefaults)
        std::cout << "  set_cc" << cc << " = " << value << std::endl;

    for (size_t i = 0; i < instrument.regions.size() && i < (size_t) count; ++i)
    {
        const auto& r = instrument.regions[i];
        std::cout << "region " << i << " (" << r.where << ") sample " << r.sample
                  << (r.sampleFile.existsAsFile() ? "" : "  [MISSING]") << std::endl;

        for (const auto& [name, value] : r.opcodes)
            std::cout << "    " << name << "=" << value << std::endl;
    }

    for (const auto& w : instrument.warnings)
        std::cout << "warning: " << w << std::endl;

    return 0;
}

static int midiAudit (const juce::StringArray& args)
{
    using namespace lithium;

    const juce::SharedResourcePointer<Settings> settings;
    const auto library = kitlibrary::scan (settings->getKitsFolder());
    const auto maps = MidiMap::factoryMaps();
    const auto play = ! args.contains ("--no-play");
    int problems = 0;

    std::cout << "MIDI audit of " << library.kits.size() << " kits and presets in "
              << settings->getKitsFolder().getFullPathName() << std::endl;

    for (const auto& entry : library.kits)
    {
        KitLoader::Options options;
        options.streaming = settings->getStreaming();
        options.streamStats = std::make_shared<StreamStats>();

        KitInfo info;
        juce::String error;
        auto kit = KitLoader::loadKit (entry.folder, MidiMap::kitDefault(), info, error, options);

        if (kit == nullptr)
        {
            std::cout << "\n" << entry.credits().name << ": FAILED TO LOAD: " << error << std::endl;
            ++problems;
            continue;
        }

        const auto numArticulations = (int) kit->articulations.size();
        std::cout << "\n" << info.credits.name << (info.isPreset ? " (preset)" : "") << ": "
                  << info.pieces.size() << " pieces, " << numArticulations << " articulations" << std::endl;

        auto name = [&] (int a)
        {
            const auto [p, local] = pieceAndArticulation (info, a);
            return info.pieces[(size_t) p].name + " " + info.pieces[(size_t) p].articulations[(size_t) local].name;
        };

        for (const auto& map : maps)
        {
            const auto table = resolveMidiMap (map, info);
            std::vector<bool> byMap ((size_t) numArticulations), bySpare ((size_t) numArticulations);
            std::vector<bool> pieceByMap (info.pieces.size());
            juce::StringArray spareNotes;

            for (int n = 0; n < 128; ++n)
            {
                const auto& t = table.targets[(size_t) n];

                if (t.articulation < 0)
                    continue;

                (t.spare ? bySpare : byMap)[(size_t) t.articulation] = true;

                if (! t.spare)
                    pieceByMap[(size_t) pieceAndArticulation (info, t.articulation).first] = true;
                else
                    spareNotes.add (juce::String (n));
            }

            juce::StringArray unreached, piecesOnlySpare;

            for (int a = 0; a < numArticulations; ++a)
                if (! byMap[(size_t) a] && ! bySpare[(size_t) a])
                    unreached.add (name (a));

            for (size_t p = 0; p < info.pieces.size(); ++p)
                if (! pieceByMap[p])
                    piecesOnlySpare.add (info.pieces[p].name);

            const auto reached = (int) std::count (byMap.begin(), byMap.end(), true);
            std::cout << "  " << map.name.paddedRight (' ', 15) << " the map plays " << reached << "/" << numArticulations
                      << " articulations, spare notes " << spareNotes.size()
                      << (spareNotes.isEmpty() ? juce::String() : " (" + spareNotes[0] + "-" + spareNotes[spareNotes.size() - 1] + ")");

            if (! piecesOnlySpare.isEmpty())
                std::cout << "; only on spare notes: " << piecesOnlySpare.joinIntoString (", ");

            std::cout << std::endl;

            if (! unreached.isEmpty())
            {
                std::cout << "    UNREACHABLE: " << unreached.joinIntoString (", ") << std::endl;
                ++problems;
            }
        }

        if (! play)
            continue;

        // Every articulation, once, at a medium velocity: anything silent is
        // a broken recording or channel map, whatever the MIDI map.
        for (int m = 0; m < kit->numMics; ++m)
            kit->mics[(size_t) m].direct = false;

        EngineFeedback feedback;
        DrumEngine engine (feedback);
        EngineParams params;
        params.resetForKit (*kit);
        params.velocityCrossfade = false;
        engine.prepare (kit->sampleRate);

        juce::AudioBuffer<float> bus (maxMicBusChannels, 512), pieceBus (maxPieces * 2, 512);
        juce::StringArray silent;

        for (int a = 0; a < numArticulations; ++a)
        {
            engine.trigger (*kit, a, 100, params);
            float peak = 0.0f;

            for (int block = 0; block < 12; ++block)
            {
                bus.clear();
                pieceBus.clear();
                engine.render (*kit, { bus, pieceBus }, 0, 512, params);

                for (int ch = 0; ch < kit->numBusChannels; ++ch)
                    peak = std::max (peak, bus.getMagnitude (ch, 0, 512));
            }

            engine.stopAll (*kit);

            if (peak < 1.0e-4f)
                silent.add (name (a));
        }

        if (silent.isEmpty())
        {
            std::cout << "  every articulation makes a sound" << std::endl;
        }
        else
        {
            std::cout << "  SILENT: " << silent.joinIntoString (", ") << std::endl;
            ++problems;
        }
    }

    std::cout << "\n" << (problems == 0 ? "MIDI AUDIT PASSED" : "MIDI AUDIT FOUND " + juce::String (problems) + " PROBLEMS") << std::endl;
    return problems == 0 ? 0 : 1;
}

static int writeSnapshot (const juce::StringArray& args)
{
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--snapshot") + 1]);
    const auto scale = args.contains ("--scale") ? args[args.indexOf ("--scale") + 1].getFloatValue() : 1.0f;
    const auto select = args.contains ("--select") ? args[args.indexOf ("--select") + 1] : juce::String();

    const auto kit = args.contains ("--kit") ? juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--kit") + 1])
                                             : juce::File();
    const auto click = args.contains ("--click") ? args[args.indexOf ("--click") + 1] : juce::String();

    const auto mixerChannel = args.contains ("--mixer-channel") ? args[args.indexOf ("--mixer-channel") + 1] : juce::String();
    const auto swaps = args.contains ("--swap") ? args[args.indexOf ("--swap") + 1] : juce::String();
    SnapshotWriter writer (file, scale, select, kit, click, mixerChannel, swaps);
    juce::MessageManager::getInstance()->runDispatchLoop();
    return writer.result;
}

//==============================================================================
/** Looks for hits nobody played: restores the standalone app's last state,
    opens the editor (so its timers run too), plays four kicks, then runs in
    real time with no input. Any piece hit counter that moves afterwards, or
    any sound starting from silence, is reported.

    A timer drives the blocks, catching up with the wall clock on each tick,
    so the message loop (kit loading, the editor) runs as it would in the app. */
class IdleCheck final : private juce::Timer
{
public:
    IdleCheck (int secondsToRun, const juce::File& replayLog, const juce::File& kitFolder) : seconds (secondsToRun)
    {
        if (replayLog.existsAsFile())
            readReplay (replayLog);

        // The standalone keeps the plugin's state as base 64 in its settings file.
        juce::PropertiesFile::Options settings;
        settings.applicationName = "Lithium Drums";
        settings.filenameSuffix = ".settings";
        settings.osxLibrarySubFolder = "Application Support";
        settings.folderName = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getFullPathName();
        juce::PropertiesFile props (settings.getDefaultFile(), settings);
        juce::MemoryBlock state;

        if (state.fromBase64Encoding (props.getValue ("filterState")))
            processor.setStateInformation (state.getData(), (int) state.getSize());
        else
            std::cout << "No saved standalone state; using the default kit" << std::endl;

        if (kitFolder.isDirectory())
            processor.loadKit (kitFolder, true);

        processor.prepareToPlay (sampleRate, blockSize);
        editor.reset (processor.createEditor());
        editor->setVisible (true);   // not on the desktop: its timers run all the same

        start = juce::Time::getMillisecondCounterHiRes();
        startTimer (2);
    }

    int result = 1;

private:
    static constexpr double sampleRate = 44100.0;
    static constexpr int blockSize = 64;

    enum class Phase { loading, kicks, idle };

    /** Lines like "16:12:27.388  24:0   Note on   9, note 36, velocity 127":
        a wall-clock time, then aseqdump's own output (channels from 0). */
    void readReplay (const juce::File& log)
    {
        juce::StringArray lines;
        log.readLines (lines);
        std::optional<double> first;

        for (const auto& line : lines)
        {
            auto tokens = juce::StringArray::fromTokens (line.removeCharacters (","), " ", {});
            tokens.removeEmptyStrings();   // runs of spaces give empty tokens

            if (tokens.size() < 6 || ! tokens[1].containsChar (':'))
                continue;

            const auto clock = juce::StringArray::fromTokens (tokens[0], ":", {});
            const auto time = clock[0].getIntValue() * 3600.0 + clock[1].getIntValue() * 60.0 + clock[2].getDoubleValue();
            const auto channel = tokens[4].getIntValue() + 1;   // after the time, port and two-word event name
            std::optional<juce::MidiMessage> message;

            if (line.contains ("Note on"))
                message = juce::MidiMessage::noteOn (channel, tokens[tokens.indexOf ("note") + 1].getIntValue(),
                                                     (juce::uint8) tokens[tokens.indexOf ("velocity") + 1].getIntValue());
            else if (line.contains ("Note off"))
                message = juce::MidiMessage::noteOff (channel, tokens[tokens.indexOf ("note") + 1].getIntValue());
            else if (line.contains ("Control change"))
                message = juce::MidiMessage::controllerEvent (channel, tokens[tokens.indexOf ("controller") + 1].getIntValue(),
                                                              tokens[tokens.indexOf ("value") + 1].getIntValue());

            if (! message.has_value())
                continue;

            first = first.value_or (time);
            replay.push_back ({ (juce::int64) ((time - *first + 0.5) * sampleRate), *message });
        }

        // aseqdump's times come from when lines were read, so a few are out of order.
        std::stable_sort (replay.begin(), replay.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
        std::cout << "Replaying " << replay.size() << " MIDI events" << std::endl;
    }

    void timerCallback() override
    {
        auto due = (juce::int64) ((juce::Time::getMillisecondCounterHiRes() - start) * sampleRate / (1000.0 * blockSize));

        // A replay runs as fast as it renders (a few hundred blocks per tick,
        // so the message loop keeps up), and the idle time after it in real time.
        if (phase == Phase::kicks && ! replay.empty())
        {
            due = blocks + 400;
            start -= 400.0 * blockSize * 1000.0 / sampleRate;
        }

        while (blocks < due)
            if (! step())
            {
                stopTimer();
                editor.reset();
                juce::MessageManager::getInstance()->stopDispatchLoop();
                return;
            }
    }

    std::vector<std::uint32_t> hitCounts() const
    {
        std::vector<std::uint32_t> now;

        for (size_t p = 0; p < info->pieces.size(); ++p)
            now.push_back (processor.getFeedback().pieceHits[p].load());

        return now;
    }

    /** One block. False when the check is over. */
    bool step()
    {
        juce::MidiBuffer midi;
        processor.applyPendingKitLoad();

        if (phase == Phase::kicks && replay.empty() && phaseBlocks % (juce::int64) (sampleRate / blockSize) == 0)
            midi.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 100), 0);

        for (const auto blockStart = phaseBlocks * blockSize;
             phase == Phase::kicks && nextReplay < replay.size() && replay[nextReplay].first < blockStart + blockSize; ++nextReplay)
            midi.addEvent (replay[nextReplay].second, (int) juce::jmax ((juce::int64) 0, replay[nextReplay].first - blockStart));

        processor.processBlock (buffer, midi);
        const auto peak = buffer.getMagnitude (0, blockSize);
        ++blocks;
        ++phaseBlocks;

        if (phase == Phase::loading)
        {
            if (! processor.isKitLoading() && ! processor.isPresetLoading() && (info = processor.getKitInfo()) != nullptr)
            {
                std::cout << "Kit: " << info->id << ", " << info->pieces.size() << " pieces. " << (replay.empty() ? "Four kicks" : "The replay") << ", then "
                          << seconds << " s of nothing..." << std::endl;
                phase = Phase::kicks;
                phaseBlocks = 0;
            }
            else if (blocks * blockSize > 60 * sampleRate)
            {
                std::cout << "The kit didn't load" << std::endl;
                return false;
            }

            return true;
        }

        if (phase == Phase::kicks)
        {
            const auto done = replay.empty() ? phaseBlocks >= (juce::int64) (4 * sampleRate / blockSize)
                                             : nextReplay >= replay.size() && phaseBlocks * blockSize > replay.back().first + (juce::int64) sampleRate;

            if (done)
            {
                std::cout << "Input over; " << processor.getFeedback().activeVoices.load() << " voices playing" << std::endl;
                phase = Phase::idle;
                phaseBlocks = 0;
                counts = hitCounts();
            }

            return true;
        }

        const auto t = (double) phaseBlocks * blockSize / sampleRate;

        if (const auto now = hitCounts(); now != counts)
        {
            for (size_t p = 0; p < now.size(); ++p)
                if (now[p] != counts[p])
                    std::cout << juce::String (t, 2) << " s: " << info->pieces[p].name << " was hit "
                              << (int) (now[p] - counts[p]) << " time(s) with no input" << std::endl;

            counts = now;
            ++problems;
        }

        // Sound starting again after at least half a second of silence.
        if (peak > onset && quietFor >= 0.5)
        {
            std::cout << juce::String (t, 2) << " s: sound from silence, peak " << juce::String (juce::Decibels::gainToDecibels (peak), 1)
                      << " dB, " << processor.getFeedback().activeVoices.load() << " voices" << std::endl;
            ++problems;
        }

        quietFor = peak < silence ? quietFor + blockSize / sampleRate : 0.0;

        if (t > 10.0)
            loudest = std::max (loudest, peak);

        if (t < seconds)
            return true;

        std::cout << "Loudest block after the first 10 s: " << juce::String (juce::Decibels::gainToDecibels (loudest), 1) << " dB" << std::endl
                  << (problems == 0 ? "IDLE CHECK PASSED" : "IDLE CHECK FOUND " + juce::String (problems) + " PROBLEMS") << std::endl;
        result = problems == 0 ? 0 : 1;
        return false;
    }

    const int seconds;
    lithium::LithiumDrumsProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::AudioBuffer<float> buffer { processor.getTotalNumOutputChannels(), blockSize };   // (declared after the processor, so made after it)
    std::shared_ptr<const lithium::KitInfo> info;
    double start = 0.0;
    juce::int64 blocks = 0, phaseBlocks = 0;
    Phase phase = Phase::loading;
    std::vector<std::uint32_t> counts;
    std::vector<std::pair<juce::int64, juce::MidiMessage>> replay;   // by sample
    size_t nextReplay = 0;
    int problems = 0;
    double quietFor = 0.0;
    float loudest = 0.0f;
    const float silence = juce::Decibels::decibelsToGain (-80.0f);
    const float onset = juce::Decibels::decibelsToGain (-50.0f);
};

static int idleCheck (const juce::StringArray& args)
{
    const auto value = args[args.indexOf ("--idle-check") + 1].getIntValue();
    const auto file = [&] (const char* option)
    {
        return args.contains (option) ? juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf (option) + 1]) : juce::File();
    };

    IdleCheck check (value > 0 ? value : 60, file ("--replay"), file ("--kit"));
    juce::MessageManager::getInstance()->runDispatchLoop();
    return check.result;
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
    const auto map = option ("--map").isNotEmpty() ? MidiMap::factoryMap (option ("--map")) : MidiMap::kitDefault();
    auto kit = KitLoader::loadKit (folder, map, info, error, options);

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

    // The steps the pedal and position controllers move between, and the
    // cymbal hits that the kit would have choke their own cymbal.
    auto artName = [&] (int a)
    {
        const auto& p = kit->pieces[(size_t) kit->articulations[(size_t) a].piece];
        return info.pieces[(size_t) kit->articulations[(size_t) a].piece].articulations[(size_t) (a - p.firstArticulation)].name;
    };

    auto ladderText = [&] (const Ladder& l)
    {
        juce::StringArray steps;

        for (int i = 0; i < l.numSteps; ++i)
            steps.add (artName (l.articulation[(size_t) i]) + " " + juce::String (l.value[(size_t) i], 2));

        return steps.joinIntoString (", ");
    };

    std::cout << "  MIDI map: " << map.name << std::endl;

    for (size_t p = 0; p < kit->pieces.size(); ++p)
    {
        const auto& pd = kit->pieces[p];

        if (pd.hatLadders[0].numSteps > 1)
            std::cout << "  " << info.pieces[p].name << " openness (bow): " << ladderText (pd.hatLadders[0]) << "\n"
                      << "  " << info.pieces[p].name << " openness (edge): " << ladderText (pd.hatLadders[1]) << std::endl;

        if (pd.positionLadder.numSteps > 1)
            std::cout << "  " << info.pieces[p].name << " positions: " << ladderText (pd.positionLadder) << std::endl;

        for (int a = pd.firstArticulation; a < pd.firstArticulation + pd.numArticulations; ++a)
            if (kit->articulations[(size_t) a].chokesMask != 0 && kit->articulations[(size_t) a].selfChoke == SelfChoke::none)
                std::cout << "  " << info.pieces[p].name << " / " << artName (a) << " rings on (the kit chokes the cymbal with it)" << std::endl;
    }

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
        const auto target = kit->notes.targets[(size_t) note].articulation;

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

    // With a map that follows controllers: sweep the pedal under an open-hat
    // note, and the snare position under a snare note, and say what played.
    auto sweep = [&] (const char* what, int controller, int note, bool openness)
    {
        const auto& target = kit->notes.targets[(size_t) note];

        if (controller < 0 || target.articulation < 0 || target.follow == Follow::none)
            return;

        const auto piece = (size_t) kit->articulations[(size_t) target.articulation].piece;
        std::cout << "  " << what << " (note " << note << ", CC " << controller << "):";

        for (const auto v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            // CC4 runs from open (0) to closed (127); positions from centre to edge.
            engine.controlChange (*kit, controller, juce::roundToInt ((openness ? 1.0f - v : v) * 127.0f));
            engine.noteOn (*kit, note, 100, params);
            const auto voices = engine.getNumActiveVoices();
            const auto hit = LastHit::unpack (feedback.lastHit[piece].load());
            std::cout << "  " << juce::String (v, 2) << " " << (hit ? artName (hit->articulation) : juce::String ("-"))
                      << (voices > 1 ? " (+ blend)" : "");
            engine.stopAll (*kit);
        }

        std::cout << std::endl;
    };

    params.velocityCrossfade = false;
    sweep ("hi-hat pedal", kit->notes.controllers.hihatPedal, 46, true);
    sweep ("snare position", kit->notes.controllers.position[(size_t) PositionSource::snare], 38, false);

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
            const auto& c = kit->articulations[(size_t) closer];
            const auto& o = kit->articulations[(size_t) open];

            if ((c.chokesMask & group) == 0)
                continue;

            // Skip pairs that are meant to ring together (see SelfChoke).
            if (c.piece == o.piece && (c.selfChoke == SelfChoke::none || (c.selfChoke == SelfChoke::moreOpen && o.hatLevel <= c.hatLevel)))
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

    // Hi-hats: the most open step rings on when hit again, and a closed hit
    // stops both.
    for (size_t p = 0; p < kit->pieces.size(); ++p)
    {
        const auto& ladder = kit->pieces[p].hatLadders[0];

        if (kit->pieces[p].role != PieceRole::hihat || ladder.numSteps < 2)
            continue;

        const auto closed = ladder.articulation[0], open = ladder.articulation[(size_t) ladder.numSteps - 1];
        engine.trigger (*kit, open, 100, params);
        render (8);
        engine.trigger (*kit, open, 100, params);
        render (8);
        const auto together = engine.getNumActiveVoices();
        engine.trigger (*kit, closed, 80, params);
        render (8);
        const auto afterClosing = engine.getNumActiveVoices();
        engine.stopAll (*kit);

        const auto ok = together == 2 && afterClosing == 1;
        std::cout << "  hi-hat: two " << artName (open) << " hits ring together (" << together << " voices); "
                  << artName (closed) << " leaves " << afterClosing << (ok ? " (right)" : " (WRONG)") << std::endl;

        if (! ok)
            ++silent;
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

    if (args.contains ("--sfz-dump"))
        return sfzDump (args);

    if (args.contains ("--midi-audit"))
        return midiAudit (args);

    if (args.contains ("--groove-audit"))
        return grooveAudit();

    if (args.contains ("--swap-audit"))
        return swapAudit();

    if (args.contains ("--idle-check"))
        return idleCheck (args);

    if (args.contains ("--measure-kit"))
        return measureKit (juce::File::getCurrentWorkingDirectory().getChildFile (args[args.indexOf ("--measure-kit") + 1]), args);

    // The tests use a kits folder holding only the synthetic test kit, so a
    // new processor loads that rather than whatever real kits are installed.
    // Likewise the stream cache goes in the build folder, starting empty so
    // the tests build it, and kits stay in RAM unless a test asks otherwise,
    // whatever the user's own settings say. New instances start with the
    // Kit default MIDI map, and saved maps go in a folder of their own.
    const auto testCache = juce::File (LITHIUM_TEST_KITS_FOLDER).getSiblingFile ("test-cache");
    testCache.deleteRecursively();
    const auto testMaps = juce::File (LITHIUM_TEST_KITS_FOLDER).getSiblingFile ("test-midi-maps");
    testMaps.deleteRecursively();

    lithium::test::setEnvironment ("LITHIUM_KITS_FOLDER", LITHIUM_TEST_KITS_FOLDER);
    lithium::test::setEnvironment ("LITHIUM_CACHE_FOLDER", testCache.getFullPathName());
    lithium::test::setEnvironment ("LITHIUM_STREAMING", "never");
    lithium::test::setEnvironment ("LITHIUM_MIDI_MAP", "kit-default");
    lithium::test::setEnvironment ("LITHIUM_MIDI_MAPS_FOLDER", testMaps.getFullPathName());

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);

    // --only <words>: just the tests whose name contains them ("Groove").
    if (const auto only = args.indexOf ("--only"); only >= 0)
    {
        juce::Array<juce::UnitTest*> chosen;

        for (auto* t : juce::UnitTest::getTestsInCategory ("Lithium"))
            if (t->getName().containsIgnoreCase (args[only + 1]))
                chosen.add (t);

        runner.runTests (chosen);
    }
    else
    {
        runner.runTestsInCategory ("Lithium");
    }

    int failures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult (i)->failures;

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
