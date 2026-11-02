// Piece swapping (mic mapping, loading a swapped piece) and presets.

#include "AllocationCounter.h"
#include "PluginProcessor.h"
#include "engine/KitLoader.h"
#include "engine/PieceSwap.h"
#include "engine/StreamCache.h"
#include "ui/ModelPicker.h"

#include <set>

namespace lithium
{
namespace
{
    const juce::File testKitFolder { LITHIUM_TEST_KIT_DIR };

    struct MicSpec
    {
        const char* id;
        const char* role;
        int channels;
    };

    struct PieceSpec
    {
        const char* id;
        const char* role;
    };

    /** A kit.json with these mics and pieces: one articulation per piece,
        one hit, recorded on every mic. Enough to parse, not to load. */
    juce::String kitJson (const juce::String& id, std::initializer_list<MicSpec> mics, std::initializer_list<PieceSpec> pieces)
    {
        juce::String micList, channelMap;
        int channel = 1;

        for (const auto& m : mics)
        {
            micList << (micList.isEmpty() ? "" : ", ") << "{ \"id\": \"" << m.id << "\", \"name\": \"" << m.id << "\", \"channels\": "
                    << m.channels << ", \"role\": \"" << m.role << "\" }";
            channelMap << (channelMap.isEmpty() ? "" : ", ") << "\"" << m.id << "\": "
                       << (m.channels == 2 ? "[" + juce::String (channel) + ", " + juce::String (channel + 1) + "]" : juce::String (channel));
            channel += m.channels;
        }

        juce::String pieceList;

        for (const auto& p : pieces)
            pieceList << (pieceList.isEmpty() ? "" : ", ") << "{ \"id\": \"" << p.id << "\", \"name\": \"" << p.id << "\", \"role\": \""
                      << p.role << "\", \"articulations\": [ { \"id\": \"hit\", \"name\": \"Hit\", \"channelMap\": { " << channelMap
                      << " }, \"zones\": [ { \"velocity\": [1, 127], \"roundRobins\": [ \"a.wav\" ] } ] } ] }";

        return "{ \"formatVersion\": 2, \"id\": \"" + id + "\", \"name\": \"" + id + "\", \"sampleRate\": 44100, \"mics\": [ " + micList
               + " ], \"pieces\": [ " + pieceList + " ] }";
    }

    KitManifest parseKit (const juce::String& json)
    {
        KitManifest m;
        const auto r = KitManifest::parse (json, juce::File::getSpecialLocation (juce::File::tempDirectory), m);
        jassert (r.wasOk());
        juce::ignoreUnused (r);
        return m;
    }

    /** The target mic (by id) a source mic (by id) maps onto, or "" if it's dropped. */
    juce::String mappedTo (const std::vector<int>& map, const KitManifest& source, const KitManifest& target, const juce::String& micId)
    {
        const auto t = map[(size_t) source.findMic (micId)];
        return t < 0 ? juce::String() : target.mics[(size_t) t].id;
    }

    /** A copy of the synthetic test kit under another id, with some mics renamed. */
    juce::File copyTestKit (const juce::File& parent, const juce::String& id, const juce::StringPairArray& renameMics)
    {
        const auto folder = parent.getChildFile (id);
        folder.deleteRecursively();
        testKitFolder.copyDirectoryTo (folder);

        auto json = folder.getChildFile ("kit.json").loadFileAsString();

        for (const auto& from : renameMics.getAllKeys())
            json = json.replace ("\"" + from + "\"", "\"" + renameMics[from] + "\"");

        json = json.replaceFirstOccurrenceOf ("{", "{ \"id\": \"" + id + "\",");
        json = json.replace ("Lithium Test Kit", "Kit " + id);
        folder.getChildFile ("kit.json").replaceWithText (json);
        return folder;
    }
}

//==============================================================================
class MicMappingTests final : public juce::UnitTest
{
public:
    MicMappingTests() : juce::UnitTest ("Piece swap mic mapping", "Lithium") {}

