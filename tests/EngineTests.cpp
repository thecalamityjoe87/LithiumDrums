#include "AllocationCounter.h"
#include "PluginProcessor.h"
#include "engine/DrumEngine.h"
#include "engine/KitLoader.h"
#include "engine/KitManifest.h"
#include "importer/DrumGizmoImporter.h"
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

            for (int i = 0; i < 20; ++i)
                largestStep = std::max (largestStep, play ({}));   // how jumpy the crash itself is

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
static EngineTests engineTests;
static ImporterTests importerTests;
static ProcessorTests processorTests;
static StageTests stageTests;

} // namespace lithium
