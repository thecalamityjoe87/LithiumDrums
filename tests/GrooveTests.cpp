#include "AllocationCounter.h"
#include "PluginProcessor.h"
#include "engine/GroovePlayer.h"
#include "engine/KitLoader.h"
#include "engine/MidiMapping.h"
#include "grooves/GrooveImporter.h"
#include "grooves/GrooveMidi.h"

namespace lithium
{

namespace
{
    const juce::File testKitFolder { LITHIUM_TEST_KIT_DIR };
    constexpr double sampleRate = 48000.0;

    /** A pattern of hits on articulation 0 at the given beats. */
    std::unique_ptr<GroovePattern> makePattern (std::initializer_list<double> beats, double length, std::uint64_t kitSerial)
    {
        auto p = std::make_unique<GroovePattern>();
        p->lengthBeats = length;
        p->bpm = 120.0;
        p->kitSerial = kitSerial;
        p->clipId = 7;

        for (const auto b : beats)
            p->events.push_back ({ b, 0, 100 });

        return p;
    }

    /** Runs the player for a number of blocks and returns where its hits land, in samples from the start. */
    std::vector<int> run (GroovePlayer& player, int numBlocks, int blockSize = 512, GroovePlayer::Transport transport = {},
                          std::uint64_t kitSerial = 1)
    {
        std::vector<int> positions;
        std::array<GroovePlayer::Hit, 64> hits {};

        for (int b = 0; b < numBlocks; ++b)
        {
            const auto n = player.process (kitSerial, transport, blockSize, sampleRate, hits);

            for (int i = 0; i < n; ++i)
                positions.push_back (b * blockSize + hits[(size_t) i].sampleOffset);

            if (transport.hostPpq.has_value())
                *transport.hostPpq += blockSize * transport.hostBpm.value_or (120.0) / (60.0 * sampleRate);
        }

        return positions;
    }

    /** A MIDI file like the Groove MIDI Dataset's: 480 PPQ, one track, Roland notes and the pedal on CC4. */
    juce::MidiFile makeTake (const std::vector<std::tuple<double, int, int>>& notes, const std::vector<std::pair<double, int>>& pedal = {})
    {
        juce::MidiMessageSequence track;
        track.addEvent (juce::MidiMessage::tempoMetaEvent (500'000), 0.0);

        for (const auto& [beat, value] : pedal)
            track.addEvent (juce::MidiMessage::controllerEvent (10, 4, value), beat * 480.0);

        for (const auto& [beat, note, velocity] : notes)
        {
            track.addEvent (juce::MidiMessage::noteOn (10, note, (juce::uint8) velocity), beat * 480.0);
            track.addEvent (juce::MidiMessage::noteOff (10, note), beat * 480.0 + 60.0);
        }

        track.sort();
        juce::MidiFile file;
        file.setTicksPerQuarterNote (480);
        file.addTrack (track);
        return file;
    }

    void write (const juce::MidiFile& midi, const juce::File& file)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::FileOutputStream out (file);
        midi.writeTo (out);
    }
}

class GroovePlayerTests final : public juce::UnitTest
{
public:
    GroovePlayerTests() : juce::UnitTest ("Groove player", "Lithium") {}

