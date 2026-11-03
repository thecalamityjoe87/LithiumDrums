// LithiumPresetGen: builds the factory presets from their recipes.
//
//   LithiumPresetGen <recipes.json> <out folder> [--preset <name>] [--wav <folder>] [--reference] [--verify]
//
// A recipe names a kit (by id) and the settings that give it its style. The
// settings are written per drum role ("snare", "toms", "cymbals") and mic
// role ("overheads", "room") rather than per slot, so one recipe format fits
// kits that are laid out differently. For each recipe the tool:
//
//  1. loads the kit headlessly from the installed kits, as a fresh instance
//     choosing it from the menu would (every setting neutral), and measures
//     it;
//  2. applies the recipe, and trims every drum's volume by the same amount
//     so the kit plays at a standard level (kits are recorded 15 dB apart),
//     which makes a channel compressor's threshold mean the same on any kit;
//  3. plays a fixed test groove through the whole plugin with the master
//     chain switched off, and sets the master volume so that mix reaches
//     the chain at the same loudness in every preset (the master volume
//     comes before the chain, so a glue threshold means the same thing on
//     every kit, however loud its recordings are);
//  4. plays it again with the chain on and, if the recipe names a makeup
//     control, sets it so every preset comes out at the same loudness;
//  5. writes the preset to <out folder>/<group>/<name>.lithiumpreset.
//
// It prints each preset's loudness (LUFS, ITU-R BS.1770), peak, crest factor
// and how much of its energy is lows and highs, against the kit at its
// defaults, and the most each compressor took off: the closest thing to
// listening a script can do.
// --wav saves every render for listening; --reference also saves the kit at
// its defaults; --preset builds just the presets whose name contains it;
// --verify loads each written preset into a fresh instance the way the
// preset menu does and checks it sounds the same.
//
// The outputs are committed in assets/presets/, so the build only compiles
// this tool. Kits must be installed; a recipe whose kit isn't is skipped.

#include "KitLibrary.h"
#include "Parameters.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "engine/IrLoader.h"
#include "engine/KitSchema.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>

namespace
{

using namespace lithium;

constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;

//==============================================================================
// The test groove: four bars at 100 bpm through the General MIDI map (a
// backbeat with ghost notes and an open hat, a bar on the ride, a tom fill),
// played three times, and a crash left to ring. Every preset plays the same
// notes, so their measurements compare. The engine picks round-robin hits at
// random, so a single pass measures a few tenths of a dB differently each
// run; three passes average that out.

struct Hit
{
    double seconds;
    int note, velocity;
};

/** One pass of the groove's four bars, through at (bar, step, note, velocity). */
template <typename At>
void addGroovePass (At&& at)
{
    constexpr int kick = 36, snare = 38, hat = 42, openHat = 46, pedal = 44, ride = 51, bell = 53;
    constexpr int tomHigh = 48, tomMid = 45, floorTom = 43;

    for (int bar = 0; bar < 2; ++bar)
    {
        for (int step = 0; step < 16; step += 2)
            if (! (bar == 1 && step == 14))
                at (bar, step, hat, step % 4 == 0 ? 96 : 64);

        at (bar, 0, kick, 112);
        at (bar, 7, kick, 84);
        at (bar, 10, kick, 104);
        at (bar, 4, snare, 116);
        at (bar, 12, snare, 120);
        at (bar, 6, snare, 34);
        at (bar, 9, snare, 30);
        at (bar, 15, snare, 38);
    }

    at (1, 14, openHat, 100);
    at (2, 0, pedal, 70);

    // A bar on the ride, with the bell on the downbeat.
    for (int step = 0; step < 16; step += 2)
        at (2, step, step == 0 ? bell : ride, step % 4 == 0 ? 100 : 72);

    at (2, 0, kick, 110);
    at (2, 6, kick, 90);
    at (2, 10, kick, 100);
    at (2, 4, snare, 112);
    at (2, 12, snare, 118);

    // The fill: four sixteenths on each tom, louder as it goes down.
    for (int step = 0; step < 4; ++step)
    {
        at (3, step, snare, 70 + step * 12);
        at (3, 4 + step, tomHigh, 96 + step * 6);
        at (3, 8 + step, tomMid, 100 + step * 6);
        at (3, 12 + step, floorTom, 104 + step * 6);
    }

    at (3, 2, kick, 96);
    at (3, 10, kick, 100);
}

std::vector<Hit> testGroove()
{
    constexpr double sixteenth = 60.0 / 100.0 / 4.0;
    constexpr int kick = 36, crash = 49;
    std::vector<Hit> hits;
    int firstBar = 0;
    auto at = [&] (int bar, int step, int note, int velocity)
    {
        hits.push_back ({ 1.0 + ((firstBar + bar) * 16 + step) * sixteenth, note, velocity });
    };

    for (int pass = 0; pass < 3; ++pass, firstBar += 4)
        addGroovePass (at);

    at (0, 0, crash, 124);
    at (0, 0, kick, 124);

    std::sort (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b) { return a.seconds < b.seconds; });
    return hits;
}

