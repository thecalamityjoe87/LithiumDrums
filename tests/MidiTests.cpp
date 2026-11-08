#include "AllocationCounter.h"
#include "MidiMapLibrary.h"
#include "PluginProcessor.h"
#include "engine/DrumEngine.h"
#include "engine/KitLoader.h"
#include "engine/MidiMapping.h"

namespace lithium
{

namespace
{
    const juce::File testKitFolder { LITHIUM_TEST_KIT_DIR };
    constexpr double sampleRate = 44100.0;

    /** The test kit, loaded with its own MIDI map, plus a DrumEngine to play it. */
    struct MidiRig
    {
        explicit MidiRig (const juce::File& folder = testKitFolder)
        {
            juce::String error;
            kit = KitLoader::loadKit (folder, MidiMap::kitDefault(), info, error);
            jassert (kit != nullptr);

            // Every mic to its own bus channel, so nothing depends on how close mics are routed.
            for (int m = 0; m < kit->numMics; ++m)
                kit->mics[(size_t) m].direct = false;

            engine.prepare (sampleRate);
            params.resetForKit (*kit);
            params.velocityCrossfade = false;   // one voice per hit, unless a ladder blends two
        }

        ~MidiRig()    { engine.stopAll (*kit); }

        void useMap (const MidiMap& map)        { kit->notes = resolveMidiMap (map, info); }
        void note (int n, int velocity = 100)   { engine.noteOn (*kit, n, velocity, params); }
        void cc (int controller, int value)     { engine.controlChange (*kit, 0, controller, value, params); }

        /** A message at a time in milliseconds, on a MIDI channel. */
        void noteAt (double ms, int channel, int n, int velocity = 100)
        {
            engine.setTime ((std::int64_t) (ms * 0.001 * sampleRate));
            engine.noteOn (*kit, n, velocity, params, channel);
        }

        void ccAt (double ms, int channel, int controller, int value)
        {
            engine.setTime ((std::int64_t) (ms * 0.001 * sampleRate));
            engine.controlChange (*kit, channel, controller, value, params);
        }

        /** The monitor events since the last call. */
        std::vector<MidiMonitorEvent> events()
        {
            std::vector<MidiMonitorEvent> result;

            for (MidiMonitorEvent e; engine.popMonitorEvent (e);)
                result.push_back (e);

            return result;
        }

        void setControllers (const std::function<void (ControllerSettings&)>& change)
        {
            change (kit->notes.controllers);
        }
        int voices() const                      { return engine.getNumActiveVoices(); }

        void render (int numSamples)
        {
            bus.setSize (maxMicBusChannels, numSamples, false, false, true);
            pieceBus.setSize (maxPieces * 2, numSamples, false, false, true);
            bus.clear();
            pieceBus.clear();
            engine.render (*kit, { bus, pieceBus }, 0, numSamples, params);
        }

        int piece (const juce::String& id) const
        {
            for (size_t p = 0; p < info.pieces.size(); ++p)
                if (info.pieces[p].id == id)
                    return (int) p;

            jassertfalse;
            return -1;
        }

        /** The flattened index of a piece's articulation. */
        int articulation (const juce::String& pieceId, const juce::String& id) const
        {
            const auto& p = info.pieces[(size_t) piece (pieceId)];

            for (size_t a = 0; a < p.articulations.size(); ++a)
                if (p.articulations[a].id == id)
                    return p.firstArticulation + (int) a;

            jassertfalse;
            return -1;
        }

        /** The articulation a piece played last. */
        int lastPlayed (const juce::String& pieceId) const
        {
            const auto hit = LastHit::unpack (feedback.lastHit[(size_t) piece (pieceId)].load());
            return hit.has_value() ? hit->articulation : -1;
        }

        KitInfo info;
        std::unique_ptr<Kit> kit;
        EngineFeedback feedback;
        DrumEngine engine { feedback };
        EngineParams params;
        juce::AudioBuffer<float> bus, pieceBus;
    };

    /** A KitInfo with no audio behind it, for testing map resolution on
        articulation names the test kit doesn't use. */
    KitInfo fakeKit (std::initializer_list<std::tuple<const char*, PieceRole, std::initializer_list<const char*>>> pieces)
    {
        KitInfo info;
        int next = 0;

        for (const auto& [id, role, articulations] : pieces)
        {
            KitPieceInfo p;
            p.id = id;
            p.name = id;
            p.role = role;
            p.firstArticulation = next;

            for (const auto* a : articulations)
                p.articulations.push_back ({ a, a, {}, {} });

            next += (int) p.articulations.size();
            info.pieces.push_back (std::move (p));
        }

        return info;
    }

    std::pair<juce::String, juce::String> played (const KitInfo& info, const NoteTable& table, int note)
    {
        const auto [p, a] = pieceAndArticulation (info, table.targets[(size_t) note].articulation);

        if (p < 0)
            return {};

        return { info.pieces[(size_t) p].id, info.pieces[(size_t) p].articulations[(size_t) a].id };
    }

    using Played = std::pair<juce::String, juce::String>;

    /** A copy of the test kit with what it lacks for the e-kit tests: a
        hi-hat foot splash, a ride edge, and position ranges on the snare. */
    juce::File makeExtendedKit()
    {
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LithiumExtendedTestKit");
        folder.deleteRecursively();
        testKitFolder.copyDirectoryTo (folder);

        auto json = juce::JSON::parse (folder.getChildFile ("kit.json"));

        for (auto& piece : *json["pieces"].getArray())
        {
            auto* articulations = piece["articulations"].getArray();

            auto copyOf = [&] (const char* from, const char* id, const char* name)
            {
                for (const auto& a : *articulations)
                {
                    if (a["id"].toString() == from)
                    {
                        auto copy = juce::JSON::parse (juce::JSON::toString (a));
                        copy.getDynamicObject()->setProperty ("id", id);
                        copy.getDynamicObject()->setProperty ("name", name);
                        articulations->add (copy);
                        return;
                    }
                }
            };

            if (piece["id"].toString() == "hihat")
                copyOf ("open", "splash", "Foot splash");

            if (piece["id"].toString() == "ride")
                copyOf ("bow", "edge", "Edge");

            if (piece["id"].toString() == "snare")
            {
                for (auto& a : *articulations)
                {
                    if (a["id"].toString() == "center")
                        a.getDynamicObject()->setProperty ("positionRange", juce::Array<juce::var> { 0.0, 0.2 });

                    if (a["id"].toString() == "off_center")
                        a.getDynamicObject()->setProperty ("positionRange", juce::Array<juce::var> { 0.4, 1.0 });
                }
            }
        }

        folder.getChildFile ("kit.json").replaceWithText (juce::JSON::toString (json));
        return folder;
    }

    /** The share of a signal's energy in its highs (its first difference against itself). */
    double brightness (const juce::AudioBuffer<float>& b)
    {
        double all = 0.0, highs = 0.0;

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            const auto* x = b.getReadPointer (ch);

            for (int i = 1; i < b.getNumSamples(); ++i)
            {
                all += (double) x[i] * x[i];
                highs += (double) (x[i] - x[i - 1]) * (x[i] - x[i - 1]);
            }
        }

        return highs / std::max (1.0e-20, all);
    }
}

class MidiMapTests final : public juce::UnitTest
{
public:
    MidiMapTests() : juce::UnitTest ("MIDI maps", "Lithium") {}

