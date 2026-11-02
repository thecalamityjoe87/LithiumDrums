#include "AllocationCounter.h"
#include "FxPresets.h"
#include "PluginProcessor.h"
#include "engine/IrLoader.h"
#include "engine/KitLoader.h"
#include "engine/Mixer.h"

#include <thread>

namespace lithium
{

namespace
{
    constexpr double rate = 48000.0;
    const juce::File testKit { LITHIUM_TEST_KIT_DIR };

    void setParam (LithiumDrumsProcessor& p, const juce::String& id, float value)
    {
        auto* param = p.getParameters().getParameter (id);
        jassert (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    float value (LithiumDrumsProcessor& p, const juce::String& id)
    {
        auto* param = p.getParameters().getParameter (id);
        return param->convertFrom0to1 (param->getValue());
    }

    bool waitFor (const std::function<bool()>& done, int ms = 5000)
    {
        for (int waited = 0; waited < ms; waited += 10)
        {
            if (done())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        return done();
    }

    bool waitForKit (LithiumDrumsProcessor& p)
    {
        const auto loaded = waitFor ([&p] { p.applyPendingKitLoad(); return ! p.isKitLoading() && p.getKitInfo() != nullptr; });
        p.applyPendingKitLoad();
        return loaded;
    }

    /** A mixer with the test kit, and an impulse on the first drum's channel. */
    struct MixerRig
    {
        MixerRig()
        {
            KitInfo info;
            juce::String error;
            kit = KitLoader::loadKit (testKit, MidiMap::kitDefault(), info, error);
            mixer.prepare (rate, block);
            params.resetForKit (*kit);
        }

        /** Runs the mixer for a while with an impulse at impulseAt (after the
            gains' 20 ms smoothing has settled); returns the main out. */
        juce::AudioBuffer<float> run (double seconds)
        {
            const auto length = (int) (seconds * rate) + impulseAt;
            juce::AudioBuffer<float> out (2, length);
            out.clear();
            Mixer::AuxOutputs aux {};

            for (int start = 0; start < length; start += block)
            {
                const auto n = std::min (block, length - start);
                mixer.clearBuses (n);

                if (impulseAt >= start && impulseAt < start + n)
                {
                    mixer.getPieceBus().setSample (0, impulseAt - start, 1.0f);
                    mixer.getPieceBus().setSample (1, impulseAt - start, 1.0f);
                }

                mixer.process (*kit, n, params, { out.getWritePointer (0, start), out.getWritePointer (1, start) }, aux);
            }

            return out;
        }

        static constexpr int block = 256;
        static constexpr int impulseAt = (int) (0.05 * rate);
        std::unique_ptr<Kit> kit;
        Mixer mixer;
        EngineParams params;
    };
}

class FxTests final : public juce::UnitTest
{
public:
    FxTests() : juce::UnitTest ("FX", "Lithium") {}

    void runTest() override
    {
        beginTest ("Projects from before the FX page load with their reverb sends and settings");
        {
            LithiumDrumsProcessor before;
            setParam (before, params::pieceParamId (0, params::reverbSend), -6.0f);
            setParam (before, params::micParamId (0, params::reverbSend), -12.0f);
            setParam (before, params::reverbDecay, 3.0f);
            setParam (before, params::reverbReturn, -3.0f);

            juce::MemoryBlock state;
            before.getStateInformation (state);

            // Take out everything an M7 project didn't have.
            auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
            auto tree = juce::ValueTree::fromXml (*xml);
            int removed = 0;

            for (int i = tree.getNumChildren(); --i >= 0;)
            {
                const auto id = tree.getChild (i)["id"].toString();

                if (id.startsWith ("room_") || id.startsWith ("delay_") || (id.startsWith ("master_") && id != params::masterVolume)
                    || id == params::fxBypass || id.endsWith ("_room_send") || id.endsWith ("_delay_send") || id.endsWith ("_send_pre")
                    || id == params::reverbPan || id == params::reverbMix)
                {
                    tree.removeChild (i, nullptr);
                    ++removed;
                }
            }

            expect (removed > 100);
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), old);

            LithiumDrumsProcessor after;
            setParam (after, params::delayFeedback, 80.0f);   // something to be put back to its default
            after.setStateInformation (old.getData(), (int) old.getSize());

            expectWithinAbsoluteError (value (after, params::pieceParamId (0, params::reverbSend)), -6.0f, 0.05f, "the old reverb send is the Plate send");
            expectWithinAbsoluteError (value (after, params::micParamId (0, params::reverbSend)), -12.0f, 0.05f);
            expectWithinAbsoluteError (value (after, params::reverbDecay), 3.0f, 0.01f);
            expectWithinAbsoluteError (value (after, params::reverbReturn), -3.0f, 0.05f);
            expectWithinAbsoluteError (value (after, params::reverbMix), 100.0f, 0.01f, "new settings start at their defaults");
            expectWithinAbsoluteError (value (after, params::delayFeedback), 35.0f, 0.01f);
            expectEquals (value (after, params::pieceParamId (0, params::roomSend)), params::minVolumeDb);
            expect (value (after, params::limitOn) < 0.5f);
            expect (value (after, params::ChannelId::master().param (params::compOn)) < 0.5f, "the master chain starts off");
        }

        beginTest ("Sends reach their return before or after the fader");
        {
            MixerRig rig;
            auto& channel = rig.params.pieces[0];
            channel.faderGain = 0.0f;
            channel.channel.reverbSend = 1.0f;

            auto energy = [] (const juce::AudioBuffer<float>& b) { return b.getRMSLevel (0, 0, b.getNumSamples()); };

            expect (energy (rig.run (0.5)) < 1.0e-7f, "post-fader, a closed fader sends nothing");

            channel.channel.sendsPreFader = true;
            expect (energy (rig.run (0.5)) > 1.0e-5f, "pre-fader, it still sends");

            rig.params.fxBypass = true;
            rig.run (0.1);   // the return fades out
            expect (energy (rig.run (0.5)) < 1.0e-7f, "the FX bypass silences the returns");
        }

        beginTest ("The delay echoes at the synced time, and in free time");
        {
            MixerRig rig;
            auto& channel = rig.params.pieces[0];
            channel.faderGain = 0.0f;
            channel.channel.sendsPreFader = true;
            channel.channel.delaySend = 1.0f;
            rig.params.hostBpm = 120.0;
            rig.params.delay.sync = true;
            rig.params.delay.note = 4;   // 1/4
            rig.params.delay.delay.feedback = 0.0f;
            rig.params.delay.delay.lowCutHz = 20.0f;
            rig.params.delay.delay.highCutHz = 20000.0f;

            auto firstEcho = [] (const juce::AudioBuffer<float>& b)
            {
                for (int i = 0; i < b.getNumSamples(); ++i)
                    if (std::abs (b.getSample (0, i)) > 0.05f)
                        return i;

                return -1;
            };

            expectWithinAbsoluteError (firstEcho (rig.run (1.0)) - MixerRig::impulseAt, (int) (0.5 * rate), 4, "a quarter note at 120 bpm is 500 ms");

            rig.params.delay.sync = false;
            rig.params.delay.freeTimeMs = 120.0f;
            rig.mixer.prepare (rate, MixerRig::block);
            expectWithinAbsoluteError (firstEcho (rig.run (0.5)) - MixerRig::impulseAt, (int) (0.12 * rate), 4);

            expectWithinAbsoluteError (TempoDelay::syncedMs (5, 120.0), 750.0f, 0.01f, "a dotted quarter");
            expectWithinAbsoluteError (TempoDelay::syncedMs (9, 120.0), 166.667f, 0.01f, "an eighth-note triplet");
        }

        beginTest ("The limiter holds its ceiling, with and without lookahead");
        {
            for (const auto lookahead : { false, true })
            {
                Limiter limiter;
                limiter.prepare (rate);
                LimiterSettings s { true, -3.0f, 50.0f, lookahead };
                const auto ceiling = juce::Decibels::decibelsToGain (-3.0f);
                juce::Random random (3);
                juce::AudioBuffer<float> audio (2, 4096);
                auto peak = 0.0f;

                for (int pass = 0; pass < 20; ++pass)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < audio.getNumSamples(); ++i)
                            audio.setSample (ch, i, (random.nextFloat() * 2.0f - 1.0f) * (i % 700 < 20 ? 4.0f : 0.3f));

                    limiter.process (audio.getArrayOfWritePointers(), audio.getNumSamples(), s);

                    if (pass > 0)   // after the switch-on fade
                        peak = std::max (peak, audio.getMagnitude (0, audio.getNumSamples()));
                }

                expect (peak <= ceiling + 1.0e-5f, "peak " + juce::String (peak) + (lookahead ? " with" : " without") + " lookahead");
                expect (limiter.takeMaxGainReductionDb() > 6.0f);
            }

            Limiter limiter;
            limiter.prepare (rate);
            LimiterSettings s { true, 0.0f, 50.0f, true };
            juce::AudioBuffer<float> audio (2, 512);
            audio.clear();
            audio.setSample (0, 10, 0.5f);
            audio.setSample (1, 10, 0.5f);
            limiter.process (audio.getArrayOfWritePointers(), 512, s);
            const auto delay = limiter.getLookaheadSamples();
            expectEquals (delay, juce::roundToInt (Limiter::lookaheadMs * 0.001 * rate));
            expect (std::abs (audio.getSample (0, 10 + delay)) > 0.0f, "lookahead delays the audio by its latency");
        }

