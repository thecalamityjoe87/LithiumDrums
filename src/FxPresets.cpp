#include "FxPresets.h"

#include "Parameters.h"
#include "engine/IrLoader.h"

namespace lithium::fxpresets
{

namespace
{
    using namespace params;

    /** The index of a built-in IR, for the room_ir choice. */
    float ir (const char* id)
    {
        const auto& list = irs::builtIn();

        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].id == id)
                return (float) i;

        return 0.0f;
    }

    juce::String m (std::string_view name)    { return ChannelId::master().param (name); }

    float note (const char* name)
    {
        for (size_t i = 0; i < delaynotes::all.size(); ++i)
            if (juce::String (delaynotes::all[i].name) == name)
                return (float) i;

        return (float) delaynotes::defaultNote;
    }

    std::vector<Preset> make (Module module)
    {
        switch (module)
        {
            case Module::room:
            {
                auto room = [] (const char* name, const char* irId, float preDelay, float decay, float lowCut, float highCut, float width)
                {
                    return Preset { name, { { roomIr, ir (irId) }, { roomPreDelay, preDelay }, { roomDecay, decay },
                                            { roomLowCut, lowCut }, { roomHighCut, highCut }, { roomWidth, width }, { roomMix, 100.0f } } };
                };

                return { room ("Tight Studio", "studio", 0.0f, 100.0f, 120.0f, 10000.0f, 100.0f),
                         room ("Live Room", "seminar-room", 4.0f, 100.0f, 100.0f, 12000.0f, 100.0f),
                         room ("Short Ambience", "seminar-room", 0.0f, 45.0f, 200.0f, 9000.0f, 80.0f),
                         room ("Garage", "tiled-room", 0.0f, 85.0f, 150.0f, 9000.0f, 100.0f),
                         room ("Big Hall", "hall", 20.0f, 100.0f, 80.0f, 8000.0f, 100.0f),
                         room ("Snare Plate", "plate", 12.0f, 70.0f, 220.0f, 11000.0f, 100.0f) };
            }

            case Module::plate:
            {
                auto plate = [] (const char* name, float size, float decay, float preDelay, float damping, float lowCut, float width)
                {
                    return Preset { name, { { reverbSize, size }, { reverbDecay, decay }, { reverbPreDelay, preDelay },
                                            { reverbDamping, damping }, { reverbLowCut, lowCut }, { reverbWidth, width }, { reverbMix, 100.0f } } };
                };

                return { plate ("Bright Plate", 35.0f, 1.8f, 10.0f, 12000.0f, 200.0f, 100.0f),
                         plate ("Dark Plate", 45.0f, 2.4f, 15.0f, 4500.0f, 180.0f, 100.0f),
                         plate ("Snare Verb", 50.0f, 1.2f, 0.0f, 9000.0f, 250.0f, 100.0f),
                         plate ("Small Room", 20.0f, 0.6f, 2.0f, 7000.0f, 120.0f, 80.0f),
                         plate ("Big Hall", 90.0f, 3.2f, 25.0f, 6000.0f, 120.0f, 100.0f) };
            }

            case Module::delay:
            {
                auto delay = [] (const char* name, bool sync, const char* syncNote, float ms, float feedback, float lowCut,
                                 float highCut, bool pingPong)
                {
                    return Preset { name, { { delaySync, sync ? 1.0f : 0.0f }, { delayNote, note (syncNote) }, { delayTime, ms },
                                            { delayFeedback, feedback }, { delayLowCut, lowCut }, { delayHighCut, highCut },
                                            { delayPingPong, pingPong ? 1.0f : 0.0f }, { delayMix, 100.0f } } };
                };

                return { delay ("Slapback", false, "1/8", 95.0f, 0.0f, 200.0f, 5000.0f, false),
                         delay ("Dotted Eighth", true, "1/8 dotted", 375.0f, 30.0f, 150.0f, 6000.0f, false),
                         delay ("Ping-Pong Quarter", true, "1/4", 500.0f, 40.0f, 200.0f, 6000.0f, true),
                         delay ("Triplet Eighth", true, "1/8 triplet", 250.0f, 25.0f, 200.0f, 7000.0f, false),
                         delay ("Dub Throw", true, "1/4 dotted", 750.0f, 65.0f, 300.0f, 2500.0f, true) };
            }

            case Module::eq:
            {
                auto eq = [] (const char* name, float lowHz, float lowDb, float mid1Hz, float mid1Db, float mid1Q,
                              float mid2Hz, float mid2Db, float mid2Q, float highHz, float highDb)
                {
                    return Preset { name, { { m (eqOn), 1.0f }, { m (eqHighPass), EqSettings::hpfOffHz },
                                            { m (eqLowFreq), lowHz }, { m (eqLowGain), lowDb },
                                            { m (eqMid1Freq), mid1Hz }, { m (eqMid1Gain), mid1Db }, { m (eqMid1Q), mid1Q },
                                            { m (eqMid2Freq), mid2Hz }, { m (eqMid2Gain), mid2Db }, { m (eqMid2Q), mid2Q },
                                            { m (eqHighFreq), highHz }, { m (eqHighGain), highDb } } };
                };

                return { eq ("Flat", 100.0f, 0.0f, 400.0f, 0.0f, 1.0f, 2500.0f, 0.0f, 1.0f, 8000.0f, 0.0f),
                         eq ("Smile", 80.0f, 3.0f, 400.0f, -2.0f, 1.0f, 2500.0f, 0.0f, 1.0f, 10000.0f, 3.0f),
                         eq ("Low Punch", 60.0f, 2.5f, 300.0f, -3.0f, 1.4f, 2500.0f, 0.0f, 1.0f, 8000.0f, 0.0f),
                         eq ("Tame Highs", 100.0f, 0.0f, 400.0f, 0.0f, 1.0f, 3500.0f, -1.5f, 0.8f, 8000.0f, -3.0f),
                         eq ("Air", 100.0f, 0.0f, 400.0f, 0.0f, 1.0f, 2500.0f, 0.0f, 1.0f, 12000.0f, 2.5f) };
            }

            case Module::glue:
            {
                auto glue = [] (const char* name, float threshold, float ratio, float attack, float release, float makeup, float mix)
                {
                    return Preset { name, { { m (compOn), 1.0f }, { m (compThreshold), threshold }, { m (compRatio), ratio },
                                            { m (compAttack), attack }, { m (compRelease), release }, { m (compMakeup), makeup },
                                            { m (compMix), mix } } };
                };

                return { glue ("Gentle Glue", -12.0f, 2.0f, 10.0f, 200.0f, 1.0f, 100.0f),
                         glue ("Punchy", -15.0f, 3.0f, 30.0f, 100.0f, 2.0f, 100.0f),
                         glue ("Bus Squeeze", -18.0f, 4.0f, 3.0f, 120.0f, 3.0f, 100.0f),
                         glue ("Parallel Glue", -20.0f, 4.0f, 10.0f, 150.0f, 4.0f, 50.0f) };
            }

            case Module::crush:
            {
                auto crush = [] (const char* name, float drive, float blend, float attack, float release)
                {
                    return Preset { name, { { crushOn, 1.0f }, { crushDrive, drive }, { crushBlend, blend },
                                            { crushAttack, attack }, { crushRelease, release } } };
                };

                return { crush ("Subtle Smash", 12.0f, 20.0f, 3.0f, 80.0f),
                         crush ("Room Smash", 20.0f, 35.0f, 1.0f, 60.0f),
                         crush ("All In", 28.0f, 60.0f, 0.5f, 50.0f) };
            }

            case Module::tape:
            {
                auto tape = [] (const char* name, float drive, float tone, float output)
                {
                    return Preset { name, { { tapeOn, 1.0f }, { tapeDrive, drive }, { tapeTone, tone }, { tapeOutput, output } } };
                };

                return { tape ("Warm", 4.0f, -20.0f, 0.0f),
                         tape ("Hot Tape", 10.0f, 0.0f, -2.0f),
                         tape ("Bright Tape", 6.0f, 40.0f, -1.0f),
                         tape ("Crunch", 18.0f, 10.0f, -5.0f) };
            }

            case Module::transient:
            {
                auto transient = [] (const char* name, float attack, float sustain)
                {
                    return Preset { name, { { m (transientOn), 1.0f }, { m (transientAttack), attack }, { m (transientSustain), sustain } } };
                };

                return { transient ("More Attack", 40.0f, 0.0f),
                         transient ("Tighten", 15.0f, -40.0f),
                         transient ("Ring Out", 0.0f, 40.0f),
                         transient ("Soften", -35.0f, 0.0f) };
            }

            case Module::limiter:
            {
                auto limiter = [] (const char* name, float ceiling, float release, bool lookahead)
                {
                    return Preset { name, { { limitOn, 1.0f }, { limitCeiling, ceiling }, { limitRelease, release },
                                            { limitLookahead, lookahead ? 1.0f : 0.0f } } };
                };

                return { limiter ("Safety", -0.3f, 60.0f, false),
                         limiter ("Transparent", -1.0f, 150.0f, true),
                         limiter ("Loud", -0.3f, 30.0f, true) };
            }
        }

        return {};
    }
}

const std::vector<Preset>& factory (Module module)
{
    // Built once per module, on first use (thread-safe, and only ever on the message thread anyway).
    static const std::array<std::vector<Preset>, numModules> all = []
    {
        std::array<std::vector<Preset>, numModules> result;

        for (int i = 0; i < numModules; ++i)
            result[(size_t) i] = make ((Module) i);

        return result;
    }();

    return all[(size_t) module];
}

void apply (juce::AudioProcessorValueTreeState& state, const Preset& preset)
{
    for (const auto& [id, value] : preset.values)
    {
        if (auto* p = state.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (value));
            p->endChangeGesture();
        }
    }
}

juce::String currentName (juce::AudioProcessorValueTreeState& state, Module module)
{
    for (const auto& preset : factory (module))
    {
        // Compared normalised, so one tolerance suits every range.
        const auto matches = std::all_of (preset.values.begin(), preset.values.end(), [&] (const auto& v)
        {
            const auto* p = state.getParameter (v.first);
            return p != nullptr && std::abs (p->getValue() - p->convertTo0to1 (v.second)) < 1.0e-3f;
        });

        if (matches)
            return preset.name;
    }

    return {};
}

} // namespace lithium::fxpresets