    void runTest() override
    {
        beginTest ("The factory maps parse, and survive a round trip through JSON");
        {
            const auto maps = MidiMap::factoryMaps();
            expectEquals ((int) maps.size(), 10);
            expectEquals (maps.front().id, juce::String ("lithium-standard"));

            for (const auto& m : maps)
            {
                MidiMap back;
                expect (MidiMap::parse (m.toJson(), back).wasOk(), m.name);
                expect (back.sameMapping (m), m.name + " changed in the round trip");
                expectEquals (back.id, m.id);
                expect (! m.entries.empty());
            }

            const auto roland = MidiMap::rolandVDrums();
            expectEquals (roland.controllers.hihatPedal, 4);
            expect (roland.find (46) != nullptr && roland.find (46)->follow == Follow::pedal);
            expect (roland.find (38) != nullptr && roland.find (38)->follow == Follow::position);
            expect (roland.find (43) != nullptr && roland.find (43)->anyTom && roland.find (43)->nth == 2);
            expect (MidiMap::generalMidi().find (59) != nullptr && MidiMap::generalMidi().find (59)->otherwise == "edge");
            expect (MidiMap::kitDefault().useKitMap);
        }

        beginTest ("Bad maps are rejected with a reason");
        {
            MidiMap m;
            expect (MidiMap::parse (R"({ "notes": [ { "note": 200, "role": "kick" } ] })", m).failed());
            expect (MidiMap::parse (R"({ "notes": [ { "note": 36, "role": "cowbell" } ] })", m).failed());
            expect (MidiMap::parse (R"({ "notes": [ { "note": 36, "role": "kick", "follow": "wind" } ] })", m).failed());
            expect (MidiMap::parse (R"({ "controllers": { "hihatPedal": 300 }, "notes": [] })", m).failed());
            expect (MidiMap::parse (R"({ "name": "No notes" })", m).failed());
        }

        beginTest ("Standard articulation names find the kit's own names");
        {
            const auto info = fakeKit ({
                { "kick",  PieceRole::kick,     { "hit" } },
                { "snare", PieceRole::snare,    { "center", "rim", "cross_stick" } },
                { "t1",    PieceRole::tom,      { "center" } },
                { "t2",    PieceRole::tom,      { "center" } },
                { "t3",    PieceRole::tom,      { "center" } },
                { "f1",    PieceRole::floorTom, { "center", "rimshot" } },
                { "crash", PieceRole::crash,    { "hit", "tip" } },
                { "ride",  PieceRole::ride,     { "tip", "shank", "bell" } },
            });

            const auto gm = resolveMidiMap (MidiMap::generalMidi(), info);
            expect (played (info, gm, 40) == Played ("snare", "rim"));
            expect (played (info, gm, 37) == Played ("snare", "cross_stick"));
            expect (played (info, gm, 51) == Played ("ride", "tip"));
            expect (played (info, gm, 53) == Played ("ride", "bell"));
            expect (played (info, gm, 43) == Played ("f1", "center"));     // GM 43: the first floor tom
            expect (played (info, gm, 59) == Played ("ride", "shank"), "GM 59 on a one-ride kit: the ride's crash");

            const auto roland = resolveMidiMap (MidiMap::rolandVDrums(), info);
            expect (played (info, roland, 59) == Played ("ride", "shank"));   // ride edge
            expect (played (info, roland, 49) == Played ("crash", "tip"));    // crash bow
            expect (played (info, roland, 55) == Played ("crash", "hit"));    // crash edge
            expect (played (info, roland, 43) == Played ("t3", "center"));    // Roland tom 3: the third tom, a rack tom here
            expect (played (info, roland, 41) == Played ("f1", "center"));    // tom 4
            expect (played (info, roland, 39) == Played ("f1", "rimshot"));   // tom 4 rim
            expect (played (info, roland, 50) == Played ("t1", "center"));    // no rim recording: the head
            expect (played (info, roland, 60) == Played());
            expect (roland.targets[38].follow == Follow::none, "this kit has no steps to follow");
        }

        beginTest ("General MIDI finds percussion by name, and never plays the wrong one");
        {
            const auto info = fakeKit ({
                { "kick",     PieceRole::kick,  { "hit" } },
                { "kick2",    PieceRole::kick,  { "hit" } },
                { "snare",    PieceRole::snare, { "center" } },
                { "crash",    PieceRole::crash, { "hit" } },
                { "splash",   PieceRole::crash, { "hit" } },
                { "crash2",   PieceRole::crash, { "hit" } },
                { "snaps",    PieceRole::other, { "hit" } },
                { "claps",    PieceRole::other, { "hit" } },
                { "cowbell",  PieceRole::other, { "hit" } },
                { "bell_tree", PieceRole::other, { "down" } },
            });

            const auto gm = resolveMidiMap (MidiMap::generalMidi(), info);
            expect (played (info, gm, 36) == Played ("kick", "hit"));
            expect (played (info, gm, 35) == Played ("kick2", "hit"), "GM 35 is the second bass drum");
            expect (played (info, gm, 55) == Played ("splash", "hit"));
            expect (played (info, gm, 57) == Played ("crash2", "hit"), "crash 2 is the second crash, not counting the splash");
            expect (played (info, gm, 39) == Played ("claps", "hit"), "claps before snaps, though the snaps come first in the kit");
            expect (played (info, gm, 56) == Played ("cowbell", "hit"));
            expect (played (info, gm, 84) == Played ("bell_tree", "down"), "the bell tree, not the cowbell");
            expect (played (info, gm, 54) == Played(), "no tambourine: nothing, rather than another percussion piece");

            const auto twoRides = fakeKit ({ { "ride",  PieceRole::ride, { "bow", "edge", "bell" } },
                                             { "ride2", PieceRole::ride, { "bow", "bell" } } });
            const auto gm2 = resolveMidiMap (MidiMap::generalMidi(), twoRides);
            expect (played (twoRides, gm2, 59) == Played ("ride2", "bow"), "GM 59 is the second ride where there is one");
            expect (played (twoRides, gm2, 51) == Played ("ride", "bow"));

            const auto alesis = resolveMidiMap (MidiMap::alesis(), twoRides);
            expect (played (twoRides, alesis, 59) == Played ("ride", "edge"));
            expect (played (twoRides, alesis, 53) == Played ("ride", "bell"));
            expect (! gm.targets[36].spare);
        }

        beginTest ("Whatever a map leaves out gets a spare note");
        {
            auto info = fakeKit ({
                { "kick",    PieceRole::kick,  { "hit" } },
                { "snare",   PieceRole::snare, { "center", "rim", "muted", "buzz" } },
                { "bucket",  PieceRole::other, { "hit" } },
                { "ride",    PieceRole::ride,  { "bow", "bell" } },
            });
            info.pieces[3].defaultChokeNote = 96;   // spares must step round choke notes

            for (const auto& map : MidiMap::factoryMaps())
            {
                const auto table = resolveMidiMap (map, info);
                std::vector<bool> reached (8);   // every articulation of the kit above

                for (const auto& t : table.targets)
                    if (t.articulation >= 0)
                        reached[(size_t) t.articulation] = true;

                expect (std::all_of (reached.begin(), reached.end(), [] (bool r) { return r; }), map.name + " leaves something unplayable");
                if (map.find (96) == nullptr)
                    expect (table.targets[96].articulation < 0, "a choke note was used as a spare");

                for (int n = 85; n <= 95; ++n)
                    expect (table.targets[(size_t) n].articulation < 0, "spares keep clear of the cymbal choke notes");
            }

            const auto gm = resolveMidiMap (MidiMap::generalMidi(), info);
            expect (played (info, gm, 97) == Played ("snare", "muted"), "spares go in kit order, from 96 up");
            expect (gm.targets[97].spare && gm.targets[97].follow == Follow::none);
            expect (played (info, gm, 98) == Played ("snare", "buzz"));
            expect (played (info, gm, 99) == Played ("bucket", "hit"));

            // Learning a note for a spare articulation frees its spare note.
            auto map = MidiMap::generalMidi();
            map.assign (entryForArticulation (info, 3, 60));   // snare muted on 60
            const auto learned = resolveMidiMap (map, info);
            expect (played (info, learned, 60) == Played ("snare", "muted") && ! learned.targets[60].spare);
            expect (played (info, learned, 97) == Played ("snare", "buzz"));

            // Spares aren't part of the map, so editing a map never saves them.
            info.kitMidiMap = { { 36, "kick", "" } };
            const auto copy = editableCopy (MidiMap::kitDefault(), info);
            expect (copy.find (36) != nullptr);
            expect (std::none_of (copy.entries.begin(), copy.entries.end(), [] (const auto& e) { return e.note >= 96; }),
                    "a spare note was saved into the map");
        }

        beginTest ("Lithium Standard means the same kind of hit on every kit");
        {
            // Two kits that name and order things differently, like DRSKit and Basalt.
            const auto a = fakeKit ({
                { "snare",       PieceRole::snare, { "center", "rim" } },
                { "crash_left",  PieceRole::crash, { "hit", "tip" } },
                { "crash_right", PieceRole::crash, { "hit" } },
                { "ride",        PieceRole::ride,  { "bow", "shank", "bell", "rest" } },
            });
            const auto b = fakeKit ({
                { "snare",  PieceRole::snare, { "center", "edge", "sidestick" } },
                { "crash1", PieceRole::crash, { "hit", "choke" } },
                { "splash", PieceRole::crash, { "hit" } },
                { "crash2", PieceRole::crash, { "hit" } },
                { "crash3", PieceRole::crash, { "hit" } },
                { "ride",   PieceRole::ride,  { "tip", "shank", "bell", "choke" } },
                { "cowbell", PieceRole::other, { "hit" } },
            });

            const auto map = MidiMap::lithiumStandard();
            const auto ta = resolveMidiMap (map, a), tb = resolveMidiMap (map, b);

            expect (played (a, ta, 51) == Played ("ride", "bow") && played (b, tb, 51) == Played ("ride", "tip"));
            expect (played (a, ta, 121) == Played ("ride", "shank") && played (b, tb, 121) == Played ("ride", "shank"), "the ride crash");
            expect (played (a, ta, 57) == Played ("crash_right", "hit"));
            expect (played (b, tb, 57) == Played ("crash2", "hit"), "crash 2 is a crash, never the splash");
            expect (played (b, tb, 55) == Played ("splash", "hit"));
            expect (played (b, tb, 120) == Played ("crash3", "hit") && played (a, ta, 120) == Played(), "crash 3, or nothing");
            expect (played (b, tb, 117) == Played ("crash1", "choke") && played (a, ta, 117) == Played(), "a grab, or nothing");
            expect (played (a, ta, 107) == Played() && played (b, tb, 107) == Played(), "no second snare: silent, not snare 1");
            expect (played (b, tb, 115) == Played(), "one splash: the second-splash note stays silent");

            auto c = b;
            KitPieceInfo splash2;
            splash2.id = "splash_r";
            splash2.name = "Splash R";
            splash2.role = PieceRole::crash;
            splash2.firstArticulation = c.pieces.back().firstArticulation + (int) c.pieces.back().articulations.size();
            splash2.articulations.push_back ({ "hit", "hit", {}, {} });
            c.pieces.push_back (splash2);
            const auto tc = resolveMidiMap (map, c);
            expect (played (c, tc, 55) == Played ("splash", "hit") && played (c, tc, 115) == Played ("splash_r", "hit"), "both splashes");

            // A spare never lands on a note the map defines, so those notes keep their meaning.
            for (const auto* t : { &ta, &tb })
                for (const auto& e : map.entries)
                    expect (! t->targets[(size_t) e.note].spare, "a spare note took map note " + juce::String (e.note));

            expect (played (a, ta, 116) == Played ("crash_left", "tip"), "crash 1 bow is the tip where the kit has one");
        }

        beginTest ("Learned notes are saved in kit-independent terms");
        {
            const auto basalt = fakeKit ({
                { "crash1", PieceRole::crash, { "hit" } },
                { "splash", PieceRole::crash, { "hit" } },
                { "crash2", PieceRole::crash, { "hit" } },
                { "ride",   PieceRole::ride,  { "tip", "shank", "bell" } },
                { "cowbell", PieceRole::other, { "hit" } },
            });
            const auto drs = fakeKit ({
                { "crash_left",  PieceRole::crash, { "hit" } },
                { "crash_right", PieceRole::crash, { "hit" } },
                { "ride",        PieceRole::ride,  { "bow", "shank", "bell" } },
            });

            const auto tip = entryForArticulation (basalt, 3, 60);
            expectEquals (tip.articulation, juce::String ("bow"), "a ride tip is saved as the ride bow");

            const auto shank = entryForArticulation (basalt, 4, 61);
            expectEquals (shank.articulation, juce::String ("edge"), "a ride shank is saved as the ride edge");

            const auto crash2 = entryForArticulation (basalt, 2, 62);
            expectEquals (crash2.nth, 1, "crash 2 is the second crash, not counting the splash");

            const auto cowbell = entryForArticulation (basalt, 6, 63);
            expectEquals (cowbell.name, juce::String ("cowbell"));

            MidiMap learned;
            for (const auto& e : { tip, shank, crash2, cowbell })
                learned.assign (e);

            const auto t = resolveMidiMap (learned, drs);
            expect (played (drs, t, 60) == Played ("ride", "bow"));
            expect (played (drs, t, 61) == Played ("ride", "shank"));
            expect (played (drs, t, 62) == Played ("crash_right", "hit"));
            expect (played (drs, t, 63) == Played(), "no cowbell on this kit");
        }

        beginTest ("Editing a Kit default map spells out the kit's own map");
        {
            auto info = fakeKit ({ { "kick", PieceRole::kick, { "hit" } }, { "snare", PieceRole::snare, { "center", "rim" } } });
            info.kitMidiMap = { { 36, "kick", "" }, { 38, "snare", "rim" } };

            const auto kitDefault = MidiMap::kitDefault();
            const auto copy = editableCopy (kitDefault, info);
            expect (! copy.useKitMap);
            expect (copy.find (36) != nullptr && copy.find (38) != nullptr && copy.find (38)->articulation == "rim");

            const auto a = resolveMidiMap (kitDefault, info), b = resolveMidiMap (copy, info);

            for (int n = 0; n < 128; ++n)
                expectEquals ((int) a.targets[(size_t) n].articulation, (int) b.targets[(size_t) n].articulation);

            // General MIDI fills the notes the kit's map leaves free, but
            // never replaces one of the kit's own notes.
            expect (played (info, a, 40) == Played ("snare", "rim"));
            info.kitMidiMap.push_back ({ 40, "kick", "" });
            expect (played (info, resolveMidiMap (kitDefault, info), 40) == Played ("kick", "hit"));

            // A kit without a map of its own plays General MIDI under Kit default.
            info.kitMidiMap.clear();
            expect (played (info, resolveMidiMap (kitDefault, info), 40) == Played ("snare", "rim"));
        }

        beginTest ("Saved maps are listed, found again and deleted");
        {
            auto map = MidiMap::rolandVDrums();
            map.assign ({ 60, PieceRole::snare, false, 0, "rimshot", Follow::none, {}, {}, false });

            MidiMap saved;
            expect (midimaps::save (map, "My TD-27", saved).wasOk());
            expect (saved.id.startsWith (midimaps::userPrefix));
            expectEquals (saved.name, juce::String ("My TD-27"));

            const auto items = midimaps::scan();
            const auto it = std::find_if (items.begin(), items.end(), [&] (const auto& i) { return i.map.id == saved.id; });
            expect (it != items.end() && ! it->factory);

            const auto found = midimaps::find (saved.id);
            expect (found.has_value() && found->sameMapping (map));
            expect (midimaps::find ("roland-vdrums").has_value());

            expect (midimaps::remove (saved.id));
            expect (! midimaps::find (saved.id).has_value());
            expect (! midimaps::remove ("general-midi"));
        }
    }
};