        beginTest ("Latency is reported only while lookahead is on");
        {
            LithiumDrumsProcessor processor;
            processor.prepareToPlay (rate, 256);
            expectEquals (processor.getLatencySamples(), 0);

            setParam (processor, params::limitLookahead, 1.0f);
            processor.prepareToPlay (rate, 256);
            expectEquals (processor.getLatencySamples(), juce::roundToInt (Limiter::lookaheadMs * 0.001 * rate));

            setParam (processor, params::limitOn, 0.0f);
            processor.prepareToPlay (rate, 256);
            expect (processor.getLatencySamples() > 0, "bypassing the limiter keeps its latency, so the host doesn't re-align");

            setParam (processor, params::limitLookahead, 0.0f);
            processor.prepareToPlay (rate, 256);
            expectEquals (processor.getLatencySamples(), 0);
        }

        beginTest ("The room reverb has no latency, and every built-in IR loads");
        {
            ConvolutionReverb reverb;
            reverb.prepare (rate, 256);
            expectEquals (reverb.getLatency(), 0);

            IrLoader loader (reverb);
            expect (irs::builtIn().size() >= 4);

            for (size_t i = 0; i < irs::builtIn().size(); ++i)
            {
                const auto serial = loader.getStatus().serial;
                loader.request ({ (int) i, {}, 1.0f });
                expect (waitFor ([&] { return loader.getStatus().serial > serial && ! loader.getStatus().loading; }));
                const auto status = loader.getStatus();
                expect (status.error.isEmpty(), status.error);
                expectEquals (status.name, irs::builtIn()[i].name);
                expect (status.seconds > 0.5f && status.rt60 > 0.1f, irs::builtIn()[i].name);
                expect (irs::builtIn()[i].licenseName.isNotEmpty(), "every IR has its licence");
            }

            // Decay shortens it.
            const auto full = loader.getStatus().seconds;
            loader.request ({ (int) irs::builtIn().size() - 1, {}, 0.3f });
            expect (waitFor ([&] { return ! loader.getStatus().loading && loader.getStatus().seconds < full; }));

            // A file that isn't there keeps the IR that was playing, and says why.
            loader.request ({ -1, juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("no-such-ir.wav"), 1.0f });
            expect (waitFor ([&] { return ! loader.getStatus().loading; }));
            expect (loader.getStatus().error.isNotEmpty());
        }

