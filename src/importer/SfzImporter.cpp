#include "SfzImporter.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace lithium::importer
{

namespace
{
    struct ImportError
    {
        juce::String message;
    };

    [[noreturn]] void fail (const juce::String& message)
    {
        throw ImportError { message };
    }

    using Controllers = std::map<int, double>;   // CC number -> value (0-127)

    void applyControllers (Controllers& state, const juce::var& changes)
    {
        if (auto* obj = changes.getDynamicObject())
            for (const auto& [cc, value] : obj->getProperties())
                state[cc.toString().getIntValue()] = (double) value;
    }

    double controller (const Controllers& state, int cc)
    {
        const auto it = state.find (cc);
        return it != state.end() ? it->second : 0.0;
    }

    /** "locc64" -> 64, or -1 if the opcode isn't prefix + a number. */
    int ccNumber (const juce::String& opcode, const char* prefix)
    {
        if (! opcode.startsWith (prefix))
            return -1;

        const auto rest = opcode.substring ((int) std::strlen (prefix));
        return rest.isNotEmpty() && rest.containsOnly ("0123456789") ? rest.getIntValue() : -1;
    }

    /** Whether a region plays on a note-on at this controller state, and the
        gain (in dB) its controller-dependent opcodes give it there. */
    bool isActive (const sfz::Region& r, const Controllers& cc, double& controllerGainDb)
    {
        // Release, legato and similar triggers don't belong to a drum hit.
        const auto trigger = r.get ("trigger", "attack");

        if (trigger != "attack" && trigger != "first")
            return false;

        // A note-on happens with no aftertouch yet (pressure 0). Regions that
        // need some (lopolyaft/lochanaft above 0) play only while a pad is
        // held: Tchimera keeps a copy of each cymbal for the grabbed state,
        // which would otherwise double every hit. Grabs reach Lithium Drums as
        // aftertouch chokes instead (DrumEngine::polyAftertouch).
        if (r.getNumber ("lopolyaft", 0.0) > 0.0 || r.getNumber ("lochanaft", 0.0) > 0.0)
            return false;

        double gain = 1.0;
        controllerGainDb = 0.0;

        for (const auto& [name, value] : r.opcodes)
        {
            if (const auto n = ccNumber (name, "locc"); n >= 0 && controller (cc, n) < value.getDoubleValue())
                return false;

            if (const auto n = ccNumber (name, "hicc"); n >= 0 && controller (cc, n) > value.getDoubleValue())
                return false;

            // Controller crossfades: power curve, as SFZ players default to.
            auto crossfade = [&] (const char* loPrefix, const char* hiName, bool fadeIn)
            {
                if (const auto n = ccNumber (name, loPrefix); n >= 0)
                {
                    const auto lo = value.getDoubleValue();
                    const auto hi = r.getNumber (juce::String (hiName) + juce::String (n), 127.0);
                    auto x = hi > lo ? juce::jlimit (0.0, 1.0, (controller (cc, n) - lo) / (hi - lo)) : 1.0;
                    gain *= std::sqrt (fadeIn ? x : 1.0 - x);
                }
            };

            crossfade ("xfin_locc", "xfin_hicc", true);
            crossfade ("xfout_locc", "xfout_hicc", false);

            // amplitude_onccN (and ARIA's amplitude_ccN) is a multiplier that
            // rises from 0 at CC 0 to N% at CC 127, as ARIA plays it: library
            // mixers use it as a fader (Naked Drums sets 200% with the
            // controller at its middle for unity; at 0 the mic is off).
            if (const auto n = std::max (ccNumber (name, "amplitude_oncc"), ccNumber (name, "amplitude_cc")); n >= 0)
                gain *= value.getDoubleValue() / 100.0 * controller (cc, n) / 127.0;

            if (const auto n = ccNumber (name, "volume_oncc"); n >= 0)
                controllerGainDb += value.getDoubleValue() * controller (cc, n) / 127.0;

            if (const auto n = ccNumber (name, "gain_oncc"); n >= 0)
                controllerGainDb += value.getDoubleValue() * controller (cc, n) / 127.0;
        }

        if (gain <= 1.0e-4)
            return false;

        controllerGainDb += 20.0 * std::log10 (gain);
        return true;
    }

    double baseGainDb (const sfz::Region& r)
    {
        // SFZ v2 adds a level per header on top of the region's own: the
        // global_, master_ and group_ volumes add up, the amplitudes multiply
        // (Tchimera balances each drum's close mic against the overheads with
        // them).
        double amplitude = 1.0, volume = r.getNumber ("volume", 0.0) + r.getNumber ("gain", 0.0);

        for (const auto* prefix : { "", "global_", "master_", "group_" })
        {
            amplitude *= std::max (1.0e-4, r.getNumber (juce::String (prefix) + "amplitude", 100.0) / 100.0);

            if (*prefix != 0)
                volume += r.getNumber (juce::String (prefix) + "volume", 0.0);
        }

        return volume + 20.0 * std::log10 (amplitude);
    }

    double pitchShiftSemitones (const sfz::Region& r, int key)
    {
        const auto centre = r.getNote ("pitch_keycenter", 60);
        const auto keytrack = r.getNumber ("pitch_keytrack", 100.0) / 100.0;
        return r.getNumber ("transpose", 0.0) + r.getNumber ("tune", 0.0) / 100.0 + (key - centre) * keytrack;
    }

    // Opcodes that change the sound in ways the native format doesn't model
    // yet. The import goes ahead without them, and says so.
    const juce::StringArray& unsupportedOpcodes()
    {
        static const juce::StringArray names { "cutoff", "fil_type", "resonance", "eq1_gain", "eq2_gain", "eq3_gain",
                                               "ampeg_decay", "ampeg_sustain", "ampeg_hold", "end", "loop_start",
                                               "pitch_veltrack", "delay" };
        return names;
    }

    //==============================================================================
    struct MicRule
    {
        juce::String id, name, role { "close" };
        juce::StringArray patterns;      // sample path wildcards; empty = every sample
        double volumeDb = 0.0, pan = 0.0;
        int channels = 0;                // 0 = from the samples
        bool invertPolarity = false;
        bool swapStereo = false;         // recorded from the drummer's seat: flip to the audience's view
    };

    struct AudioInfo
    {
        int channels = 0;
        std::int64_t frames = 0;
        double sampleRate = 0.0;
    };

    bool matchesAny (const juce::String& path, const juce::StringArray& patterns)
    {
        for (const auto& p : patterns)
            if (path.matchesWildcard (p, true))
                return true;

        return false;
    }

    /** A region chosen for an articulation, with what the conversion needs from it. */
    struct Chosen
    {
        const sfz::Region* region = nullptr;
        int mic = 0;
        juce::String file;               // relative to the source root
        double gainDb = 0.0;             // everything but velocity
        double seqPosition = 1.0, randomLow = 0.0;
        int velocityLow = 1, velocityHigh = 127;
    };

    //==============================================================================
    class KitConverter
    {
    public:
        KitConverter (const sfz::Instrument& i, const juce::var& m, const juce::var& k, juce::File root,
                      std::map<juce::String, AudioInfo>& audio)
            : instrument (i), mapping (m), kitEntry (k), sourceRoot (std::move (root)), audioInfo (audio)
        {
            controllers.insert (instrument.controllerDefaults.begin(), instrument.controllerDefaults.end());
            applyControllers (controllers, mapping["cc"]);
            applyControllers (controllers, kitEntry["cc"]);

            excludes = toStringArray (mapping["exclude"]);
            excludes.addArray (toStringArray (kitEntry["exclude"]));
            audienceView = (bool) mapping.getProperty ("audienceView", true);
            levelDb = (double) mapping.getProperty ("levelDb", 0.0) + (double) kitEntry.getProperty ("levelDb", 0.0);
        }

        KitModel convert()
        {
            kit.name = kitEntry["name"].toString();

            if (kit.name.isEmpty())
                kit.name = instrument.file.getFileNameWithoutExtension().replaceCharacter ('_', ' ').trim();

            kit.id = kitEntry["id"].toString().isNotEmpty() ? kitEntry["id"].toString()
                                                             : toId (kit.name).replaceCharacter ('_', '-');
            kit.description = kitEntry["description"].toString();
            kit.tags = toStringArray (kitEntry["tags"]);

            for (const auto& w : instrument.warnings)
                kit.warnings.add (w);

            setUpMics();
            for (const auto& spec : pieceSpecs())
                convertPiece (spec);

            if (kit.pieces.empty())
                fail (kit.name + ": no pieces with samples were found");

            if (kit.pieces.size() > 24)
            {
                kit.warnings.add (juce::String ((int) kit.pieces.size()) + " pieces; only the first 24 are kept");
                kit.pieces.resize (24);
            }

            finishMics();
            finishChokes();
            reportUnsupported();

            auto from = object();
            put (from, "format", "SFZ");
            put (from, "file", instrument.file.getRelativePathFrom (sourceRoot).replaceCharacter ('\\', '/'));
            kit.importedFrom = from;

            return std::move (kit);
        }

    private:
        //==============================================================================
        void setUpMics()
        {
            const auto& mics = mapping["mics"];

            if (mics.toString() == "perPiece")
            {
                perPieceMics = true;
                return;
            }

            if (const auto* list = mics.getArray())
            {
                for (const auto& m : *list)
                {
                    MicRule rule;
                    rule.id = m["id"].toString();
                    rule.name = m["name"].toString().isNotEmpty() ? m["name"].toString() : rule.id;
                    rule.role = m["role"].toString().isNotEmpty() ? m["role"].toString() : juce::String ("close");
                    rule.patterns = toStringArray (m["samples"]);
                    rule.volumeDb = (double) m.getProperty ("volumeDb", 0.0);
                    rule.pan = (double) m.getProperty ("pan", 0.0);
                    rule.channels = (int) m.getProperty ("channels", 0);
                    rule.invertPolarity = (bool) m["invertPolarity"];
                    rule.swapStereo = (bool) m["swapStereo"];
                    rules.push_back (rule);
                }
            }

            if (rules.empty())
                rules.push_back ({ "kit", "Kit", "overheads", {}, 0.0, 0.0, 0, false, false });
        }

        int micFor (const juce::String& path, const juce::String& pieceId, const juce::String& pieceName)
        {
            if (perPieceMics)
            {
                // AVL-style kits: one (mono) recording per piece, panned per
                // piece. Each piece gets a mic of its own, which keeps the
                // pans and gives every piece a mixer channel.
                for (size_t i = 0; i < rules.size(); ++i)
                    if (rules[i].id == pieceId)
                        return (int) i;

                rules.push_back ({ pieceId, pieceName, "close", {}, 0.0, 0.0, 0, false, false });
                return (int) rules.size() - 1;
            }

            for (size_t i = 0; i < rules.size(); ++i)
                if (rules[i].patterns.isEmpty() || matchesAny (path, rules[i].patterns))
                    return (int) i;

            return -1;
        }

        const AudioInfo& infoFor (const juce::File& file)
        {
            const auto key = file.getFullPathName();
            auto it = audioInfo.find (key);

            if (it == audioInfo.end())
            {
                if (formats.getNumKnownFormats() == 0)
                    formats.registerBasicFormats();

                AudioInfo info;

                if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file)); reader != nullptr)
                    info = { (int) reader->numChannels, reader->lengthInSamples, reader->sampleRate };

                it = audioInfo.emplace (key, info).first;
            }

            return it->second;
        }

        //==============================================================================
        /** The mapping's pieces for this kit, or automatic ones without a mapping. */
        std::vector<juce::var> pieceSpecs()
        {
            std::vector<juce::var> specs;
            const auto& list = kitEntry["pieces"].isArray() ? kitEntry["pieces"] : mapping["pieces"];
            const auto skip = toStringArray (kitEntry["skipPieces"]);

            for (const auto* source : { list.getArray(), kitEntry["extraPieces"].getArray() })
                if (source != nullptr)
                    for (const auto& p : *source)
                        if (! skip.contains (p["id"].toString()))
                            specs.push_back (p);

            if (specs.empty())
                specs = automaticPieces();

            return specs;
        }

        /** Without a mapping: one piece per key, named after its samples, with
            the hi-hat keys gathered into one hi-hat piece. */
        std::vector<juce::var> automaticPieces()
        {
            std::map<int, juce::String> keyNames;

            for (const auto& r : instrument.regions)
            {
                double ignored = 0.0;

                if (r.isBuiltInSample() || ! isActive (r, controllers, ignored))
                    continue;

                const auto lo = r.getNote ("lokey", 0), hi = r.getNote ("hikey", 127);

                if (hi - lo > 12)
                    continue;   // a melodic range, not a drum

                for (int key = lo; key <= hi; ++key)
                    if (keyNames.count (key) == 0)
                        keyNames[key] = nameFromSample (r.sample);
            }

            std::vector<juce::var> specs;
            juce::var hihat;

            for (const auto& [key, name] : keyNames)
            {
                auto art = object();
                put (art, "key", key);
                const auto role = roleForName (name);

                if (role == "hihat")
                {
                    const auto lower = name.toLowerCase();
                    put (art, "id", lower.contains ("open") ? (lower.contains ("semi") || lower.contains ("half") ? "half_open" : "open")
                                  : (lower.contains ("pedal") || lower.contains ("foot") ? "pedal" : "closed"));
                    put (art, "name", name);

                    if (hihat.isVoid())
                    {
                        hihat = object();
                        put (hihat, "id", "hihat");
                        put (hihat, "name", "Hi-Hat");
                        put (hihat, "role", "hihat");
                        put (hihat, "articulations", juce::Array<juce::var>());
                        specs.push_back (hihat);
                    }

                    // Articulation ids must be unique within the piece.
                    auto* arts = hihat["articulations"].getArray();
                    auto id = art["id"].toString();

                    for (int n = 2; std::any_of (arts->begin(), arts->end(), [&] (const auto& a) { return a["id"].toString() == id; }); ++n)
                        id = art["id"].toString() + juce::String (n);

                    put (art, "id", id);
                    arts->add (art);
                    continue;
                }

                put (art, "id", "hit");
                put (art, "name", "Hit");

                auto piece = object();
                put (piece, "id", toId (name) + "_" + juce::String (key));
                put (piece, "name", name);
                put (piece, "role", role);
                put (piece, "articulations", juce::Array<juce::var> { art });
                specs.push_back (piece);
            }

            return specs;
        }

        static juce::String nameFromSample (const juce::String& sample)
        {
            // "OH/snare_OH_FF_3.wav" -> "snare"; "38-Ludwig-14-Snare-2.wav" -> "Ludwig 14 Snare"
            auto words = juce::StringArray::fromTokens (juce::File::createFileWithoutCheckingPath (sample)
                                                            .getFileNameWithoutExtension(), "_- ", "");
            static const juce::StringArray dynamics { "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff", "ghost", "oh", "close", "room" };

            while (words.size() > 1 && (words[words.size() - 1].containsOnly ("0123456789")
                                        || dynamics.contains (words[words.size() - 1].toLowerCase())
                                        || words[words.size() - 1].toLowerCase().startsWith ("vl")
                                        || words[words.size() - 1].toLowerCase().startsWith ("rr")))
                words.remove (words.size() - 1);

            while (words.size() > 1 && words[0].containsOnly ("0123456789"))
                words.remove (0);

            return words.joinIntoString (" ");
        }

        //==============================================================================
        void convertPiece (const juce::var& spec)
        {
            KitModel::Piece piece;
            piece.id = spec["id"].toString();
            piece.name = spec["name"].toString().isNotEmpty() ? spec["name"].toString() : piece.id;
            piece.role = spec["role"].toString().isNotEmpty() ? spec["role"].toString() : roleForName (piece.name);
            piece.volumeDb = (double) spec.getProperty ("volumeDb", 0.0);

            std::vector<double> shifts;

            if (const auto* arts = spec["articulations"].getArray())
            {
                for (const auto& a : *arts)
                {
                    auto art = convertArticulation (piece, a, shifts);

                    if (! art.zones.empty())
                        piece.articulations.push_back (std::move (art));
                }
            }

            if (piece.articulations.empty())
            {
                kit.warnings.add ("Piece \"" + piece.name + "\" has no samples in this kit; left out");
                return;
            }

            // SFZ tunes per region; the native format tunes per piece. A piece
            // whose regions all share one shift gets it as its tuning.
            if (! shifts.empty())
            {
                const auto [lo, hi] = std::minmax_element (shifts.begin(), shifts.end());

                if (*hi - *lo < 0.01)
                    piece.tune = std::round (*lo * 100.0) / 100.0;
                else
                    kit.warnings.add ("Piece \"" + piece.name + "\" is tuned differently per sample; tuning ignored");
            }

            piece.tune += (double) spec.getProperty ("tune", 0.0);
            kit.pieces.push_back (std::move (piece));
        }

        KitModel::Articulation convertArticulation (const KitModel::Piece& piece, const juce::var& spec, std::vector<double>& shifts)
        {
            KitModel::Articulation art;
            art.id = spec["id"].toString();
            art.name = spec["name"].toString().isNotEmpty() ? spec["name"].toString() : art.id;
            art.openness = spec.hasProperty ("openness") ? juce::jlimit (0.0, 1.0, (double) spec["openness"]) : -1.0;
            art.stroke = spec["stroke"].toString();

            const auto key = sfz::noteNumber (spec["key"].toString());
            const auto include = toStringArray (spec["samples"]);
            const auto exclude = toStringArray (spec["exclude"]);

            if (key < 0)
                fail ("Piece \"" + piece.id + "\", articulation \"" + art.id + "\" needs a \"key\"");

            auto state = controllers;
            applyControllers (state, spec["cc"]);

            // The regions that play together when this key is hit.
            std::vector<Chosen> chosen;
            std::set<juce::String> chokes, chokedBy;

            for (const auto& r : instrument.regions)
            {
                double ccGainDb = 0.0;

                if (key < r.getNote ("lokey", 0) || key > r.getNote ("hikey", 127) || ! isActive (r, state, ccGainDb))
                    continue;

                // Silence regions and others on the key still choke what they're set to choke.
                if (const auto g = r.get ("group"); g.isNotEmpty() && g != "0")
                    chokes.insert (g);

                if (r.isBuiltInSample() || matchesAny (r.sample, excludes) || matchesAny (r.sample, exclude)
                    || (! include.isEmpty() && ! matchesAny (r.sample, include)))
                    continue;

                if (const auto end = r.getNumber ("end", 0.0); r.has ("end") && end <= 0.0)
                    continue;   // end=0 or -1: a region that plays nothing

                if (! r.sampleFile.existsAsFile())
                {
                    kit.warnings.addIfNotAlreadyThere ("Missing sample " + r.sample + " (" + r.where + ")");
                    continue;
                }

                const auto relative = r.sampleFile.getRelativePathFrom (sourceRoot).replaceCharacter ('\\', '/');

                if (relative.startsWith ("..") || juce::File::isAbsolutePath (relative))
                    fail ("Sample " + r.sample + " is outside the folder being imported (" + sourceRoot.getFullPathName() + ")");

                const auto mic = micFor (relative, piece.id, piece.name);

                if (mic < 0)
                {
                    kit.warnings.addIfNotAlreadyThere ("No mic matches " + relative + "; left out");
                    continue;
                }

                // An envelope that sustains at 100% never changes the sound.
                const auto envelopeMatters = r.getNumber ("ampeg_sustain", 100.0) < 99.9;

                for (const auto& name : unsupportedOpcodes())
                    if (r.has (name) && (envelopeMatters || ! name.startsWith ("ampeg_")))
                        ++unsupportedCounts[name];

                Chosen c;
                c.region = &r;
                c.mic = mic;
                c.file = relative;
                c.gainDb = baseGainDb (r) + ccGainDb;
                c.seqPosition = r.getNumber ("seq_position", 1.0);
                c.randomLow = r.getNumber ("lorand", 0.0);
                c.velocityLow = juce::jlimit (1, 127, (int) r.getNumber ("lovel", 1.0));
                c.velocityHigh = juce::jlimit (1, 127, (int) r.getNumber ("hivel", 127.0));

                if (c.velocityHigh < c.velocityLow)
                    continue;

                if (const auto g = r.get ("off_by"); g.isNotEmpty() && g != "0")
                    chokedBy.insert (g);

                shifts.push_back (pitchShiftSemitones (r, key));
                pieceRegions[piece.id].push_back (&r);
                chosen.push_back (c);
            }

            if (chosen.empty())
            {
                kit.warnings.add ("No samples for " + piece.name + " / " + art.name + " on key " + juce::String (key));
                return art;
            }

            buildZones (art, chosen);

            // Chokes: explicit in the mapping, or from the SFZ's group/off_by
            // (tidied up once the whole kit is known, in finishChokes()).
            if (spec.hasProperty ("chokeGroup") || spec.hasProperty ("chokes"))
            {
                art.chokeGroup = spec["chokeGroup"].toString();
                art.chokes = toStringArray (spec["chokes"]);
            }
            else
            {
                if (! chokedBy.empty())
                    art.chokeGroup = "sfz_group_" + *chokedBy.begin();

                for (const auto& g : chokes)
                    art.chokes.add ("sfz_group_" + g);

            }

            // The MIDI note: the SFZ key, unless the mapping gives another
            // (two articulations read from one key need notes of their own).
            const auto note = spec.hasProperty ("note") ? sfz::noteNumber (spec["note"].toString()) : key;
            juce::Array<int> notes;

            if (note >= 0)
                notes.add (note);

            for (const auto& n : toStringArray (spec["notes"]))
                if (const auto extra = sfz::noteNumber (n); extra >= 0)
                    notes.addIfNotAlreadyThere (extra);

            for (const auto n : notes)
            {
                if (mappedNotes.count (n) != 0)
                {
                    kit.warnings.add ("Note " + juce::String (n) + " is mapped twice; " + piece.id + "/" + art.id + " left off it");
                    continue;
                }

                mappedNotes.insert (n);
                kit.midiMap.push_back ({ n, piece.id, art.id });
            }

            return art;
        }

        /** Velocity zones from the regions' velocity ranges. Each mic may split
            velocity differently, so the zone edges are the union of every
            region's edges; adjacent stretches that play exactly the same
            regions are merged back into one zone. */
        void buildZones (KitModel::Articulation& art, const std::vector<Chosen>& chosen)
        {
            std::set<int> edges { 1, 128 };

            for (const auto& c : chosen)
            {
                edges.insert (c.velocityLow);
                edges.insert (c.velocityHigh + 1);
            }

            struct Stretch
            {
                int low, high;
                std::vector<std::vector<const Chosen*>> hits;   // per round-robin, one region per mic
            };

            std::vector<Stretch> stretches;

            for (auto it = edges.begin(); std::next (it) != edges.end(); ++it)
            {
                const auto low = *it, high = *std::next (it) - 1;
                std::map<int, std::vector<const Chosen*>> byMic;

                for (const auto& c : chosen)
                    if (c.velocityLow <= low && c.velocityHigh >= high)
                        byMic[c.mic].push_back (&c);

                if (byMic.empty())
                    continue;   // a velocity gap: the engine plays the nearest zone

                size_t numRoundRobins = 0;

                for (auto& [mic, list] : byMic)
                {
                    // Round-robins in sequence order (seq_position), then random-robin order (lorand).
                    std::stable_sort (list.begin(), list.end(), [] (const auto* a, const auto* b)
                    {
                        return a->seqPosition < b->seqPosition || (! (b->seqPosition < a->seqPosition) && a->randomLow < b->randomLow);
                    });
                    numRoundRobins = std::max (numRoundRobins, list.size());
                }

                Stretch s { low, high, {} };

                for (size_t i = 0; i < numRoundRobins; ++i)
                {
                    std::vector<const Chosen*> hit;

                    for (const auto& [mic, list] : byMic)
                        hit.push_back (list[i % list.size()]);

                    s.hits.push_back (std::move (hit));
                }

                if (! stretches.empty() && stretches.back().hits == s.hits && stretches.back().high + 1 == low)
                    stretches.back().high = high;
                else
                    stretches.push_back (std::move (s));
            }

            for (const auto& s : stretches)
            {
                KitModel::Zone zone;
                zone.velocityLow = s.low;
                zone.velocityHigh = s.high;

                // The zone's level at its top velocity, and how much quieter its
                // bottom is, from the regions' gains and velocity curves.
                double gainSum = 0.0, rangeSum = 0.0;
                int count = 0;

                for (const auto& hit : s.hits)
                {
                    for (const auto* c : hit)
                    {
                        const auto top = velocityGain (*c->region, s.high), bottom = velocityGain (*c->region, s.low);
                        gainSum += c->gainDb + juce::Decibels::gainToDecibels (top, -120.0);
                        rangeSum += juce::Decibels::gainToDecibels (top, -120.0) - juce::Decibels::gainToDecibels (bottom, -120.0);
                        ++count;
                    }
                }

                zone.gainDb = juce::jlimit (-60.0, 24.0, gainSum / count);
                zone.velocityRangeDb = juce::jlimit (0.0, 24.0, rangeSum / count);

                for (const auto& hit : s.hits)
                {
                    KitModel::Hit h;

                    for (const auto* c : hit)
                    {
                        const auto& info = infoFor (c->region->sampleFile);
                        const auto micChannels = rules[(size_t) c->mic].channels;
                        KitModel::Part part;
                        part.file = c->file;
                        part.start = (std::int64_t) std::max (0.0, c->region->getNumber ("offset", 0.0));

                        // Stereo recordings on stereo mics keep both sides; anything
                        // else uses the first channel (centred on a stereo mic).
                        const auto stereo = info.channels >= 2 && micChannels != 1;
                        // A negative width (Tchimera's width=-100) plays the
                        // recording's sides swapped.
                        const auto reversed = c->region->getNumber ("width", 100.0) < 0.0;
                        const auto swap = stereo && (rules[(size_t) c->mic].swapStereo != reversed);
                        part.channels.push_back ({ c->mic, { swap ? 2 : 1, stereo ? (swap ? 1 : 2) : 0 } });
                        micFileChannels[c->mic] = std::max (micFileChannels[c->mic], stereo ? 2 : 1);
                        h.parts.push_back (std::move (part));
                    }

                    zone.hits.push_back (std::move (h));
                }

                art.zones.push_back (std::move (zone));
            }
        }

        //==============================================================================
        void finishMics()
        {
            // Mics that no hit uses are dropped, and the hits renumbered.
            std::vector<int> newIndex (rules.size(), -1);

            for (size_t i = 0; i < rules.size(); ++i)
            {
                if (micFileChannels.count ((int) i) == 0)
                    continue;

                const auto& rule = rules[i];
                KitModel::Mic mic;
                mic.id = rule.id;
                mic.name = rule.name;
                mic.role = rule.role;
                mic.channels = rule.channels > 0 ? rule.channels : micFileChannels[(int) i];
                mic.volumeDb = rule.volumeDb + levelDb;
                mic.pan = rule.pan;
                mic.invertPolarity = rule.invertPolarity;

                if (perPieceMics)
                {
                    // The pan of the piece's samples. SFZ pans are usually from
                    // the drummer's seat; the kit stage shows the audience's view.
                    double sum = 0.0;
                    int n = 0;

                    for (const auto* r : pieceRegions[rule.id])
                    {
                        sum += r->getNumber ("pan", 0.0);
                        ++n;
                    }

                    mic.pan = juce::jlimit (-1.0, 1.0, (n > 0 ? sum / n : 0.0) / 100.0 * (audienceView ? -1.0 : 1.0));
                }

                newIndex[i] = (int) kit.mics.size();
                kit.mics.push_back (mic);
            }

            if (kit.mics.empty() || kit.mics.size() > 24)
                fail (kit.name + ": " + juce::String ((int) kit.mics.size()) + " mics (Lithium Drums takes 1 to 24)");

            for (auto& p : kit.pieces)
                for (auto& a : p.articulations)
                    for (auto& z : a.zones)
                        for (auto& h : z.hits)
                            for (auto& part : h.parts)
                                for (auto& [mic, ch] : part.channels)
                                {
                                    mic = newIndex[(size_t) mic];

                                    // A stereo mic whose samples here are mono plays them centred.
                                    if (kit.mics[(size_t) mic].channels == 1)
                                        ch[1] = 0;
                                }
        }

        /** SFZ choke groups that no articulation belongs to (an SFZ "group"
            nobody is "off_by") would be warned about by the loader; drop them. */
        void finishChokes()
        {
            std::set<juce::String> members;

            for (const auto& p : kit.pieces)
                for (const auto& a : p.articulations)
                    if (a.chokeGroup.isNotEmpty())
                        members.insert (a.chokeGroup);

            for (auto& p : kit.pieces)
                for (auto& a : p.articulations)
                    for (int i = a.chokes.size(); --i >= 0;)
                        if (members.count (a.chokes[i]) == 0)
                            a.chokes.remove (i);

            std::set<juce::String> choked;

            for (const auto& p : kit.pieces)
                for (const auto& a : p.articulations)
                    for (const auto& c : a.chokes)
                        choked.insert (c);

            for (auto& p : kit.pieces)
                for (auto& a : p.articulations)
                    if (a.chokeGroup.isNotEmpty() && choked.count (a.chokeGroup) == 0)
                        a.chokeGroup = {};
        }

        void reportUnsupported()
        {
            for (const auto& [name, count] : unsupportedCounts)
                kit.warnings.add ("SFZ opcode \"" + name + "\" isn't supported yet; ignored on " + juce::String (count) + " regions");
        }

        const sfz::Instrument& instrument;
        const juce::var& mapping;
        const juce::var& kitEntry;
        const juce::File sourceRoot;
        std::map<juce::String, AudioInfo>& audioInfo;
        juce::AudioFormatManager formats;

        Controllers controllers;
        juce::StringArray excludes;
        bool audienceView = true;
        double levelDb = 0.0;
        bool perPieceMics = false;
        std::vector<MicRule> rules;
        std::map<int, int> micFileChannels;                          // mic -> channels its samples have
        std::map<juce::String, std::vector<const sfz::Region*>> pieceRegions;
        std::set<int> mappedNotes;
        std::map<juce::String, int> unsupportedCounts;
        KitModel kit;
    };

    //==============================================================================
    juce::Array<juce::File> findSfzFiles (const juce::File& folder, int depth)
    {
        juce::Array<juce::File> files;

        for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.sfz"))
            files.add (f);

        if (depth > 0)
            for (const auto& d : folder.findChildFiles (juce::File::findDirectories, false))
                files.addArray (findSfzFiles (d, depth - 1));

        files.sort();
        return files;
    }

    juce::File findByName (const juce::Array<juce::File>& files, const juce::String& path)
    {
        // A mapping names files relative to the download's root, but the user
        // may point the importer at a subfolder: match on the trailing path.
        const auto wanted = path.replaceCharacter ('\\', '/');

        for (const auto& f : files)
            if (f.getFullPathName().replaceCharacter ('\\', '/').endsWith ("/" + wanted))
                return f;

        for (const auto& f : files)
            if (f.getFileName().equalsIgnoreCase (juce::File::createFileWithoutCheckingPath (wanted).getFileName()))
                return f;

        return {};
    }
} // namespace

