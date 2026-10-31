#include "AllocationCounter.h"
#include "PluginProcessor.h"
#include "engine/DrumEngine.h"
#include "engine/KitLoader.h"
#include "engine/KitManifest.h"
#include "engine/StreamCache.h"
#include "KitLibrary.h"
#include "importer/DrumGizmoImporter.h"
#include "importer/SfzImporter.h"
#include "ui/KitStage.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace lithium
{

namespace
{
    const juce::File testKitFolder { LITHIUM_TEST_KIT_DIR };
    constexpr double sampleRate = 44100.0;

    /** The generated test kit, loaded once and shared by the engine tests. */
    struct SharedKit
    {
        std::unique_ptr<Kit> kit;
        KitInfo info;

        int mic (const juce::String& id) const
        {
            for (size_t i = 0; i < info.mics.size(); ++i)
                if (info.mics[i].id == id)
                    return (int) i;

            jassertfalse;
            return 0;
        }

        int piece (const juce::String& id) const
        {
            for (size_t i = 0; i < info.pieces.size(); ++i)
                if (info.pieces[i].id == id)
                    return (int) i;

            jassertfalse;
            return 0;
        }
    };

    SharedKit& shared()
    {
        static SharedKit s = []
        {
            SharedKit k;
            juce::String error;
            k.kit = KitLoader::loadKit (testKitFolder, MidiMap::generalMidi(), k.info, error);
            jassert (k.kit != nullptr);
            return k;
        }();

        return s;
    }

    float peak (const juce::AudioBuffer<float>& b, int channel)
    {
        return b.getMagnitude (channel, 0, b.getNumSamples());
    }

    float micPeak (const juce::AudioBuffer<float>& b, const Kit& kit, int mic)
    {
        float p = 0.0f;
        const auto& m = kit.mics[(size_t) mic];

        for (int ch = 0; ch < m.numChannels; ++ch)
            p = std::max (p, peak (b, m.busChannel + ch));

        return p;
    }

    /** Drives a DrumEngine directly, without the processor around it. */
    struct EngineRig
    {
        explicit EngineRig (double rate = sampleRate) : kit (*shared().kit)
        {
            engine.prepare (rate);
            params.resetForKit (kit);
            params.velocityCrossfade = false;
        }

        ~EngineRig()     { engine.stopAll (kit); }

        void render (int numSamples)
        {
            bus.setSize (maxMicBusChannels, numSamples, false, false, true);
            bus.clear();
            engine.render (kit, bus, 0, numSamples, params);
        }

        void note (int n, int velocity = 100)     { engine.noteOn (kit, n, velocity, params); }
        int voices() const                         { return engine.getNumActiveVoices(); }
        float peakOf (const juce::String& mic)     { return micPeak (bus, kit, shared().mic (mic)); }
        PieceParams& piece (const juce::String& id) { return params.pieces[(size_t) shared().piece (id)]; }

        /** Renders until all voices stop; returns how many samples that took. */
        int renderUntilSilent (int maxSamples = 44100 * 10)
        {
            int total = 0;

            while (voices() > 0 && total < maxSamples)
            {
                render (512);
                total += 512;
            }

            return total;
        }

        Kit& kit;
        EngineFeedback feedback;
        DrumEngine engine { feedback };
        EngineParams params;
        juce::AudioBuffer<float> bus;
    };

    bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, int bits)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                       .withSampleRate (sampleRate)
                                                       .withNumChannels (audio.getNumChannels())
                                                       .withBitsPerSample (bits));
        return writer != nullptr && writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }
}

//==============================================================================
class ManifestTests final : public juce::UnitTest
{
public:
    ManifestTests() : juce::UnitTest ("Kit manifest", "Lithium") {}

    void runTest() override
    {
        beginTest ("The generated test kit parses cleanly");
        {
            KitManifest m;
            const auto r = KitManifest::parse (testKitFolder, m);
            expect (r.wasOk(), r.getErrorMessage());
            expectEquals (m.warnings.size(), 0, m.warnings.joinIntoString ("; "));
            expectEquals ((int) m.pieces.size(), 9);
            expectEquals ((int) m.mics.size(), 10);
            expectEquals (m.credits.licenseName, juce::String ("CC0 1.0"));

            for (const auto& p : m.pieces)
                for (const auto& a : p.articulations)
                {
                    expect (a.zones.size() >= 3);

                    for (const auto& z : a.zones)
                        expect (z.roundRobins.size() >= 2);
                }

            expectEquals ((int) m.pieces[(size_t) m.findPiece ("snare")].articulations.size(), 3);
            expectEquals ((int) m.pieces[(size_t) m.findPiece ("hihat")].articulations.size(), 3);
        }

        beginTest ("Invalid manifests are rejected with a reason");
        {
            auto check = [this] (const juce::String& json, const juce::String& expectedFragment)
            {
                KitManifest m;
                const auto r = KitManifest::parse (json, {}, m);
                expect (r.failed(), "should fail: " + expectedFragment);
                expect (r.getErrorMessage().containsIgnoreCase (expectedFragment),
                        "\"" + r.getErrorMessage() + "\" should mention \"" + expectedFragment + "\"");
            };

            const juce::String head = R"({ "formatVersion": 2, "name": "T", "sampleRate": 48000,
                "mics": [ { "id": "top", "channels": 1 } ], "pieces": )";

            check (R"({ "formatVersion": 1, "name": "T", "sampleRate": 48000, "mics": [], "pieces": [] })", "formatVersion");
            check (R"({ "formatVersion": 2, "type": "preset", "name": "T" })", "preset");
            check (head + R"([ { "id": "s", "role": "snare", "articulations": [ { "id": "c", "zones": [
                       { "velocity": [1, 64], "roundRobins": [ { "files": { "top": "a.wav" } } ] },
                       { "velocity": [60, 127], "roundRobins": [ { "files": { "top": "b.wav" } } ] } ] } ] } ] })", "overlap");
            check (head + R"([ { "id": "s", "role": "snare", "articulations": [ { "id": "c", "zones": [
                       { "velocity": [1, 127], "roundRobins": [ { "files": { "top": "../../etc/passwd" } } ] } ] } ] } ] })", "relative path");
            check (head + R"([ { "id": "s", "role": "snare", "articulations": [ { "id": "c", "zones": [
                       { "velocity": [1, 127], "roundRobins": [ "a.wav" ] } ] } ] } ] })", "channelMap");
            check (head + R"([ { "id": "s", "role": "drum", "articulations": [] } ] })", "role");
            check (head + R"([ { "id": "s", "role": "snare", "articulations": [] } ] })", "at least one articulation");
            check (head + R"([ { "id": "s", "role": "snare", "articulations": [ { "id": "c", "channelMap": { "oh": 1 },
                       "zones": [ { "velocity": [1, 127], "roundRobins": [ "a.wav" ] } ] } ] } ] })", "not one of the kit's mics");
        }

        beginTest ("Hits can be multichannel files, file lists or one file per mic");
        {
            KitManifest m;
            const auto r = KitManifest::parse (R"({ "formatVersion": 2, "name": "T", "sampleRate": 48000,
                "mics": [ { "id": "top", "channels": 1 }, { "id": "oh", "channels": 2, "role": "overheads" } ],
                "pieces": [ { "id": "s", "role": "snare", "articulations": [ { "id": "c",
                    "channelMap": { "top": 3, "oh": [1, 2] },
                    "zones": [ { "velocity": [1, 127], "roundRobins": [
                        "hit1.wav",
                        { "file": "hit2.wav", "channelMap": { "top": 1 } },
                        [ { "file": "a.wav", "channelMap": { "top": 1 } }, { "file": "b.wav", "channelMap": { "oh": [2, 1] } } ],
                        { "files": { "top": "t.wav", "oh": "o.wav" } } ] } ] } ] } ],
                "midiMap": [ { "note": 38, "piece": "s", "articulation": "c" }, { "note": 40, "piece": "nope" } ] })", {}, m);
            expect (r.wasOk(), r.getErrorMessage());

            const auto& hits = m.pieces[0].articulations[0].zones[0].roundRobins;
            expectEquals ((int) hits.size(), 4);
            expectEquals ((int) hits[0].sources.size(), 2);
            expect (hits[0].sources[1].fileChannels == std::array<int, 2> { 1, 2 });
            expectEquals ((int) hits[1].sources.size(), 1);
            expectEquals (hits[2].sources[1].file, juce::String ("b.wav"));
            expect (hits[2].sources[1].fileChannels == std::array<int, 2> { 2, 1 });
            expect (hits[3].sources[1].fileChannels == std::array<int, 2> { 1, 2 });
            expectEquals ((int) m.midiMap.size(), 1);
            expectEquals (m.warnings.size(), 1);   // the unknown piece in the MIDI map
        }
    }
};

//==============================================================================
class LoaderTests final : public juce::UnitTest
{
public:
    LoaderTests() : juce::UnitTest ("Kit loader", "Lithium") {}