//==============================================================================
// Measuring a render.

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    double process (double x) noexcept
    {
        const auto y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    /** RBJ cookbook low- or high-pass, Q = 1/sqrt 2. */
    static Biquad pass (bool high, double hz)
    {
        const auto w = juce::MathConstants<double>::twoPi * hz / sampleRate;
        const auto alpha = std::sin (w) / juce::MathConstants<double>::sqrt2;   // sin w / 2Q
        const auto c = std::cos (w), a0 = 1.0 + alpha;
        Biquad f;
        f.b1 = (high ? -(1.0 + c) : 1.0 - c) / a0;
        f.b0 = f.b2 = (high ? (1.0 + c) : 1.0 - c) / 2.0 / a0;
        f.a1 = -2.0 * c / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }
};

struct Measurement
{
    double lufs = -100.0, peakDb = -100.0;
    double lowDb = 0.0, highDb = 0.0;   // the share of energy below 150 Hz and above 5 kHz, in dB

    double crestDb() const noexcept     { return peakDb - lufs; }
};

double toDb (double power) noexcept     { return power > 1.0e-12 ? 10.0 * std::log10 (power) : -120.0; }

Measurement measure (const juce::AudioBuffer<float>& audio)
{
    Measurement m;
    const auto n = audio.getNumSamples();

    // BS.1770 at 48 kHz: a high shelf and a high-pass ("K-weighting"), then
    // 400 ms blocks with 75% overlap, gated at -70 LUFS and then 10 LU below
    // the ungated loudness.
    std::vector<double> weighted ((size_t) n, 0.0);
    double total = 0.0, low = 0.0, high = 0.0;

    for (int ch = 0; ch < 2; ++ch)
    {
        Biquad shelf { 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585 };
        Biquad highPass { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };
        auto lowPass = Biquad::pass (false, 150.0), lowPass2 = lowPass;
        auto air = Biquad::pass (true, 5000.0), air2 = air;
        const auto* x = audio.getReadPointer (ch);

        for (int i = 0; i < n; ++i)
        {
            const auto k = highPass.process (shelf.process (x[i]));
            weighted[(size_t) i] += k * k;
            m.peakDb = std::max (m.peakDb, (double) juce::Decibels::gainToDecibels (std::abs (x[i]), -100.0f));

            const auto l = lowPass2.process (lowPass.process (x[i]));
            const auto h = air2.process (air.process (x[i]));
            total += (double) x[i] * x[i];
            low += l * l;
            high += h * h;
        }
    }

    const auto blockLength = (int) (0.4 * sampleRate), hop = blockLength / 4;
    std::vector<double> blocks;

    for (int start = 0; start + blockLength <= n; start += hop)
    {
        double sum = 0.0;

        for (int i = start; i < start + blockLength; ++i)
            sum += weighted[(size_t) i];

        blocks.push_back (sum / blockLength);
    }

    auto gatedMean = [&] (double thresholdLufs)
    {
        double sum = 0.0;
        int count = 0;

        for (auto b : blocks)
            if (-0.691 + toDb (b) > thresholdLufs)
                sum += b, ++count;

        return count > 0 ? sum / count : 0.0;
    };

    const auto ungated = -0.691 + toDb (gatedMean (-70.0));
    m.lufs = -0.691 + toDb (gatedMean (ungated - 10.0));
    m.lowDb = toDb (low) - toDb (total);
    m.highDb = toDb (high) - toDb (total);
    return m;
}

