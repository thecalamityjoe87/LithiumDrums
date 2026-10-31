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

    juce::String formatHz (float hz)
    {
        if (hz >= 1000.0f)
            return juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz";

        return juce::String (juce::roundToInt (hz)) + " Hz";
    }

    juce::String formatSignedDb (float db)
    {
        return (db > 0.05f ? "+" : "") + juce::String (db, 1) + " dB";
    }

    /** A logarithmic range, so every octave (or decade of milliseconds) gets
        the same share of a knob's travel. */
    juce::NormalisableRange<float> logRange (float low, float high)
    {
        return { low, high,
                 [] (float start, float end, float t) { return start * std::pow (end / start, t); },
                 [] (float start, float end, float v) { return std::log (v / start) / std::log (end / start); },
                 [] (float start, float end, float v) { return juce::jlimit (start, end, v); } };
    }

    auto hzAttributes (bool offAtBottom = false)
    {
        return juce::AudioParameterFloatAttributes()
                   .withLabel ("Hz")
                   .withStringFromValueFunction ([offAtBottom] (float v, int)
                   {
                       return offAtBottom && v <= EqSettings::hpfOffHz + 0.5f ? juce::String ("Off") : formatHz (v);
                   })
                   .withValueFromStringFunction ([] (const juce::String& text)
                   {
                       // Typed values may be in kHz ("2.5k").
                       const auto v = text.getFloatValue();
                       return text.containsIgnoreCase ("k") ? v * 1000.0f : v;
                   });
    }

    auto signedDbAttributes()
    {
        return juce::AudioParameterFloatAttributes()
                   .withLabel ("dB")
                   .withStringFromValueFunction ([] (float v, int) { return formatSignedDb (v); });
    }

    auto unitAttributes (juce::String unit, int decimals)
    {
        return juce::AudioParameterFloatAttributes()
                   .withLabel (unit)
                   .withStringFromValueFunction ([unit, decimals] (float v, int)
                   {
                       return juce::String (v, v >= 100.0f ? 0 : decimals) + (unit.isEmpty() ? "" : " " + unit);
                   });
    }

    juce::String slotLabel (bool isMic, int slot)
    {
        return (isMic ? "Mic " : "Piece ") + juce::String (slot + 1);
    }

    void setToDefault (juce::RangedAudioParameter& p)
    {
        p.beginChangeGesture();
        p.setValueNotifyingHost (p.getDefaultValue());
        p.endChangeGesture();
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

juce::StringArray outputNames()
{
    juce::StringArray names { "Main" };

    for (int i = 1; i <= numAuxOuts; ++i)
        names.add ("Aux " + juce::String (i));

    return names;
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
namespace
{
    using Float = SlotParameter<juce::AudioParameterFloat>;
    using Bool = SlotParameter<juce::AudioParameterBool>;
    using Choice = SlotParameter<juce::AudioParameterChoice>;
    using Int = SlotParameter<juce::AudioParameterInt>;
    using Group = juce::AudioProcessorParameterGroup;

    auto panAttributes()
    {
        return juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
        {
            const auto p = juce::roundToInt (v);
            return p == 0 ? juce::String ("C") : juce::String (std::abs (p)) + (p < 0 ? "L" : "R");
        });
    }

    auto bipolarPercentAttributes()
    {
        return juce::AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction ([] (float v, int)
        {
            const auto p = juce::roundToInt (v);
            return (p > 0 ? "+" : "") + juce::String (p) + "%";
        });
    }

    /** Adds parameters to one slot's group, named after the slot's piece or mic. */
    struct SlotAdder
    {
        Group& group;
        const SlotNames& names;
        ChannelId channel;
        juce::String label;

        juce::ParameterID id (std::string_view name) const   { return { channel.param (name), 1 }; }

        void addFloat (std::string_view name, const juce::String& what, juce::NormalisableRange<float> range,
                       float defaultValue, juce::AudioParameterFloatAttributes attributes) const
        {
            group.addChild (std::make_unique<Float> (names, ! channel.isPiece, channel.slot, what, id (name), label + " " + what,
                                                     range, defaultValue, std::move (attributes)));
        }

        void addBool (std::string_view name, const juce::String& what, bool defaultValue) const
        {
            group.addChild (std::make_unique<Bool> (names, ! channel.isPiece, channel.slot, what, id (name), label + " " + what,
                                                    defaultValue, juce::AudioParameterBoolAttributes()));
        }
    };

    /** A mixer channel: its routing and its chain (EQ, transient shaper,
        compressor, saturation). The defaults come from the DSP's own
        settings structs, so the parameters' idea of "flat" and the engine's
        can't drift apart. */
    void addChannelParameters (const SlotAdder& add)
    {
        // Routing isn't something to automate, so hosts are told not to offer it.
        add.group.addChild (std::make_unique<Choice> (add.names, ! add.channel.isPiece, add.channel.slot, "Output",
                                                      add.id (output), add.label + " Output", outputNames(), 0,
                                                      juce::AudioParameterChoiceAttributes().withAutomatable (false)));
        add.addFloat (reverbSend, "Reverb Send", volumeRange (6.0f), minVolumeDb, volumeAttributes());

        const ChannelSettings defaults;
        const auto& eq = defaults.eq;
        add.addBool (eqOn, "EQ On", eq.enabled);
        add.addFloat (eqHighPass, "High-Pass", logRange (EqSettings::hpfOffHz, 1000.0f), eq.hpfHz, hzAttributes (true));
        add.addFloat (eqLowFreq, "Low Freq", logRange (20.0f, 1000.0f), eq.lowHz, hzAttributes());
        add.addFloat (eqLowGain, "Low Gain", { -18.0f, 18.0f, 0.1f }, eq.lowDb, signedDbAttributes());
        add.addFloat (eqMid1Freq, "Mid 1 Freq", logRange (40.0f, 10000.0f), eq.mid1Hz, hzAttributes());
        add.addFloat (eqMid1Gain, "Mid 1 Gain", { -18.0f, 18.0f, 0.1f }, eq.mid1Db, signedDbAttributes());
        add.addFloat (eqMid1Q, "Mid 1 Q", logRange (0.3f, 10.0f), eq.mid1Q, unitAttributes ({}, 2));
        add.addFloat (eqMid2Freq, "Mid 2 Freq", logRange (100.0f, 18000.0f), eq.mid2Hz, hzAttributes());
        add.addFloat (eqMid2Gain, "Mid 2 Gain", { -18.0f, 18.0f, 0.1f }, eq.mid2Db, signedDbAttributes());
        add.addFloat (eqMid2Q, "Mid 2 Q", logRange (0.3f, 10.0f), eq.mid2Q, unitAttributes ({}, 2));
        add.addFloat (eqHighFreq, "High Freq", logRange (1000.0f, 20000.0f), eq.highHz, hzAttributes());
        add.addFloat (eqHighGain, "High Gain", { -18.0f, 18.0f, 0.1f }, eq.highDb, signedDbAttributes());

        const auto& comp = defaults.comp;
        add.addBool (compOn, "Comp On", comp.enabled);
        add.addFloat (compThreshold, "Threshold", { -60.0f, 0.0f, 0.1f }, comp.thresholdDb, signedDbAttributes());
        add.addFloat (compRatio, "Ratio", logRange (1.0f, 20.0f), comp.ratio,
                      juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                      {
                          return juce::String (v, v >= 10.0f ? 0 : 1) + ":1";
                      }));
        add.addFloat (compAttack, "Attack", logRange (0.1f, 100.0f), comp.attackMs, unitAttributes ("ms", 1));
        add.addFloat (compRelease, "Release", logRange (10.0f, 2000.0f), comp.releaseMs, unitAttributes ("ms", 0));
        add.addFloat (compMakeup, "Makeup", { 0.0f, 24.0f, 0.1f }, comp.makeupDb, signedDbAttributes());
        add.addFloat (compMix, "Comp Mix", { 0.0f, 100.0f, 0.1f }, comp.mix * 100.0f, percentAttributes());

        const auto& sat = defaults.saturation;
        add.addBool (satOn, "Saturation On", sat.enabled);
        add.addFloat (satDrive, "Drive", { 0.0f, 24.0f, 0.1f }, sat.driveDb, signedDbAttributes());
        add.addFloat (satMix, "Saturation Mix", { 0.0f, 100.0f, 0.1f }, sat.mix * 100.0f, percentAttributes());

        const auto& transient = defaults.transient;
        add.addBool (transientOn, "Transient On", transient.enabled);
        add.addFloat (transientAttack, "Transient Attack", { -100.0f, 100.0f, 0.1f }, transient.attack * 100.0f, bipolarPercentAttributes());
        add.addFloat (transientSustain, "Transient Sustain", { -100.0f, 100.0f, 0.1f }, transient.sustain * 100.0f, bipolarPercentAttributes());
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const SlotNames& names)
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const PieceParams pieceDefaults;

    for (int i = 0; i < maxPieces; ++i)
    {
        const auto label = slotLabel (false, i);
        auto group = std::make_unique<Group> ("piece" + juce::String (i + 1), label, " | ");
        const SlotAdder add { *group, names, { true, i }, label };

        // The drum as a whole, through every mic.
        add.addFloat ("volume", "Volume", volumeRange (12.0f), 0.0f, volumeAttributes());
        add.addFloat ("tune", "Tune", { -12.0f, 12.0f, 0.01f }, 0.0f,
                      juce::AudioParameterFloatAttributes()
                          .withLabel ("st")
                          .withStringFromValueFunction ([] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 2) + " st"; }));
        add.addFloat ("decay", "Decay", { 0.0f, 100.0f, 0.1f }, 100.0f,
                      juce::AudioParameterFloatAttributes()
                          .withLabel ("%")
                          .withStringFromValueFunction ([] (float v, int)
                          {
                              return v >= 99.9f ? juce::String ("Natural") : juce::String (juce::roundToInt (v)) + "%";
                          }));
        add.addBool ("mute", "Mute", false);
        add.addBool ("solo", "Solo", false);

        for (size_t r = 0; r < (size_t) numMicRoles; ++r)
            add.addFloat (sendNames[r], juce::String (sendLabels[r].data(), sendLabels[r].size()), { 0.0f, 100.0f, 0.1f }, 100.0f,
                          percentAttributes());

        // Which note chokes this piece (cymbals). Not something to automate.
        group->addChild (std::make_unique<Int> (
            names, false, i, "Choke Note", add.id ("choke_note"), label + " Choke Note", -1, 127, -1,
            juce::AudioParameterIntAttributes()
                .withAutomatable (false)
                .withStringFromValueFunction ([] (int v, int)
                {
                    return v < 0 ? juce::String ("Off")
                                 : juce::String (v) + " (" + juce::MidiMessage::getMidiNoteName (v, true, true, 3) + ")";
                })));

        // Shaping each hit.
        add.addFloat (dynamics, "Dynamics", { -100.0f, 100.0f, 0.1f }, pieceDefaults.dynamics * 100.0f, bipolarPercentAttributes());
        add.addFloat (pitchEnv, "Pitch Envelope", { -12.0f, 12.0f, 0.01f }, pieceDefaults.pitchEnvSemitones,
                      juce::AudioParameterFloatAttributes()
                          .withLabel ("st")
                          .withStringFromValueFunction ([] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " st"; }));
        add.addFloat (pitchEnvTime, "Pitch Envelope Time", logRange (5.0f, 1000.0f), pieceDefaults.pitchEnvMs, unitAttributes ("ms", 0));
        add.addFloat (toneHighPass, "Tone High-Pass", logRange (PieceParams::toneOffHz, 2000.0f), pieceDefaults.toneHighPassHz,
                      hzAttributes (true));
        add.addFloat (toneLowPass, "Tone Low-Pass", logRange (500.0f, PieceParams::toneOpenHz), pieceDefaults.toneLowPassHz,
                      juce::AudioParameterFloatAttributes()
                          .withLabel ("Hz")
                          .withStringFromValueFunction ([] (float v, int)
                          {
                              return v >= PieceParams::toneOpenHz - 1.0f ? juce::String ("Open") : formatHz (v);
                          }));
        add.addFloat (attack, "Attack", logRange (0.0f + 0.1f, 200.0f), 0.1f,
                      juce::AudioParameterFloatAttributes()
                          .withLabel ("ms")
                          .withStringFromValueFunction ([] (float v, int)
                          {
                              return v <= 0.15f ? juce::String ("Instant") : juce::String (v, v >= 10.0f ? 0 : 1) + " ms";
                          }));
        add.addFloat (hold, "Hold", logRange (1.0f, 2000.0f), pieceDefaults.holdMs, unitAttributes ("ms", 0));

        // The drum's own mixer channel.
        add.addFloat ("fader", "Channel Fader", volumeRange (12.0f), 0.0f, volumeAttributes());
        add.addFloat ("pan", "Channel Pan", { -100.0f, 100.0f, 1.0f }, 0.0f, panAttributes());
        addChannelParameters (add);

        layout.add (std::move (group));
    }

    for (int i = 0; i < maxMics; ++i)
    {
        const auto label = slotLabel (true, i);
        auto group = std::make_unique<Group> ("mic" + juce::String (i + 1), label, " | ");
        const SlotAdder add { *group, names, { false, i }, label };

        add.addFloat ("volume", "Volume", volumeRange (12.0f), 0.0f, volumeAttributes());
        add.addFloat ("pan", "Pan", { -100.0f, 100.0f, 1.0f }, 0.0f, panAttributes());
        add.addBool ("mute", "Mute", false);
        add.addBool ("solo", "Solo", false);
        addChannelParameters (add);

        layout.add (std::move (group));
    }

    // The reverb bus: one shared room, fed by every channel's reverb send.
    {
        const ReverbSettings room;
        auto group = std::make_unique<Group> ("reverb", "Reverb", " | ");
        auto add = [&group] (const char* id, const juce::String& name, juce::NormalisableRange<float> range,
                             float defaultValue, juce::AudioParameterFloatAttributes attributes)
        {
            group->addChild (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range,
                                                                          defaultValue, std::move (attributes)));
        };

        add (reverbReturn, "Reverb Return", volumeRange (6.0f), 0.0f, volumeAttributes());
        group->addChild (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { reverbMute, 1 }, "Reverb Mute", false));
        group->addChild (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { reverbOutput, 1 }, "Reverb Output",
                                                                       outputNames(), 0,
                                                                       juce::AudioParameterChoiceAttributes().withAutomatable (false)));
        add (reverbSize, "Reverb Size", { 0.0f, 100.0f, 0.1f }, room.size * 100.0f, percentAttributes());
        add (reverbDecay, "Reverb Decay", logRange (0.2f, 8.0f), room.decaySeconds, unitAttributes ("s", 2));
        add (reverbPreDelay, "Reverb Pre-Delay", { 0.0f, 200.0f, 0.1f }, room.preDelayMs, unitAttributes ("ms", 0));
        add (reverbDamping, "Reverb Damping", logRange (1000.0f, 20000.0f), room.dampingHz, hzAttributes());
        add (reverbLowCut, "Reverb Low Cut", logRange (20.0f, 1000.0f), room.lowCutHz, hzAttributes());
        add (reverbWidth, "Reverb Width", { 0.0f, 100.0f, 0.1f }, room.width * 100.0f, percentAttributes());
        layout.add (std::move (group));
    }

    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { masterVolume, 1 }, "Master Volume",
                                                             volumeRange (6.0f), 0.0f, volumeAttributes()));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { velocityCrossfade, 1 }, "Velocity Crossfade", true));

    return layout;
}