    void runTest() override
    {
        beginTest ("Loads the test kit and resolves the General MIDI map by role");
        {
            const auto& s = shared();
            expect (s.kit != nullptr);

            if (s.kit == nullptr)
                return;

            const auto& kit = *s.kit;
            expect (s.info.memoryBytes > 0);
            expectEquals (s.info.bitsPerSample, 24);
            expectEquals (s.info.pieces[(size_t) s.piece ("snare")].name, juce::String ("Lithium Brass Snare 14x6.5"));

            auto pieceOf = [&] (int note) { return kit.articulations[(size_t) kit.noteMap[(size_t) note].articulation].piece; };
            expectEquals (pieceOf (36), s.piece ("kick"));
            expectEquals (pieceOf (38), s.piece ("snare"));
            expectEquals (pieceOf (42), s.piece ("hihat"));
            expectEquals (pieceOf (48), s.piece ("tom2"));     // GM 48: the second rack tom
            expectEquals (pieceOf (50), s.piece ("tom1"));
            expectEquals (pieceOf (45), s.piece ("tom2"));     // GM 45 asks for a third rack tom: the last one
            expectEquals (pieceOf (43), s.piece ("floor"));
            expectEquals (pieceOf (52), s.piece ("china"));
            expect (kit.noteMap[60].articulation < 0);

            // The open hat is in a choke group that the closed hat chokes.
            const auto& open = kit.articulations[(size_t) kit.noteMap[46].articulation];
            const auto& closed = kit.articulations[(size_t) kit.noteMap[42].articulation];
            expect (open.chokeGroupMask != 0 && (closed.chokesMask & open.chokeGroupMask) != 0);

            // Cymbals get choke notes from 85 up.
            expectEquals (s.info.pieces[(size_t) s.piece ("crash")].defaultChokeNote, 85);
            expectEquals (s.info.pieces[(size_t) s.piece ("ride")].defaultChokeNote, 86);
            expectEquals (s.info.pieces[(size_t) s.piece ("snare")].defaultChokeNote, -1);

            std::cout << "    test kit: " << s.info.numSampleFiles << " files, "
                      << (s.info.memoryBytes / (1024 * 1024)) << " MB as " << s.info.bitsPerSample << "-bit, loaded in "
                      << juce::String (s.info.loadSeconds, 2) << " s" << std::endl;
        }

        beginTest ("Pieces go to stage slots by role");
        {
            using R = PieceRole;

            // DRSKit: one rack tom, two floor toms, two crashes, no china.
            const auto drs = assignStageSlots ({ R::kick, R::snare, R::hihat, R::tom, R::floorTom, R::floorTom, R::crash, R::crash, R::ride });
            expectEquals (drs[(size_t) index (StageSlot::tom1)], 3);
            expectEquals (drs[(size_t) index (StageSlot::tom2)], -1);
            expectEquals (drs[(size_t) index (StageSlot::floorTom)], 4);
            expectEquals (drs[(size_t) index (StageSlot::crash)], 6);
            expectEquals (drs[(size_t) index (StageSlot::china)], -1);

            // MuldjordKit: two kicks, three rack toms, a china.
            const auto mj = assignStageSlots ({ R::kick, R::kick, R::snare, R::hihat, R::tom, R::tom, R::tom, R::floorTom,
                                                R::crash, R::crash, R::ride, R::ride, R::china });
            expectEquals (mj[(size_t) index (StageSlot::kick)], 0);
            expectEquals (mj[(size_t) index (StageSlot::tom2)], 5);
            expectEquals (mj[(size_t) index (StageSlot::china)], 12);
        }

        beginTest ("A missing folder fails cleanly");
        {
            KitInfo info;
            juce::String error;
            auto kit = KitLoader::loadKit (juce::File ("/nonexistent/kit"), MidiMap::generalMidi(), info, error);
            expect (kit == nullptr);
            expect (error.contains ("kit.json"));
        }

        beginTest ("Cancelling stops a load");
        {
            KitInfo info;
            juce::String error;
            KitLoader::Options options;
            options.shouldAbort = [] { return true; };
            auto kit = KitLoader::loadKit (testKitFolder, MidiMap::generalMidi(), info, error, options);
            expect (kit == nullptr);
        }

        beginTest ("Muted mics can be left out, and unloaded and reloaded later");
        {
            // A loader with a simulated audio thread: this thread plays the
            // audio thread's part of the handoff.
            const auto snareTop = shared().mic ("snareTop");
            const auto everything = shared().info.memoryBytes;
            const auto withoutSnareTop = allMics & ~(1u << snareTop);

            KitHandoff handoff;
            KitLoader loader (handoff, MidiMap::generalMidi());
            loader.setWantedMics (withoutSnareTop);   // muted, so it stays out until it's wanted

            KitLoader::Request request;
            request.folder = testKitFolder;
            request.mics = withoutSnareTop;   // as if the mic were muted in a restored project
            loader.requestLoad (request);

            auto runAudioThreadUntil = [&] (std::function<bool()> done)
            {
                for (int i = 0; i < 400 && ! done(); ++i)
                {
                    if (handoff.pollIncoming())
                        handoff.commitIncoming();

                    handoff.applyMicUpdates();
                    juce::Thread::sleep (10);
                }

                return done();
            };

            expect (runAudioThreadUntil ([&] { return handoff.getCurrent() != nullptr; }), "kit never arrived");

            if (handoff.getCurrent() == nullptr)
                return;

            auto& source = *handoff.getCurrent()->samples;
            expect (! source.isMicLoaded (snareTop));
            expect (loader.getMemoryUsage() < everything);

            // Unmuting: the loader catches up with the wanted mics and reloads the snare top.
            loader.setWantedMics (allMics);
            expect (runAudioThreadUntil ([&] { return source.isMicLoaded (snareTop); }), "mic not reloaded");
            expect (runAudioThreadUntil ([&] { return loader.getMemoryUsage() == everything; }));

            // Unloading frees the store (on the loader thread) and the mic plays silence.
            loader.setWantedMics (withoutSnareTop);
            expect (runAudioThreadUntil ([&] { return ! source.isMicLoaded (snareTop); }), "mic not unloaded");
            expect (loader.getMemoryUsage() < everything);
        }
    }
};

//==============================================================================
class StorageTests final : public juce::UnitTest
{
public:
    StorageTests() : juce::UnitTest ("Sample storage", "Lithium") {}

    void runTest() override
    {
        beginTest ("16- and 24-bit samples round-trip exactly");

        for (const auto bits : { 16, 24 })
        {
            SampleLayout layout (bits);
            const auto id = layout.add (0, 2, 4);
            auto store = layout.createStore (0);

            const auto full = bits == 16 ? 32767 : 8388607;
            const int values[] { 0, 1, -1, full, -full - 1, 12345, -12345, full / 3 };
            auto* p = layout.getWritePointer (*store, id);

            for (const auto v : values)
            {
                SampleLayout::pack (p, layout.bytesPerSample, v);
                p += layout.bytesPerSample;
            }

            InMemorySampleSource source (std::make_shared<SampleLayout> (layout));
            source.attachStore (0, std::move (store));

            juce::AudioBuffer<float> out (2, 6);
            SampleCursor cursor;
            source.open (id, cursor);
            source.read (cursor, out.getArrayOfWritePointers(), 6);

            const auto scale = bits == 16 ? 32768.0f : 8388608.0f;

            for (int i = 0; i < 8; ++i)
                expectEquals (juce::roundToInt (out.getSample (i % 2, i / 2) * scale), values[i]);

            // Reading past the end gives silence.
            expectEquals (out.getSample (0, 5), 0.0f);
        }
    }
};

//==============================================================================
class EngineTests final : public juce::UnitTest
{
public:
    EngineTests() : juce::UnitTest ("Engine", "Lithium") {}