//==============================================================================
// Driving the plugin.

bool waitForKit (LithiumDrumsProcessor& p, const juce::File& folder)
{
    juce::AudioBuffer<float> buffer (p.getTotalNumOutputChannels(), blockSize);

    for (int i = 0; i < 30000; ++i)   // ten minutes: a kit's first load may build its stream cache
    {
        p.applyPendingKitLoad();
        p.prepareToPlay (sampleRate, blockSize);
        juce::MidiBuffer midi;
        p.processBlock (buffer, midi);
        const auto info = p.getKitInfo();

        if (! p.isKitLoading() && ! p.isPresetLoading() && info != nullptr && info->folder == folder)
            return true;

        juce::Thread::sleep (20);
    }

    return false;
}

/** Has the Room reverb load the IR its parameters ask for, and waits for it.
    (The processor's timer would ask, but there's no message loop here.) */
void waitForRoomIr (LithiumDrumsProcessor& p)
{
    const auto choice = juce::roundToInt (p.getParameters().getRawParameterValue (params::roomIr)->load());
    const auto wanted = params::roomIrNames()[choice];
    p.requestRoomIr();

    for (int i = 0; i < 500 && (p.getIrLoader().getStatus().loading || p.getIrLoader().getStatus().name != wanted); ++i)
        juce::Thread::sleep (20);

    juce::Thread::sleep (50);   // a Decay change re-trims the IR it already has
}

juce::AudioBuffer<float> render (LithiumDrumsProcessor& p, const std::vector<Hit>& hits)
{
    const auto length = (int) ((hits.back().seconds + 4.0) * sampleRate);
    juce::AudioBuffer<float> result (2, length), block (p.getTotalNumOutputChannels(), blockSize);
    size_t next = 0;

    // Whatever the last render left ringing in the reverbs and delay first.
    for (int i = 0; i < (int) (8.0 * sampleRate / blockSize); ++i)
    {
        juce::MidiBuffer none;
        block.clear();
        p.processBlock (block, none);
    }

    for (int start = 0; start < length; start += blockSize)
    {
        const auto n = std::min (blockSize, length - start);
        juce::MidiBuffer midi;

        for (; next < hits.size() && (int) (hits[next].seconds * sampleRate) < start + n; ++next)
            midi.addEvent (juce::MidiMessage::noteOn (10, hits[next].note, (juce::uint8) hits[next].velocity),
                           std::max (0, (int) (hits[next].seconds * sampleRate) - start));

        block.clear();
        juce::AudioBuffer<float> view (block.getArrayOfWritePointers(), block.getNumChannels(), n);
        p.processBlock (view, midi);

        for (int ch = 0; ch < 2; ++ch)
            result.copyFrom (ch, start, block, ch, 0, n);
    }

    return result;
}

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (24));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

//==============================================================================
// Applying a recipe.

struct Recipe
{
    juce::String name, group, kit, description, makeup;
    juce::var pieces, mics, fx;
    double chainInputLufs = 0.0, outputLufs = 0.0;   // this preset's own targets, if not the file's
};

/** Sets a parameter from a recipe's value: a number in the parameter's own
    units, true/false, or a choice's name. Out-of-range values and unknown
    names are errors, so a typo in a recipe can't pass unnoticed. */
juce::Result set (juce::AudioProcessorValueTreeState& state, const juce::String& id, const juce::var& value)
{
    auto* p = state.getParameter (id);

    if (p == nullptr)
        return juce::Result::fail ("no parameter " + id);

    float v = 0.0f;

    if (value.isBool())
        v = (bool) value ? 1.0f : 0.0f;
    else if (value.isString())
    {
        auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p);
        const auto index = choice != nullptr ? choice->choices.indexOf (value.toString(), true) : -1;

        if (index < 0)
            return juce::Result::fail (id + " has no choice \"" + value.toString() + "\"");

        v = (float) index;
    }
    else
        v = (float) value;

    const auto& range = p->getNormalisableRange();

    if (v < range.start - 1.0e-4f || v > range.end + 1.0e-4f)
        return juce::Result::fail (id + " = " + juce::String (v) + " is outside " + juce::String (range.start) + " .. " + juce::String (range.end));

    p->setValueNotifyingHost (p->convertTo0to1 (v));
    return juce::Result::ok();
}