class ControllerTests final : public juce::UnitTest
{
public:
    ControllerTests() : juce::UnitTest ("E-kit controllers", "Lithium") {}

    void runTest() override
    {
        MidiRig rig;
        rig.useMap (MidiMap::rolandVDrums());

        const auto closed = rig.articulation ("hihat", "closed");
        const auto half = rig.articulation ("hihat", "half_open");
        const auto open = rig.articulation ("hihat", "open");
        const auto center = rig.articulation ("snare", "center");
        const auto offCenter = rig.articulation ("snare", "off_center");

        beginTest ("The test kit's openness and position steps become ladders");
        {
            const auto& hat = rig.kit->pieces[(size_t) rig.piece ("hihat")];
            expectEquals (hat.hatLadders[0].numSteps, 3);
            expectEquals (hat.hatLadders[0].articulation[1], half);
            expectEquals (hat.hatLadders[1].numSteps, 3, "with no edge recordings, edge hits use the bow's steps");

            const auto& snare = rig.kit->pieces[(size_t) rig.piece ("snare")];
            expectEquals (snare.positionLadder.numSteps, 2);
            expectEquals (snare.positionLadder.articulation[1], offCenter);

            expect (rig.kit->notes.targets[46].follow == Follow::pedal);
            expect (rig.kit->notes.targets[38].follow == Follow::position);
            expect (rig.kit->notes.targets[44].follow == Follow::none);
        }

        beginTest ("Until CC4 arrives, hi-hat notes play their own articulation");
        {
            rig.note (46);
            expectEquals (rig.lastPlayed ("hihat"), open);
            rig.note (42);
            expectEquals (rig.lastPlayed ("hihat"), closed);
            rig.engine.stopAll (*rig.kit);
        }

        beginTest ("Hi-hat hits follow the pedal");
        {
            // Roland sends 0 = open, 127 = closed.
            rig.cc (4, 127);
            rig.note (46);
            expectEquals (rig.lastPlayed ("hihat"), closed, "an open-hat note with the pedal down plays closed");

            rig.cc (4, 0);
            rig.note (42);
            expectEquals (rig.lastPlayed ("hihat"), open, "a closed-hat note with the pedal up plays open");

            rig.cc (4, 64);
            rig.note (26);   // open edge: no edge recordings, so the bow's steps
            expectEquals (rig.lastPlayed ("hihat"), half);
            expectEquals (rig.feedback.hihatOpenness.load(), 1.0f - 64.0f / 127.0f);
            rig.engine.stopAll (*rig.kit);

            // Between two steps, near either one: that step alone.
            rig.cc (4, 114);
            rig.note (46);
            expectEquals (rig.voices(), 1);
            expectEquals (rig.lastPlayed ("hihat"), closed);
            rig.engine.stopAll (*rig.kit);

            // Halfway between: both, crossfaded.
            rig.cc (4, 95);
            rig.note (46);
            expectEquals (rig.voices(), 2, "halfway between closed and half open plays both");
            rig.engine.stopAll (*rig.kit);

            // The pedal note doesn't follow.
            rig.note (44);
            expectEquals (rig.lastPlayed ("hihat"), rig.articulation ("hihat", "pedal"));
            rig.engine.stopAll (*rig.kit);
        }

        beginTest ("Inverted pedals and other pedal CCs");
        {
            auto map = MidiMap::rolandVDrums();
            map.controllers.hihatPedal = 11;
            map.controllers.pedalInverted = true;
            rig.useMap (map);

            rig.cc (11, 127);    // inverted: 127 = open
            rig.note (42);
            expectEquals (rig.lastPlayed ("hihat"), open);

            rig.cc (4, 127);     // CC4 is now nothing in particular
            rig.note (42);
            expectEquals (rig.lastPlayed ("hihat"), open);

            rig.engine.stopAll (*rig.kit);
            rig.useMap (MidiMap::rolandVDrums());
        }

        beginTest ("Closing the pedal damps ringing hats; shutting it chokes them");
        {
            rig.cc (4, 0);
            rig.note (46);
            rig.render (4410);
            expectEquals (rig.voices(), 1);

            rig.cc (4, 25);      // a little closed: still rings
            rig.render (8820);
            expectEquals (rig.voices(), 1);

            rig.cc (4, 70);      // well closed: damped over 80 ms
            rig.render (2205);
            expectEquals (rig.voices(), 1, "damping fades rather than cuts");
            rig.render (4410);
            expectEquals (rig.voices(), 0);

            rig.cc (4, 0);
            rig.note (46);
            rig.render (4410);
            rig.cc (4, 127);     // shut: the 30 ms hi-hat choke
            rig.render (2205);
            expectEquals (rig.voices(), 0);

            // Opening the pedal never damps anything.
            rig.note (46);
            rig.render (512);
            rig.cc (4, 0);
            rig.render (8820);
            expectEquals (rig.voices(), 1);
            rig.engine.stopAll (*rig.kit);
        }

        beginTest ("Snare hits follow the strike position");
        {
            rig.note (38);
            expectEquals (rig.lastPlayed ("snare"), center, "no position received yet: the centre");

            rig.cc (16, 127);    // at the edge: the outermost step
            rig.note (38);
            expectEquals (rig.lastPlayed ("snare"), offCenter);
            expectEquals (rig.feedback.position[(size_t) PositionSource::snare].load(), 1.0f);

            rig.cc (16, 0);
            rig.note (38);
            expectEquals (rig.lastPlayed ("snare"), center);
            rig.engine.stopAll (*rig.kit);

            rig.cc (16, 32);     // halfway between the two steps
            rig.note (38);
            expectEquals (rig.voices(), 2);
            rig.engine.stopAll (*rig.kit);

            rig.cc (17, 127);    // the ride's position doesn't move the snare
            rig.cc (16, 0);
            rig.note (38);
            expectEquals (rig.lastPlayed ("snare"), center);

            rig.note (40);       // rimshots don't follow
            expectEquals (rig.lastPlayed ("snare"), rig.articulation ("snare", "rimshot"));
            rig.engine.stopAll (*rig.kit);
        }

        beginTest ("Maps without follow ignore the controllers");
        {
            rig.useMap (MidiMap::generalMidi());
            rig.cc (4, 127);
            rig.note (46);
            expectEquals (rig.lastPlayed ("hihat"), open);
            rig.cc (16, 127);
            rig.note (38);
            expectEquals (rig.lastPlayed ("snare"), center);
            rig.engine.stopAll (*rig.kit);
        }

        beginTest ("Cymbal hits ring on under the next hit; only grabs cut the cymbal");
        {
            const auto bow = rig.articulation ("ride", "bow"), bell = rig.articulation ("ride", "bell");
            expect (rig.kit->articulations[(size_t) bow].selfChoke == SelfChoke::none);
            expect (rig.kit->articulations[(size_t) rig.articulation ("hihat", "closed")].selfChoke == SelfChoke::moreOpen);
            expect (articulations::isGrab ("choke") && articulations::isGrab ("grab") && ! articulations::isGrab ("bell"));

            // Like an SFZ kit with "group=1 off_by=1" on every ride region.
            auto& b = rig.kit->articulations[(size_t) bow];
            auto& c = rig.kit->articulations[(size_t) bell];
            const auto saved = std::pair { b, c };
            b.chokeGroupMask = c.chokeGroupMask = b.chokesMask = c.chokesMask = 1u << 30;

            rig.useMap (MidiMap::generalMidi());
            rig.note (51);       // bow
            rig.render (4410);
            rig.note (53);       // bell over it
            rig.note (51);       // and the bow again
            rig.render (4410);
            expectEquals (rig.voices(), 3, "the bow keeps ringing under the bell");

            std::tie (b, c) = saved;
            rig.engine.stopAll (*rig.kit);
            rig.useMap (MidiMap::rolandVDrums());
        }

        beginTest ("Hi-hats only stop ringing when they close");
        {
            // Like the AVL (Basalt) kits: every hi-hat hit in one group that chokes itself.
            auto& arts = rig.kit->articulations;
            const std::array<const char*, 4> ids { "closed", "half_open", "open", "pedal" };
            std::vector<Articulation> saved;

            for (const auto* id : ids)
            {
                auto& a = arts[(size_t) rig.articulation ("hihat", id)];
                saved.push_back (a);
                a.chokeGroupMask = a.chokesMask = 1u << 30;
            }

            rig.useMap (MidiMap::generalMidi());
            auto play = [&] (const char* id) { rig.engine.trigger (*rig.kit, rig.articulation ("hihat", id), 100, rig.params); };

            play ("half_open");
            rig.render (2205);
            play ("half_open");      // a second half-open hit rings on with the first
            play ("open");           // and so does an open one
            rig.render (4410);
            expectEquals (rig.voices(), 3, "open hits don't choke each other");

            play ("closed");         // closing the hats stops all three
            rig.render (2205);
            expectEquals (rig.voices(), 1);

            play ("closed");         // a closed hit doesn't cut the last closed hit's tail
            expectEquals (rig.voices(), 2);
            rig.engine.stopAll (*rig.kit);

            play ("open");
            rig.render (2205);
            play ("pedal");          // the pedal chick closes them too
            rig.render (2205);
            expectEquals (rig.voices(), 1);
            rig.engine.stopAll (*rig.kit);

            for (size_t i = 0; i < saved.size(); ++i)
                arts[(size_t) rig.articulation ("hihat", ids[i])] = saved[i];

            rig.useMap (MidiMap::rolandVDrums());
        }

        beginTest ("A note that plays something never chokes a cymbal");
        {
            rig.useMap (MidiMap::generalMidi());
            const auto crashChoke = rig.params.pieces[(size_t) rig.piece ("crash")].chokeNote;
            expectEquals (crashChoke, 85);

            rig.note (49);
            rig.render (2205);
            rig.note (85);       // unmapped: the crash's choke note
            rig.render (4410);
            expectEquals (rig.voices(), 0);

            auto map = MidiMap::generalMidi();
            map.assign ({ 85, PieceRole::snare, false, 0, "center", Follow::none, {}, {}, false });
            rig.useMap (map);
            rig.note (49);
            rig.render (2205);
            rig.note (85);       // now it plays the snare, and the crash rings on
            rig.render (4410);
            expectEquals (rig.voices(), 2);
            rig.engine.stopAll (*rig.kit);
            rig.useMap (MidiMap::rolandVDrums());
        }

        beginTest ("MIDI input is reported for the MIDI panel");
        {
            const auto before = rig.feedback.lastNote.load();
            rig.note (99, 77);   // unmapped, but still reported (for MIDI learn)
            const auto after = rig.feedback.lastNote.load();
            expect (after != before);
            const auto n = EngineFeedback::unpackNote (after);
            expect (n.has_value() && n->first == 99 && n->second == 77);

            rig.note (99, 77);   // the same note again is still a new event
            expect (rig.feedback.lastNote.load() != after);
        }
    }
};