        beginTest ("The master chain leaves the mix alone with every stage off");
        {
            MasterChain chain;
            chain.prepare (rate, 512);
            juce::Random random (9);
            juce::AudioBuffer<float> audio (2, 512);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    audio.setSample (ch, i, random.nextFloat() - 0.5f);

            juce::AudioBuffer<float> original (audio);
            chain.process (audio.getArrayOfWritePointers(), 512, MasterSettings {});

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    expectEquals (audio.getSample (ch, i), original.getSample (ch, i));
        }

        beginTest ("The tape stage keeps quiet signals at their level");
        {
            TapeSaturator tape;
            tape.prepare (rate, 512);
            TapeSettings s { true, 12.0f, 0.0f, 0.0f };
            juce::AudioBuffer<float> audio (2, 512);
            double in = 0.0, out = 0.0;

            for (int pass = 0; pass < 40; ++pass)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                        audio.setSample (ch, i, 0.02f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * (pass * 512 + i) / rate));

                if (pass >= 20)
                    in += audio.getRMSLevel (0, 0, 512);

                tape.process (audio.getArrayOfWritePointers(), 512, s);

                if (pass >= 20)
                    out += audio.getRMSLevel (0, 0, 512);
            }

            expectWithinAbsoluteError (juce::Decibels::gainToDecibels (out / in), 0.0, 1.0);
        }

        LithiumDrumsProcessor processor;
        expect (waitForKit (processor), "no test kit");

        beginTest ("FX settings are saved in presets; the e-kit setup isn't");
        {
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Lithium FX test.lithiumpreset");
            setParam (processor, params::delayFeedback, 60.0f);
            setParam (processor, params::roomIr, 3.0f);
            setParam (processor, params::ChannelId::master().param (params::compOn), 1.0f);
            auto controllers = processor.getMidiMap().controllers;
            controllers.pedalMin = 7;
            processor.setControllers (controllers);
            expect (processor.savePreset (file).wasOk());

            setParam (processor, params::delayFeedback, 10.0f);
            setParam (processor, params::roomIr, 0.0f);
            setParam (processor, params::ChannelId::master().param (params::compOn), 0.0f);
            controllers.pedalMin = 21;
            processor.setControllers (controllers);

            expect (processor.loadPreset (file).wasOk());
            expectWithinAbsoluteError (value (processor, params::delayFeedback), 60.0f, 0.01f);
            expectWithinAbsoluteError (value (processor, params::roomIr), 3.0f, 0.01f);
            expect (value (processor, params::ChannelId::master().param (params::compOn)) > 0.5f);
            expectEquals (processor.getMidiMap().controllers.pedalMin, 21, "loading a preset doesn't change the hardware setup");
            file.deleteFile();
        }

        beginTest ("A/B compares two settings of the whole FX page, and leaves the rest alone");
        {
            setParam (processor, params::delayFeedback, 20.0f);
            setParam (processor, params::masterVolume, -3.0f);
            processor.selectFxSlot (1);
            expectWithinAbsoluteError (value (processor, params::delayFeedback), 20.0f, 0.01f, "B starts as a copy of A");
            setParam (processor, params::delayFeedback, 70.0f);
            setParam (processor, params::masterVolume, -6.0f);

            processor.selectFxSlot (0);
            expectWithinAbsoluteError (value (processor, params::delayFeedback), 20.0f, 0.01f);
            expectWithinAbsoluteError (value (processor, params::masterVolume), -6.0f, 0.05f, "the master fader isn't part of the FX page");
            processor.selectFxSlot (1);
            expectWithinAbsoluteError (value (processor, params::delayFeedback), 70.0f, 0.01f);
            processor.selectFxSlot (0);
        }

        beginTest ("Each effect's factory presets apply and are recognised");
        {
            for (int m = 0; m < fxpresets::numModules; ++m)
            {
                for (const auto& preset : fxpresets::factory ((fxpresets::Module) m))
                {
                    fxpresets::apply (processor.getParameters(), preset);
                    expectEquals (fxpresets::currentName (processor.getParameters(), (fxpresets::Module) m), preset.name);
                }
            }
        }

        beginTest ("processBlock doesn't allocate with every effect on");
        {
            using namespace params;
            const auto master = ChannelId::master();

            for (const auto& id : { master.param (eqOn), master.param (compOn), juce::String (crushOn), juce::String (tapeOn),
                                    master.param (transientOn), juce::String (limitOn), juce::String (limitLookahead) })
                setParam (processor, id, 1.0f);

            setParam (processor, fxBypass, 0.0f);
            setParam (processor, roomIr, 3.0f);
            setParam (processor, delaySync, 1.0f);

            for (int p = 0; p < 4; ++p)
                for (const auto send : { roomSend, reverbSend, delaySend })
                    setParam (processor, pieceParamId (p, send), 0.0f);

            processor.setMidiMap (MidiMap::rolandVDrums(), false);
            constexpr int blockSize = 256;
            processor.prepareToPlay (rate, blockSize);

            // The IR loads on its own thread; wait for it, then give the
            // Convolution's background thread a moment to get it ready.
            expect (waitFor ([&] { return processor.getIrLoader().getStatus().name == irs::builtIn()[3].name; }));
            std::this_thread::sleep_for (std::chrono::milliseconds (200));

            juce::AudioBuffer<float> buffer (2, blockSize);
            std::vector<juce::MidiBuffer> blocks (400);

            for (size_t i = 0; i < blocks.size(); ++i)
            {
                blocks[i].addEvent (juce::MidiMessage::controllerEvent (10, 4, (int) ((i * 13) % 128)), 0);
                blocks[i].addEvent (juce::MidiMessage::controllerEvent (10, 16, (int) ((i * 29) % 128)), 3);

                for (const auto note : { 36, 38, 42, 44, 46, 48, 51 })
                    if ((i + (size_t) note) % 4 == 0)
                        blocks[i].addEvent (juce::MidiMessage::noteOn (10, note, (juce::uint8) 110), (int) ((i * 31 + (size_t) note) % blockSize));
            }

            // One block first: switching the IR in happens on it.
            juce::MidiBuffer none;
            processor.processBlock (buffer, none);

            int allocations = 0;
            float peak = 0.0f;
            {
                test::ScopedAllocationCounter counter;

                for (auto& midi : blocks)
                {
                    processor.processBlock (buffer, midi);
                    peak = std::max (peak, buffer.getMagnitude (0, blockSize));
                }

                allocations = counter.count();
            }

            expectEquals (allocations, 0);
            expect (peak > 0.0f, "it played");
            expect (peak <= juce::Decibels::decibelsToGain (value (processor, limitCeiling)) + 1.0e-4f, "and the limiter held");

            MidiMonitorEvent e;
            int events = 0;

            while (processor.popMonitorEvent (e))
                ++events;

            expect (events > 100, "the monitor heard the MIDI");
        }
    }
};

static FxTests fxTests;

} // namespace lithium