juce::Result setAll (juce::AudioProcessorValueTreeState& state, const juce::var& settings,
                     const std::function<juce::String (const juce::String&)>& idFor)
{
    if (auto* object = settings.getDynamicObject())
        for (const auto& [name, value] : object->getProperties())
            if (const auto r = set (state, idFor (name.toString()), value); r.failed())
                return r;

    return juce::Result::ok();
}

/** How specific a selector is: the general ones apply first, so "piece:kick_in"
    overrides "kick", which overrides "all". -1 if it isn't one. */
int pieceSelectorRank (const juce::String& key)
{
    if (key == "all")
        return 0;

    if (key == "toms" || key == "cymbals")
        return 1;

    if (roleFromKey (key.toStdString()).has_value())
        return 2;

    return key.startsWith ("piece:") ? 3 : -1;
}

bool pieceMatches (const juce::String& key, const KitPieceInfo& piece)
{
    const auto group = info (piece.role).group;

    if (key == "all")
        return true;

    if (key == "toms")
        return group == PieceGroup::toms;

    if (key == "cymbals")
        return group == PieceGroup::cymbals;

    if (key.startsWith ("piece:"))
        return key.fromFirstOccurrenceOf (":", false, false) == piece.id;

    return key == juce::String (info (piece.role).key.data(), info (piece.role).key.size());
}

int micSelectorRank (const juce::String& key)
{
    if (key == "all")
        return 0;

    if (micRoleFromKey (key.toStdString()).has_value())
        return 1;

    return key.startsWith ("mic:") ? 2 : -1;
}

bool micMatches (const juce::String& key, const KitMicInfo& mic)
{
    if (key == "all")
        return true;

    if (key.startsWith ("mic:"))
        return key.fromFirstOccurrenceOf (":", false, false) == mic.id;

    return micRoleFromKey (key.toStdString()) == mic.role;
}

/** Applies a "pieces" or "mics" block, general selectors first. */
template <typename Item>
juce::Result applySlots (juce::AudioProcessorValueTreeState& state, const juce::var& block, const std::vector<Item>& items,
                         int (*rank) (const juce::String&), bool (*matches) (const juce::String&, const Item&),
                         juce::String (*paramId) (int, std::string_view))
{
    auto* object = block.getDynamicObject();

    if (object == nullptr)
        return juce::Result::ok();

    std::vector<juce::String> keys;

    for (const auto& property : object->getProperties())
    {
        const auto key = property.name.toString();

        if (rank (key) < 0)
            return juce::Result::fail ("unknown selector \"" + key + "\"");

        keys.push_back (key);
    }

    std::stable_sort (keys.begin(), keys.end(), [rank] (const auto& a, const auto& b) { return rank (a) < rank (b); });

    for (const auto& key : keys)
    {
        auto any = false;

        for (size_t slot = 0; slot < items.size(); ++slot)
        {
            if (! matches (key, items[slot]))
                continue;

            any = true;
            const auto r = setAll (state, object->getProperty (key), [&] (const juce::String& name)
            {
                return paramId ((int) slot, name.toStdString());
            });

            if (r.failed())
                return juce::Result::fail (key + ": " + r.getErrorMessage());
        }

        // A role the kit doesn't have is fine (a recipe for any kit); an id
        // that matches nothing is a typo.
        if (! any && key.containsChar (':'))
            return juce::Result::fail ("\"" + key + "\" matches nothing in this kit");
    }

    return juce::Result::ok();
}

juce::Result apply (LithiumDrumsProcessor& p, const KitInfo& kit, const Recipe& recipe)
{
    auto& state = p.getParameters();

    if (const auto r = applySlots<KitPieceInfo> (state, recipe.pieces, kit.pieces, pieceSelectorRank, pieceMatches, params::pieceParamId); r.failed())
        return juce::Result::fail ("pieces, " + r.getErrorMessage());

    if (const auto r = applySlots<KitMicInfo> (state, recipe.mics, kit.mics, micSelectorRank, micMatches, params::micParamId); r.failed())
        return juce::Result::fail ("mics, " + r.getErrorMessage());

    if (recipe.fx.getDynamicObject() != nullptr && recipe.fx.getDynamicObject()->hasProperty (params::masterVolume))
        return juce::Result::fail ("fx: master_volume is set by the level matching");

    return setAll (state, recipe.fx, [] (const juce::String& id) { return id; });
}