class MidiProcessorTests final : public juce::UnitTest
{
public:
    MidiProcessorTests() : juce::UnitTest ("MIDI in the processor", "Lithium") {}

    bool waitForKit (LithiumDrumsProcessor& p)
    {
        juce::AudioBuffer<float> buffer (p.getTotalNumOutputChannels(), 512);

        for (int i = 0; i < 200; ++i)
        {
            p.applyPendingKitLoad();
            p.prepareToPlay (sampleRate, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 100), 0);
            p.processBlock (buffer, midi);

            if (buffer.getMagnitude (0, 0, 512) > 0.0f && p.getKitInfo() != nullptr)
                return true;

            juce::Thread::sleep (20);
        }

        return false;
    }

    static int articulation (const KitInfo& info, const juce::String& pieceId, const juce::String& id)
    {
        for (const auto& p : info.pieces)
            if (p.id == pieceId)
                for (size_t a = 0; a < p.articulations.size(); ++a)
                    if (p.articulations[a].id == id)
                        return p.firstArticulation + (int) a;

        return -1;
    }

    static int lastPlayed (const LithiumDrumsProcessor& p, const KitInfo& info, const juce::String& pieceId)
    {
        for (size_t i = 0; i < info.pieces.size(); ++i)
            if (info.pieces[i].id == pieceId)
                if (const auto hit = LastHit::unpack (p.getFeedback().lastHit[i].load()))
                    return hit->articulation;

        return -1;
    }

    void runTest() override
    {
        beginTest ("A new instance starts with the default map");

        LithiumDrumsProcessor processor;
        expect (waitForKit (processor), "no sound from the test kit");
        const auto info = processor.getKitInfo();

        if (info == nullptr)
            return;

        expectEquals (processor.getMidiMap().id, juce::String (MidiMap::kitDefaultId));   // set by TestMain
        expect (! processor.isMidiMapModified());

        beginTest ("Changing the map reaches the audio thread, and CC4 drives the hats");
        {
            processor.setMidiMap (MidiMap::rolandVDrums(), false);
            expect (processor.getNoteTable().targets[46].follow == Follow::pedal);

            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::controllerEvent (10, 4, 127), 0);
            midi.addEvent (juce::MidiMessage::noteOn (10, 46, (juce::uint8) 100), 100);
            processor.processBlock (buffer, midi);
            expectEquals (lastPlayed (processor, *info, "hihat"), articulation (*info, "hihat", "closed"));
        }

        beginTest ("The on-screen keyboard plays notes through the map, like MIDI input");
        {
            // The pedal is still shut from the last test, so an open-hat note plays closed.
            const auto before = processor.getFeedback().lastNote.load();
            processor.playNote (46, 90);

            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer none;
            processor.processBlock (buffer, none);

            expectEquals (lastPlayed (processor, *info, "hihat"), articulation (*info, "hihat", "closed"));
            const auto n = EngineFeedback::unpackNote (processor.getFeedback().lastNote.load());
            expect (processor.getFeedback().lastNote.load() != before && n.has_value() && n->first == 46 && n->second == 90,
                    "keyboard notes show in the input monitor too");
        }

        beginTest ("MIDI learn and controller settings survive save and restore");
        {
            const auto offCenter = articulation (*info, "snare", "off_center");
            processor.learnNote (60, offCenter);
            processor.forgetNote (38);

            auto controllers = processor.getMidiMap().controllers;
            controllers.hihatPedal = 11;
            controllers.positionVelocity[(size_t) PositionSource::snare] = 0.4f;
            processor.setControllers (controllers);

            // Velocity settings are part of the map, so they're saved with the project.
            auto velocity = processor.getMidiMap().velocity;
            velocity.global = -0.5f;
            PieceVelocity snare;
            snare.curve = VelocityCurve::harder();
            const auto snarePiece = (int) (std::find_if (info->pieces.begin(), info->pieces.end(), [] (const auto& p) { return p.id == "snare"; })
                                           - info->pieces.begin());
            setPieceVelocity (velocity, *info, snarePiece, snare);
            processor.setVelocity (velocity);

            expect (processor.isMidiMapModified());
            expectEquals ((int) processor.getNoteTable().targets[60].articulation, offCenter);
            expectEquals ((int) processor.getNoteTable().targets[38].articulation, -1);
            expect (processor.getMidiMap().find (60)->follow == Follow::position, "a learned head hit follows its position");

            juce::MemoryBlock state;
            processor.getStateInformation (state);

            LithiumDrumsProcessor restored;
            restored.setStateInformation (state.getData(), (int) state.getSize());
            expect (waitForKit (restored));

            expect (restored.getMidiMap().sameMapping (processor.getMidiMap()));
            expect (restored.isMidiMapModified());
            expectEquals (restored.getMidiMap().controllers.hihatPedal, 11);
            expect (restored.getMidiMap().velocity == processor.getMidiMap().velocity);
            expect (! restored.getMidiMap().velocity.isNeutral());
            expect (restored.getNoteTable().velocityCurves[(size_t) snarePiece] == makeVelocityTable (VelocityCurve::harder(), -0.5f));
            expectEquals ((int) restored.getNoteTable().targets[60].articulation, offCenter);

            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (10, 60, (juce::uint8) 100), 0);
            restored.processBlock (buffer, midi);
            expectEquals (lastPlayed (restored, *info, "snare"), offCenter);
        }

        beginTest ("Following notes can be switched off per articulation");
        {
            const auto open = articulation (*info, "hihat", "open");
            processor.setFollow (open, Follow::none);
            expect (processor.getNoteTable().targets[46].follow == Follow::none);
            expect (processor.getNoteTable().targets[26].follow == Follow::none, "open edge plays the same articulation here");
            expect (processor.getNoteTable().targets[42].follow == Follow::pedal);
        }

        beginTest ("processBlock doesn't allocate with controllers and a new note table");
        {
            processor.setMidiMap (MidiMap::rolandVDrums(), false);   // waits in the queue for the first block

            constexpr int blockSize = 256;
            juce::AudioBuffer<float> buffer (2, blockSize);
            std::vector<juce::MidiBuffer> blocks (300);

            for (size_t i = 0; i < blocks.size(); ++i)
            {
                blocks[i].addEvent (juce::MidiMessage::controllerEvent (10, 4, (int) ((i * 13) % 128)), 0);
                blocks[i].addEvent (juce::MidiMessage::controllerEvent (10, 16, (int) ((i * 29) % 128)), 5);

                for (const auto note : { 22, 26, 38, 42, 44, 46, 48, 51 })
                    if ((i + (size_t) note) % 3 == 0)
                        blocks[i].addEvent (juce::MidiMessage::noteOn (10, note, (juce::uint8) 100), (int) ((i * 17 + (size_t) note) % blockSize));
            }

            processor.prepareToPlay (sampleRate, blockSize);
            int allocations = 0;
            {
                test::ScopedAllocationCounter counter;

                for (auto& midi : blocks)
                    processor.processBlock (buffer, midi);

                allocations = counter.count();
            }

            expectEquals (allocations, 0);
            expect (processor.getFeedback().hihatOpenness.load() >= 0.0f);
        }
    }
};