    void runTest() override
    {
        // Shaped like DRSKit: two kick mics, snare top and bottom, tom mics
        // numbered across the rack and floor toms, overheads and room.
        const auto drs = parseKit (kitJson ("drs", { { "kick_back", "close", 1 }, { "kick_front", "close", 1 }, { "snare_top", "close", 1 },
                                                      { "snare_bottom", "close", 1 }, { "hihat", "close", 1 }, { "tom1", "close", 1 },
                                                      { "tom2", "close", 1 }, { "tom3", "close", 1 }, { "overheads", "overheads", 2 },
                                                      { "room", "room", 2 } },
                                            { { "kick", "kick" }, { "snare", "snare" }, { "hihat", "hihat" }, { "tom1", "tom" },
                                              { "floor1", "floor_tom" }, { "floor2", "floor_tom" }, { "ride", "ride" } }));

        // Shaped like Naked Drums: two snare tops, a kick sub, five toms, no room of that id.
        const auto naked = parseKit (kitJson ("naked", { { "kick_in", "close", 1 }, { "kick_sub", "close", 1 }, { "snare_top1", "close", 1 },
                                                          { "snare_top2", "close", 1 }, { "snare_bottom", "close", 1 }, { "tom1", "close", 1 },
                                                          { "tom2", "close", 1 }, { "tom3", "close", 1 }, { "tom4", "close", 1 },
                                                          { "tom5", "close", 1 }, { "overheads", "overheads", 2 },
                                                          { "close_room", "room", 1 }, { "far_rooms", "room", 2 } },
                                                { { "kick", "kick" }, { "snare", "snare" }, { "tom1", "tom" }, { "tom2", "tom" },
                                                  { "tom3", "tom" }, { "floor1", "floor_tom" }, { "floor2", "floor_tom" } }));

        // One close mic per piece, named after it, like the Basalt kits; no overheads.
        const auto basalt = parseKit (kitJson ("basalt", { { "kick", "close", 1 }, { "snare", "close", 1 }, { "tom", "close", 1 } },
                                               { { "kick", "kick" }, { "snare", "snare" }, { "tom", "tom" } }));

        beginTest ("A piece's own mics are found by name, and tom mics pair up with toms in order");
        {
            auto ids = [] (const KitManifest& k, std::vector<int> mics)
            {
                juce::StringArray result;

                for (const auto m : mics)
                    result.add (k.mics[(size_t) m].id);

                return result.joinIntoString (",");
            };

            expectEquals (ids (drs, swaps::ownMics (drs, drs.findPiece ("snare"))), juce::String ("snare_top,snare_bottom"));
            expectEquals (ids (drs, swaps::ownMics (drs, drs.findPiece ("kick"))), juce::String ("kick_back,kick_front"));
            expectEquals (ids (drs, swaps::ownMics (drs, drs.findPiece ("tom1"))), juce::String ("tom1"));
            expectEquals (ids (drs, swaps::ownMics (drs, drs.findPiece ("floor1"))), juce::String ("tom2"));
            expectEquals (ids (drs, swaps::ownMics (drs, drs.findPiece ("floor2"))), juce::String ("tom3"));
            expectEquals (ids (naked, swaps::ownMics (naked, naked.findPiece ("floor2"))), juce::String ("tom5"));
            expectEquals (ids (basalt, swaps::ownMics (basalt, basalt.findPiece ("snare"))), juce::String ("snare"));
            expect (swaps::ownMics (drs, drs.findPiece ("ride")).empty());
        }

        beginTest ("Close mics map by placement, overheads and room by role");
        {
            const auto map = swaps::mapMics (naked, naked.findPiece ("snare"), drs, drs.findPiece ("snare"), true);
            expectEquals (mappedTo (map, naked, drs, "snare_top1"), juce::String ("snare_top"));
            expectEquals (mappedTo (map, naked, drs, "snare_bottom"), juce::String ("snare_bottom"));
            expectEquals (mappedTo (map, naked, drs, "snare_top2"), juce::String(), "a second top mic doesn't take the bottom one");
            expectEquals (mappedTo (map, naked, drs, "overheads"), juce::String ("overheads"));
            expectEquals (mappedTo (map, naked, drs, "far_rooms"), juce::String ("room"), "the stereo room goes to the stereo room");
            expectEquals (mappedTo (map, naked, drs, "close_room"), juce::String(), "one room mic, so the second is dropped");
            expectEquals (mappedTo (map, naked, drs, "kick_in"), juce::String ("kick_back"));

            // Each target mic receives at most one source mic.
            std::set<int> used;

            for (const auto t : map)
                expect (t < 0 || used.insert (t).second);
        }

        beginTest ("With ambience off, only the close mics come along");
        {
            const auto map = swaps::mapMics (naked, naked.findPiece ("snare"), drs, drs.findPiece ("snare"), false);
            expectEquals (mappedTo (map, naked, drs, "overheads"), juce::String());
            expectEquals (mappedTo (map, naked, drs, "far_rooms"), juce::String());
            expectEquals (mappedTo (map, naked, drs, "snare_top1"), juce::String ("snare_top"));
        }

        beginTest ("A tom goes to the target tom's own mic, whatever the mics are numbered");
        {
            const auto map = swaps::mapMics (naked, naked.findPiece ("tom2"), drs, drs.findPiece ("floor1"), true);
            expectEquals (mappedTo (map, naked, drs, "tom2"), juce::String ("tom2"), "DRSKit's floor tom 1 is on its tom2 mic");
        }

        beginTest ("A one-mic-per-piece kit swaps both ways");
        {
            auto map = swaps::mapMics (basalt, basalt.findPiece ("snare"), drs, drs.findPiece ("snare"), true);
            expectEquals (mappedTo (map, basalt, drs, "snare"), juce::String ("snare_top"));

            map = swaps::mapMics (drs, drs.findPiece ("snare"), basalt, basalt.findPiece ("snare"), true);
            expectEquals (mappedTo (map, drs, basalt, "snare_top"), juce::String ("snare"));
            expectEquals (mappedTo (map, drs, basalt, "snare_bottom"), juce::String(), "the kit has one snare mic");
            expectEquals (mappedTo (map, drs, basalt, "overheads"), juce::String(), "the kit has no overheads");
        }

        beginTest ("Which roles can stand in for which");
        {
            expect (swaps::canReplace (PieceRole::tom, PieceRole::floorTom));
            expect (swaps::canReplace (PieceRole::snare, PieceRole::snare));
            expect (! swaps::canReplace (PieceRole::crash, PieceRole::ride));
            expect (! swaps::canReplace (PieceRole::kick, PieceRole::snare));
        }

        beginTest ("Swaps survive a trip through a ValueTree");
        {
            PieceSwaps list;
            swaps::set (list, { "snare", "muldjordkit", "snare", true });
            swaps::set (list, { "kick", "crocellkit-full", "kick_left", false });
            swaps::set (list, { "snare", "naked-drums", "snare", true });   // replaces the first
            expectEquals ((int) list.size(), 2);
            expect (swaps::fromValueTree (swaps::toValueTree (list)) == list);

            swaps::set (list, { "kick", {}, {}, true });   // back to the kit's own
            expectEquals ((int) list.size(), 1);
        }
    }
};

//==============================================================================
class SwapLoadTests final : public juce::UnitTest
{
public:
    SwapLoadTests() : juce::UnitTest ("Piece swap loading", "Lithium") {}