    void runTest() override
    {
        beginTest ("Round-robins never repeat back to back");
        {
            Kit kit;
            kit.numMics = 1;
            kit.hitSamples.resize (5);
            kit.zones.push_back ({ 1, 127, 1.0f, 0.0f, 0, 5 });
            kit.initialiseRoundRobins();
            juce::Random random (1);

            std::array<int, 5> counts {};
            int last = -1;

            for (int i = 0; i < 1000; ++i)
            {
                const auto hit = kit.nextRoundRobin (0, random);
                expect (hit != last, "immediate repeat at pick " + juce::String (i));
                ++counts[(size_t) hit];
                last = hit;
            }

            for (auto c : counts)
                expectEquals (c, 200);   // shuffle bags play every round-robin once per cycle
        }

        beginTest ("Harder hits pick louder zones");
        {
            EngineRig rig;
            float previous = 0.0f;

            for (const auto velocity : { 25, 75, 120 })
            {
                rig.note (36, velocity);
                rig.render (4096);
                const auto p = rig.peakOf ("kickOut");
                expect (p > previous * 1.3f, "velocity " + juce::String (velocity) + " not louder");
                previous = p;
                rig.engine.stopAll (rig.kit);
            }
        }

        beginTest ("Velocity crossfade plays both neighbouring zones near a boundary");
        {
            EngineRig rig;
            rig.params.velocityCrossfade = true;
            rig.note (38, 20);
            expectEquals (rig.voices(), 1);
            rig.engine.stopAll (rig.kit);

            rig.note (38, 51);   // zones meet between 50 and 51
            expectEquals (rig.voices(), 2);
            rig.engine.stopAll (rig.kit);

            rig.params.velocityCrossfade = false;
            rig.note (38, 51);
            expectEquals (rig.voices(), 1);
        }

        beginTest ("All mics of a hit play sample-aligned");
        {
            EngineRig rig;
            rig.note (38, 127);
            rig.render (2048);

            // At unity pitch, every mic's output must be its recording times one
            // common gain (and its default pan), sample for sample.
            const auto& kit = rig.kit;
            const auto& art = kit.articulations[(size_t) kit.noteMap[38].articulation];
            const auto& zone = kit.zones[(size_t) (art.firstZone + art.numZones - 1)];
            float commonGain = -1.0f;
            bool aligned = true;

            auto readMic = [&] (int hit, int mic)
            {
                juce::AudioBuffer<float> recording (2, 2048);
                SampleCursor cursor;
                kit.samples->open (kit.getSample (hit, mic), cursor);
                kit.samples->read (cursor, recording.getArrayOfWritePointers(), 2048);
                return recording;
            };

            const auto snareTop = shared().mic ("snareTop");

            for (int rr = 0; rr < zone.numHits && commonGain < 0.0f; ++rr)
            {
                const auto hit = zone.firstHit + rr;
                const auto recording = readMic (hit, snareTop);
                const auto* out = rig.bus.getReadPointer (kit.mics[(size_t) snareTop].busChannel);
                const auto* rec = recording.getReadPointer (0);
                const auto i = (int) (std::max_element (rec, rec + 2048, [] (float a, float b) { return std::abs (a) < std::abs (b); }) - rec);
                const auto g = out[i] / rec[i];
                bool matches = true;

                for (int n = 0; n < 2048; ++n)
                    matches = matches && std::abs (out[n] - rec[n] * g) < 1.0e-5f;

                if (! matches)
                    continue;

                commonGain = g;

                for (const auto* id : { "snareBottom", "overheads", "room", "hihat" })
                {
                    const auto mic = shared().mic (id);
                    const auto other = readMic (hit, mic);
                    const auto* o = rig.bus.getReadPointer (kit.mics[(size_t) mic].busChannel);

                    for (int n = 0; n < 2048; ++n)
                        aligned = aligned && std::abs (o[n] - other.getSample (0, n) * commonGain) < 1.0e-5f;
                }
            }

            expect (commonGain > 0.0f, "close mic output didn't match either round-robin");
            expect (aligned, "mics are not sample-aligned");
        }

        beginTest ("Every mic channel is audible for the hits that use it");
        {
            EngineRig rig;

            for (const auto note : { 36, 38, 42, 48, 50, 41, 49, 51 })
                rig.note (note, 110);

            rig.render (8192);

            for (const auto& m : shared().info.mics)
                expect (rig.peakOf (m.id) > 0.001f, m.id + " is silent");
        }

        beginTest ("Closing the hi-hat chokes the open hi-hat");
        {
            EngineRig rig;
            rig.note (46);
            rig.render (4410);
            expectEquals (rig.voices(), 1);

            rig.note (42);           // closed
            expectEquals (rig.voices(), 2);
            rig.render (2205);       // 50 ms: the 30 ms choke fade is done
            expectEquals (rig.voices(), 1);

            rig.engine.stopAll (rig.kit);
            rig.note (46);
            rig.render (4410);
            rig.note (44);           // pedal
            rig.render (2205);
            expectEquals (rig.voices(), 1);

            rig.engine.stopAll (rig.kit);
            rig.note (46);
            rig.note (46);           // open doesn't choke open
            rig.render (2205);
            expectEquals (rig.voices(), 2);
        }

        beginTest ("Cymbals are choked by their choke note and by poly aftertouch");
        {
            EngineRig rig;          // choke notes come from the kit: crash 85, ride 86, china 87
            rig.note (49);
            rig.render (4410);
            expectEquals (rig.voices(), 1);
            rig.note (85);
            rig.render (4410);
            expectEquals (rig.voices(), 0);

            rig.note (51);
            rig.render (4410);
            rig.engine.polyAftertouch (rig.kit, 51, 127);
            rig.render (4410);
            expectEquals (rig.voices(), 0);
        }

        beginTest ("Chokes fade rather than cut");
        {
            EngineRig rig;
            rig.note (46, 127);
            rig.render (4410);
            rig.note (42, 1);      // a quiet closed hat chokes the loud open one...
            rig.render (256);      // ...but 6 ms later the open hat is still fading out
            expectEquals (rig.voices(), 2);
            rig.render (2048);
            expectEquals (rig.voices(), 1);
        }

        beginTest ("Tune changes pitch by resampling");
        {
            EngineRig rig;
            rig.note (50);
            const auto natural = rig.renderUntilSilent();

            rig.piece ("tom1").tuneSemitones = 12.0f;
            rig.note (50);
            const auto octaveUp = rig.renderUntilSilent();

            rig.piece ("tom1").tuneSemitones = -12.0f;
            rig.note (50);
            const auto octaveDown = rig.renderUntilSilent();

            expectWithinAbsoluteError ((double) octaveUp / natural, 0.5, 0.05);
            expectWithinAbsoluteError ((double) octaveDown / natural, 2.0, 0.1);
        }

        beginTest ("Decay shortens the envelope");
        {
            EngineRig rig;
            rig.note (49);
            const auto natural = rig.renderUntilSilent();

            rig.piece ("crash").decay = 0.0f;
            rig.note (49);
            const auto shortest = rig.renderUntilSilent();
            expect (shortest < natural / 4, "decay 0 should be far shorter than natural");
        }

        beginTest ("Piece mute, solo and mic sends");
        {
            EngineRig rig;
            rig.piece ("snare").mute = true;
            rig.note (38);
            rig.render (4096);
            expectEquals (rig.peakOf ("snareTop"), 0.0f);
            rig.engine.stopAll (rig.kit);

            rig.piece ("snare").mute = false;
            rig.piece ("kick").solo = true;
            rig.note (38);
            rig.render (4096);
            expectEquals (rig.peakOf ("snareTop"), 0.0f);
            rig.engine.stopAll (rig.kit);

            rig.piece ("kick").solo = false;
            rig.piece ("snare").sends[(size_t) MicRole::overheads] = 0.0f;
            rig.note (38);
            rig.render (4096);
            expect (rig.peakOf ("snareTop") > 0.01f);
            expectEquals (rig.peakOf ("overheads"), 0.0f);
        }

        beginTest ("Voice stealing keeps the pool bounded");
        {
            EngineRig rig;

            for (int i = 0; i < 400; ++i)
            {
                rig.note (i % 2 == 0 ? 49 : 51, 100);
                rig.note (38, 100);
                rig.note (36, 100);
                rig.render (32);
                expect (rig.voices() <= DrumEngine::numVoices);
            }

            rig.renderUntilSilent();
            expectEquals (rig.voices(), 0);
        }

        beginTest ("Host sample rates other than the kit's");
        {
            EngineRig rig (96000.0);
            rig.note (50);
            const auto at96 = rig.renderUntilSilent();
            EngineRig rig44;
            rig44.note (50);
            const auto at44 = rig44.renderUntilSilent();
            expectWithinAbsoluteError ((double) at96 / at44, 96000.0 / 44100.0, 0.1);
        }

        beginTest ("The mixer applies the kit's default level, pan and polarity");
        {
            EngineRig rig;
            Mixer mixer;
            mixer.prepare (sampleRate, 4096);

            Kit kit;   // a one-mic kit, to look at the mixer on its own
            kit.numMics = 1;
            kit.mics[0] = { 1, MicRole::close, 0, -0.5f, 1.0f };   // half level, inverted, hard right

            juce::AudioBuffer<float> out (2, 4096);
            EngineParams params;

            // Let the mixer's 20 ms gain smoothing reach its target, then send an impulse.
            mixer.getMicBus().clear();
            mixer.process (kit, 4096, params, { out.getWritePointer (0), out.getWritePointer (1) }, {});

            out.clear();
            mixer.getMicBus().setSample (0, 0, 1.0f);
            mixer.process (kit, 64, params, { out.getWritePointer (0), out.getWritePointer (1) }, {});

            expectWithinAbsoluteError (out.getSample (0, 0), 0.0f, 1.0e-6f);
            expect (out.getSample (1, 0) < -0.5f, "right channel should carry the inverted signal");
        }
    }
};

//==============================================================================
class ImporterTests final : public juce::UnitTest
{
public:
    ImporterTests() : juce::UnitTest ("DrumGizmo importer", "Lithium") {}