void resetForKit (juce::AudioProcessorValueTreeState& state, const KitInfo& info)
{
    // Every piece and mic parameter back to its default, whatever it is...
    for (auto* p : state.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            if (const auto& id = ranged->getParameterID(); id.startsWith ("piece") || id.startsWith ("mic"))
                setToDefault (*ranged);

    // ... except the choke notes, which come from the kit.
    for (int i = 0; i < maxPieces; ++i)
        set (state, pieceParamId (i, "choke_note"), i < (int) info.pieces.size() ? (float) info.pieces[(size_t) i].defaultChokeNote : -1.0f);
}

void resetEq (juce::AudioProcessorValueTreeState& state, ChannelId channel)
{
    for (auto name : { eqOn, eqHighPass, eqLowFreq, eqLowGain, eqMid1Freq, eqMid1Gain, eqMid1Q,
                       eqMid2Freq, eqMid2Gain, eqMid2Q, eqHighFreq, eqHighGain })
        if (auto* p = state.getParameter (channel.param (name)))
            setToDefault (*p);
}

void resetCompressor (juce::AudioProcessorValueTreeState& state, ChannelId channel)
{
    for (auto name : { compOn, compThreshold, compRatio, compAttack, compRelease, compMakeup, compMix })
        if (auto* p = state.getParameter (channel.param (name)))
            setToDefault (*p);
}

//==============================================================================
namespace
{
    std::atomic<float>* find (juce::AudioProcessorValueTreeState& state, const juce::String& id)
    {
        auto* p = state.getRawParameterValue (id);
        jassert (p != nullptr);   // every ID used here must exist in createLayout()
        return p;
    }
}

Snapshot::ChannelRefs Snapshot::findChannel (juce::AudioProcessorValueTreeState& state, ChannelId c)
{
    auto get = [&] (std::string_view name) { return find (state, c.param (name)); };

    return { get (output), get (reverbSend),
             get (eqOn), get (eqHighPass), get (eqLowFreq), get (eqLowGain), get (eqMid1Freq), get (eqMid1Gain), get (eqMid1Q),
             get (eqMid2Freq), get (eqMid2Gain), get (eqMid2Q), get (eqHighFreq), get (eqHighGain),
             get (compOn), get (compThreshold), get (compRatio), get (compAttack), get (compRelease), get (compMakeup), get (compMix),
             get (satOn), get (satDrive), get (satMix), get (transientOn), get (transientAttack), get (transientSustain) };
}

Snapshot::Snapshot (juce::AudioProcessorValueTreeState& state)
{
    for (int i = 0; i < maxPieces; ++i)
    {
        auto& r = pieces[(size_t) i];
        auto get = [&] (std::string_view name) { return find (state, pieceParamId (i, name)); };
        r.volume = get ("volume");
        r.tune = get ("tune");
        r.decay = get ("decay");
        r.mute = get ("mute");
        r.solo = get ("solo");
        r.chokeNote = get ("choke_note");

        for (size_t s = 0; s < (size_t) numMicRoles; ++s)
            r.sends[s] = get (sendNames[s]);

        r.dynamics = get (dynamics);
        r.pitchEnv = get (pitchEnv);
        r.pitchEnvTime = get (pitchEnvTime);
        r.toneHighPass = get (toneHighPass);
        r.toneLowPass = get (toneLowPass);
        r.attack = get (attack);
        r.hold = get (hold);
        r.fader = get ("fader");
        r.pan = get ("pan");
        r.channel = findChannel (state, { true, i });
    }

    for (int i = 0; i < maxMics; ++i)
    {
        auto& r = mics[(size_t) i];
        auto get = [&] (std::string_view name) { return find (state, micParamId (i, name)); };
        r.volume = get ("volume");
        r.pan = get ("pan");
        r.mute = get ("mute");
        r.solo = get ("solo");
        r.channel = findChannel (state, { false, i });
    }

    reverb = { find (state, reverbReturn), find (state, reverbMute), find (state, reverbOutput), find (state, reverbSize),
               find (state, reverbDecay), find (state, reverbPreDelay), find (state, reverbDamping), find (state, reverbLowCut),
               find (state, reverbWidth) };
    master = find (state, masterVolume);
    crossfade = find (state, velocityCrossfade);
}

namespace
{
    // Relaxed loads are enough: each parameter is independent, and a value
    // that is one block late is fine.
    float load (const std::atomic<float>* a) noexcept   { return a->load (std::memory_order_relaxed); }
    bool loadBool (const std::atomic<float>* a) noexcept { return load (a) >= 0.5f; }
}

void Snapshot::readChannel (const ChannelRefs& r, ChannelParams& c) noexcept
{
    c.output = juce::jlimit (0, numAuxOuts, juce::roundToInt (load (r.output)));
    c.reverbSend = dbToGain (load (r.reverbSend));

    auto& eq = c.chain.eq;
    eq.enabled = loadBool (r.eqOn);
    eq.hpfHz = load (r.hpf);
    eq.lowHz = load (r.lowFreq);
    eq.lowDb = load (r.lowGain);
    eq.mid1Hz = load (r.mid1Freq);
    eq.mid1Db = load (r.mid1Gain);
    eq.mid1Q = load (r.mid1Q);
    eq.mid2Hz = load (r.mid2Freq);
    eq.mid2Db = load (r.mid2Gain);
    eq.mid2Q = load (r.mid2Q);
    eq.highHz = load (r.highFreq);
    eq.highDb = load (r.highGain);

    auto& comp = c.chain.comp;
    comp.enabled = loadBool (r.compOn);
    comp.thresholdDb = load (r.threshold);
    comp.ratio = load (r.ratio);
    comp.attackMs = load (r.attack);
    comp.releaseMs = load (r.release);
    comp.makeupDb = load (r.makeup);
    comp.mix = load (r.mix) / 100.0f;

    auto& sat = c.chain.saturation;
    sat.enabled = loadBool (r.satOn);
    sat.driveDb = load (r.satDrive);
    sat.mix = load (r.satMix) / 100.0f;

    auto& transient = c.chain.transient;
    transient.enabled = loadBool (r.transientOn);
    transient.attack = load (r.transientAttack) / 100.0f;
    transient.sustain = load (r.transientSustain) / 100.0f;
}

void Snapshot::read (EngineParams& out) const noexcept
{
    for (size_t i = 0; i < pieces.size(); ++i)
    {
        const auto& r = pieces[i];
        auto& p = out.pieces[i];
        p.volume = dbToGain (load (r.volume));
        p.tuneSemitones = load (r.tune);
        p.decay = load (r.decay) / 100.0f;
        p.mute = loadBool (r.mute);
        p.solo = loadBool (r.solo);
        p.chokeNote = juce::roundToInt (load (r.chokeNote));

        for (size_t s = 0; s < (size_t) numMicRoles; ++s)
            p.sends[s] = load (r.sends[s]) / 100.0f;

        p.dynamics = load (r.dynamics) / 100.0f;
        p.pitchEnvSemitones = load (r.pitchEnv);
        p.pitchEnvMs = load (r.pitchEnvTime);
        p.toneHighPassHz = load (r.toneHighPass);
        p.toneLowPassHz = load (r.toneLowPass);
        p.attackMs = load (r.attack) <= 0.15f ? 0.0f : load (r.attack);
        p.holdMs = load (r.hold);
        p.faderGain = dbToGain (load (r.fader));
        p.pan = load (r.pan) / 100.0f;
        readChannel (r.channel, p.channel);
    }

    for (size_t i = 0; i < mics.size(); ++i)
    {
        const auto& r = mics[i];
        auto& m = out.mics[i];
        m.volume = dbToGain (load (r.volume));
        m.pan = load (r.pan) / 100.0f;
        m.mute = loadBool (r.mute);
        m.solo = loadBool (r.solo);
        readChannel (r.channel, m.channel);
    }

    auto& rv = out.reverb;
    rv.returnGain = dbToGain (load (reverb.returnLevel));
    rv.mute = loadBool (reverb.mute);
    rv.output = juce::jlimit (0, numAuxOuts, juce::roundToInt (load (reverb.output)));
    rv.room.size = load (reverb.size) / 100.0f;
    rv.room.decaySeconds = load (reverb.decay);
    rv.room.preDelayMs = load (reverb.preDelay);
    rv.room.dampingHz = load (reverb.damping);
    rv.room.lowCutHz = load (reverb.lowCut);
    rv.room.width = load (reverb.width) / 100.0f;

    out.masterGain = dbToGain (load (master));
    out.velocityCrossfade = loadBool (crossfade);
}

std::array<bool, maxMics> Snapshot::readMicMutes() const noexcept
{
    std::array<bool, maxMics> result {};

    for (size_t i = 0; i < mics.size(); ++i)
        result[i] = loadBool (mics[i].mute);

    return result;
}

} // namespace lithium::params