    void runTest() override
    {
        const auto kits = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("lithium-swap-test-kits");
        kits.deleteRecursively();
        kits.createDirectory();

        // The host is the test kit; the source is a copy with other mic names
        // (and, as in a real kit, its snare bottom mic in reverse polarity).
        const auto host = copyTestKit (kits, "swap-host", {});
        juce::StringPairArray renames;
        renames.set ("snareTop", "snare_top");
        renames.set ("snareBottom", "snare_bottom");
        const auto source = copyTestKit (kits, "swap-source", renames);

        {
            auto json = source.getChildFile ("kit.json").loadFileAsString();
            json = json.replace ("\"id\": \"snare_bottom\"", "\"id\": \"snare_bottom\", \"invertPolarity\": true");
            source.getChildFile ("kit.json").replaceWithText (json);
        }

        beginTest ("A swapped piece plays the other kit's audio through this kit's mics");
        {
            KitLoader::Options options;
            options.swaps = { { "snare", "swap-source", "snare", true } };
            KitInfo info;
            juce::String error;
            std::shared_ptr<const LoadPlan> plan;
            const auto kit = KitLoader::loadKit (host, MidiMap::lithiumStandard(), info, error, options, &plan);

            expect (kit != nullptr, error);

            if (kit != nullptr)
            {
                const auto snare = [&] { for (size_t i = 0; i < info.pieces.size(); ++i) if (info.pieces[i].id == "snare") return (int) i; return -1; }();
                expect (snare >= 0);
                expectEquals (info.pieces[(size_t) snare].swappedFrom, juce::String ("Kit swap-source"));
                expect (info.pieces[(size_t) snare].swapNotes.isEmpty(), info.pieces[(size_t) snare].swapNotes.joinIntoString ("; "));
                expectEquals ((int) plan->folders.size(), 2);
                expect (plan->folders[1] == source);

                // Its hits read files from the source folder, on the host's snare mics.
                const auto snareTop = [&] { for (size_t m = 0; m < info.mics.size(); ++m) if (info.mics[m].id == "snareTop") return (int) m; return -1; }();
                const auto snareBottom = [&] { for (size_t m = 0; m < info.mics.size(); ++m) if (info.mics[m].id == "snareBottom") return (int) m; return -1; }();
                const auto& firstHit = kit->zones[(size_t) kit->articulations[(size_t) kit->pieces[(size_t) snare].firstArticulation].firstZone].firstHit;
                expect (kit->getSample (firstHit, snareTop) != invalidSampleId);
                expect (std::any_of (plan->files.begin(), plan->files.end(), [] (const auto& f) { return f.source == 1; }));

                // The source's reversed bottom mic stays reversed on the host's (normal) one.
                expect (kit->pieces[(size_t) snare].micGain[(size_t) snareBottom] < 0.0f);
                expectWithinAbsoluteError (kit->pieces[(size_t) snare].micGain[(size_t) snareTop], 1.0f, 0.001f);
                expectWithinAbsoluteError ((float) kit->pieces[(size_t) snare].sampleRate, 44100.0f, 0.5f);
            }
        }

        beginTest ("A swap from a kit that isn't installed leaves the piece as it was, and says so");
        {
            KitLoader::Options options;
            options.swaps = { { "snare", "not-installed", "snare", true } };
            KitInfo info;
            juce::String error;
            const auto kit = KitLoader::loadKit (host, MidiMap::lithiumStandard(), info, error, options);
            expect (kit != nullptr, error);

            for (const auto& p : info.pieces)
            {
                if (p.id == "snare")
                {
                    expect (p.swappedFrom.isEmpty());
                    expect (p.swapNotes.joinIntoString ("").contains ("isn't installed"));
                }
            }
        }

        beginTest ("A streaming kit streams a swapped piece from its own cache pack");
        {
            const auto cache = kits.getSiblingFile ("lithium-swap-test-cache");
            cache.deleteRecursively();

            KitLoader::Options options;
            options.swaps = { { "snare", "swap-source", "snare", true } };
            options.streaming.mode = StreamingMode::always;
            options.streaming.cacheFolder = cache;
            options.streaming.preloadFrames = 8192;
            options.streamStats = std::make_shared<StreamStats>();
            KitInfo info;
            juce::String error;
            std::shared_ptr<const LoadPlan> plan;
            auto kit = KitLoader::loadKit (host, MidiMap::lithiumStandard(), info, error, options, &plan);

            expect (kit != nullptr, error);

            if (kit != nullptr)
            {
                expect (kit->samples->isStreaming());
                expectEquals ((int) plan->stream->dataFiles.size(), 2);
                expect (plan->stream->dataFiles[0] != plan->stream->dataFiles[1]);
                expect (plan->stream->dataFiles[1].getParentDirectory() == streamcache::Pack::folderFor (cache, source));
            }

            kit = nullptr;   // closes the cache files before they're deleted
            cache.deleteRecursively();
        }

        kits.deleteRecursively();
    }
};

//==============================================================================
class UserPresetTests final : public juce::UnitTest
{
public:
    UserPresetTests() : juce::UnitTest ("Presets and swaps in the processor", "Lithium") {}