//==============================================================================
// The master chain's stages, switched off for the level matching's first pass.
const juce::StringArray masterStages { "master_eq_on", "master_comp_on", params::crushOn, params::tapeOn, "master_trans_on", params::limitOn };

float get (LithiumDrumsProcessor& p, const juce::String& id)
{
    return p.getParameters().getRawParameterValue (id)->load();
}

void setValue (LithiumDrumsProcessor& p, const juce::String& id, float v)
{
    auto* param = p.getParameters().getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (v));
}

/** The most each switched-on compressor took off since the last call. */
juce::String reductions (LithiumDrumsProcessor& p, const KitInfo& kit)
{
    auto& fb = p.getMixerFeedback();
    juce::StringArray parts;

    auto add = [&] (const juce::String& name, std::atomic<float>& value, bool on)
    {
        const auto db = MixerFeedback::take (value);

        if (on)
            parts.add (name + " " + juce::String (db, 1));
    };

    for (size_t i = 0; i < kit.pieces.size(); ++i)
        add (kit.pieces[i].id, fb.pieceReductionDb[i], get (p, params::pieceParamId ((int) i, params::compOn)) > 0.5f);

    for (size_t i = 0; i < kit.mics.size(); ++i)
        add ("mic:" + kit.mics[i].id, fb.micReductionDb[i], kit.mics[i].role != MicRole::close && get (p, params::micParamId ((int) i, params::compOn)) > 0.5f);

    add ("glue", fb.glueReductionDb, get (p, "master_comp_on") > 0.5f);
    add ("crush", fb.crushReductionDb, get (p, params::crushOn) > 0.5f);
    add ("limiter", fb.limiterReductionDb, get (p, params::limitOn) > 0.5f);
    return parts.isEmpty() ? juce::String() : "  gain reduction (dB): " + parts.joinIntoString (", ");
}

juce::String describe (const Measurement& m)
{
    auto f = [] (double v) { return juce::String (v, 1).paddedLeft (' ', 6); };
    return f (m.lufs) + " LUFS" + f (m.peakDb) + " dBFS peak" + f (m.crestDb()) + " crest" + f (m.lowDb) + " low" + f (m.highDb) + " high";
}