    void runTest() override
    {
        constexpr int samplesPerBeat = 24000;   // 120 BPM at 48 kHz

        beginTest ("Hits land on their sample, and the loop comes round again");
        {
            GroovePlayer player;
            player.tempoMode = GrooveTempo::original;
            auto pattern = makePattern ({ 0.0, 1.0, 2.5 }, 4.0, 1);
            expect (player.send (pattern));
            player.playing = true;

            // 4 beats = 96,000 samples; run a little over two loops.
            const auto hits = run (player, 400);
            const std::vector<int> expected { 0, 24000, 60000, 96000, 120000, 156000, 192000 };

            expect (hits.size() >= expected.size());

            for (size_t i = 0; i < expected.size() && i < hits.size(); ++i)
                expect (std::abs (hits[i] - expected[i]) <= 1, "hit " + juce::String ((int) i) + " at " + juce::String (hits[i]));

            expectEquals (player.getFeedback().clipId.load(), 7);
        }

        beginTest ("Without loop, it plays once and stops");
        {
            GroovePlayer player;
            player.tempoMode = GrooveTempo::original;
            player.loop = false;
            auto pattern = makePattern ({ 0.0, 3.0 }, 4.0, 1);
            player.send (pattern);
            player.playing = true;

            expectEquals ((int) run (player, 400).size(), 2);
            expect (! player.playing.load());
            expectEquals (player.getFeedback().clipId.load(), -1);
        }

        beginTest ("Tempo modes: the host's tempo, a custom one, and half time");
        {
            for (const auto [mode, speed, expectedSecond] : { std::tuple { GrooveTempo::host, 1.0f, samplesPerBeat / 2 },        // host at 240
                                                              std::tuple { GrooveTempo::custom, 1.0f, samplesPerBeat * 2 },      // custom 60
                                                              std::tuple { GrooveTempo::original, 0.5f, samplesPerBeat * 2 } })  // half time
            {
                GroovePlayer player;
                player.tempoMode = mode;
                player.customBpm = 60.0f;
                player.speed = speed;
                auto pattern = makePattern ({ 0.0, 1.0 }, 4.0, 1);
                player.send (pattern);
                player.playing = true;

                GroovePlayer::Transport transport;
                transport.hostBpm = 240.0;   // known, but the host isn't playing, so the preview runs free

                const auto hits = run (player, 200, 512, transport);
                expect (hits.size() >= 2 && std::abs (hits[1] - expectedSecond) <= 1,
                        "second hit at " + juce::String (hits.size() >= 2 ? hits[1] : -1));
            }
        }

        beginTest ("With the host playing, it follows the host's beat position");
        {
            GroovePlayer player;
            auto pattern = makePattern ({ 0.0, 2.5 }, 4.0, 1);
            player.send (pattern);
            player.playing = true;

            // The host is at beat 6 (2 beats into the second 4-beat loop), at 120 BPM.
            GroovePlayer::Transport transport;
            transport.hostBpm = 120.0;
            transport.hostPpq = 6.0;
            transport.hostPlaying = true;

            const auto hits = run (player, 200, 512, transport);
            expect (! hits.empty() && std::abs (hits[0] - samplesPerBeat / 2) <= 1, "beat 2.5 is half a beat away");
            expect (hits.size() >= 2 && std::abs (hits[1] - (samplesPerBeat / 2 + samplesPerBeat * 3 / 2)) <= 1, "then the downbeat");
            expect (player.getFeedback().hostSynced.load());
        }

        beginTest ("A pattern for another kit stays silent; a new one restarts, a re-resolved one carries on");
        {
            GroovePlayer player;
            player.tempoMode = GrooveTempo::original;
            auto pattern = makePattern ({ 0.0, 1.0, 2.0, 3.0 }, 4.0, 5);
            player.send (pattern);
            player.playing = true;

            expect (run (player, 100, 512, {}, 6).empty(), "kit 6 is playing, the pattern is for kit 5");

            // The same clip, resolved for kit 6, picks up where the old one was.
            const auto before = player.getFeedback().position.load();
            auto resolved = makePattern ({ 0.0, 1.0, 2.0, 3.0 }, 4.0, 6);
            resolved->restart = false;
            player.send (resolved);
            run (player, 1, 512, {}, 6);
            expect (player.getFeedback().position.load() > before);

            auto fresh = makePattern ({ 0.0 }, 4.0, 6);
            player.send (fresh);
            const auto hits = run (player, 1, 512, {}, 6);
            expect (hits.size() == 1 && hits[0] == 0, "a new clip starts from the top");
        }

        beginTest ("Playing never allocates, and old patterns go back for deletion");
        {
            GroovePlayer player;
            player.playing = true;
            std::array<GroovePlayer::Hit, 64> hits {};

            for (int i = 0; i < 20; ++i)
            {
                auto pattern = makePattern ({ 0.0, 0.5, 1.0, 1.5 }, 2.0, 1);
                player.send (pattern);

                test::ScopedAllocationCounter counter;
                GroovePlayer::Transport transport;
                transport.hostBpm = 133.0;
                transport.hostPpq = i * 0.37;
                transport.hostPlaying = (i % 2) == 0;

                for (int b = 0; b < 8; ++b)
                    player.process (1, transport, 256, sampleRate, hits);

                expectEquals (counter.count(), 0);
            }

            player.collectGarbage();   // the leak detector would complain at exit about anything left behind
        }
    }
};

//==============================================================================
class GrooveLibraryTests final : public juce::UnitTest
{
public:
    GrooveLibraryTests() : juce::UnitTest ("Grooves", "Lithium") {}