    void runTest() override
    {
        beginTest ("Power values become contiguous velocity zones");
        {
            std::vector<double> powers;

            for (int i = 0; i < 30; ++i)
                powers.push_back (0.02 + 0.98 * std::pow ((double) ((i * 7) % 30) / 29.0, 2.0));   // unsorted, uneven

            const auto zones = importer::makeZones (powers, 8, 3);
            expectEquals ((int) zones.size(), 8);
            expectEquals (zones.front().velocityLow, 1);
            expectEquals (zones.back().velocityHigh, 127);

            int total = 0;
            double lastPower = -1.0;

            for (size_t z = 0; z < zones.size(); ++z)
            {
                expect (zones[z].samples.size() >= 3);
                expect (zones[z].velocityHigh >= zones[z].velocityLow);

                if (z > 0)
                    expectEquals (zones[z].velocityLow, zones[z - 1].velocityHigh + 1);

                for (const auto s : zones[z].samples)
                {
                    expect (powers[(size_t) s] >= lastPower, "zones must hold samples in power order");
                    lastPower = powers[(size_t) s];
                }

                total += (int) zones[z].samples.size();
            }

            expectEquals (total, 30);

            // Too few samples for the minimum: one zone with all of them.
            const auto few = importer::makeZones ({ 0.3, 0.1 }, 8, 3);
            expectEquals ((int) few.size(), 1);
            expectEquals ((int) few[0].samples.size(), 2);
            expectEquals (few[0].samples[0], 1);   // quietest first
        }

        beginTest ("Imports a DrumGizmo kit with its licence, chokes, MIDI map and audio");
        {
            const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("lithium-importer-test-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
            const auto source = temp.getChildFile ("TinyKit");
            const auto kits = temp.getChildFile ("Kits");
            makeTinyDrumGizmoKit (source);

            importer::Options options;
            options.source = source;
            options.kitsFolder = kits;
            options.maxZones = 2;
            options.minRoundRobins = 2;

            std::vector<importer::ImportedKit> results;
            const auto r = importer::importKits (options, results);
            expect (r.wasOk(), r.getErrorMessage());

            if (r.failed() || results.empty())
            {
                temp.deleteRecursively();
                return;
            }

            const auto folder = results[0].folder;
            expect (folder.getChildFile ("LICENSE.txt").loadFileAsString().contains ("CC BY 4.0"));
            expect (folder.getChildFile ("Original README.md").existsAsFile());

            KitManifest m;
            const auto parsed = KitManifest::parse (folder, m);
            expect (parsed.wasOk(), parsed.getErrorMessage());
            expectEquals (m.credits.name, juce::String ("TinyKit"));
            expectEquals (m.credits.licenseName, juce::String ("CC BY 4.0"));

            // Mics: the overheads pair up (audience side: the drummer's right on the left).
            const auto oh = m.findMic ("oh");
            expect (oh >= 0 && m.mics[(size_t) oh].channels == 2 && m.mics[(size_t) oh].role == MicRole::overheads);
            expect (m.findMic ("sntop") >= 0);

            // Pieces: the hi-hat group became one piece; open is choked by closed.
            const auto hihat = m.findPiece ("hihat");
            expect (hihat >= 0);

            if (hihat >= 0)
            {
                const auto& arts = m.pieces[(size_t) hihat].articulations;
                expectEquals ((int) arts.size(), 2);
                expect (arts[0].chokes.contains (arts[1].chokeGroup) || arts[1].chokes.contains (arts[0].chokeGroup));
            }

            const auto snare = m.findPiece ("snare");
            expect (snare >= 0 && m.pieces[(size_t) snare].role == PieceRole::snare);

            if (snare >= 0)
                expectEquals ((int) m.pieces[(size_t) snare].articulations[0].zones.size(), 2);

            expectEquals ((int) m.midiMap.size(), 3);

            // It loads and plays, and 16-bit audio is stored exactly as it came.
            KitInfo info;
            juce::String error;
            auto kit = KitLoader::loadKit (folder, MidiMap::generalMidi(), info, error);
            expect (kit != nullptr, error);

            if (kit != nullptr)
            {
                expectEquals (info.bitsPerSample, 16);

                const auto& art = kit->articulations[(size_t) kit->noteMap[38].articulation];
                const auto& zone = kit->zones[(size_t) art.firstZone];
                const auto hit = zone.firstHit;
                const auto mic = m.findMic ("sntop");

                juce::AudioBuffer<float> got (1, 64);
                SampleCursor cursor;
                kit->samples->open (kit->getSample (hit, mic), cursor);
                kit->samples->read (cursor, got.getArrayOfWritePointers(), 64);

                // The quietest snare sample has its snare-top channel on file channel 3, at 0.1 * 2^-k.
                bool exact = true;

                for (int i = 0; i < 64; ++i)
                    exact = exact && std::abs (got.getSample (0, i) - tinySample (0, 3, i)) < 1.0e-6f;

                expect (exact, "16-bit audio should load bit-exact");
            }

            temp.deleteRecursively();
        }
    }

private:
    // A recognisable value per (sample, file channel, frame), exactly
    // representable in 16 bits.
    static float tinySample (int sampleIndex, int fileChannel, int frame)
    {
        return (float) (((sampleIndex + 1) * 1000 + fileChannel * 100 + frame) % 16000) / 32768.0f;
    }

    static void makeTinyDrumGizmoKit (const juce::File& root)
    {
        root.createDirectory();

        // The older drumkit format, as DRSKit and MuldjordKit use. The file's
        // channel order differs from the kit's, as in the real kits.
        root.getChildFile ("TinyKit.xml").replaceWithText (R"(<?xml version="1.0" encoding="UTF-8"?>
<drumkit name="TinyKit" description="A test kit">
  <channels>
    <channel name="OHL"/><channel name="OHR"/><channel name="SnTop"/><channel name="Trigger"/>
  </channels>
  <instruments>
    <instrument name="Snare" file="Snare/Snare.xml">
      <channelmap in="OHL" out="OHL"/><channelmap in="OHR" out="OHR"/><channelmap in="SnTop" out="SnTop" main="true"/>
    </instrument>
    <instrument name="HihatClosed" group="hihat" file="HihatClosed/HihatClosed.xml"/>
    <instrument name="HihatOpen" group="hihat" file="HihatOpen/HihatOpen.xml"/>
  </instruments>
</drumkit>
)");

        root.getChildFile ("Midimap.xml").replaceWithText (R"(<?xml version="1.0" encoding="UTF-8"?>
<midimap><map note="38" instr="Snare"/><map note="42" instr="HihatClosed"/><map note="46" instr="HihatOpen"/></midimap>
)");

        root.getChildFile ("README.md").replaceWithText ("TinyKit is released under the Creative Commons Attribution 4.0 license.\n");

        int sampleIndex = 0;

        for (const auto* instrument : { "Snare", "HihatClosed", "HihatOpen" })
        {
            const auto folder = root.getChildFile (instrument);
            juce::String samples;
            const double powers[] { 0.1, 0.15, 0.8, 0.9 };

            for (int s = 0; s < 4; ++s)
            {
                const auto name = juce::String (s + 1) + "-" + instrument + ".wav";
                juce::AudioBuffer<float> audio (4, 2000);

                for (int ch = 0; ch < 4; ++ch)
                    for (int i = 0; i < 2000; ++i)
                        audio.setSample (ch, i, tinySample (sampleIndex, ch + 1, i));

                writeWav (folder.getChildFile ("samples/" + name), audio, 16);

                // File channel 1: OHR, 2: OHL, 3: SnTop, 4: Trigger.
                samples << "  <sample name=\"" << instrument << s << "\" power=\"" << powers[s] << "\">\n"
                        << "   <audiofile channel=\"OHR\" file=\"samples/" << name << "\" filechannel=\"1\"/>\n"
                        << "   <audiofile channel=\"OHL\" file=\"samples/" << name << "\" filechannel=\"2\"/>\n"
                        << "   <audiofile channel=\"SnTop\" file=\"samples/" << name << "\" filechannel=\"3\"/>\n"
                        << "   <audiofile channel=\"Trigger\" file=\"samples/" << name << "\" filechannel=\"4\"/>\n"
                        << "  </sample>\n";
                ++sampleIndex;
            }

            folder.getChildFile (juce::String (instrument) + ".xml")
                .replaceWithText ("<?xml version='1.0' encoding='UTF-8'?>\n<instrument version=\"2.0\" name=\""
                                  + juce::String (instrument) + "\">\n <samples>\n" + samples + " </samples>\n</instrument>\n");
        }
    }
};

//==============================================================================
class PresetTests final : public juce::UnitTest
{
public:
    PresetTests() : juce::UnitTest ("Kit presets and packs", "Lithium") {}

    void runTest() override
    {
        // A preset next to the generated test kit (presets find their base
        // kit by id, first in their own folder). Removed again at the end.
        const auto presetFolder = testKitFolder.getSiblingFile ("Preset Test");
        presetFolder.createDirectory();
        presetFolder.getChildFile ("kit.json").replaceWithText (R"({
            "formatVersion": 2, "type": "preset", "id": "preset-test", "name": "Preset Test",
            "base": "lithiumtestkit", "description": "Three pieces, tuned.",
            "pieces": [ { "piece": "snare", "articulations": ["rimshot", "center"], "tune": 3, "volumeDb": -6, "decay": 50 },
                        { "piece": "kick" },
                        { "piece": "tom1" },
                        { "piece": "tom1", "id": "tom_up", "name": "Tom Up", "tune": 5 } ],
            "mics": { "room": { "enabled": false }, "overheads": { "volumeDb": -3 } } })");

        beginTest ("A preset is its base kit with its own pieces, tuning and mics");
        {
            KitManifest base, m;
            expect (KitManifest::parse (testKitFolder, base).wasOk());
            const auto r = KitManifest::parse (presetFolder, m);
            expect (r.wasOk(), r.getErrorMessage());

            expect (m.isPreset);
            expectEquals (m.id, juce::String ("preset-test"));
            expectEquals (m.credits.name, juce::String ("Preset Test"));
            expectEquals (m.credits.basedOn, base.credits.name);
            expectEquals (m.credits.licenseName, base.credits.licenseName);
            expect (m.audioFolder == testKitFolder);

            expectEquals ((int) m.pieces.size(), 4);
            expectEquals (m.pieces[0].articulations[0].id, juce::String ("rimshot"));    // the preset's order
            expectEquals ((int) m.pieces[0].articulations.size(), 2);
            expectWithinAbsoluteError (m.pieces[0].tuneSemitones, 3.0f, 1.0e-6f);
            expectWithinAbsoluteError (m.pieces[0].decay, 0.5f, 1.0e-6f);
            expectEquals (m.pieces[3].id, juce::String ("tom_up"));

            // The room mic is gone, from the mic list and from every hit.
            expectEquals ((int) m.mics.size(), (int) base.mics.size() - 1);
            expect (m.findMic ("room") < 0);
            const auto oh = m.findMic ("overheads");
            expectWithinAbsoluteError (m.mics[(size_t) oh].volumeDb, -3.0f, 1.0e-6f);

            for (const auto& p : m.pieces)
                for (const auto& a : p.articulations)
                    for (const auto& z : a.zones)
                        for (const auto& h : z.roundRobins)
                            for (const auto& src : h.sources)
                                expect (src.mic < (int) m.mics.size() && ! src.file.containsIgnoreCase ("room"));
        }

        beginTest ("A preset loads only what it uses, and plays its tuning");
        {
            KitInfo info;
            juce::String error;
            auto kit = KitLoader::loadKit (presetFolder, MidiMap::generalMidi(), info, error);
            expect (kit != nullptr, error);

            if (kit != nullptr)
            {
                expect (info.isPreset);
                expect (info.folder == presetFolder);
                expect (info.memoryBytes < shared().info.memoryBytes / 2);
                expectWithinAbsoluteError (kit->pieces[0].defaultTune, 3.0f, 1.0e-6f);
                expectWithinAbsoluteError (kit->pieces[0].defaultGain, juce::Decibels::decibelsToGain (-6.0f), 1.0e-4f);
                expectWithinAbsoluteError (kit->pieces[0].defaultDecay, 0.5f, 1.0e-6f);

                // The same tom, tuned up five semitones, plays about 2^(-5/12) as long.
                EngineFeedback feedback;
                DrumEngine engine (feedback);
                engine.prepare (sampleRate);
                EngineParams params;
                params.resetForKit (*kit);
                params.velocityCrossfade = false;
                juce::AudioBuffer<float> bus (maxMicBusChannels, 512);

                auto lengthOf = [&] (int piece)
                {
                    engine.trigger (*kit, kit->pieces[(size_t) piece].firstArticulation, 100, params);
                    int n = 0;

                    for (; engine.getNumActiveVoices() > 0 && n < 44100 * 10; n += 512)
                    {
                        bus.clear();
                        engine.render (*kit, bus, 0, 512, params);
                    }

                    return n;
                };

                const auto natural = lengthOf (2), tuned = lengthOf (3);
                expectWithinAbsoluteError ((double) tuned / natural, std::exp2 (-5.0 / 12.0), 0.06);

                // A piece's kit level is applied under its Volume parameter.
                kit->pieces[1].defaultGain = 0.0f;
                engine.trigger (*kit, kit->pieces[1].firstArticulation, 100, params);
                bus.clear();
                engine.render (*kit, bus, 0, 512, params);
                expectEquals (bus.getMagnitude (0, 512), 0.0f);
                engine.stopAll (*kit);
            }
        }

