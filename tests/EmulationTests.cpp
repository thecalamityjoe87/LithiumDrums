#include "AllocationCounter.h"
#include "PluginProcessor.h"
#include "engine/DrumEngine.h"
#include "engine/KitLoader.h"
#include "engine/MicEmulation.h"
#include "engine/Mixer.h"

namespace lithium
{

namespace
{
    constexpr double rate = 48000.0;
    constexpr int blockSize = 256;
    const juce::File testKit { LITHIUM_TEST_KIT_DIR };

    /** The test kit (which has its own overheads and room) with emulated ones
        as well, played through an engine and mixer of its own. */
    struct Rig
    {
        EngineFeedback feedback;
        DrumEngine engine { feedback };
        Mixer mixer;
        EngineParams params;
        KitInfo info;
        std::unique_ptr<Kit> kit;

        explicit Rig (bool emulated = true)
        {
            KitLoadOptions options;
            options.emulateRecordedMics = emulated;
            juce::String error;
            kit = KitLoader::loadKit (testKit, MidiMap::kitDefault(), info, error, options);
            jassert (kit != nullptr);
            engine.prepare (rate);
            mixer.prepare (rate, blockSize);
            params.resetForKit (*kit);
            params.velocityCrossfade = false;
        }

        ~Rig()   { engine.stopAll (*kit); }

        int pieceOf (PieceRole role) const
        {
            for (size_t p = 0; p < kit->pieces.size(); ++p)
                if (kit->pieces[p].role == role)
                    return (int) p;

            return -1;
        }

        int sourceOf (int piece) const
        {
            for (int m = 0; m < kit->numMics; ++m)
                if (((kit->pieces[(size_t) piece].emulationSources >> m) & 1u) != 0)
                    return m;

            return -1;
        }

        /** Plays one hit of a piece and renders `seconds`, keeping each
            emulated pair's channels and the piece's own channel. */
        struct Take
        {
            std::array<juce::AudioBuffer<float>, numMicRoles> pairs;   // [overheads] and [room] used
            juce::AudioBuffer<float> drum, main;
        };

        /** Lets everything ring out: hits, the delay lines, the room. */
        void settle()
        {
            engine.stopAll (*kit);
            juce::AudioBuffer<float> out (2, blockSize);

            for (int i = 0; i < (int) (4.0 * rate) / blockSize; ++i)
            {
                mixer.clearBuses (blockSize);
                out.clear();
                mixer.process (*kit, blockSize, params, { out.getWritePointer (0), out.getWritePointer (1) }, {});
            }
        }

        Take play (int piece, double seconds = 0.5)
        {
            settle();
            const auto length = (int) (seconds * rate);
            Take t;

            for (auto* b : { &t.pairs[1], &t.pairs[2], &t.drum, &t.main })
            {
                b->setSize (2, length);
                b->clear();
            }

            engine.trigger (*kit, kit->pieces[(size_t) piece].firstArticulation, 110, params);

            for (int start = 0; start < length; start += blockSize)
            {
                const auto n = std::min (blockSize, length - start);
                mixer.clearBuses (n);
                engine.render (*kit, mixer.getBuses(), 0, n, params);

                for (int ch = 0; ch < 2; ++ch)
                    t.drum.copyFrom (ch, start, mixer.getPieceBus(), piece * 2 + ch, 0, n);

                mixer.process (*kit, n, params, { t.main.getWritePointer (0, start), t.main.getWritePointer (1, start) }, {});

                for (const auto role : { MicRole::overheads, MicRole::room })
                {
                    const auto slot = kit->emulatedMic[(size_t) role];

                    if (slot < 0)
                        continue;

                    const auto& mic = kit->mics[(size_t) slot];

                    for (int ch = 0; ch < 2; ++ch)
                        t.pairs[(size_t) role].copyFrom (ch, start, mixer.getMicBus(), mic.busChannel + ch, 0, n);
                }
            }

            return t;
        }
    };

    float peakOf (const juce::AudioBuffer<float>& b)
    {
        auto p = 0.0f;

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            p = std::max (p, b.getMagnitude (ch, 0, b.getNumSamples()));

        return p;
    }

    int onsetOf (const juce::AudioBuffer<float>& b, int channel)
    {
        const auto threshold = b.getMagnitude (channel, 0, b.getNumSamples()) * 0.1f;

        for (int i = 0; i < b.getNumSamples(); ++i)
            if (std::abs (b.getSample (channel, i)) >= threshold)
                return i;

        return -1;
    }
}

class EmulationTests final : public juce::UnitTest
{
public:
    EmulationTests() : juce::UnitTest ("Mic emulation", "Lithium") {}