    static constexpr double sampleRate = 44100.0;

    bool waitForKit (LithiumDrumsProcessor& p)
    {
        juce::AudioBuffer<float> buffer (p.getTotalNumOutputChannels(), 512);

        for (int i = 0; i < 300; ++i)
        {
            p.applyPendingKitLoad();
            p.prepareToPlay (sampleRate, 512);
            juce::MidiBuffer midi;
            p.processBlock (buffer, midi);

            if (! p.isKitLoading() && p.getKitInfo() != nullptr && ! p.isPresetLoading())
                return true;

            juce::Thread::sleep (20);
        }

        return false;
    }

    static void setParam (LithiumDrumsProcessor& p, const juce::String& id, float value)
    {
        auto* param = p.getParameters().getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    static float getParam (LithiumDrumsProcessor& p, const juce::String& id)
    {
        auto* param = p.getParameters().getParameter (id);
        return param->convertFrom0to1 (param->getValue());
    }

    static int pieceIndex (const KitInfo& info, const juce::String& id)
    {
        for (size_t i = 0; i < info.pieces.size(); ++i)
            if (info.pieces[i].id == id)
                return (int) i;

        return -1;
    }

    void runTest() override
    {
        // A kits folder of its own: the test kit and a copy to swap pieces from.
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("lithium-preset-test");
        root.deleteRecursively();
        const auto kits = root.getChildFile ("Kits");
        kits.createDirectory();
        const auto host = copyTestKit (kits, "preset-host", {});
        const auto other = copyTestKit (kits, "preset-other", {});

        const auto oldKits = juce::SystemStats::getEnvironmentVariable ("LITHIUM_KITS_FOLDER", {});
        const auto oldPresets = juce::SystemStats::getEnvironmentVariable ("LITHIUM_PRESETS_FOLDER", {});
        test::setEnvironment ("LITHIUM_KITS_FOLDER", kits.getFullPathName());
        test::setEnvironment ("LITHIUM_PRESETS_FOLDER", root.getChildFile ("Presets").getFullPathName());

        {
            beginTest ("The test kit loads from a kits folder of its own");
            LithiumDrumsProcessor processor;
            processor.loadKit (host, true);
            expect (waitForKit (processor));

            beginTest ("Swapping a piece reloads the kit with it, keeping the settings");
            {
                const auto snare = pieceIndex (*processor.getKitInfo(), "snare");
                setParam (processor, params::pieceParamId (snare, "tune"), 3.0f);
                processor.setPieceSwap ({ "snare", "preset-other", "snare", true });
                expect (waitForKit (processor));

                const auto info = processor.getKitInfo();
                expectEquals (info->pieces[(size_t) snare].swappedFrom, juce::String ("Kit preset-other"));
                expectWithinAbsoluteError (getParam (processor, params::pieceParamId (snare, "tune")), 3.0f, 0.01f);
            }

            beginTest ("The model picker draws a swapped piece");
            {
                // Painting the list looks up the current swap for every row;
                // it once kept a pointer into a copy of the swaps that had
                // already gone (a crash in the standalone).
                ModelPicker picker (processor, processor.getKitInfo()->pieces[(size_t) pieceIndex (*processor.getKitInfo(), "snare")].slot);
                const auto image = picker.createComponentSnapshot (picker.getLocalBounds());
                expect (image.isValid());
            }

            beginTest ("A big kit's pieces go to their slots, the rest to the extras in order");
            {
                using R = PieceRole;
                const auto slots = kitslots::assign ({ { R::kick, false }, { R::snare, false }, { R::hihat, false }, { R::tom, false },
                                                       { R::floorTom, false }, { R::crash, false }, { R::ride, false }, { R::china, false },
                                                       { R::kick, false }, { R::snare, false }, { R::crash, false }, { R::crash, false },
                                                       { R::crash, true }, { R::ride, false }, { R::china, false }, { R::other, false } });
                const std::vector<juce::String> expected { "kick", "snare", "hihat", "tom1", "floor1", "crash1", "ride", "china",
                                                           "extra1", "snare2", "crash2", "crash3", "splash", "extra2", "extra3", "extra4" };

                for (size_t i = 0; i < expected.size(); ++i)
                    expectEquals (slots[i], kitslots::find (expected[i]), expected[i]);
            }

            beginTest ("Every piece has a slot, and the kit's own are remembered");
            {
                const auto info = processor.getKitInfo();

                for (const auto& p : info->pieces)
                    expect (p.slot >= 0, p.id);

                expectEquals (info->slotOwners[(size_t) kitslots::find ("tom2")].id, juce::String ("tom2"));
                expect (info->slotOwners[(size_t) kitslots::find ("tom3")].id.isEmpty());
            }

            const auto tom3 = kitslots::find ("tom3");

            beginTest ("Filling an empty slot adds the piece at the end, at its defaults");
            {
                const auto before = (int) processor.getKitInfo()->pieces.size();
                setParam (processor, params::pieceParamId (before, "volume"), -20.0f);   // left over from some earlier kit
                processor.setPieceSwap ({ kitslots::addedPieceId (tom3), "preset-other", "floor", true });
                expect (waitForKit (processor));

                const auto info = processor.getKitInfo();
                expectEquals ((int) info->pieces.size(), before + 1);
                const auto& added = info->pieces.back();
                expectEquals (added.slot, tom3);
                expectEquals (added.name, juce::String ("Tom 3"));
                expect (added.role == PieceRole::tom, "a floor tom in a rack tom's slot plays as a rack tom");
                expectEquals (added.swappedFrom, juce::String ("Kit preset-other"));
                expectWithinAbsoluteError (getParam (processor, params::pieceParamId (before, "volume")), 0.0f, 0.01f);
            }

            beginTest ("Emptying a slot removes the piece; the others keep their settings");
            {
                const auto info = processor.getKitInfo();
                const auto china = pieceIndex (*info, "china");
                setParam (processor, params::pieceParamId (china, "volume"), -7.0f);
                processor.setPieceSwap ({ "kick", {}, {}, true, true });
                expect (waitForKit (processor));

                const auto after = processor.getKitInfo();
                expectEquals (pieceIndex (*after, "kick"), -1);
                expectEquals (after->slotOwners[(size_t) kitslots::find ("kick")].id, juce::String ("kick"), "the slot still knows its own kick");
                expectEquals (pieceIndex (*after, "china"), china - 1);
                expectWithinAbsoluteError (getParam (processor, params::pieceParamId (china - 1, "volume")), -7.0f, 0.01f);
                expectWithinAbsoluteError (getParam (processor, params::pieceParamId (pieceIndex (*after, "snare"), "tune")), 3.0f, 0.01f,
                                           "the swapped snare's tuning moved with it");

                const auto saved = swaps::toValueTree (processor.getPieceSwaps());
                expect (swaps::fromValueTree (saved) == processor.getPieceSwaps(), "emptied and filled slots save and load");

                // And back: the kit's own kick in its own place, the extra tom gone.
                processor.setPieceSwap ({ "kick", {}, {}, true });
                expect (waitForKit (processor));
                expectEquals (pieceIndex (*processor.getKitInfo(), "kick"), 0);
                processor.setPieceSwap ({ kitslots::addedPieceId (tom3), {}, {}, true });
                expect (waitForKit (processor));
                expectEquals ((int) processor.getPieceSwaps().size(), 1);
            }

            beginTest ("Saving a preset, changing things, and loading it back");
            {
                const auto file = presets::userFileFor ("Test Sound");
                expect (processor.savePreset (file).wasOk());
                expect (file.existsAsFile());
                expect (! processor.isPresetModified());

                const auto kick = pieceIndex (*processor.getKitInfo(), "kick");
                setParam (processor, params::pieceParamId (kick, "volume"), -12.0f);
                expect (processor.isPresetModified(), "changing a parameter marks the preset modified");

                processor.setPieceSwap ({ "snare", {}, {}, true });
                expect (waitForKit (processor));
                expect (processor.getPieceSwaps().empty());

                expect (processor.loadPreset (file).wasOk());
                expect (waitForKit (processor));
                expectWithinAbsoluteError (getParam (processor, params::pieceParamId (kick, "volume")), 0.0f, 0.01f);
                expectEquals ((int) processor.getPieceSwaps().size(), 1, "the swap comes back with the preset");
                expectEquals (processor.getKitInfo()->pieces[(size_t) pieceIndex (*processor.getKitInfo(), "snare")].swappedFrom,
                              juce::String ("Kit preset-other"));
                expect (! processor.isPresetModified());
            }

            beginTest ("A preset for another kit loads that kit, then its settings");
            {
                processor.loadKit (other, true);
                expect (waitForKit (processor));
                expect (processor.getPresetFile() == juce::File(), "choosing a kit lets go of the preset");

                expect (processor.loadPreset (presets::userFileFor ("Test Sound")).wasOk());
                expect (waitForKit (processor));
                expect (processor.getKitInfo()->folder == host);
                expect (! processor.isPresetModified());
            }

            beginTest ("A preset whose kit isn't installed says so");
            {
                presets::Contents contents;
                expect (presets::read (presets::userFileFor ("Test Sound"), contents).wasOk());
                contents.kitId = "nowhere";
                contents.kitPath = "/nowhere/kit";
                contents.kitName = "Nowhere Kit";
                const auto file = presets::userFileFor ("Missing Kit");
                expect (presets::write (file, contents).wasOk());
                const auto r = processor.loadPreset (file);
                expect (r.failed() && r.getErrorMessage().contains ("Nowhere Kit"));
            }

            beginTest ("Swaps are saved with the project");
            {
                juce::MemoryBlock state;
                processor.getStateInformation (state);

                LithiumDrumsProcessor restored;
                restored.setStateInformation (state.getData(), (int) state.getSize());
                expect (waitForKit (restored));
                expect (restored.getPieceSwaps() == processor.getPieceSwaps());
                expectEquals (restored.getKitInfo()->pieces[(size_t) pieceIndex (*restored.getKitInfo(), "snare")].swappedFrom,
                              juce::String ("Kit preset-other"));
            }

            beginTest ("Playing a swapped kit doesn't allocate");
            {
                juce::AudioBuffer<float> buffer (processor.getTotalNumOutputChannels(), 256);
                juce::MidiBuffer midi;
                midi.addEvent (juce::MidiMessage::noteOn (10, 38, (juce::uint8) 110), 0);
                processor.processBlock (buffer, midi);   // warm up

                const test::ScopedAllocationCounter counter;
                midi.clear();
                midi.addEvent (juce::MidiMessage::noteOn (10, 38, (juce::uint8) 110), 10);
                midi.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 90), 100);

                for (int i = 0; i < 20; ++i)
                    processor.processBlock (buffer, midi);

                expectEquals (counter.count(), 0);
            }
        }

        test::setEnvironment ("LITHIUM_KITS_FOLDER", oldKits);
        test::setEnvironment ("LITHIUM_PRESETS_FOLDER", oldPresets);
        root.deleteRecursively();
    }
};

static MicMappingTests micMappingTests;
static SwapLoadTests swapLoadTests;
static UserPresetTests userPresetTests;

} // namespace lithium