        beginTest ("Presets fail clearly without their base kit");
        {
            KitManifest m;
            const auto r = KitManifest::parse (R"({ "formatVersion": 2, "type": "preset", "name": "P", "base": "no-such-kit" })",
                                               presetFolder, m);
            expect (r.failed() && r.getErrorMessage().contains ("no-such-kit"), r.getErrorMessage());

            const auto bad = KitManifest::parse (R"({ "formatVersion": 2, "type": "preset", "name": "P", "base": "lithiumtestkit",
                                                      "pieces": [ { "piece": "cowbell" } ] })", presetFolder, m);
            expect (bad.failed() && bad.getErrorMessage().contains ("cowbell"), bad.getErrorMessage());
        }

        presetFolder.deleteRecursively();

        beginTest ("The library finds kits in content packs, and finds moved kits by id");
        {
            const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("lithium-library-test-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
            const auto pack = temp.getChildFile ("Pack");
            pack.createDirectory();
            pack.getChildFile ("pack.json").replaceWithText (R"({ "formatVersion": 1, "id": "pack", "name": "A Pack",
                "license": { "name": "CC BY-SA 3.0", "file": "LICENSE.txt" } })");

            const juce::String kitJson = R"({ "formatVersion": 2, "id": "base-kit", "name": "Base", "sampleRate": 48000,
                "license": { "name": "CC BY-SA 3.0" }, "stats": { "ramBytes": 1234 },
                "mics": [ { "id": "m" } ],
                "pieces": [ { "id": "s", "role": "snare", "articulations": [ { "id": "c", "zones": [
                    { "velocity": [1, 127], "roundRobins": [ { "files": { "m": "a.wav" } } ] } ] } ] } ] })";
            pack.getChildFile ("Base").createDirectory();
            pack.getChildFile ("Base/kit.json").replaceWithText (kitJson);
            pack.getChildFile ("Pre").createDirectory();
            pack.getChildFile ("Pre/kit.json").replaceWithText (R"({ "formatVersion": 2, "type": "preset", "name": "Pre", "base": "base-kit" })");
            temp.getChildFile ("Loose").createDirectory();
            temp.getChildFile ("Loose/kit.json").replaceWithText (kitJson.replace ("base-kit", "loose").replace ("\"Base\"", "\"Loose\""));

            const auto library = kitlibrary::scan (temp);
            expectEquals ((int) library.packs.size(), 1);
            expect (library.packs[0].shareAlike);
            expectEquals ((int) library.kits.size(), 3);
            expectEquals (library.kits[0].credits().name, juce::String ("Base"));   // the pack's kits, kit first
            expect (library.kits[1].summary.isPreset);
            expectEquals (library.kits[1].credits().basedOn, juce::String ("Base"));
            expect (library.kits[1].credits().shareAlike);
            expectEquals (library.kits[2].pack, -1);                               // loose kits last
            expectEquals ((int) library.kits[0].summary.ramBytes, 1234);

            expect (findKitById ("base-kit", pack.getChildFile ("Pre")) == pack.getChildFile ("Base"));
            expect (findKitById ("loose", pack.getChildFile ("Pre")) == temp.getChildFile ("Loose"));

            // A project saved when the kit was a loose folder finds it in its pack by id.
            const auto oldPath = temp.getChildFile ("Base").getFullPathName();
            expect (kitlibrary::resolveSavedKit (oldPath, "base-kit", temp) == pack.getChildFile ("Base"));

            temp.deleteRecursively();
        }
    }
};

//==============================================================================
class SfzTests final : public juce::UnitTest
{
public:
    SfzTests() : juce::UnitTest ("SFZ importer", "Lithium") {}

    void runTest() override
    {
        beginTest ("The parser resolves headers, #define, #include, note names and paths");
        {
            const auto temp = makeTemp();
            temp.getChildFile ("inc/defs.sfz").create();
            temp.getChildFile ("inc/defs.sfz").replaceWithText ("#define $KEY 38\n#define $KEYS 40 // comment\n");

            sfz::Instrument inst;
            const auto r = sfz::parse (R"(
                #include "inc/defs.sfz"
                <control> default_path=samples\ set_cc7=100
                <global> volume=-3
                <group> key=$KEY
                <region> sample=my snare.wav lovel=1 hivel=63
                <region> sample=b.wav lovel=64 volume=1 /* block
                comment */
                <group> lokey=c#4 hikey=$KEYS <region> sample=*silence
            )", temp.getChildFile ("kit.sfz"), inst);

            expect (r.wasOk(), r.getErrorMessage());
            expectEquals ((int) inst.regions.size(), 3);
            expectEquals (inst.regions[0].sample, juce::String ("samples/my snare.wav"));
            expectEquals (inst.regions[0].getNote ("lokey", -1), 38);
            expectEquals (inst.regions[0].getNumber ("volume", 0.0), -3.0);
            expectEquals (inst.regions[1].getNumber ("volume", 0.0), 1.0);   // the region's own value wins
            expect (inst.regions[2].isBuiltInSample());
            expectEquals (inst.regions[2].getNote ("lokey", -1), 61);
            expectEquals (inst.regions[2].getNote ("hikey", -1), 40);
            expectEquals (inst.controllerDefaults[7], 100.0);
            expectEquals (sfz::noteNumber ("c4"), 60);
            expectEquals (sfz::noteNumber ("a-1"), 9);
            expectEquals (sfz::noteNumber ("bb3"), 58);

            temp.deleteRecursively();
        }

        beginTest ("Velocity curves follow amp_veltrack and amp_velcurve_N");
        {
            sfz::Region r;
            expectWithinAbsoluteError (importer::velocityGain (r, 127), 1.0, 1.0e-9);
            expectWithinAbsoluteError (importer::velocityGain (r, 64), (64.0 / 127.0) * (64.0 / 127.0), 1.0e-9);

            r.opcodes["amp_veltrack"] = "0";
            expectWithinAbsoluteError (importer::velocityGain (r, 1), 1.0, 1.0e-9);

            r.opcodes["amp_veltrack"] = "100";
            r.opcodes["amp_velcurve_63"] = "0.8";
            expectWithinAbsoluteError (importer::velocityGain (r, 63), 0.8, 1.0e-9);
            expectWithinAbsoluteError (importer::velocityGain (r, 95), 0.9, 1.0e-9);   // halfway from (63, 0.8) to (127, 1)
        }

        beginTest ("Imports a multi-mic SFZ kit: zones, round-robins, offsets, chokes and presets");
        {
            const auto temp = makeTemp();
            const auto source = temp.getChildFile ("TinySfz");
            makeTinySfzKit (source);

            importer::Options options;
            options.source = source;
            options.kitsFolder = temp.getChildFile ("Kits");
            options.mappingFile = source.getChildFile ("mapping.json");

            std::vector<importer::ImportedKit> results;
            const auto r = importer::importKits (options, results);
            expect (r.wasOk(), r.getErrorMessage());

            if (r.failed() || results.size() != 2)
            {
                temp.deleteRecursively();
                return;
            }

            const auto packFolder = options.kitsFolder.getChildFile ("Tiny SFZ Pack");
            expect (packFolder.getChildFile ("pack.json").existsAsFile());
            expect (packFolder.getChildFile ("LICENSE.txt").loadFileAsString().contains ("CC0"));
            expect (results[1].isPreset);

            KitManifest m;
            const auto parsed = KitManifest::parse (results[0].folder, m);
            expect (parsed.wasOk(), parsed.getErrorMessage());
            expectEquals ((int) m.mics.size(), 2);

            const auto& snare = m.pieces[(size_t) m.findPiece ("snare")];
            const auto& zones = snare.articulations[0].zones;
            expectEquals ((int) zones.size(), 2);                  // 1-63 and 64-127, from the close mic's layers
            expectEquals (zones[0].velocityHigh, 63);
            expectEquals ((int) zones[0].roundRobins.size(), 2);   // seq_position 1 and 2
            expectWithinAbsoluteError (zones[0].gainDb, -3.0f, 0.01f);
            expectWithinAbsoluteError (zones[0].velocityRangeDb, 0.0f, 0.01f);   // amp_veltrack=0

            // Each hit plays both mics: the close mic's layer and the overhead's
            // round-robin, which starts 100 frames into its file. The CC-gated
            // "epic" region is left out.
            const auto& hit = zones[0].roundRobins[0];
            expectEquals ((int) hit.sources.size(), 2);
            const auto oh = m.findMic ("oh");

            for (const auto& src : hit.sources)
            {
                expect (! src.file.contains ("epic"));
                expectEquals ((int) src.start, src.mic == oh ? 100 : 0);
            }

            // Chokes from SFZ groups: closed and the pedal (through its silence
            // region) choke the open hi-hat.
            const auto& hh = m.pieces[(size_t) m.findPiece ("hihat")].articulations;
            expectEquals ((int) hh.size(), 3);
            expect (hh[1].chokeGroup.isNotEmpty() && hh[0].chokes.contains (hh[1].chokeGroup) && hh[2].chokes.contains (hh[1].chokeGroup));
            expectEquals ((int) m.midiMap.size(), 4);

            // It loads, the overhead sample really starts at frame 100, and the preset drops the close mic.
            KitInfo info;
            juce::String error;
            auto kit = KitLoader::loadKit (results[0].folder, MidiMap::generalMidi(), info, error);
            expect (kit != nullptr, error);

            if (kit != nullptr)
            {
                const auto& art = kit->articulations[(size_t) kit->noteMap[38].articulation];
                const auto& zone = kit->zones[(size_t) art.firstZone];
                juce::AudioBuffer<float> got (2, 8);
                SampleCursor cursor;
                kit->samples->open (kit->getSample (zone.firstHit, oh), cursor);
                kit->samples->read (cursor, got.getArrayOfWritePointers(), 8);
                expectWithinAbsoluteError (got.getSample (0, 0), rampValue (100), 1.0e-4f);
                expectWithinAbsoluteError (got.getSample (0, 5), rampValue (105), 1.0e-4f);
            }

            KitInfo presetInfo;
            auto preset = KitLoader::loadKit (results[1].folder, MidiMap::generalMidi(), presetInfo, error);
            expect (preset != nullptr, error);
            expectEquals ((int) presetInfo.mics.size(), 1);
            expect (presetInfo.memoryBytes < info.memoryBytes);

            temp.deleteRecursively();
        }
    }

private:
    static juce::File makeTemp()
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("lithium-sfz-test-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
    }