    void runTest() override
    {
        const juce::TemporaryFile temp;
        const auto root = temp.getFile();

        beginTest ("Reading a take: hits, pedal position and bars");
        {
            const auto file = root.getChildFile ("take.mid");
            write (makeTake ({ { 0.0, 36, 100 }, { 1.0, 46, 80 }, { 2.0, 42, 70 }, { 7.9, 38, 90 } }, { { 0.5, 0 }, { 1.5, 90 } }), file);

            std::vector<grooves::Hit> hits;
            expect (grooves::readHits (file, 4, 90, hits).wasOk());
            expectEquals ((int) hits.size(), 4);
            expectEquals (hits[0].openness, -1.0f);                        // no pedal data yet
            expectWithinAbsoluteError (hits[1].openness, 1.0f, 0.001f);    // 0 = open
            expectWithinAbsoluteError (hits[2].openness, 0.0f, 0.001f);    // 90 = closed, on the TD-11
            expectWithinAbsoluteError (hits[3].beat, 7.9, 0.01);

            // The hit at 7.9 is a pushed downbeat of bar 3, so the take has 3 bars.
            expectEquals (grooves::countBars (hits, 4.0), 3);
        }

        beginTest ("Clips: hits belong to the bar they were aimed at");
        {
            grooves::Groove groove;
            groove.bpm = 100.0;
            groove.bars = 3;
            const std::vector<grooves::Hit> hits { { 0.0, 36, 100 }, { 3.95, 38, 100 }, { 4.0, 42, 100 }, { 7.0, 38, 100 }, { 7.95, 36, 100 } };

            const auto bar2 = grooves::makeClip (groove, hits, 1, 1, "bar 2");
            expectEquals (bar2.lengthBeats, 4.0);
            expectEquals ((int) bar2.hits.size(), 3);   // the early snare, the hat, and the snare on 4; not the pushed kick
            expectWithinAbsoluteError (bar2.hits[0].beat, -0.05, 0.001);
            expectWithinAbsoluteError (bar2.hits[2].beat, 3.0, 0.001);

            const auto bar1 = grooves::makeClip (groove, hits, 0, 1, "bar 1");
            expectEquals ((int) bar1.hits.size(), 1);

            grooves::Pack pack;
            pack.phraseBars = 2;
            expectEquals (grooves::numPhrases (pack, groove), 2);
            expect (grooves::phraseBars (pack, groove, 1) == juce::Range<int> (2, 3));
        }

        beginTest ("Resolving through the Roland map: hats follow the pedal to the nearest step");
        {
            KitInfo info;
            juce::String error;
            const auto kit = KitLoader::loadKit (testKitFolder, MidiMap::kitDefault(), info, error);
            expect (kit != nullptr, error);

            if (kit == nullptr)
                return;

            auto articulation = [&info] (const juce::String& piece, const juce::String& id)
            {
                for (const auto& p : info.pieces)
                    if (p.id == piece)
                        for (size_t a = 0; a < p.articulations.size(); ++a)
                            if (p.articulations[a].id == id)
                                return p.firstArticulation + (int) a;

                return -1;
            };

            grooves::Clip clip;
            clip.lengthBeats = 4.0;
            clip.hits = { { 0.0, 36, 100, -1.0f }, { 1.0, 46, 90, 1.0f }, { 1.5, 46, 90, 0.45f }, { 2.0, 42, 90, 0.0f },
                          { 2.5, 48, 90, -1.0f }, { 3.0, 99, 90, -1.0f } };

            const auto resolved = grooves::resolve (clip, MidiMap::rolandVDrums(), info);
            expectEquals ((int) resolved.size(), 5);   // note 99 plays nothing in the Roland map

            if (resolved.size() == 5)
            {
                expectEquals (resolved[0].articulation, articulation ("kick", "hit"));
                expectEquals (resolved[1].articulation, articulation ("hihat", "open"));
                expectEquals (resolved[2].articulation, articulation ("hihat", "half_open"));
                expectEquals (resolved[3].articulation, articulation ("hihat", "closed"));
                expectEquals (resolved[4].articulation, articulation ("tom1", "center"));   // Roland's tom 1: the first tom
            }

            beginTest ("Exported MIDI plays the same hits through the user's map");

            for (const auto& map : { MidiMap::lithiumStandard(), MidiMap::generalMidi(), MidiMap::rolandVDrums(), MidiMap::alesis() })
            {
                const auto table = resolveMidiMap (map, info);
                const auto midi = grooves::toMidiFile (clip, MidiMap::rolandVDrums(), &info, table);
                expectEquals (midi.getTimeFormat(), (short) 480);

                std::vector<int> played;

                for (const auto* e : *midi.getTrack (0))
                    if (e->message.isNoteOn())
                        played.push_back (table.targets[(size_t) e->message.getNoteNumber()].articulation);

                expectEquals ((int) played.size(), (int) resolved.size(), map.name);

                for (size_t i = 0; i < played.size() && i < resolved.size(); ++i)
                    expectEquals (played[i], resolved[i].articulation, map.name + ", hit " + juce::String ((int) i));
            }

            const auto plain = grooves::toMidiFile (clip, MidiMap::rolandVDrums(), nullptr, {});
            int notes = 0;

            for (const auto* e : *plain.getTrack (0))
                notes += e->message.isNoteOn() ? 1 : 0;

            expectEquals (notes, (int) clip.hits.size(), "without a kit, every recorded note is written as it was");
        }

        beginTest ("The Groove MIDI Dataset importer: names, bars, fills and licence");
        {
            const auto source = root.getChildFile ("gmd").getChildFile ("groove");
            source.createDirectory();
            source.getChildFile ("LICENSE").replaceWithText ("This work is licensed under the Creative Commons Attribution 4.0 International License.");
            source.getChildFile ("info.csv").replaceWithText (
                "drummer,session,id,style,bpm,beat_type,time_signature,midi_filename,audio_filename,duration,split\n"
                "drummer1,drummer1/session1,drummer1/session1/1,rock,120,beat,4-4,drummer1/session1/1_rock_120_beat_4-4.mid,x.wav,30,train\n"
                "drummer1,drummer1/session1,drummer1/session1/2,rock,120,fill,4-4,drummer1/session1/2_rock_120_fill_4-4.mid,x.wav,2,train\n"
                "drummer1,drummer1/session1,drummer1/session1/3,rock/halftime,80,beat,4-4,drummer1/session1/3_rock-halftime_80_beat_4-4.mid,x.wav,30,train\n"
                "drummer2,drummer2/session1,drummer2/session1/1,funk/purdieshuffle,95,beat,4-4,drummer2/session1/1_funk-purdieshuffle_95_beat_4-4.mid,x.wav,30,train\n");

            write (makeTake ({ { 0.0, 36, 100 }, { 1.0, 38, 100 }, { 37.0, 38, 100 } }), source.getChildFile ("drummer1/session1/1_rock_120_beat_4-4.mid"));
            write (makeTake ({ { 0.0, 48, 100 }, { 3.5, 43, 100 } }), source.getChildFile ("drummer1/session1/2_rock_120_fill_4-4.mid"));
            write (makeTake ({ { 0.0, 36, 100 }, { 2.0, 38, 100 } }), source.getChildFile ("drummer1/session1/3_rock-halftime_80_beat_4-4.mid"));
            write (makeTake ({ { 0.0, 36, 100 } }), source.getChildFile ("drummer2/session1/1_funk-purdieshuffle_95_beat_4-4.mid"));

            expect (grooves::isGrooveMidiDataset (root.getChildFile ("gmd")));
            expect (! grooves::isGrooveMidiDataset (root));

            grooves::ImportOptions options;
            options.source = root.getChildFile ("gmd");
            options.groovesFolder = root.getChildFile ("Grooves");
            grooves::ImportedPack imported;
            const auto result = grooves::importGrooveMidiDataset (options, imported);
            expect (result.wasOk(), result.getErrorMessage());
            expectEquals (imported.numGrooves, 3);
            expectEquals (imported.numFills, 1);

            const auto library = grooves::scan (options.groovesFolder);
            expectEquals ((int) library.packs.size(), 1);

            if (library.packs.size() == 1)
            {
                const auto& pack = library.packs[0];
                expectEquals (pack.licenseName, juce::String ("CC BY 4.0"));
                expect (pack.licenseFile.existsAsFile());
                expect (pack.credits.contains ("Gillick"));
                expectEquals (pack.noteMap, juce::String ("roland-vdrums"));
                expectEquals (pack.pedalClosedAt, 90);

                juce::StringArray names;

                for (const auto& g : pack.grooves)
                    names.add (g.name);

                expectEquals (names.joinIntoString (","), juce::String ("Rock 1,Halftime Rock 1,Purdie Shuffle 1"));
                expectEquals (pack.grooves[0].bars, 10);   // the last hit is on beat 37, in bar 10
                expectEquals ((int) pack.grooves[0].fills.size(), 1);
                expectEquals (pack.grooves[0].fills[0].bars, 1);
                expect (pack.grooves[0].fills[0].file.existsAsFile());
                expectEquals (pack.grooves[0].styleName, juce::String ("Rock"));
            }

            expect (grooves::importGrooveMidiDataset (options, imported).failed(), "it doesn't overwrite unless asked");
            options.overwrite = true;
            expect (grooves::importGrooveMidiDataset (options, imported).wasOk());
        }
    }
};

//==============================================================================
class GrooveProcessorTests final : public juce::UnitTest
{
public:
    GrooveProcessorTests() : juce::UnitTest ("Grooves in the processor", "Lithium") {}

