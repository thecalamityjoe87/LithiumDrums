#include "Parameters.h"

namespace lithium::params
{

namespace
{
    constexpr std::array<std::string_view, numMicRoles> sendNames { "send_close", "send_overheads", "send_room" };
    constexpr std::array<std::string_view, numMicRoles> sendLabels { "Close Send", "Overheads Send", "Room Send" };

    juce::String formatDb (float db)
    {
        return db <= minVolumeDb + 0.05f ? juce::String ("-inf dB") : juce::String (db, 1) + " dB";
    }

    float dbToGain (float db)
    {
        return db <= minVolumeDb + 0.05f ? 0.0f : juce::Decibels::decibelsToGain (db);
    }

    /** A JUCE parameter whose name follows the loaded kit: "Snare Volume"
        while the kit's second piece is a snare, "Piece 2 Volume" with no kit.
        The VST3 wrapper re-reads names when the processor reports that
        parameter info changed (see LithiumDrumsProcessor). */
    template <typename Base>
    class SlotParameter final : public Base
    {
    public:
        template <typename... Args>
        SlotParameter (const SlotNames& n, bool mic, int s, juce::String what, Args&&... args)
            : Base (std::forward<Args> (args)...), names (n), isMic (mic), slot (s), suffix (std::move (what))
        {
        }

        juce::String getName (int maximumStringLength) const override
        {
            return (names.get (isMic, slot) + " " + suffix).substring (0, maximumStringLength);
        }

    private:
        const SlotNames& names;
        const bool isMic;
        const int slot;
        const juce::String suffix;
    };

    juce::NormalisableRange<float> volumeRange (float maxDb)    { return { minVolumeDb, maxDb, 0.1f }; }

    auto volumeAttributes()
    {
        return juce::AudioParameterFloatAttributes()
                   .withLabel ("dB")
                   .withStringFromValueFunction ([] (float v, int) { return formatDb (v); });
    }

    auto percentAttributes()
    {
        return juce::AudioParameterFloatAttributes()
                   .withLabel ("%")
                   .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + "%"; });
    }

    juce::String slotLabel (bool isMic, int slot)
    {
        return (isMic ? "Mic " : "Piece ") + juce::String (slot + 1);
    }

    void set (juce::AudioProcessorValueTreeState& state, const juce::String& id, float value)
    {
        if (auto* p = state.getParameter (id))
        {
            // Wrapped in a gesture so hosts record it as one deliberate change.
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (value));
            p->endChangeGesture();
        }
    }
}

juce::String pieceParamId (int slot, std::string_view name)
{
    return "piece" + juce::String (slot + 1) + "_" + juce::String (name.data(), name.size());
}

juce::String micParamId (int slot, std::string_view name)
{
    return "mic" + juce::String (slot + 1) + "_" + juce::String (name.data(), name.size());
}

//==============================================================================
void SlotNames::set (const KitInfo* info)
{
    std::scoped_lock lock (mutex);

    for (int i = 0; i < maxPieces; ++i)
        pieces[(size_t) i] = info != nullptr && i < (int) info->pieces.size() ? info->pieces[(size_t) i].name : slotLabel (false, i);

    for (int i = 0; i < maxMics; ++i)
        mics[(size_t) i] = info != nullptr && i < (int) info->mics.size() ? info->mics[(size_t) i].name : slotLabel (true, i);
}