    // A ramp exactly representable in 16 bits, so the offset is easy to check.
    static float rampValue (int frame)   { return (float) (frame % 2000) / 32768.0f; }

    static void writeRamp (const juce::File& file, int channels)
    {
        juce::AudioBuffer<float> audio (channels, 2000);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < 2000; ++i)
                audio.setSample (ch, i, rampValue (i) * (ch == 0 ? 1.0f : 0.5f));

        writeWav (file, audio, 16);
    }

    static void makeTinySfzKit (const juce::File& root)
    {
        root.createDirectory();

        for (const auto* name : { "close/sn_lo_1.wav", "close/sn_lo_2.wav", "close/sn_hi_1.wav", "close/sn_hi_2.wav",
                                  "close/sn_epic.wav", "close/hh_closed.wav", "close/hh_open.wav", "close/hh_pedal.wav" })
            writeRamp (root.getChildFile ("samples/" + juce::String (name)), 1);

        for (const auto* name : { "oh/sn_1.wav", "oh/sn_2.wav" })
            writeRamp (root.getChildFile ("samples/" + juce::String (name)), 2);

        root.getChildFile ("inc/hats.sfz").create();
        root.getChildFile ("inc/hats.sfz").replaceWithText (R"(
            #define $HH_CLOSED 42
            <group> group=1 key=$HH_CLOSED <region> sample=close/hh_closed.wav
            <group> off_by=1 key=46 <region> sample=close/hh_open.wav
            <group> key=44 <region> sample=close/hh_pedal.wav <region> sample=*silence group=1
        )");

        root.getChildFile ("kit.sfz").replaceWithText (R"(
            <control> default_path=samples/ set_cc20=127
            #define $SNARE 38
            #include "inc/hats.sfz"
            <global> amp_veltrack=0 volume=-3
            <group> key=$SNARE seq_length=2
            <region> sample=close/sn_lo_1.wav lovel=1 hivel=63 seq_position=1 locc20=1
            <region> sample=close/sn_lo_2.wav lovel=1 hivel=63 seq_position=2 locc20=1
            <region> sample=close/sn_hi_1.wav lovel=64 hivel=127 seq_position=1 locc20=1
            <region> sample=close/sn_hi_2.wav lovel=64 hivel=127 seq_position=2 locc20=1
            <region> sample=oh/sn_1.wav seq_position=1 offset=100
            <region> sample=oh/sn_2.wav seq_position=2 offset=100
            <region> sample=close/sn_epic.wav locc21=1
        )");

        root.getChildFile ("mapping.json").replaceWithText (R"({
            "format": "sfz",
            "pack": { "id": "tiny-sfz", "name": "Tiny SFZ Pack" },
            "author": "The tests", "license": { "name": "CC0 1.0", "url": "https://creativecommons.org/publicdomain/zero/1.0/" },
            "mics": [ { "id": "close", "name": "Close", "samples": ["*/close/*"] },
                      { "id": "oh", "name": "Overheads", "role": "overheads", "samples": ["*/oh/*"] } ],
            "pieces": [
                { "id": "snare", "name": "Snare", "role": "snare", "articulations": [ { "id": "center", "name": "Center", "key": 38 } ] },
                { "id": "hihat", "name": "Hi-Hat", "role": "hihat", "articulations": [
                    { "id": "closed", "key": 42 }, { "id": "open", "key": 46 }, { "id": "pedal", "key": 44 } ] } ],
            "kits": [ { "file": "kit.sfz", "id": "tiny-sfz", "name": "Tiny SFZ" } ],
            "presets": [ { "id": "tiny-oh", "name": "Tiny OH", "base": "tiny-sfz", "mics": { "close": { "enabled": false } } } ]
        })");
    }
};

//==============================================================================
class StreamingTests final : public juce::UnitTest
{
public:
    StreamingTests() : juce::UnitTest ("Streaming", "Lithium") {}

    static juce::File testCache()   { return juce::File (LITHIUM_TEST_KITS_FOLDER).getSiblingFile ("test-cache").getChildFile ("unit"); }

    struct StreamingKit
    {
        std::unique_ptr<Kit> kit;
        KitInfo info;
        std::shared_ptr<const LoadPlan> plan;
        std::shared_ptr<StreamStats> stats = std::make_shared<StreamStats>();
        juce::String error;

        StreamingSampleSource& source()   { return static_cast<StreamingSampleSource&> (*kit->samples); }
    };

    static StreamingKit loadStreaming (const juce::File& folder, const juce::File& cache, MicMask mics = allMics)
    {
        StreamingKit k;
        KitLoader::Options options;
        options.mics = mics;
        options.streaming.mode = StreamingMode::always;
        options.streaming.cacheFolder = cache;
        options.streaming.preloadFrames = 4096;    // small, so most of each test sample streams
        options.streamStats = k.stats;
        k.kit = KitLoader::loadKit (folder, MidiMap::generalMidi(), k.info, k.error, options, &k.plan);
        return k;
    }

    /** The samples that stream furthest past their preload, longest first. */
    static std::vector<SampleId> longSamples (const SampleLayout& layout, size_t count)
    {
        std::vector<SampleId> ids;

        for (SampleId id = 0; id < (SampleId) layout.samples.size(); ++id)
            if (layout.samples[id].numFrames > layout.samples[id].storedFrames + 2 * StreamingSampleSource::ringFrames)
                ids.push_back (id);

        std::sort (ids.begin(), ids.end(), [&] (auto a, auto b) { return layout.samples[a].numFrames > layout.samples[b].numFrames; });
        ids.resize (std::min (ids.size(), count));
        return ids;
    }

    /** Reads a whole sample (or its first maxFrames) from a source in 512-frame blocks. */
    static juce::AudioBuffer<float> readAll (SampleSource& source, SampleId id, std::int64_t maxFrames = 1 << 30)
    {
        const auto n = (int) std::min (source.getNumFrames (id), maxFrames);
        juce::AudioBuffer<float> out (source.getNumChannels (id), n);
        SampleCursor cursor;
        source.open (id, cursor);

        for (int pos = 0; pos < n; pos += 512)
        {
            std::array<float*, 2> dest { out.getWritePointer (0, pos), out.getWritePointer (out.getNumChannels() - 1, pos) };
            source.read (cursor, dest.data(), std::min (512, n - pos));
        }

        source.close (cursor);
        return out;
    }