//==============================================================================
class VelocityTests final : public juce::UnitTest
{
public:
    VelocityTests() : juce::UnitTest ("Velocity curves", "Lithium") {}

    /** The velocity a piece's last hit played at. */
    static int lastVelocity (const MidiRig& rig, const juce::String& pieceId)
    {
        const auto hit = LastHit::unpack (rig.feedback.lastHit[(size_t) rig.piece (pieceId)].load());
        return hit.has_value() ? hit->velocity : -1;
    }

    void runTest() override
    {
        beginTest ("Curves: the presets, smooth between nodes, never overshooting");
        {
            const auto linear = VelocityCurve::linear();
            expect (linear.isLinear());

            for (int v = 0; v < 128; ++v)
                expectWithinAbsoluteError (linear ((float) v), (float) v, 0.001f);

            const auto softer = VelocityCurve::softer(), harder = VelocityCurve::harder();
            expectWithinAbsoluteError (softer (64.0f), 36.0f, 0.001f, "a curve passes through its nodes");
            expectWithinAbsoluteError (harder (64.0f), 92.0f, 0.001f);
            expectWithinAbsoluteError (softer (127.0f), 127.0f, 0.001f);
            expectWithinAbsoluteError (harder (0.0f), 0.0f, 0.001f);

            for (int v = 1; v < 128; ++v)
            {
                expect (softer ((float) v) >= softer ((float) v - 1.0f) && harder ((float) v) >= harder ((float) v - 1.0f), "rising nodes, rising curve");
                expect (softer ((float) v) <= (float) v && harder ((float) v) >= (float) v);
            }

            // A plateau stays flat: no bump above it between its nodes.
            const VelocityCurve plateau { { { 0, 0 }, { 30, 100 }, { 60, 100 }, { 127, 127 } } };

            for (int v = 30; v <= 60; ++v)
                expectWithinAbsoluteError (plateau ((float) v), 100.0f, 0.001f);

            for (int v = 0; v < 128; ++v)
                expect (plateau ((float) v) >= 0.0f && plateau ((float) v) <= 127.0f);
        }

        beginTest ("Curves are sanitised: sorted, ends at 0 and 127, limited nodes");
        {
            const VelocityCurve messy { { { 127, 200 }, { 40, 50 }, { 3, -10 }, { 40.2f, 60 }, { 90, 100 } } };
            const auto clean = messy.sanitised();
            expectEquals ((int) clean.nodes.size(), 4, "the node a fraction away from another is dropped");
            expectEquals (clean.nodes.front().in, 0.0f);
            expectEquals (clean.nodes.front().out, 0.0f);
            expectEquals (clean.nodes.back().in, 127.0f);
            expectEquals (clean.nodes.back().out, 127.0f);

            VelocityCurve crowded;

            for (int i = 0; i <= 20; ++i)
                crowded.nodes.push_back ({ (float) i * 6.0f, (float) i * 3.0f });

            expectEquals ((int) crowded.sanitised().nodes.size(), VelocityCurve::maxNodes);

            const VelocityCurve straight { { { 0, 0 }, { 50, 50 }, { 127, 127 } } };
            expect (straight.sanitised().nodes.empty(), "a straight line is stored as no nodes");
            expect (straight == VelocityCurve::linear());
        }

        beginTest ("Global Velocity: Softer (max 90), Default and Harder (min 60)");
        {
            expectWithinAbsoluteError (applyGlobalVelocity (127.0f, -1.0f), 90.0f, 0.001f);
            expectWithinAbsoluteError (applyGlobalVelocity (1.0f, -1.0f), 1.0f, 0.001f);
            expectWithinAbsoluteError (applyGlobalVelocity (1.0f, 1.0f), 60.0f, 0.001f);
            expectWithinAbsoluteError (applyGlobalVelocity (127.0f, 1.0f), 127.0f, 0.001f);
            expectWithinAbsoluteError (applyGlobalVelocity (127.0f, -0.5f), 108.5f, 0.001f, "it moves smoothly between the marks");
            expectWithinAbsoluteError (applyGlobalVelocity (1.0f, 0.5f), 30.5f, 0.001f);

            for (int v = 1; v < 128; ++v)
                expectWithinAbsoluteError (applyGlobalVelocity ((float) v, 0.0f), (float) v, 0.0001f);

            expect (makeVelocityTable (VelocityCurve::linear(), 0.0f) == linearVelocityTable(), "neutral settings change nothing");
        }

        beginTest ("A hit never drops to silence, and never goes past 127");
        {
            const VelocityCurve silent { { { 0, 0 }, { 64, 0 }, { 127, 0 } } };

            for (const auto v : makeVelocityTable (silent, 0.0f))
                expectEquals ((int) v, 1);

            NoteTable table;
            table.velocityAdjust[38] = PieceVelocity::maxAdjust;
            table.velocityAdjust[40] = -PieceVelocity::maxAdjust;
            expectEquals (table.velocityFor (38, 0, 100), 127);
            expectEquals (table.velocityFor (40, 0, 20), 1);
            expectEquals (table.velocityFor (41, 0, 127, 1.0f), 127, "edge compensation is clamped too");
            expectEquals (table.velocityFor (41, 0, 80, 1.0f), 120);
            expectEquals (table.velocityFor (41, 0, 80, -1.0f), 40);

            // The adjust comes before the curve: 50 + 10 goes through the curve as 60.
            table.velocityCurves[2] = makeVelocityTable (VelocityCurve::harder(), 0.0f);
            table.velocityAdjust[45] = 10;
            expectEquals (table.velocityFor (45, 2, 50), (int) table.velocityCurves[2][60]);
        }

        beginTest ("Old maps load as linear and neutral");
        {
            MidiMap old;
            expect (MidiMap::parse (R"({ "name": "Old", "controllers": { "pads": { "snare": { "curve": 0.2 } } },
                                         "notes": [ { "note": 38, "role": "snare" } ] })", old).wasOk());
            expect (old.velocity.isNeutral());
            expect (old.controllers.positionVelocity == std::array<float, numPositionSources> {});
            expect (! old.toJson().contains ("\"velocity\""), "neutral settings aren't written out");

            for (const auto& m : MidiMap::factoryMaps())
                expect (m.velocity.isNeutral(), m.name);

            MidiRig rig;
            const auto table = resolveMidiMap (old, rig.info);

            for (const auto& curve : table.velocityCurves)
                expect (curve == linearVelocityTable());

            expect (table.velocityAdjust == std::array<std::int8_t, 128> {});
        }

        beginTest ("Velocity settings survive a round trip through JSON");
        {
            MidiRig rig;
            auto map = MidiMap::rolandVDrums();
            map.velocity.global = 0.25f;
            map.controllers.positionVelocity[(size_t) PositionSource::toms] = -0.3f;

            const auto snare = rig.piece ("snare");
            PieceVelocity pv;
            pv.curve = { { { 0, 0 }, { 40, 70.5f }, { 100, 110 }, { 127, 120 } } };
            pv.setAdjust (velocityAdjustKey (rig.info, rig.articulation ("snare", "rimshot")), 15);
            setPieceVelocity (map.velocity, rig.info, snare, pv);
            expectEquals ((int) map.velocity.pieces.size(), 1);

            MidiMap back;
            expect (MidiMap::parse (map.toJson(), back).wasOk());
            expect (back.sameMapping (map));
            expect (back.velocity == map.velocity);
            expectEquals (back.controllers.positionVelocity[(size_t) PositionSource::toms], -0.3f);

            expect (MidiMap::parse (R"({ "velocity": { "pieces": [ { "role": "drum" } ] }, "notes": [] })", back).failed());
            expect (MidiMap::parse (R"({ "velocity": { "pieces": [ { "role": "snare", "curve": [ 3 ] } ] }, "notes": [] })", back).failed());
        }

        beginTest ("Each piece gets its own curve, each articulation its own adjust");
        {
            MidiRig rig;
            const auto snare = rig.piece ("snare"), kick = rig.piece ("kick");
            const auto rimshot = rig.articulation ("snare", "rimshot");

            auto map = MidiMap::kitDefault();
            PieceVelocity pv;
            pv.curve = VelocityCurve::harder();
            pv.setAdjust (velocityAdjustKey (rig.info, rimshot), 20);
            setPieceVelocity (map.velocity, rig.info, snare, pv);
            map.velocity.global = -1.0f;

            const auto table = resolveMidiMap (map, rig.info);
            expect (table.velocityCurves[(size_t) snare] == makeVelocityTable (VelocityCurve::harder(), -1.0f));
            expect (table.velocityCurves[(size_t) kick] == makeVelocityTable (VelocityCurve::linear(), -1.0f), "global applies to every piece");

            for (int n = 0; n < 128; ++n)
                expectEquals ((int) table.velocityAdjust[(size_t) n], table.targets[(size_t) n].articulation == rimshot ? 20 : 0);

            expect (findPieceVelocity (map.velocity, rig.info, snare) != nullptr);
            expect (findPieceVelocity (map.velocity, rig.info, kick) == nullptr);

            // Saved names are matched like notes: "rim" from another kit is
            // this kit's rimshot, "cross_stick" its sidestick. The
            // articulation's own name wins, and setting it replaces the alias.
            const auto sidestick = rig.articulation ("snare", "sidestick");
            PieceVelocity other;
            other.setAdjust ("rim", 9);
            other.setAdjust ("cross_stick", -7);
            expectEquals (velocityAdjustFor (other, rig.info, rimshot), 9);
            expectEquals (velocityAdjustFor (other, rig.info, sidestick), -7);
            expectEquals (velocityAdjustFor (other, rig.info, rig.articulation ("snare", "center")), 0);
            other.setAdjust ("rimshot", 4);
            expectEquals (velocityAdjustFor (other, rig.info, rimshot), 4);
            setVelocityAdjust (other, rig.info, sidestick, 11);
            expectEquals (velocityAdjustFor (other, rig.info, sidestick), 11);
            expectEquals (other.adjustFor ("cross_stick"), 0, "the alias it was saved under is replaced");

            // Back to neutral: the piece's entry goes.
            setPieceVelocity (map.velocity, rig.info, snare, {});
            expect (map.velocity.pieces.empty());
        }

        beginTest ("MIDI notes play through the curves; grooves and clicks don't");
        {
            MidiRig rig;
            auto map = MidiMap::rolandVDrums();
            PieceVelocity pv;
            pv.curve = VelocityCurve::softer();
            setPieceVelocity (map.velocity, rig.info, rig.piece ("snare"), pv);
            rig.useMap (map);

            rig.note (40, 64);   // the rimshot: no position
            expectEquals (lastVelocity (rig, "snare"), juce::roundToInt (VelocityCurve::softer() (64.0f)));

            rig.note (36, 64);
            expectEquals (lastVelocity (rig, "kick"), 64, "the kick has no curve");

            // A groove hit or a click on the kit names an articulation and keeps its velocity.
            rig.engine.trigger (*rig.kit, rig.articulation ("snare", "rimshot"), 64, rig.params);
            expectEquals (lastVelocity (rig, "snare"), 64);
            rig.engine.stopAll (*rig.kit);
        }

        beginTest ("Edge compensation: hits that follow the position get louder towards the edge");
        {
            MidiRig rig;
            auto map = MidiMap::rolandVDrums();
            map.controllers.positionVelocity[(size_t) PositionSource::snare] = 1.0f;
            rig.useMap (map);

            rig.note (38, 80);
            expectEquals (lastVelocity (rig, "snare"), 80, "no position yet");

            rig.cc (16, 0);
            rig.note (38, 80);
            expectEquals (lastVelocity (rig, "snare"), 80, "a centre hit is left alone");

            rig.cc (16, 127);
            rig.note (38, 80);
            expectEquals (lastVelocity (rig, "snare"), 120, "the far edge, half as loud again");

            rig.note (40, 80);
            expectEquals (lastVelocity (rig, "snare"), 80, "rimshots don't follow the position");

            rig.setControllers ([] (auto& c) { c.positionEnabled[(size_t) PositionSource::snare] = false; });
            rig.note (38, 80);
            expectEquals (lastVelocity (rig, "snare"), 80, "positional sensing off: no compensation");
            rig.engine.stopAll (*rig.kit);
        }
    }
};