    void runTest() override
    {
        beginTest ("A kit with its own overheads and room gets no emulated ones");
        {
            Rig rig (false);
            expectEquals (rig.kit->emulatedMic[(size_t) MicRole::overheads], -1);
            expectEquals (rig.kit->emulatedMic[(size_t) MicRole::room], -1);
            expectEquals (rig.kit->getNumMixerMics(), rig.kit->numMics);
            expect (std::none_of (rig.info.mics.begin(), rig.info.mics.end(), [] (const KitMicInfo& m) { return m.emulated; }));
        }

        beginTest ("Emulated mics take stereo slots after the kit's own, and each piece is heard through one mic");
        {
            Rig rig;
            const auto n = rig.kit->numMics;
            expectEquals (rig.kit->emulatedMic[(size_t) MicRole::overheads], n);
            expectEquals (rig.kit->emulatedMic[(size_t) MicRole::room], n + 1);
            expectEquals (rig.kit->getNumMixerMics(), n + 2);
            expectEquals ((int) rig.info.mics.size(), n + 2);

            for (const auto role : { MicRole::overheads, MicRole::room })
            {
                const auto slot = rig.kit->emulatedMic[(size_t) role];
                const auto& mic = rig.kit->mics[(size_t) slot];
                const auto& micInfo = rig.info.mics[(size_t) slot];
                expect (mic.emulated && micInfo.emulated);
                expect (! mic.direct && ! micInfo.direct);
                expect (mic.role == role && micInfo.role == role);
                expectEquals (mic.numChannels, 2);
                expect (mic.busChannel + 2 <= maxMicBusChannels);
                expectEquals (rig.info.emulatedSlot (role), slot);
            }

            for (const auto& piece : rig.kit->pieces)
                expect (piece.emulationSources == 0 || (piece.emulationSources & (piece.emulationSources - 1)) == 0,
                        "one source mic per piece");

            // The snare, which its close mic recorded, is heard through that.
            const auto snare = rig.pieceOf (PieceRole::snare);
            expect (snare >= 0 && rig.sourceOf (snare) >= 0);
            expect (rig.kit->mics[(size_t) rig.sourceOf (snare)].role == MicRole::close);
        }

        beginTest ("Switched off, the emulated mics are silent");
        {
            Rig rig;
            const auto take = rig.play (rig.pieceOf (PieceRole::snare));
            expect (peakOf (take.drum) > 0.01f, "the snare plays");
            expectEquals (peakOf (take.pairs[(size_t) MicRole::overheads]), 0.0f);
            expectEquals (peakOf (take.pairs[(size_t) MicRole::room]), 0.0f);
        }

        beginTest ("Switched on, the overheads hear the drum after the close mic, in stereo");
        {
            Rig rig;
            rig.params.emulate[(size_t) MicRole::overheads] = true;
            const auto snare = rig.pieceOf (PieceRole::snare);
            const auto take = rig.play (snare);
            const auto& oh = take.pairs[(size_t) MicRole::overheads];
            expect (peakOf (oh) > 0.001f, "the overheads hear the snare");
            expectEquals (peakOf (take.pairs[(size_t) MicRole::room]), 0.0f, "the room is still off");

            // The snare stands about a metre from the pair: 2-5 ms later than its close mic.
            const auto late = (onsetOf (oh, 0) - onsetOf (take.drum, 0)) / rate * 1000.0;
            expect (late > 1.5 && late < 6.0, "arrives " + juce::String (late, 2) + " ms after the close mic");

            // It stands to the right of the snare-centred pair's middle, so
            // it reaches the right mic first (x to the listener's right).
            expect (onsetOf (oh, 1) <= onsetOf (oh, 0), "the right mic hears the snare first");
            expect (! juce::approximatelyEqual (oh.getMagnitude (0, 0, oh.getNumSamples()), oh.getMagnitude (1, 0, oh.getNumSamples())), "not mono");
        }

        beginTest ("The emulated room hears the drum even with its close mic muted or turned down");
        {
            Rig rig;
            rig.params.emulate[(size_t) MicRole::room] = true;
            const auto snare = rig.pieceOf (PieceRole::snare);
            const auto loud = peakOf (rig.play (snare).pairs[(size_t) MicRole::room]);
            expect (loud > 0.001f, "the room hears the snare");

            auto& close = rig.params.mics[(size_t) rig.sourceOf (snare)];
            close.mute = true;
            close.volume = 0.1f;
            const auto take = rig.play (snare, 1.0);
            expectWithinAbsoluteError (peakOf (take.pairs[(size_t) MicRole::room]), loud, loud * 0.25f);
        }

        beginTest ("A drum's Room level takes it out of the emulated room, and its mute out of both");
        {
            Rig rig;
            rig.params.emulate[(size_t) MicRole::overheads] = rig.params.emulate[(size_t) MicRole::room] = true;
            const auto snare = rig.pieceOf (PieceRole::snare);
            auto& piece = rig.params.pieces[(size_t) snare];
            piece.sends[(size_t) MicRole::room] = 0.0f;
            auto take = rig.play (snare, 1.0);
            expect (peakOf (take.pairs[(size_t) MicRole::overheads]) > 0.001f);
            expectEquals (peakOf (take.pairs[(size_t) MicRole::room]), 0.0f);

            piece.sends[(size_t) MicRole::room] = 1.0f;
            piece.mute = true;
            take = rig.play (snare, 1.0);
            expectEquals (peakOf (take.pairs[(size_t) MicRole::overheads]), 0.0f);
            expectEquals (peakOf (take.pairs[(size_t) MicRole::room]), 0.0f);
        }

        beginTest ("Muting an emulated pair's channel stops its feed, and soloing it silences the rest");
        {
            Rig rig;
            rig.params.emulate[(size_t) MicRole::overheads] = true;
            const auto slot = rig.kit->emulatedMic[(size_t) MicRole::overheads];
            const auto snare = rig.pieceOf (PieceRole::snare);

            rig.params.mics[(size_t) slot].solo = true;
            auto take = rig.play (snare);
            expect (peakOf (take.pairs[(size_t) MicRole::overheads]) > 0.001f, "soloed, it plays");
            expectEquals (peakOf (take.drum), 0.0f, "soloed, the close mics are silent");

            rig.params.mics[(size_t) slot].solo = false;
            rig.params.mics[(size_t) slot].mute = true;
            take = rig.play (snare);
            expectEquals (peakOf (take.pairs[(size_t) MicRole::overheads]), 0.0f);
        }

        beginTest ("Emulated mics don't allocate on the audio thread");
        {
            Rig rig;
            rig.params.emulate[(size_t) MicRole::overheads] = rig.params.emulate[(size_t) MicRole::room] = true;
            const auto snare = rig.pieceOf (PieceRole::snare);
            rig.play (snare, 0.1);   // the first block works out the paths
            rig.engine.stopAll (*rig.kit);

            juce::AudioBuffer<float> out (2, blockSize);
            int allocations = 0;

            {
                test::ScopedAllocationCounter counter;

                for (int i = 0; i < 100; ++i)
                {
                    if (i % 20 == 0)
                        rig.engine.trigger (*rig.kit, rig.kit->pieces[(size_t) snare].firstArticulation, 100, rig.params);

                    rig.mixer.clearBuses (blockSize);
                    rig.engine.render (*rig.kit, rig.mixer.getBuses(), 0, blockSize, rig.params);
                    out.clear();
                    rig.mixer.process (*rig.kit, blockSize, rig.params, { out.getWritePointer (0), out.getWritePointer (1) }, {});
                }

                allocations = counter.count();
            }

            expectEquals (allocations, 0);
        }

        beginTest ("A kit change switches emulation off and picks the kit's room");
        {
            LithiumDrumsProcessor processor;
            auto& state = processor.getParameters();
            params::setEmulating (state, MicRole::room, true);
            state.getParameter (params::emulatedRoomIr)->setValueNotifyingHost (1.0f);
            expect (params::isEmulating (state, MicRole::room));

            KitInfo info;
            info.id = "big-rusty";
            params::resetForKit (state, info);
            expect (! params::isEmulating (state, MicRole::room));
            expect (! params::isEmulating (state, MicRole::overheads));

            const auto choice = juce::roundToInt (state.getRawParameterValue (params::emulatedRoomIr)->load());
            expectEquals (choice, emulation::defaultRoomFor ("big-rusty"));
            expect (irs::builtIn()[(size_t) choice].id == "seminar-room");

            info.id = "basalt-onyx";
            params::resetForKit (state, info);
            expect (irs::builtIn()[(size_t) juce::roundToInt (state.getRawParameterValue (params::emulatedRoomIr)->load())].id == "studio");
        }

        beginTest ("Only emulated mics that are switched on have a channel");
        {
            Rig rig;
            LithiumDrumsProcessor processor;
            auto& state = processor.getParameters();
            const auto room = rig.info.emulatedSlot (MicRole::room);

            for (int m = 0; m < rig.kit->numMics; ++m)
                expect (params::hasMicChannel (state, rig.info, m));

            expect (! params::hasMicChannel (state, rig.info, room));
            params::setEmulating (state, MicRole::room, true);
            expect (params::hasMicChannel (state, rig.info, room));
            expect (! params::hasMicChannel (state, rig.info, rig.info.emulatedSlot (MicRole::overheads)));
        }
    }
};

static EmulationTests emulationTests;

} // namespace lithium