    void runTest() override
    {
        auto& ram = *shared().kit;

        beginTest ("A streaming kit keeps only its preloads in RAM and caches its audio once");
        {
            testCache().deleteRecursively();
            auto k = loadStreaming (testKitFolder, testCache());
            expect (k.kit != nullptr, k.error);

            if (k.kit == nullptr)
                return;

            expect (k.info.streaming);
            expect (k.kit->samples->isStreaming());
            expectEquals (k.info.fullMemoryBytes, shared().info.memoryBytes);
            expect (k.info.memoryBytes - k.info.streamBufferBytes < k.info.fullMemoryBytes / 2,
                    "preloads should be a fraction of the kit");

            const auto pack = streamcache::Pack::open (testCache(), k.plan->folder);
            const auto dataSize = pack.getDataFile().getSize();
            expect (dataSize > 0);

            for (const auto& f : k.plan->files)
                expect (pack.find (f.path) != nullptr, f.path);

            // Loading again reads the cache rather than adding to it.
            auto again = loadStreaming (testKitFolder, testCache());
            expect (again.kit != nullptr, again.error);
            expectEquals (pack.getDataFile().getSize(), dataSize);
        }

        auto k = loadStreaming (testKitFolder, testCache());

        if (k.kit == nullptr)
            return;

        auto& source = k.source();
        const auto ids = longSamples (source.getLayout(), 12);
        expect (ids.size() >= 4, "the test kit needs samples longer than preload + ring");

        beginTest ("Streamed audio is bit-identical to the same audio held in RAM");
        {
            // Twelve voices at once, read a little faster than real time
            // (512 frames per 4 ms), which the disk thread easily keeps up with.
            constexpr std::int64_t maxFrames = 3 * 44100;
            std::vector<juce::AudioBuffer<float>> streamed;
            std::vector<SampleCursor> cursors (ids.size());

            for (size_t v = 0; v < ids.size(); ++v)
            {
                streamed.emplace_back (source.getNumChannels (ids[v]), (int) std::min (source.getNumFrames (ids[v]), maxFrames));
                source.open (ids[v], cursors[v]);
                expect (cursors[v].streamSlot >= 0);
            }

            for (int pos = 0; pos < (int) maxFrames; pos += 512)
            {
                for (size_t v = 0; v < ids.size(); ++v)
                {
                    auto& out = streamed[v];

                    if (pos >= out.getNumSamples())
                        continue;

                    std::array<float*, 2> dest { out.getWritePointer (0, pos), out.getWritePointer (out.getNumChannels() - 1, pos) };
                    source.read (cursors[v], dest.data(), std::min (512, out.getNumSamples() - pos));
                }

                juce::Thread::sleep (4);
            }

            for (auto& c : cursors)
                source.close (c);

            expectEquals ((int) k.stats->dropouts.load(), 0);
            expectEquals ((int) k.stats->activeStreams.load(), 0);
            expect (k.stats->bytesRead.load() > 0);

            for (size_t v = 0; v < ids.size(); ++v)
            {
                const auto reference = readAll (*ram.samples, ids[v], maxFrames);
                float maxDiff = 0.0f;

                for (int ch = 0; ch < reference.getNumChannels(); ++ch)
                    for (int i = 0; i < reference.getNumSamples(); ++i)
                        maxDiff = std::max (maxDiff, std::abs (reference.getSample (ch, i) - streamed[v].getSample (ch, i)));

                expectEquals (maxDiff, 0.0f);
            }
        }

        beginTest ("A voice that outruns the disk plays silence for the gap and stays in step");
        {
            // Reading a whole sample in one go (thousands of times faster than
            // real time) must run past what the disk thread has read ahead.
            const auto id = ids[0];
            const auto before = k.stats->dropouts.load();
            const auto burst = readAll (source, id);
            const auto reference = readAll (*ram.samples, id);
            expect (k.stats->dropouts.load() > before, "expected dropouts");

            // Every frame is either the right audio or silence: never audio from elsewhere.
            int wrong = 0, silent = 0;

            for (int i = 0; i < reference.getNumSamples(); ++i)
            {
                const auto got = burst.getSample (0, i);

                const auto expected = reference.getSample (0, i);

                if (juce::exactlyEqual (got, 0.0f) && ! juce::exactlyEqual (expected, 0.0f))
                    ++silent;
                else if (! juce::exactlyEqual (got, expected))
                    ++wrong;
            }

            expectEquals (wrong, 0);
            expect (silent > 0);

            // The preload itself always plays.
            for (int i = 0; i < 4096; ++i)
                if (! juce::exactlyEqual (burst.getSample (0, i), reference.getSample (0, i)))
                    ++wrong;

            expectEquals (wrong, 0);
        }

        beginTest ("Stream slots are reused safely");
        {
            // Many more opens than there are slots, closed straight away while
            // the disk thread may still be reading for them.
            for (int i = 0; i < 1000; ++i)
            {
                SampleCursor c;
                source.open (ids[(size_t) i % ids.size()], c);
                std::array<float, 512> a {}, b {};
                std::array<float*, 2> dest { a.data(), b.data() };
                source.read (c, dest.data(), 512);
                source.close (c);
            }

            expectEquals ((int) k.stats->activeStreams.load(), 0);

            // ... and a slot that was reused plays the right audio afterwards.
            const auto id = ids[1];
            juce::AudioBuffer<float> out (source.getNumChannels (id), 3 * 44100);
            SampleCursor c;
            const auto before = k.stats->dropouts.load();
            source.open (id, c);

            for (int pos = 0; pos < out.getNumSamples(); pos += 512)
            {
                std::array<float*, 2> dest { out.getWritePointer (0, pos), out.getWritePointer (out.getNumChannels() - 1, pos) };
                source.read (c, dest.data(), std::min (512, out.getNumSamples() - pos));
                juce::Thread::sleep (2);
            }

            source.close (c);
            const auto reference = readAll (*ram.samples, id, out.getNumSamples());
            expectEquals ((int) (k.stats->dropouts.load() - before), 0);

            float maxDiff = 0.0f;

            for (int i = 0; i < reference.getNumSamples(); ++i)
                maxDiff = std::max (maxDiff, std::abs (reference.getSample (0, i) - out.getSample (0, i)));

            expectEquals (maxDiff, 0.0f);
        }

        beginTest ("A muted mic's preload comes back from the cache when it's unmuted");
        {
            const auto snareTop = shared().mic ("snareTop");
            const auto bit = 1u << snareTop;
            auto muted = loadStreaming (testKitFolder, testCache(), allMics & ~bit);
            expect (muted.kit != nullptr, muted.error);

            if (muted.kit == nullptr)
                return;

            expect (! muted.source().isMicLoaded (snareTop));

            std::array<std::unique_ptr<MicStore>, maxMics> stores;
            juce::String error;
            expect (KitLoader::decode (*muted.plan, bit, stores, error), error);
            muted.source().swapStore (snareTop, std::move (stores[(size_t) snareTop]));   // this thread plays the audio thread

            int checked = 0;

            for (SampleId id = 0; id < (SampleId) muted.source().getLayout().samples.size() && checked < 4; ++id)
            {
                if (muted.source().getLayout().samples[id].mic != snareTop)
                    continue;

                const auto reference = readAll (*ram.samples, id, 4096);
                const auto reloaded = readAll (muted.source(), id, 4096);

                for (int i = 0; i < reference.getNumSamples(); ++i)
                    expectEquals (reloaded.getSample (0, i), reference.getSample (0, i));

                ++checked;
            }

            expect (checked > 0);
        }

        beginTest ("Changed source files are cached again, and the cache can be cleared");
        {
            const auto temp = juce::File::createTempFile ("lithium-stream-test");
            const auto kitCopy = temp.getChildFile ("kit");
            const auto cache = temp.getChildFile ("cache");
            expect (testKitFolder.copyDirectoryTo (kitCopy));

            auto first = loadStreaming (kitCopy, cache);
            expect (first.kit != nullptr, first.error);

            if (first.kit != nullptr)
            {
                const auto path = first.plan->files.front().path;   // a copy: the plan goes away below
                const auto pack = streamcache::Pack::open (cache, first.plan->folder);
                const auto size = pack.getDataFile().getSize();
                const auto entryBytes = pack.find (path)->offsetOf (pack.find (path)->numChannels, 0) - pack.find (path)->offset;

                // Touch one file: it no longer matches its entry.
                const auto touched = first.plan->folder.getChildFile (path);
                expect (touched.setLastModificationTime (touched.getLastModificationTime() + juce::RelativeTime::seconds (10)));
                expect (streamcache::Pack::open (cache, first.plan->folder).find (path) == nullptr);

                first = {};   // close the old kit's cache file before appending to it
                auto second = loadStreaming (kitCopy, cache);
                expect (second.kit != nullptr, second.error);

                const auto updated = streamcache::Pack::open (cache, second.plan->folder);
                expect (updated.find (path) != nullptr);
                expectEquals (updated.getDataFile().getSize(), size + entryBytes);

                // Clearing leaves packs that are in use alone.
                expectEquals (streamcache::clear (cache, { updated.getFolder() }), 0);
                second = {};
                expectEquals (streamcache::clear (cache, {}), 1);
                expectEquals (streamcache::getCacheSize (cache), (std::int64_t) 0);
            }

            temp.deleteRecursively();
        }
    }
};

//==============================================================================
class ProcessorTests final : public juce::UnitTest
{
public:
    ProcessorTests() : juce::UnitTest ("Processor", "Lithium") {}