//==============================================================================
class EdrumTests final : public juce::UnitTest
{
public:
    EdrumTests() : juce::UnitTest ("E-drum controllers", "Lithium") {}

    void runTest() override
    {
        beginTest ("Calibration, invert and the curve shape the pedal and the pads");
        {
            ControllerSettings c;
            expectWithinAbsoluteError (DrumEngine::pedalOpenness (127, c), 0.0f, 1.0e-6f, "Roland: 127 is closed");
            expectWithinAbsoluteError (DrumEngine::pedalOpenness (0, c), 1.0f, 1.0e-6f);

            c.pedalMin = 20;
            c.pedalMax = 100;
            expectWithinAbsoluteError (DrumEngine::pedalOpenness (100, c), 0.0f, 1.0e-6f, "the calibrated top is fully closed");
            expectWithinAbsoluteError (DrumEngine::pedalOpenness (10, c), 1.0f, 1.0e-6f, "below the calibrated bottom is fully open");
            expectWithinAbsoluteError (DrumEngine::pedalOpenness (60, c), 0.5f, 1.0e-6f);

            c.pedalInverted = true;
            expectWithinAbsoluteError (DrumEngine::pedalOpenness (100, c), 1.0f, 1.0e-6f);

            c.pedalCurve = 1.0f;
            expect (DrumEngine::pedalOpenness (60, c) > 0.75f, "a positive curve opens the hats sooner");
            c.pedalCurve = -1.0f;
            expect (DrumEngine::pedalOpenness (60, c) < 0.1f, "a negative curve opens them later");

            c.positionMin[0] = 10;
            c.positionMax[0] = 110;
            expectWithinAbsoluteError (DrumEngine::padPosition (0, 60, c), 0.5f, 1.0e-6f);
            expectWithinAbsoluteError (DrumEngine::padPosition (0, 120, c), 1.0f, 1.0e-6f);
        }

        beginTest ("The thresholds move where the hats change step");
        {
            ControllerSettings c;

            for (const auto v : { 0.0f, 0.1f, 0.375f, 0.6f, 1.0f })
                expectWithinAbsoluteError (DrumEngine::hatLadderValue (v, c), v, 1.0e-5f, "the default thresholds change nothing");

            c.hatThresholds = { 0.3f, 0.5f, 0.7f, 0.9f };
            expectWithinAbsoluteError (DrumEngine::hatLadderValue (0.3f, c), 0.125f, 1.0e-5f);
            expectWithinAbsoluteError (DrumEngine::hatLadderValue (0.15f, c), 0.0625f, 1.0e-5f);
            expectWithinAbsoluteError (DrumEngine::hatLadderValue (0.95f, c), 0.9375f, 1.0e-5f);
        }

        MidiRig rig (makeExtendedKit());
        rig.useMap (MidiMap::rolandVDrums());
        const auto closed = rig.articulation ("hihat", "closed"), open = rig.articulation ("hihat", "open");
        const auto pedal = rig.articulation ("hihat", "pedal"), splash = rig.articulation ("hihat", "splash");

        beginTest ("The kit's pedal, foot splash and ride edge are found");
        {
            const auto& hat = rig.kit->pieces[(size_t) rig.piece ("hihat")];
            expectEquals (hat.pedalArticulation, pedal);
            expectEquals (hat.splashArticulation, splash);
            expectEquals (rig.kit->pieces[(size_t) rig.piece ("ride")].edgeArticulation, rig.articulation ("ride", "edge"));
        }

        beginTest ("A pedal resting on a value doesn't wobble, and a real move is taken as it is");
        {
            rig.ccAt (0.0, 10, 4, 64);
            const auto resting = rig.feedback.hihatOpenness.load();
            rig.ccAt (10.0, 10, 4, 65);
            expectEquals (rig.feedback.hihatOpenness.load(), resting, "one step is sensor noise");
            rig.ccAt (20.0, 10, 4, 80);
            expectWithinAbsoluteError (rig.feedback.hihatOpenness.load(), 1.0f - 80.0f / 127.0f, 1.0e-6f);
            rig.events();
        }

        beginTest ("Controllers latch per MIDI channel, so a note uses its own channel's pedal");
        {
            rig.ccAt (100.0, 2, 4, 0);      // channel 2: open
            rig.ccAt (101.0, 10, 4, 127);   // channel 10: closed
            rig.noteAt (102.0, 2, 46);
            expectEquals (rig.lastPlayed ("hihat"), open);
            rig.noteAt (103.0, 10, 46);
            expectEquals (rig.lastPlayed ("hihat"), closed);
            rig.noteAt (104.0, 5, 46);
            expectEquals (rig.lastPlayed ("hihat"), closed, "a channel with no pedal of its own uses the latest");
            rig.engine.stopAll (*rig.kit);
            rig.events();
        }

        beginTest ("A pedal on another channel than the one set is ignored, and the monitor says so");
        {
            rig.setControllers ([] (auto& c) { c.hihatChannel = 10; });
            const auto before = rig.feedback.hihatOpenness.load();
            rig.ccAt (200.0, 3, 4, 0);
            expectEquals (rig.feedback.hihatOpenness.load(), before);
            const auto events = rig.events();
            expect (events.size() == 1 && events[0].detail == MidiMonitorEvent::Detail::ignored);
            rig.setControllers ([] (auto& c) { c.hihatChannel = 0; });
        }

        beginTest ("Closing the pedal fast plays a chick, until the module sends pedal notes itself");
        {
            auto chicks = [&rig]
            {
                int n = 0;

                for (const auto& e : rig.events())
                    n += e.kind == MidiMonitorEvent::Kind::pedalChick ? 1 : 0;

                return n;
            };

            rig.ccAt (1000.0, 10, 4, 0);
            rig.ccAt (1020.0, 10, 4, 127);
            expectEquals (chicks(), 1, "open to shut in 20 ms is a stomp");
            expectEquals (rig.lastPlayed ("hihat"), pedal);

            // Slowly, over a second: no chick.
            rig.ccAt (2000.0, 10, 4, 0);

            for (int i = 1; i <= 20; ++i)
                rig.ccAt (2000.0 + i * 50.0, 10, 4, i * 127 / 20);

            expectEquals (chicks(), 0, "a slow close isn't a chick");

            // Once the module sends its own chick (note 44), the pedal stops making them.
            rig.noteAt (3000.0, 10, 44, 80);
            rig.ccAt (3100.0, 10, 4, 0);
            rig.ccAt (3115.0, 10, 4, 127);
            expectEquals (chicks(), 0, "automatic mode leaves chicks to the module once it sends them");

            rig.setControllers ([] (auto& c) { c.chickFromPedal = ChickMode::always; });
            rig.ccAt (3200.0, 10, 4, 0);
            rig.ccAt (3215.0, 10, 4, 127);
            expectEquals (chicks(), 1, "always means always");
            rig.setControllers ([] (auto& c) { c.chickFromPedal = ChickMode::off; });
            rig.ccAt (3300.0, 10, 4, 0);
            rig.ccAt (3315.0, 10, 4, 127);
            expectEquals (chicks(), 0);
            rig.engine.stopAll (*rig.kit);
        }

        beginTest ("Closing and quickly reopening the pedal plays the foot splash");
        {
            auto splashes = [&rig]
            {
                int n = 0;

                for (const auto& e : rig.events())
                    n += e.kind == MidiMonitorEvent::Kind::footSplash ? 1 : 0;

                return n;
            };

            rig.ccAt (4000.0, 10, 4, 0);
            rig.ccAt (4015.0, 10, 4, 127);
            rig.ccAt (4080.0, 10, 4, 60);
            rig.ccAt (4100.0, 10, 4, 0);
            expectEquals (splashes(), 1);
            expectEquals (rig.lastPlayed ("hihat"), splash);

            // Reopening a second later is just opening the hats.
            rig.ccAt (5000.0, 10, 4, 127);
            rig.ccAt (6000.0, 10, 4, 0);
            expectEquals (splashes(), 0);

            rig.setControllers ([] (auto& c) { c.footSplash = false; });
            rig.ccAt (7000.0, 10, 4, 0);
            rig.ccAt (7015.0, 10, 4, 127);
            rig.ccAt (7080.0, 10, 4, 0);
            expectEquals (splashes(), 0, "switched off");
            rig.setControllers ([] (auto& c) { c.footSplash = true; });
            rig.engine.stopAll (*rig.kit);
        }

        beginTest ("Shutting the pedal on ringing open hats fades them out, not cuts them");
        {
            rig.ccAt (8000.0, 10, 4, 0);
            rig.noteAt (8001.0, 10, 46);
            rig.render (512);
            rig.ccAt (8020.0, 10, 4, 127);
            rig.render (64);
            expect (rig.voices() >= 1, "still sounding just after the pedal shuts");
            rig.render ((int) (0.1 * sampleRate));
            expectEquals (rig.voices(), 0, "gone within 100 ms");
            rig.events();
        }

        beginTest ("Position ranges from kit.json decide where each step plays alone");
        {
            const auto& ladder = rig.kit->pieces[(size_t) rig.piece ("snare")].positionLadder;
            expectEquals (ladder.numSteps, 2);
            expectWithinAbsoluteError (ladder.high[0], 0.2f, 1.0e-6f);
            expectWithinAbsoluteError (ladder.low[1], 0.4f, 1.0e-6f);

            rig.ccAt (9000.0, 10, 16, juce::roundToInt (0.5f * 127.0f));
            rig.noteAt (9001.0, 10, 38);
            expectEquals (rig.voices(), 1, "0.5 is inside the off-centre range");
            expectEquals (rig.lastPlayed ("snare"), rig.articulation ("snare", "off_center"));
            rig.engine.stopAll (*rig.kit);

            rig.ccAt (9100.0, 10, 16, juce::roundToInt (0.3f * 127.0f));
            rig.noteAt (9101.0, 10, 38);
            expectEquals (rig.voices(), 2, "between the ranges, both");
            rig.engine.stopAll (*rig.kit);
            rig.events();
        }

        beginTest ("On a drum without positional recordings, the edge sounds brighter");
        {
            auto hit = [&rig] (int position)
            {
                rig.engine.stopAll (*rig.kit);
                rig.ccAt (0.0, 10, 18, position);
                rig.noteAt (0.0, 10, 48, 100);
                rig.render (8192);
                juce::AudioBuffer<float> copy (rig.bus);
                return copy;
            };

            const auto centre = hit (0), edge = hit (127);
            expect (brightness (edge) > brightness (centre) * 1.3, "the edge has more highs, relative to its body");
            expect (edge.getMagnitude (0, edge.getNumSamples()) < centre.getMagnitude (0, centre.getNumSamples()) * 1.2f);

            const auto events = rig.events();
            expect (! events.empty() && events.back().detail == MidiMonitorEvent::Detail::emulated);

            rig.setControllers ([] (auto& c) { c.positionEnabled[(size_t) PositionSource::toms] = false; });
            rig.ccAt (0.0, 10, 18, 127);
            rig.noteAt (0.0, 10, 48, 100);
            expect (rig.events().back().detail == MidiMonitorEvent::Detail::none, "a pad switched off doesn't follow its position");
            rig.setControllers ([] (auto& c) { c.positionEnabled[(size_t) PositionSource::toms] = true; });
            rig.engine.stopAll (*rig.kit);
        }

        beginTest ("A digital hi-hat's bow position brightens the hats, on top of the pedal's openness");
        {
            auto hit = [&rig] (int position)
            {
                rig.engine.stopAll (*rig.kit);
                rig.ccAt (0.0, 10, 4, 127);        // pedal shut: closed hats
                rig.ccAt (0.0, 10, 19, position);
                rig.noteAt (0.0, 10, 42, 100);
                rig.render (4096);
                juce::AudioBuffer<float> copy (rig.bus);
                return copy;
            };

            // At full strength, so the tilt is easy to measure.
            rig.setControllers ([] (auto& c) { c.positionEmulation[(size_t) PositionSource::hihat] = 1.0f; });
            const auto bell = hit (0), edge = hit (127);
            expectEquals (rig.lastPlayed ("hihat"), closed, "the pedal still picks how open they are");
            expect (brightness (edge) > brightness (bell), "the hats played at the edge are brighter");

            // The filters themselves, at the far edge: less body, more sizzle.
            const auto [tone, level] = DrumEngine::positionTone (PieceRole::hihat, 1.0f, sampleRate);
            auto db = [&tone] (double hz)
            {
                return 20.0 * std::log10 (std::abs (dsp::response (tone[0], hz, sampleRate) * dsp::response (tone[1], hz, sampleRate)));
            };

            expectWithinAbsoluteError (db (60.0), -3.0, 0.3);
            expectWithinAbsoluteError (db (15000.0), 4.0, 0.5);
            expect (level < 1.0f, "and a little quieter");
            rig.setControllers ([] (auto& c) { c.positionEmulation[(size_t) PositionSource::hihat] = 0.5f; });

            const auto events = rig.events();
            expect (! events.empty() && events.back().detail == MidiMonitorEvent::Detail::pedal);
            expectWithinAbsoluteError (events.back().position, 1.0f, 1.0e-6f, "the monitor shows the bow position");

            rig.setControllers ([] (auto& c) { c.positionEnabled[(size_t) PositionSource::hihat] = false; });
            hit (127);
            expectEquals (rig.events().back().position, -1.0f, "switched off, the hats ignore it");
            rig.setControllers ([] (auto& c) { c.positionEnabled[(size_t) PositionSource::hihat] = true; });
            rig.engine.stopAll (*rig.kit);
        }

        beginTest ("The ride's bow blends towards its edge with the position; the bell stays itself");
        {
            rig.ccAt (0.0, 10, 17, 127);
            rig.noteAt (0.0, 10, 51);
            expectEquals (rig.voices(), 2);
            auto events = rig.events();
            expect (events.back().detail == MidiMonitorEvent::Detail::rideBlend);
            expectEquals ((int) events.back().blendedWith, rig.articulation ("ride", "edge"));
            expectWithinAbsoluteError (events.back().blend, 0.5f, 0.01f, "the default emulation blends half the edge in at the far edge");
            rig.engine.stopAll (*rig.kit);

            rig.ccAt (0.0, 10, 17, 0);
            rig.noteAt (0.0, 10, 51);
            expectEquals (rig.voices(), 1, "near the bell, the bow alone");
            rig.engine.stopAll (*rig.kit);

            rig.noteAt (0.0, 10, 53);
            expectEquals (rig.lastPlayed ("ride"), rig.articulation ("ride", "bell"));
            expectEquals (rig.voices(), 1);
            rig.engine.stopAll (*rig.kit);
            rig.events();
        }

        beginTest ("Controller settings survive a round trip through JSON");
        {
            auto map = MidiMap::rolandVDrums();
            auto& c = map.controllers;
            c.hihatChannel = 10;
            c.pedalMin = 12;
            c.pedalMax = 120;
            c.pedalCurve = -0.25f;
            c.hatThresholds = { 0.2f, 0.4f, 0.6f, 0.8f };
            c.chickFromPedal = ChickMode::always;
            c.chickSensitivity = 0.75f;
            c.footSplash = false;
            c.positionChannel = { 10, 11, 12, 13 };
            c.positionEnabled = { true, false, true, false };
            c.positionMin = { 5, 6, 7, 8 };
            c.positionMax = { 120, 121, 122, 123 };
            c.positionCurve = { 0.5f, -0.5f, 0.0f, 0.25f };
            c.positionEmulation = { 0.25f, 1.0f, 0.75f, 0.6f };

            MidiMap parsed;
            expect (MidiMap::parse (map.toJson(), parsed).wasOk());
            expect (parsed.controllers == c);

            MidiMap old;
            expect (MidiMap::parse (R"({ "controllers": { "hihatPedal": 4 }, "notes": [] })", old).wasOk());
            expect (old.controllers == ControllerSettings(), "maps saved before these settings get the defaults");
        }

        beginTest ("The Millenium MPS-1000's notes play what its pads are");
        {
            const auto map = MidiMap::factoryMap ("millenium-mps1000");
            expectEquals (map.name, juce::String ("Millenium MPS-1000 / 600 / 850"));
            const auto table = resolveMidiMap (map, rig.info);
            auto plays = [&] (int note) { return played (rig.info, table, note); };

            expect (plays (36) == Played { "kick", "hit" });
            expect (plays (38) == Played { "snare", "center" });
            expect (plays (40) == Played { "snare", "rimshot" });
            expect (plays (37) == Played { "snare", "sidestick" });
            expect (plays (48) == Played { "tom1", "center" });
            expect (plays (45) == Played { "tom2", "center" });
            expect (plays (43) == Played { "floor", "center" });
            expect (plays (46) == Played { "hihat", "open" });
            expect (plays (42) == Played { "hihat", "closed" });
            expect (plays (23) == Played { "hihat", "half_open" });
            expect (plays (44) == Played { "hihat", "pedal" });
            expect (plays (22).first == "hihat" && plays (26).first == "hihat", "the MPS-600's hi-hat edge notes");
            expect (plays (51) == Played { "ride", "bow" });
            expect (plays (53) == Played { "ride", "bell" });
            expect (plays (49).first == "crash");
            expect (table.targets[46].follow == Follow::pedal, "the hats follow the pedal once it sends CC4");
        }

        beginTest ("The factory e-kit maps have the controllers of their module family");
        {
            const auto roland = MidiMap::rolandVDrums().controllers;
            expectEquals (roland.hihatPedal, 4);
            expect (roland.position == std::array<int, numPositionSources> { 16, 17, 18, 19 });
            const auto alesis = MidiMap::alesis().controllers;
            expectEquals (alesis.hihatPedal, 4);
            expectEquals (alesis.position[0], -1, "Alesis modules don't send positional sensing");
            expectEquals (alesis.position[(size_t) PositionSource::hihat], -1);
        }
    }
};

static EdrumTests edrumTests;
static MidiMapTests midiMapTests;
static ControllerTests controllerTests;
static MidiProcessorTests midiProcessorTests;
static VelocityTests velocityTests;

} // namespace lithium