//==============================================================================
double velocityGain (const sfz::Region& r, int velocity)
{
    const auto v = juce::jlimit (0, 127, velocity);
    std::map<int, double> points;

    for (const auto& [name, value] : r.opcodes)
        if (const auto n = ccNumber (name, "amp_velcurve_"); n >= 0 && n <= 127)
            points[n] = juce::jlimit (0.0, 1.0, value.getDoubleValue());

    double curve = 0.0;

    if (points.empty())
    {
        curve = (v / 127.0) * (v / 127.0);    // the SFZ default: velocity squared
    }
    else
    {
        points.try_emplace (0, 0.0);
        points.try_emplace (127, 1.0);

        auto upper = points.lower_bound (v);

        if (upper->first == v)
        {
            curve = upper->second;
        }
        else
        {
            const auto lower = std::prev (upper);
            const auto t = (double) (v - lower->first) / (double) (upper->first - lower->first);
            curve = lower->second + t * (upper->second - lower->second);
        }
    }

    const auto track = juce::jlimit (0.0, 1.0, r.getNumber ("amp_veltrack", 100.0) / 100.0);
    return 1.0 - track + track * curve;
}

bool isSfzSource (const juce::File& source)
{
    if (source.existsAsFile())
        return source.hasFileExtension ("sfz");

    // As deep as importSfz() looks: some libraries keep their programs three
    // folders down (Naked Drums: "Wilkinson Audio/Naked Drums/Stereo").
    return ! findSfzFiles (source, 3).isEmpty();
}