    static void setParam (LithiumDrumsProcessor& p, const juce::String& id, float value)
    {
        auto* param = p.getParameters().getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    /** Kits load in the background; wait for the test kit to be playable.
        The message loop isn't running in the tests, so the load result is
        applied by hand. */
    bool waitForKit (LithiumDrumsProcessor& p)
    {
        juce::AudioBuffer<float> buffer (p.getTotalNumOutputChannels(), 512);

        for (int i = 0; i < 200; ++i)
        {
            p.applyPendingKitLoad();
            p.prepareToPlay (sampleRate, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (10, 38, (juce::uint8) 100), 0);
            p.processBlock (buffer, midi);

            if (buffer.getMagnitude (0, 0, 512) > 0.0f && p.getKitInfo() != nullptr)
                return true;

            juce::Thread::sleep (20);
        }

        return false;
    }

    void runTest() override
    {
        beginTest ("A new instance loads the default kit and plays MIDI");

        // The tests' kits folder holds only the synthetic kit (see TestMain).
        LithiumDrumsProcessor processor;
        expect (waitForKit (processor), "no sound from the test kit");
        expect (processor.getKitFolder() == testKitFolder);

        const auto& s = shared();

        beginTest ("Slot parameters are named after the kit's pieces and mics");
        {
            auto* p = processor.getParameters().getParameter (params::pieceParamId (s.piece ("snare"), "volume"));
            expect (p->getName (100).startsWith ("Lithium Brass Snare"), p->getName (100));
            auto* m = processor.getParameters().getParameter (params::micParamId (s.mic ("overheads"), "pan"));
            expectEquals (m->getName (100), juce::String ("Overheads Pan"));
        }

        beginTest ("processBlock doesn't allocate");
        {
            constexpr int blockSize = 256;
            juce::AudioBuffer<float> buffer (2, blockSize);

            // MIDI buffers allocate as events are added, so build them first.
            std::vector<juce::MidiBuffer> blocks (400);

            for (size_t i = 0; i < blocks.size(); ++i)
            {
                for (const auto note : { 36, 38, 42, 44, 46, 49, 51, 53, 52, 48, 45, 41, 85 })
                    if ((i + (size_t) note) % 5 == 0)
                        blocks[i].addEvent (juce::MidiMessage::noteOn (10, note, (juce::uint8) (20 + (i * 7) % 100)),
                                            (int) ((i * 31 + (size_t) note) % blockSize));

                if (i % 50 == 0)
                    blocks[i].addEvent (juce::MidiMessage::aftertouchChange (10, 49, 127), 10);
            }

            processor.prepareToPlay (sampleRate, blockSize);
            processor.audition (s.piece ("snare"), 90);
            processor.audition (s.piece ("ride"), 90);

            int allocations = 0;
            {
                test::ScopedAllocationCounter counter;

                for (auto& midi : blocks)
                    processor.processBlock (buffer, midi);

                allocations = counter.count();
            }

            expectEquals (allocations, 0);
        }

        beginTest ("processBlock doesn't allocate while a kit streams from disk");
        {
            test::setEnvironment ("LITHIUM_STREAMING", "always");
            LithiumDrumsProcessor streaming;
            expect (waitForKit (streaming), "no sound from the streaming test kit");
            expect (streaming.getKitInfo() != nullptr && streaming.getKitInfo()->streaming, "the kit should stream");

            constexpr int blockSize = 256;
            juce::AudioBuffer<float> buffer (2, blockSize);
            std::vector<juce::MidiBuffer> blocks (600);

            for (size_t i = 0; i < blocks.size(); ++i)
                for (const auto note : { 36, 38, 42, 46, 49, 51, 57 })
                    if ((i + (size_t) note) % 7 == 0)
                        blocks[i].addEvent (juce::MidiMessage::noteOn (10, note, (juce::uint8) 110), 17);

            streaming.prepareToPlay (sampleRate, blockSize);
            int allocations = 0;

            {
                test::ScopedAllocationCounter counter;

                // Roughly real time, so the disk thread has to keep up.
                for (auto& midi : blocks)
                {
                    streaming.processBlock (buffer, midi);
                    juce::Thread::sleep (3);
                }

                allocations = counter.count();
            }

            expectEquals (allocations, 0);
            expect (streaming.getStreamStats().bytesRead.load() > 0, "nothing was read from disk");
            expectEquals ((int) streaming.getStreamStats().dropouts.load(), 0);

            // Turning streaming off reloads the kit into RAM.
            test::setEnvironment ("LITHIUM_STREAMING", "never");
            streaming.reloadKit();

            for (int i = 0; i < 200 && (streaming.isKitLoading() || streaming.getKitInfo()->streaming); ++i)
            {
                juce::Thread::sleep (20);
                streaming.applyPendingKitLoad();
            }

            expect (! streaming.getKitInfo()->streaming, "the kit should be back in RAM");
        }

        beginTest ("Mics routed to a direct out leave the main mix");
        {
            auto layout = processor.getBusesLayout();

            for (int i = 1; i < layout.outputBuses.size(); ++i)
                layout.outputBuses.getReference (i) = juce::AudioChannelSet::stereo();

            expect (processor.setBusesLayout (layout));
            processor.prepareToPlay (sampleRate, 512);

            const auto snareTop = s.mic ("snareTop");
            setParam (processor, params::micParamId (snareTop, "solo"), 1.0f);
            setParam (processor, params::micParamId (snareTop, "output"), 1.0f);

            juce::AudioBuffer<float> buffer (processor.getTotalNumOutputChannels(), 512);
            juce::MidiBuffer midi;

            // Let the mixer's 20 ms gain smoothing settle on the new solo first.
            for (int i = 0; i < 3; ++i)
                processor.processBlock (buffer, midi);

            midi.addEvent (juce::MidiMessage::noteOn (10, 38, (juce::uint8) 120), 0);
            processor.processBlock (buffer, midi);

            const auto snareChannel = processor.getChannelIndexInProcessBlockBuffer (false, 1 + snareTop, 0);
            expectEquals (buffer.getMagnitude (0, 0, 512), 0.0f);
            expect (buffer.getMagnitude (snareChannel, 0, 512) > 0.0f);

            setParam (processor, params::micParamId (snareTop, "solo"), 0.0f);
            setParam (processor, params::micParamId (snareTop, "output"), 0.0f);
            layout = processor.getBusesLayout();

            for (int i = 1; i < layout.outputBuses.size(); ++i)
                layout.outputBuses.getReference (i) = juce::AudioChannelSet::disabled();

            expect (processor.setBusesLayout (layout));
        }

        beginTest ("Resetting for a kit puts everything back to neutral");
        {
            setParam (processor, params::micParamId (s.mic ("room"), "volume"), -20.0f);
            setParam (processor, params::pieceParamId (s.piece ("crash"), "choke_note"), -1.0f);
            params::resetForKit (processor.getParameters(), s.info);

            auto value = [&] (const juce::String& id) { return processor.getParameters().getRawParameterValue (id)->load(); };
            expectWithinAbsoluteError (value (params::micParamId (s.mic ("room"), "volume")), 0.0f, 0.01f);
            expectEquals (juce::roundToInt (value (params::pieceParamId (s.piece ("crash"), "choke_note"))), 85);
        }

        beginTest ("Switching kits while a cymbal rings doesn't click");
        {
            // A second kit: the test kit again, under another folder name.
            const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("lithium-switch-test-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
            temp.createDirectory();
            const auto other = temp.getChildFile ("Other Kit");
            expect (testKitFolder.createSymbolicLink (other, true));

            constexpr int blockSize = 256;
            processor.prepareToPlay (sampleRate, blockSize);
            juce::AudioBuffer<float> buffer (2, blockSize);
            float previous = 0.0f, largestStep = 0.0f;

            auto play = [&] (juce::MidiBuffer midi)
            {
                processor.processBlock (buffer, midi);
                float step = 0.0f;
                const auto* x = buffer.getReadPointer (0);

                for (int i = 0; i < blockSize; ++i)
                {
                    step = std::max (step, std::abs (x[i] - previous));
                    previous = x[i];
                }

                return step;
            };

            juce::MidiBuffer crash;
            crash.addEvent (juce::MidiMessage::noteOn (10, 49, (juce::uint8) 127), 0);
            play (crash);

            // How jumpy the crash itself is. A cymbal's largest sample-to-sample
            // steps can come a while after the hit, and the crash keeps ringing
            // while the new kit loads, so the baseline covers its first 350 ms.
            for (int i = 0; i < 60; ++i)
                largestStep = std::max (largestStep, play ({}));

            processor.loadKit (other, true);
            float switchStep = 0.0f;

            for (int i = 0; i < 300 && (processor.getKitInfo() == nullptr || processor.getKitInfo()->folder != other); ++i)
            {
                switchStep = std::max (switchStep, play ({}));
                processor.applyPendingKitLoad();
                juce::Thread::sleep (5);
            }

            for (int i = 0; i < 20; ++i)
                switchStep = std::max (switchStep, play ({}));

            expect (processor.getKitInfo() != nullptr && processor.getKitInfo()->folder == other, "the new kit never arrived");
            expect (switchStep <= largestStep * 1.05f, "a jump of " + juce::String (switchStep) + " during the switch, against "
                                                           + juce::String (largestStep) + " while playing");

            processor.loadKit (testKitFolder, true);
            expect (waitForKit (processor));
            temp.deleteRecursively();
        }

        beginTest ("State round-trips through save and restore");
        {
            setParam (processor, params::pieceParamId (s.piece ("snare"), "volume"), -6.0f);
            setParam (processor, params::micParamId (s.mic ("room"), "pan"), 30.0f);
            setParam (processor, params::pieceParamId (s.piece ("ride"), "tune"), 2.5f);
            processor.getSelection().select (s.piece ("ride"));

            juce::MemoryBlock state;
            processor.getStateInformation (state);

            LithiumDrumsProcessor restored;
            restored.setStateInformation (state.getData(), (int) state.getSize());
            expect (waitForKit (restored));

            auto value = [] (LithiumDrumsProcessor& p, const juce::String& id)
            {
                return p.getParameters().getRawParameterValue (id)->load();
            };

            expectWithinAbsoluteError (value (restored, params::pieceParamId (s.piece ("snare"), "volume")), -6.0f, 0.05f);
            expectWithinAbsoluteError (value (restored, params::micParamId (s.mic ("room"), "pan")), 30.0f, 0.5f);
            expectWithinAbsoluteError (value (restored, params::pieceParamId (s.piece ("ride"), "tune")), 2.5f, 0.01f);
            expect (restored.getKitFolder() == testKitFolder);
            expectEquals (restored.getSelection().getSelected(), s.piece ("ride"));
        }
    }
};

//==============================================================================
class StageTests final : public juce::UnitTest
{
public:
    StageTests() : juce::UnitTest ("Kit stage", "Lithium") {}

    void runTest() override
    {
        beginTest ("Clicks hit slots by their visible pixels");

        LithiumDrumsProcessor processor;
        KitStage stage (processor);
        stage.setBounds (0, 0, 1280, 520);

        // Points in 1x stage coordinates, found from the renders' alpha channels.
        expect (stage.getSlotAt ({ 676.7f, 407.3f }) == StageSlot::kick);
        expect (stage.getSlotAt ({ 771.3f, 286.0f }) == StageSlot::snare);
        expect (stage.getSlotAt ({ 862.0f, 176.0f }) == StageSlot::hihat);
        expect (stage.getSlotAt ({ 822.7f, 100.7f }) == StageSlot::crash);
        expect (stage.getSlotAt ({ 474.0f, 183.3f }) == StageSlot::ride);
        expect (stage.getSlotAt ({ 376.0f, 91.3f }) == StageSlot::china);
        expect (stage.getSlotAt ({ 724.7f, 247.3f }) == StageSlot::tom1);
        expect (stage.getSlotAt ({ 620.7f, 242.0f }) == StageSlot::tom2);
        expect (stage.getSlotAt ({ 511.3f, 332.0f }) == StageSlot::floorTom);

        // Inside the snare's rectangle, but on a transparent pixel of the
        // snare: the click falls through to tom 1 behind it.
        expect (stage.getSlotAt ({ 717.5f, 248.2f }) == StageSlot::tom1);

        // Empty background, and the stands (base layer) aren't clickable.
        expect (! stage.getSlotAt ({ 100.0f, 100.0f }).has_value());

        // Hit testing follows the stage when the window is scaled.
        stage.setBounds (0, 0, 1920, 780);
        expect (stage.getSlotAt ({ 676.7f * 1.5f, 407.3f * 1.5f }) == StageSlot::kick);
        stage.setBounds (0, 0, 1280, 520);

        beginTest ("Slots map to the kit's pieces; empty slots can't be clicked");

        // A DRSKit-shaped kit: no second rack tom and no china.
        auto info = std::make_shared<KitInfo>();
        using R = PieceRole;
        const std::vector<PieceRole> roles { R::kick, R::snare, R::hihat, R::tom, R::floorTom, R::floorTom, R::crash, R::crash, R::ride };

        for (size_t i = 0; i < roles.size(); ++i)
            info->pieces.push_back ({ "p" + juce::String ((int) i), "Piece", roles[i], { "Hit" }, -1 });

        info->slotPiece = assignStageSlots (roles);
        stage.setKitInfo (info);

        expect (stage.getPieceAt ({ 676.7f, 407.3f }) == 0);
        expect (stage.getPieceAt ({ 511.3f, 332.0f }) == 4);
        expect (! stage.getPieceAt ({ 620.7f, 242.0f }).has_value());   // tom2 slot: dimmed
        expect (! stage.getPieceAt ({ 376.0f, 91.3f }).has_value());    // china slot: dimmed
    }
};

static ManifestTests manifestTests;
static LoaderTests loaderTests;
static StorageTests storageTests;
static StreamingTests streamingTests;
static EngineTests engineTests;
static ImporterTests importerTests;
static PresetTests presetTests;
static SfzTests sfzTests;
static ProcessorTests processorTests;
static StageTests stageTests;

} // namespace lithium