    void runTest() override
    {
        beginTest ("The preview plays through the kit without allocating, and its settings are saved");

        LithiumDrumsProcessor processor;
        juce::AudioBuffer<float> buffer (processor.getTotalNumOutputChannels(), 512);
        juce::MidiBuffer midi;

        for (int i = 0; i < 200 && processor.getKitInfo() == nullptr; ++i)
        {
            processor.applyPendingKitLoad();
            juce::Thread::sleep (20);
        }

        const auto info = processor.getKitInfo();
        expect (info != nullptr, "the test kit loads");

        if (info == nullptr)
            return;

        processor.prepareToPlay (sampleRate, 512);
        processor.processBlock (buffer, midi);   // swaps the kit in

        auto clip = std::make_shared<grooves::Clip>();
        clip->bpm = 120.0;
        clip->lengthBeats = 4.0;
        clip->hits = { { 0.0, 36, 110 }, { 1.0, 38, 110 }, { 2.0, 36, 110 }, { 3.0, 38, 110 } };

        auto& player = processor.getGroovePlayer();
        player.tempoMode = GrooveTempo::original;
        processor.previewGroove (clip, "roland-vdrums", 3);
        expect (processor.isGroovePlaying());

        float peak = 0.0f;
        int allocations = 0;

        for (int b = 0; b < 200; ++b)
        {
            test::ScopedAllocationCounter counter;
            processor.processBlock (buffer, midi);
            allocations += counter.count();
            peak = std::max (peak, buffer.getMagnitude (0, 512));
        }

        expect (peak > 0.01f, "the groove is heard");
        expectEquals (allocations, 0);
        expectEquals (player.getFeedback().clipId.load(), 3);

        processor.stopGroove();
        processor.processBlock (buffer, midi);
        expectEquals (player.getFeedback().clipId.load(), -1);

        // A dragged clip comes out in the current map's notes.
        processor.setMidiMap (MidiMap::generalMidi(), false);
        const auto file = processor.grooveToMidi (*clip, "roland-vdrums");
        juce::Array<int> notes;

        for (const auto* e : *file.getTrack (0))
            if (e->message.isNoteOn())
                notes.add (e->message.getNoteNumber());

        expect (notes == juce::Array<int> { 36, 38, 36, 38 }, "General MIDI kick and snare");

        player.tempoMode = GrooveTempo::custom;
        player.customBpm = 87.5f;
        player.speed = 2.0f;
        player.loop = false;
        processor.setGrooveSetting ("groove", "gmd-test");

        juce::MemoryBlock state;
        processor.getStateInformation (state);

        LithiumDrumsProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        auto& restoredPlayer = restored.getGroovePlayer();
        expect (restoredPlayer.tempoMode.load() == GrooveTempo::custom);
        expectEquals (restoredPlayer.customBpm.load(), 87.5f);
        expectEquals (restoredPlayer.speed.load(), 2.0f);
        expect (! restoredPlayer.loop.load());
        expectEquals (restored.getGrooveSetting ("groove").toString(), juce::String ("gmd-test"));
        expect (restored.getParameters().state.getChildWithName ("grooves") == juce::ValueTree(), "not mixed in with the parameters");
    }
};

static GroovePlayerTests groovePlayerTests;
static GrooveLibraryTests grooveLibraryTests;
static GrooveProcessorTests grooveProcessorTests;

} // namespace lithium