juce::String SlotNames::get (bool isMic, int slot) const
{
    std::scoped_lock lock (mutex);
    const auto& name = isMic ? mics[(size_t) slot] : pieces[(size_t) slot];
    return name.isNotEmpty() ? name : slotLabel (isMic, slot);
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const SlotNames& names)
{
    using Float = SlotParameter<juce::AudioParameterFloat>;
    using Bool = SlotParameter<juce::AudioParameterBool>;
    using Choice = SlotParameter<juce::AudioParameterChoice>;
    using Int = SlotParameter<juce::AudioParameterInt>;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int i = 0; i < maxPieces; ++i)
    {
        const auto label = slotLabel (false, i);
        auto group = std::make_unique<juce::AudioProcessorParameterGroup> ("piece" + juce::String (i + 1), label, " | ");
        auto id = [i] (std::string_view name) { return juce::ParameterID { pieceParamId (i, name), 1 }; };

        group->addChild (std::make_unique<Float> (names, false, i, "Volume", id ("volume"), label + " Volume",
                                                  volumeRange (12.0f), 0.0f, volumeAttributes()));

        group->addChild (std::make_unique<Float> (
            names, false, i, "Tune", id ("tune"), label + " Tune", juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f,
            juce::AudioParameterFloatAttributes()
                .withLabel ("st")
                .withStringFromValueFunction ([] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 2) + " st"; })));

        group->addChild (std::make_unique<Float> (
            names, false, i, "Decay", id ("decay"), label + " Decay", juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
            juce::AudioParameterFloatAttributes()
                .withLabel ("%")
                .withStringFromValueFunction ([] (float v, int)
                {
                    return v >= 99.9f ? juce::String ("Natural") : juce::String (juce::roundToInt (v)) + "%";
                })));

        group->addChild (std::make_unique<Bool> (names, false, i, "Mute", id ("mute"), label + " Mute", false,
                                                 juce::AudioParameterBoolAttributes()));
        group->addChild (std::make_unique<Bool> (names, false, i, "Solo", id ("solo"), label + " Solo", false,
                                                 juce::AudioParameterBoolAttributes()));

        for (size_t r = 0; r < (size_t) numMicRoles; ++r)
        {
            const auto what = juce::String (sendLabels[r].data(), sendLabels[r].size());
            group->addChild (std::make_unique<Float> (names, false, i, what, id (sendNames[r]), label + " " + what,
                                                      juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                      percentAttributes()));
        }

        // Which note chokes this piece (cymbals). Not something to automate.
        group->addChild (std::make_unique<Int> (
            names, false, i, "Choke Note", id ("choke_note"), label + " Choke Note", -1, 127, -1,
            juce::AudioParameterIntAttributes()
                .withAutomatable (false)
                .withStringFromValueFunction ([] (int v, int)
                {
                    return v < 0 ? juce::String ("Off")
                                 : juce::String (v) + " (" + juce::MidiMessage::getMidiNoteName (v, true, true, 3) + ")";
                })));

        layout.add (std::move (group));
    }

    for (int i = 0; i < maxMics; ++i)
    {
        const auto label = slotLabel (true, i);
        auto group = std::make_unique<juce::AudioProcessorParameterGroup> ("mic" + juce::String (i + 1), label, " | ");
        auto id = [i] (std::string_view name) { return juce::ParameterID { micParamId (i, name), 1 }; };

        group->addChild (std::make_unique<Float> (names, true, i, "Volume", id ("volume"), label + " Volume",
                                                  volumeRange (12.0f), 0.0f, volumeAttributes()));

        group->addChild (std::make_unique<Float> (
            names, true, i, "Pan", id ("pan"), label + " Pan", juce::NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f,
            juce::AudioParameterFloatAttributes()
                .withStringFromValueFunction ([] (float v, int)
                {
                    const auto p = juce::roundToInt (v);
                    return p == 0 ? juce::String ("C") : juce::String (std::abs (p)) + (p < 0 ? "L" : "R");
                })));

        group->addChild (std::make_unique<Bool> (names, true, i, "Mute", id ("mute"), label + " Mute", false,
                                                 juce::AudioParameterBoolAttributes()));
        group->addChild (std::make_unique<Bool> (names, true, i, "Solo", id ("solo"), label + " Solo", false,
                                                 juce::AudioParameterBoolAttributes()));

        // Routing isn't something to automate, so hosts are told not to offer it.
        group->addChild (std::make_unique<Choice> (names, true, i, "Output", id ("output"), label + " Output",
                                                   juce::StringArray { "Main", "Direct out" }, 0,
                                                   juce::AudioParameterChoiceAttributes().withAutomatable (false)));

        layout.add (std::move (group));
    }

    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { masterVolume, 1 }, "Master Volume",
                                                             volumeRange (6.0f), 0.0f, volumeAttributes()));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { velocityCrossfade, 1 }, "Velocity Crossfade", true));

    return layout;
}