juce::Result importSfz (const Options& options, std::vector<ImportedKit>& results)
{
    auto log = [&] (const juce::String& s) { if (options.log) options.log (s); };

    try
    {
        const auto sourceRoot = options.source.isDirectory() ? options.source : options.source.getParentDirectory();
        const auto allFiles = options.source.isDirectory() ? findSfzFiles (sourceRoot, 3) : juce::Array<juce::File> { options.source };

        if (allFiles.isEmpty())
            fail ("No .sfz files found at " + options.source.getFullPathName());

        // --- Mapping ------------------------------------------------------------------
        juce::var mapping;

        if (options.mappingFile != juce::File())
        {
            if (auto r = juce::JSON::parse (options.mappingFile.loadFileAsString(), mapping); r.failed())
                fail ("Can't read the mapping file: " + r.getErrorMessage());
        }
        else
        {
            juce::StringArray candidates { sourceRoot.getFileName() };

            for (const auto& f : allFiles)
                candidates.add (f.getFileName());

            mapping = findBuiltInMapping (candidates, "sfz");
        }

        // --- Which instruments become kits ---------------------------------------------
        std::vector<std::pair<juce::File, juce::var>> kitFiles;   // .sfz, its entry in the mapping

        if (const auto* kits = mapping["kits"].getArray())
        {
            for (const auto& k : *kits)
            {
                const auto f = findByName (allFiles, k["file"].toString());

                if (f != juce::File())
                    kitFiles.emplace_back (f, k);
            }

            if (kitFiles.empty())
                fail ("None of the instruments the mapping names were found at " + options.source.getFullPathName());

            log ("Using the mapping for \"" + mapping["pack"]["name"].toString() + "\"");
        }
        else
        {
            // Without a mapping, every instrument in the top two folder levels.
            for (const auto& f : options.source.isDirectory() ? findSfzFiles (sourceRoot, 1) : allFiles)
                kitFiles.emplace_back (f, juce::var());

            log ("No mapping file: automatic mapping");
        }

        // --- The pack -------------------------------------------------------------------
        PackModel pack;
        pack.sourceRoot = sourceRoot;
        pack.sourceFormat = "SFZ";
        pack.name = mapping["pack"]["name"].toString();

        if (pack.name.isEmpty())
            pack.name = sourceRoot.getFileName().replaceCharacter ('_', ' ');

        pack.id = mapping["pack"]["id"].toString().isNotEmpty() ? mapping["pack"]["id"].toString()
                                                                : toId (pack.name).replaceCharacter ('_', '-');
        pack.author = mapping["author"].toString();
        pack.sourceUrl = mapping["sourceUrl"].toString();
        pack.credits = mapping["credits"].toString();
        pack.description = mapping["description"].toString();
        pack.licenseName = mapping["license"]["name"].toString();
        pack.licenseUrl = mapping["license"]["url"].toString();
        pack.licenseFiles = toStringArray (mapping["license"]["files"]);
        pack.licenseNote = mapping["licenseNote"].toString();
        pack.conversionNote = mapping["conversionNote"].toString().isNotEmpty()
                                  ? mapping["conversionNote"].toString()
                                  : juce::String ("The audio itself is unchanged. The SFZ regions were regrouped into pieces,\n"
                                                  "articulations, velocity zones and round-robins, and the mics were named.");

        if (pack.licenseFiles.isEmpty())
            for (const auto& f : sourceRoot.findChildFiles (juce::File::findFiles, false))
                if (f.getFileName().startsWithIgnoreCase ("license") || f.getFileName().startsWithIgnoreCase ("copying")
                    || f.getFileName().startsWithIgnoreCase ("readme"))
                    pack.licenseFiles.add (f.getFileName());

        if (pack.licenseName.isEmpty())
        {
            juce::String text;

            for (const auto& name : pack.licenseFiles)
                text << "\n" << sourceRoot.getChildFile (name).loadFileAsString();

            std::tie (pack.licenseName, pack.licenseUrl) = detectLicense (text);
        }

        if (pack.licenseName.isEmpty())
            pack.warnings.add ("No licence found. Check the kit's terms before using or sharing it.");

        pack.shareAlike = mapping["license"]["shareAlike"].isVoid() ? pack.licenseName.containsIgnoreCase ("-SA")
                                                                    : (bool) mapping["license"]["shareAlike"];

        // --- Convert ------------------------------------------------------------------------
        std::map<juce::String, AudioInfo> audioInfo;   // shared, so headers are read once per file

        for (const auto& [file, entry] : kitFiles)
        {
            log ("Reading " + file.getRelativePathFrom (sourceRoot));
            sfz::Instrument instrument;

            if (const auto r = sfz::parse (file, instrument); r.failed())
                fail (r.getErrorMessage());

            pack.kits.push_back (KitConverter (instrument, mapping, entry, sourceRoot, audioInfo).convert());
        }

        for (auto& p : presetsFromMapping (mapping, pack.kits))
            pack.presets.push_back (std::move (p));

        return writePack (pack, options, results);
    }
    catch (const ImportError& e)
    {
        return juce::Result::fail (e.message);
    }
}

} // namespace lithium::importer