std::vector<Recipe> readRecipes (const juce::File& file, double& kitLevel, double& targetPre, double& target, juce::String& error)
{
    const auto json = juce::JSON::parse (file);
    std::vector<Recipe> result;

    if (json["format"].toString() != "lithium-factory-presets")
    {
        error = file.getFullPathName() + " isn't a factory preset recipe file";
        return {};
    }

    kitLevel = json.getProperty ("kitLufs", -22.0);
    targetPre = json.getProperty ("chainInputLufs", -18.0);
    target = json.getProperty ("outputLufs", -16.0);

    if (const auto* items = json["presets"].getArray())
        for (const auto& v : *items)
            result.push_back ({ v["name"].toString(), v["group"].toString(), v["kit"].toString(), v["description"].toString(),
                                v["makeup"].toString(), v["pieces"], v["mics"], v["fx"],
                                v.getProperty ("chainInputLufs", targetPre), v.getProperty ("outputLufs", target) });

    return result;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    if (args.size() < 2 || args[0].startsWith ("--"))
    {
        std::cerr << "Usage: LithiumPresetGen <recipes.json> <out folder> [--preset <name>] [--wav <folder>] [--reference] [--verify]" << std::endl;
        return 2;
    }

    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const auto recipeFile = cwd.getChildFile (args[0]);
    const auto outFolder = cwd.getChildFile (args[1]);
    const auto only = args.indexOf ("--preset") >= 0 ? args[args.indexOf ("--preset") + 1] : juce::String();
    const auto wavFolder = args.indexOf ("--wav") >= 0 ? cwd.getChildFile (args[args.indexOf ("--wav") + 1]) : juce::File();
    const auto reference = args.contains ("--reference");
    const auto verify = args.contains ("--verify");

    double kitLevel = -22.0, targetPre = -18.0, target = -16.0;
    juce::String error;
    const auto recipes = readRecipes (recipeFile, kitLevel, targetPre, target, error);

    if (error.isNotEmpty())
    {
        std::cerr << error << std::endl;
        return 1;
    }

    // The groove's notes are General MIDI ones, whatever map the user picked.
   #if JUCE_WINDOWS
    _putenv_s ("LITHIUM_MIDI_MAP", "general-midi");
   #else
    setenv ("LITHIUM_MIDI_MAP", "general-midi", 1);
   #endif

    const auto hits = testGroove();
    int failures = 0, written = 0;

    for (const auto& recipe : recipes)
    {
        if (only.isNotEmpty() && ! recipe.name.containsIgnoreCase (only))
            continue;

        std::cout << "\n" << recipe.name << " (" << recipe.group << ", " << recipe.kit << ")" << std::endl;

        // A new instance per preset, so nothing carries over from the last one.
        auto processor = std::make_unique<LithiumDrumsProcessor>();
        processor->prepareToPlay (sampleRate, blockSize);
        const auto folder = kitlibrary::resolveSavedKit ({}, recipe.kit, processor->getKitsFolder());

        if (folder == juce::File() || ! folder.getChildFile ("kit.json").existsAsFile())
        {
            std::cout << "  skipped: kit " << recipe.kit << " isn't installed" << std::endl;
            continue;
        }

        processor->loadKit (folder, true);

        if (! waitForKit (*processor, folder))
        {
            std::cout << "  FAILED: the kit didn't load: " << processor->getKitError() << std::endl;
            ++failures;
            continue;
        }

        const auto kit = processor->getKitInfo();
        waitForRoomIr (*processor);
        const auto before = measure (render (*processor, hits));
        std::cout << "  kit defaults  " << describe (before) << std::endl;

        if (reference && wavFolder != juce::File())
            writeWav (wavFolder.getChildFile ("Reference - " + kit->credits.name + ".wav"), render (*processor, hits));

        if (const auto r = apply (*processor, *kit, recipe); r.failed())
        {
            std::cout << "  FAILED: " << r.getErrorMessage() << std::endl;
            ++failures;
            continue;
        }

        // The kit trim, on top of the recipe's own drum volumes.
        const auto trim = (float) (kitLevel - before.lufs);

        for (size_t i = 0; i < kit->pieces.size(); ++i)
        {
            const auto id = params::pieceParamId ((int) i, "volume");
            const auto wanted = get (*processor, id) + trim;

            if (wanted > 12.0f)
                std::cout << "  warning: " << kit->pieces[i].id << " can't take the kit trim (it would be at "
                          << juce::String (wanted, 1) << " dB)" << std::endl;

            setValue (*processor, id, juce::jlimit (-60.0f, 12.0f, wanted));
        }

        waitForRoomIr (*processor);

        // Pass 1, the chain off: the master volume (before the chain) brings
        // the mix to the chain's input level. Without the chain the level is
        // linear in the master volume, so one correction lands on it.
        std::map<juce::String, float> stages;

        for (const auto& id : masterStages)
        {
            stages[id] = get (*processor, id);
            setValue (*processor, id, 0.0f);
        }

        const auto dry = measure (render (*processor, hits));
        const auto volume = (float) juce::jlimit (-30.0, 6.0, recipe.chainInputLufs - dry.lufs);
        setValue (*processor, params::masterVolume, volume);

        if (std::abs (volume - (recipe.chainInputLufs - dry.lufs)) > 0.05)
            std::cout << "  warning: the master volume can't reach the chain input level (it would need "
                      << juce::String (recipe.chainInputLufs - dry.lufs, 1) << " dB)" << std::endl;

        for (const auto& [id, value] : stages)
            setValue (*processor, id, value);

        // Pass 2, the chain on: the named makeup control takes up whatever the
        // chain added or took away. The chain isn't linear, so this goes round
        // a few times.
        const auto output = recipe.outputLufs;
        auto audio = render (*processor, hits);
        auto after = measure (audio);

        for (int round = 0; round < 4 && recipe.makeup.isNotEmpty() && std::abs (after.lufs - output) > 0.2; ++round)
        {
            if (const auto r = set (processor->getParameters(), recipe.makeup,
                                    get (*processor, recipe.makeup) + (float) (output - after.lufs)); r.failed())
            {
                std::cout << "  warning: makeup " << r.getErrorMessage() << std::endl;
                break;
            }

            audio = render (*processor, hits);
            after = measure (audio);
        }

        // The meters keep the most since they were last read: clear them
        // and play it once more for this setting alone.
        reductions (*processor, *kit);
        render (*processor, hits);
        const auto reduced = reductions (*processor, *kit);

        std::cout << "  preset        " << describe (after) << "   (kit trim " << juce::String (trim, 1) << " dB, master volume " << juce::String (volume, 1) << " dB"
                  << (recipe.makeup.isNotEmpty() ? ", " + recipe.makeup + " " + juce::String (get (*processor, recipe.makeup), 1) : juce::String())
                  << ")" << std::endl;

        if (reduced.isNotEmpty())
            std::cout << reduced << std::endl;

        if (std::abs (after.lufs - output) > 1.5)
            std::cout << "  warning: " << juce::String (after.lufs - output, 1) << " LU from the " << output << " LUFS target" << std::endl;

        if (get (*processor, params::limitOn) < 0.5f && after.peakDb > -0.5)
            std::cout << "  warning: peaks at " << juce::String (after.peakDb, 2) << " dBFS: it needs the limiter" << std::endl;

        if (wavFolder != juce::File())
            writeWav (wavFolder.getChildFile (recipe.group + " - " + recipe.name + ".wav"), audio);

        // Factory presets find their kit by id: a path would only be right on this machine.
        presets::Contents contents;
        contents.kitId = kit->id;
        contents.kitName = kit->credits.name;
        contents.description = recipe.description;
        contents.parameters = processor->getParameters().copyState();

        const auto file = outFolder.getChildFile (recipe.group).getChildFile (juce::File::createLegalFileName (recipe.name) + presets::fileExtension);

        if (const auto r = presets::write (file, contents); r.failed())
        {
            std::cout << "  FAILED: " << r.getErrorMessage() << std::endl;
            ++failures;
            continue;
        }

        ++written;

        if (verify)
        {
            // Through LithiumDrumsProcessor::loadPreset, as the preset menu
            // loads it: the kit found by its id, then the settings.
            processor.reset();
            auto fresh = std::make_unique<LithiumDrumsProcessor>();
            fresh->prepareToPlay (sampleRate, blockSize);
            waitForKit (*fresh, fresh->getKitFolder());   // the default kit a new instance starts with

            const auto r = fresh->loadPreset (file);
            const auto loaded = r.wasOk() && waitForKit (*fresh, folder);
            waitForRoomIr (*fresh);
            const auto again = loaded ? measure (render (*fresh, hits)) : Measurement {};

            // Every setting as saved...
            juce::StringArray differ;

            for (const auto& child : contents.parameters)
            {
                // Log-scaled knobs round-trip through their 0..1 position, so
                // only to a float's precision (9000 Hz comes back 8999.997).
                const auto id = child["id"].toString();
                const auto value = (float) child["value"];

                if (std::abs (get (*fresh, id) - value) > 1.0e-5f * std::max (1.0f, std::abs (value)))
                    differ.add (id);
            }

            // ... and the same sound. Round-robin hits are picked at random,
            // which moves the loudest single hit (the peak) by up to about
            // half a dB between runs, and the loudness by a little.
            const auto ok = loaded && differ.isEmpty() && std::abs (again.lufs - after.lufs) < 0.5
                            && std::abs (again.peakDb - after.peakDb) < 1.0 && ! fresh->isPresetModified() && fresh->getPresetFile() == file;

            std::cout << "  verify        " << (loaded ? describe (again) : r.getErrorMessage())
                      << (fresh->isPresetModified() ? "   (shows as modified)" : "")
                      << (differ.isEmpty() ? juce::String() : "   (loaded differently: " + differ.joinIntoString (", ") + ")")
                      << (ok ? "   ok" : "   MISMATCH") << std::endl;

            if (! ok)
                ++failures;
        }
    }

    std::cout << "\n" << written << " presets written to " << outFolder.getFullPathName()
              << (failures > 0 ? ", " + juce::String (failures) + " failed" : juce::String()) << std::endl;
    return failures > 0 ? 1 : 0;
}