void resetForKit (juce::AudioProcessorValueTreeState& state, const KitInfo& info)
{
    for (int i = 0; i < maxPieces; ++i)
    {
        set (state, pieceParamId (i, "volume"), 0.0f);
        set (state, pieceParamId (i, "tune"), 0.0f);
        set (state, pieceParamId (i, "decay"), 100.0f);
        set (state, pieceParamId (i, "mute"), 0.0f);
        set (state, pieceParamId (i, "solo"), 0.0f);

        for (const auto& s : sendNames)
            set (state, pieceParamId (i, s), 100.0f);

        set (state, pieceParamId (i, "choke_note"),
             i < (int) info.pieces.size() ? (float) info.pieces[(size_t) i].defaultChokeNote : -1.0f);
    }

    for (int i = 0; i < maxMics; ++i)
    {
        set (state, micParamId (i, "volume"), 0.0f);
        set (state, micParamId (i, "pan"), 0.0f);
        set (state, micParamId (i, "mute"), 0.0f);
        set (state, micParamId (i, "solo"), 0.0f);
        set (state, micParamId (i, "output"), 0.0f);
    }
}

//==============================================================================
Snapshot::Snapshot (juce::AudioProcessorValueTreeState& state)
{
    auto get = [&state] (const juce::String& id)
    {
        auto* p = state.getRawParameterValue (id);
        jassert (p != nullptr);   // every ID used here must exist in createLayout()
        return p;
    };

    for (int i = 0; i < maxPieces; ++i)
    {
        auto& r = pieces[(size_t) i];
        r.volume = get (pieceParamId (i, "volume"));
        r.tune = get (pieceParamId (i, "tune"));
        r.decay = get (pieceParamId (i, "decay"));
        r.mute = get (pieceParamId (i, "mute"));
        r.solo = get (pieceParamId (i, "solo"));
        r.chokeNote = get (pieceParamId (i, "choke_note"));

        for (size_t s = 0; s < (size_t) numMicRoles; ++s)
            r.sends[s] = get (pieceParamId (i, sendNames[s]));
    }

    for (int i = 0; i < maxMics; ++i)
    {
        auto& r = mics[(size_t) i];
        r.volume = get (micParamId (i, "volume"));
        r.pan = get (micParamId (i, "pan"));
        r.mute = get (micParamId (i, "mute"));
        r.solo = get (micParamId (i, "solo"));
        r.output = get (micParamId (i, "output"));
    }

    master = get (masterVolume);
    crossfade = get (velocityCrossfade);
}

void Snapshot::read (EngineParams& out) const noexcept
{
    // Relaxed loads are enough: each parameter is independent, and a value
    // that is one block late is fine.
    auto load = [] (const std::atomic<float>* a) { return a->load (std::memory_order_relaxed); };

    for (size_t i = 0; i < pieces.size(); ++i)
    {
        const auto& r = pieces[i];
        auto& p = out.pieces[i];
        p.volume = dbToGain (load (r.volume));
        p.tuneSemitones = load (r.tune);
        p.decay = load (r.decay) / 100.0f;
        p.mute = load (r.mute) >= 0.5f;
        p.solo = load (r.solo) >= 0.5f;
        p.chokeNote = juce::roundToInt (load (r.chokeNote));

        for (size_t s = 0; s < (size_t) numMicRoles; ++s)
            p.sends[s] = load (r.sends[s]) / 100.0f;
    }

    for (size_t i = 0; i < mics.size(); ++i)
    {
        const auto& r = mics[i];
        auto& m = out.mics[i];
        m.volume = dbToGain (load (r.volume));
        m.pan = load (r.pan) / 100.0f;
        m.mute = load (r.mute) >= 0.5f;
        m.solo = load (r.solo) >= 0.5f;
        m.output = load (r.output) >= 0.5f ? MicOutput::direct : MicOutput::main;
    }

    out.masterGain = dbToGain (load (master));
    out.velocityCrossfade = load (crossfade) >= 0.5f;
}

std::array<bool, maxMics> Snapshot::readMicMutes() const noexcept
{
    std::array<bool, maxMics> result {};

    for (size_t i = 0; i < mics.size(); ++i)
        result[i] = mics[i].mute->load (std::memory_order_relaxed) >= 0.5f;

    return result;
}

} // namespace lithium::params
