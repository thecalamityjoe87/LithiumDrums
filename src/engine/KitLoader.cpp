#include "KitLoader.h"

#include "DrumEngine.h"
#include "KitManifest.h"
#include "MidiMapping.h"
#include "ParallelJobs.h"
#include "StreamCache.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>

namespace lithium
{

namespace
{
    // Turns choke group names into bit positions, shared across the whole kit.
    struct ChokeGroups
    {
        juce::StringArray names;

        std::uint32_t maskFor (const juce::String& name, juce::StringArray& warnings)
        {
            if (name.isEmpty())
                return 0;

            auto i = names.indexOf (name);

            if (i < 0)
            {
                if (names.size() == 32)
                {
                    warnings.addIfNotAlreadyThere ("More than 32 choke groups, \"" + name + "\" ignored");
                    return 0;
                }

                i = names.size();
                names.add (name);
            }

            return 1u << i;
        }
    };

    MicMask validMics (int numMics) noexcept
    {
        return numMics >= 32 ? allMics : (MicMask) ((1u << numMics) - 1);
    }

    struct FileHeader
    {
        int numChannels = 0;
        std::int64_t length = 0;
        int bitsPerSample = 0;
    };

    bool readHeader (juce::AudioFormatManager& formats, const juce::File& file, const juce::String& path,
                     double expectedRate, FileHeader& header, juce::String& error)
    {
        if (! file.existsAsFile())
        {
            error = "Missing sample file " + path;
            return false;
        }

        // createReaderFor returns an owning raw pointer; take ownership at once.
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
        {
            error = "Can't read " + path + " (only WAV and FLAC are supported)";
            return false;
        }

        if (std::abs (reader->sampleRate - expectedRate) > 0.5)
        {
            error = path + " is " + juce::String (reader->sampleRate) + " Hz, but the kit is " + juce::String (expectedRate) + " Hz";
            return false;
        }

        if (reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max())
        {
            error = path + " is empty or too long";
            return false;
        }

        header.numChannels = (int) reader->numChannels;
        header.length = reader->lengthInSamples;
        header.bitsPerSample = reader->usesFloatingPointData ? 32 : (int) reader->bitsPerSample;
        return true;
    }

    bool decodeFile (juce::AudioFormatManager& formats, const LoadPlan& plan, const LoadPlan::File& f, MicMask mask,
                     std::array<std::unique_ptr<MicStore>, maxMics>& stores, juce::String& error)
    {
        const auto& layout = *plan.layout;
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (plan.folders[(size_t) f.source].getChildFile (f.path)));

        if (reader == nullptr || (int) reader->numChannels != f.numChannels)
        {
            error = "Can't read " + f.path + " (has it changed since the kit was loaded?)";
            return false;
        }

        // Read only the file channels that a wanted mic uses: the reader skips
        // channels whose destination pointer is null.
        constexpr int chunk = 16384;
        std::vector<std::vector<int>> buffers ((size_t) f.numChannels);
        std::vector<int*> pointers ((size_t) f.numChannels, nullptr);

        for (const auto& t : f.targets)
        {
            if ((mask & (1u << t.mic)) == 0)
                continue;

            for (const auto ch : t.fileChannels)
            {
                buffers[(size_t) ch].resize (chunk);
                pointers[(size_t) ch] = buffers[(size_t) ch].data();
            }
        }

        // Fixed-point formats arrive as full-scale 32-bit ints, so an
        // arithmetic shift gives the stored bit depth. Floating-point formats
        // arrive as float bit patterns in the same buffers.
        const auto bytes = layout.bytesPerSample;
        const auto shift = 32 - bytes * 8;
        const auto isFloat = reader->usesFloatingPointData;
        const auto floatScale = bytes == 2 ? 32767.0f : 8388607.0f;

        auto convert = [&] (int raw) noexcept
        {
            if (! isFloat)
                return raw >> shift;

            float x;
            std::memcpy (&x, &raw, sizeof (x));
            return (int) std::lrint (juce::jlimit (-1.0f, 1.0f, x) * floatScale);
        };

        // Targets may skip the start of the file (a "start" offset), so
        // reading begins at the earliest frame any wanted target uses.
        const std::int64_t length = reader->lengthInSamples;
        auto firstFrame = length;

        for (const auto& t : f.targets)
            if ((mask & (1u << t.mic)) != 0)
                firstFrame = std::min (firstFrame, t.start);

        for (std::int64_t pos = firstFrame; pos < length; pos += chunk)
        {
            const auto n = (int) std::min<std::int64_t> (chunk, length - pos);

            if (! reader->read (pointers.data(), f.numChannels, pos, n, false))
            {
                error = "Error decoding " + f.path;
                return false;
            }

            for (const auto& t : f.targets)
            {
                if ((mask & (1u << t.mic)) == 0 || pos + n <= t.start)
                    continue;

                const auto skip = (int) std::max<std::int64_t> (0, t.start - pos);   // frames of this chunk before the start
                const auto channels = layout.samples[t.sample].numChannels;
                auto* dest = layout.getWritePointer (*stores[(size_t) t.mic], t.sample)
                           + (size_t) (pos + skip - t.start) * (size_t) channels * (size_t) bytes;

                for (int i = skip; i < n; ++i)
                {
                    for (int ch = 0; ch < channels; ++ch, dest += bytes)
                        SampleLayout::pack (dest, bytes, convert (buffers[(size_t) t.fileChannels[(size_t) ch]][(size_t) i]));
                }
            }
        }

        return true;
    }

    void setProgress (const KitLoader::Options& options, float value)
    {
        if (options.progress != nullptr)
            options.progress->store (value, std::memory_order_relaxed);
    }

    void setPhase (const KitLoader::Options& options, LoadPhase phase)
    {
        if (options.phase != nullptr)
            options.phase->store (phase, std::memory_order_relaxed);
    }

    /** Where each step of a ladder plays alone. The kit's own ranges
        (positionRange) where it gives them; otherwise the step plays alone up
        to (1 - crossfade) / 2 of the way to its neighbours, so only the
        middle of each gap crossfades. Mostly playing single real
        recordings keeps the sound natural; the crossfade means a slowly
        moving pedal or a roll across the head never jumps. */
    void setRanges (Ladder& ladder, const std::vector<KitArticulationInfo>& articulations, int firstArticulation, bool positions)
    {
        constexpr float alone = (1.0f - Ladder::crossfadeWidth) * 0.5f;
        const auto n = ladder.numSteps;

        for (int i = 0; i < n; ++i)
        {
            const auto v = ladder.value[(size_t) i];
            ladder.low[(size_t) i] = i == 0 ? 0.0f : v - alone * (v - ladder.value[(size_t) i - 1]);
            ladder.high[(size_t) i] = i == n - 1 ? 1.0f : v + alone * (ladder.value[(size_t) i + 1] - v);

            const auto local = ladder.articulation[(size_t) i] - firstArticulation;

            if (positions && juce::isPositiveAndBelow (local, (int) articulations.size()))
            {
                if (const auto& t = articulations[(size_t) local].traits; t.positionLow >= 0.0f)
                {
                    ladder.low[(size_t) i] = t.positionLow;
                    ladder.high[(size_t) i] = t.positionHigh;
                }
            }
        }
    }

    /** Fills a piece's ladders from its articulations' traits (KitArticulationInfo::traits).

        Each ladder is sorted by value, with one articulation per value (the
        first in kit order). The shank (edge) ladder borrows tip steps at the
        openness levels the kit has no shank recording of, so an edge hit with
        the pedal half up still sounds half open. */
    void buildLadders (PieceData& pd, KitPieceInfo& info)
    {
        auto build = [&] (Ladder& ladder, auto&& valueOf, bool positions)
        {
            std::vector<std::pair<float, int>> steps;

            for (size_t a = 0; a < info.articulations.size(); ++a)
            {
                const auto v = valueOf (info.articulations[a].traits);

                if (v >= 0.0f && std::none_of (steps.begin(), steps.end(), [v] (const auto& s) { return std::abs (s.first - v) < 0.001f; }))
                    steps.emplace_back (v, pd.firstArticulation + (int) a);
            }

            std::stable_sort (steps.begin(), steps.end(), [] (const auto& x, const auto& y) { return x.first < y.first; });
            ladder.numSteps = std::min ((int) steps.size(), Ladder::maxSteps);

            for (int i = 0; i < ladder.numSteps; ++i)
            {
                ladder.value[(size_t) i] = steps[(size_t) i].first;
                ladder.articulation[(size_t) i] = steps[(size_t) i].second;
            }

            setRanges (ladder, info.articulations, pd.firstArticulation, positions);
        };

        auto opennessOf = [] (HatStroke stroke)
        {
            return [stroke] (const ArticulationTraits& t) { return t.stroke == stroke ? t.openness : -1.0f; };
        };

        build (pd.hatLadders[0], opennessOf (HatStroke::tip), false);
        build (pd.hatLadders[1], opennessOf (HatStroke::shank), false);

        if (pd.hatLadders[1].numSteps > 0)
        {
            auto& edge = pd.hatLadders[1];
            const auto& tip = pd.hatLadders[0];

            for (int i = 0; i < tip.numSteps && edge.numSteps < Ladder::maxSteps; ++i)
            {
                const auto v = tip.value[(size_t) i];

                if (std::none_of (edge.value.begin(), edge.value.begin() + edge.numSteps, [v] (float e) { return std::abs (e - v) < 0.001f; }))
                {
                    // Insert in order.
                    auto j = edge.numSteps++;

                    for (; j > 0 && edge.value[(size_t) j - 1] > v; --j)
                    {
                        edge.value[(size_t) j] = edge.value[(size_t) j - 1];
                        edge.articulation[(size_t) j] = edge.articulation[(size_t) j - 1];
                    }

                    edge.value[(size_t) j] = v;
                    edge.articulation[(size_t) j] = tip.articulation[(size_t) i];
                }
            }

            setRanges (edge, info.articulations, pd.firstArticulation, false);
        }
        else
        {
            pd.hatLadders[1] = pd.hatLadders[0];   // no edge recordings: edge hits use the bow's steps
        }

        build (pd.positionLadder, [] (const ArticulationTraits& t) { return t.position; }, true);

        info.hatSteps = { pd.hatLadders[0].numSteps, pd.hatLadders[1].numSteps };
        info.positionSteps = pd.positionLadder.numSteps;
    }

    /** Reads one packed little-endian 16- or 24-bit value, sign-extended. */
    int unpackInt (const std::byte* p, int bytes) noexcept
    {
        const auto* b = reinterpret_cast<const unsigned char*> (p);

        if (bytes == 2)
            return (std::int16_t) (std::uint16_t) (b[0] | (b[1] << 8));

        return (std::int32_t) ((std::uint32_t) b[0] << 8 | (std::uint32_t) b[1] << 16 | (std::uint32_t) b[2] << 24) >> 8;
    }

    /** Fills the preloads of one file's samples from the stream cache: no
        decoding, just reads, so it's much quicker than decodeFile(). */
    bool fillFromCache (const LoadPlan& plan, const LoadPlan::File& f, MicMask mask,
                        std::array<std::unique_ptr<MicStore>, maxMics>& stores, juce::String& error)
    {
        const auto& layout = *plan.layout;
        const auto& dataFile = plan.stream->dataFiles[(size_t) f.source];
        juce::FileInputStream in (dataFile);

        if (! in.openedOk())
        {
            error = "Can't open the stream cache " + dataFile.getFullPathName();
            return false;
        }

        const auto outBytes = layout.bytesPerSample;
        std::vector<std::byte> buffer;

        for (const auto& t : f.targets)
        {
            if ((mask & (1u << t.mic)) == 0)
                continue;

            const auto& s = layout.samples[t.sample];
            const auto& region = plan.stream->regions[t.sample];
            const auto inBytes = region.bytesPerSample;
            const auto n = (size_t) s.storedFrames;
            auto* dest = layout.getWritePointer (*stores[(size_t) t.mic], t.sample);
            buffer.resize (n * (size_t) inBytes);

            for (int ch = 0; ch < s.numChannels; ++ch)
            {
                if (! in.setPosition (region.offset[(size_t) ch]) || in.read (buffer.data(), buffer.size()) != (int) buffer.size())
                {
                    error = "The stream cache for " + f.path + " is damaged; clear the cache in Settings";
                    return false;
                }

                // A 16-bit file in a kit that also has deeper ones is held as 24-bit.
                const auto shift = (outBytes - inBytes) * 8;

                for (size_t i = 0; i < n; ++i)
                    SampleLayout::pack (dest + (i * (size_t) s.numChannels + (size_t) ch) * (size_t) outBytes, outBytes,
                                        unpackInt (buffer.data() + i * (size_t) inBytes, inBytes) * (1 << shift));
            }
        }

        return true;
    }
} // namespace

//==============================================================================
std::size_t LoadPlan::bytesFor (MicMask mask) const noexcept
{
    std::size_t bytes = 0;

    for (int m = 0; m < numMics; ++m)
        if ((mask & (1u << m)) != 0)
            bytes += layout->storeSizes[(size_t) m];

    return bytes + streamBufferBytes;
}

bool KitLoader::decode (const LoadPlan& plan, MicMask mask, std::array<std::unique_ptr<MicStore>, maxMics>& stores,
                        juce::String& error, const Options& options)
{
    mask &= validMics (plan.numMics);

    for (int m = 0; m < plan.numMics; ++m)
        if ((mask & (1u << m)) != 0)
            stores[(size_t) m] = plan.layout->createStore (m);

    std::vector<const LoadPlan::File*> jobs;

    for (const auto& f : plan.files)
        if (std::any_of (f.targets.begin(), f.targets.end(), [mask] (const auto& t) { return (mask & (1u << t.mic)) != 0; }))
            jobs.push_back (&f);

    std::atomic<size_t> done { 0 };

    return runParallel (jobs.size(), options.shouldAbort, error,
                        [&] (size_t i, juce::AudioFormatManager& formats, juce::String& jobError)
                        {
                            const auto ok = plan.stream != nullptr ? fillFromCache (plan, *jobs[i], mask, stores, jobError)
                                                                   : decodeFile (formats, plan, *jobs[i], mask, stores, jobError);
                            if (! ok)
                                return false;

                            const auto from = options.progressFrom;
                            setProgress (options, from + (1.0f - from) * (float) ++done / (float) jobs.size());
                            return true;
                        });
}

//==============================================================================
std::unique_ptr<Kit> KitLoader::loadKit (const juce::File& folder, const MidiMap& midiMap, KitInfo& info,
                                         juce::String& error, const Options& options,
                                         std::shared_ptr<const LoadPlan>* planOut)
{
    const auto startTime = juce::Time::getMillisecondCounterHiRes();
    setProgress (options, 0.0f);
    setPhase (options, LoadPhase::reading);

    KitManifest manifest;

    if (auto r = KitManifest::parse (folder, manifest); r.failed())
    {
        error = r.getErrorMessage();
        return nullptr;
    }

    // Pieces played with other kits' recordings: from here on they're just
    // pieces whose audio is in another folder.
    swaps::apply (manifest, options.swaps);

    auto kit = std::make_unique<Kit>();
    kit->name = manifest.credits.name;
    kit->folder = folder;
    kit->sampleRate = manifest.sampleRate;
    kit->numMics = (int) manifest.mics.size();

    info = {};
    info.credits = manifest.credits;
    info.id = manifest.id;
    info.folder = folder;
    info.isPreset = manifest.isPreset;
    info.warnings = manifest.warnings;
    info.slotOwners = manifest.slotOwners;

    for (const auto& [i, m] : juce::enumerate (manifest.mics))
    {
        auto& mic = kit->mics[(size_t) i];
        mic.numChannels = m.channels;
        mic.role = m.role;
        mic.busChannel = kit->numBusChannels;
        mic.defaultGain = juce::Decibels::decibelsToGain (m.volumeDb, -200.0f) * (m.invertPolarity ? -1.0f : 1.0f);
        mic.defaultPan = m.pan;
        mic.startGain = juce::Decibels::decibelsToGain (m.startVolumeDb, -200.0f);
        mic.startPan = m.startPan;
        kit->numBusChannels += m.channels;

        info.mics.push_back ({ m.id, m.name, m.channels, m.role, m.volumeDb, m.pan, m.startVolumeDb, m.startPan });
    }

    const auto hasCloseMics = std::any_of (manifest.mics.begin(), manifest.mics.end(),
                                           [] (const auto& m) { return m.role == MicRole::close; });

    for (int i = 0; i < kit->numMics; ++i)
    {
        auto& mic = kit->mics[(size_t) i];
        mic.direct = mic.role == MicRole::close || ! hasCloseMics;
        info.mics[(size_t) i].direct = mic.direct;
    }

    // Flatten pieces, articulations and zones into the kit's index-based
    // tables, and note where every hit's audio comes from.
    struct PendingSource
    {
        int hit;
        MicSource source;
    };

    std::vector<PendingSource> sources;
    ChokeGroups chokeGroups;
    int numHits = 0;

    for (const auto& [pi, pm] : juce::enumerate (manifest.pieces))
    {
        PieceData pd;
        pd.role = pm.role;
        pd.firstArticulation = (int) kit->articulations.size();
        pd.numArticulations = (int) pm.articulations.size();
        pd.defaultChokeNote = pm.chokeNote;
        // A piece swapped in from a kit preset keeps that preset's changes
        // built in: a swap leaves the slot's controls as they were, so they
        // can't take them.
        const auto builtIn = pm.swap.has_value() ? pm.start : PieceManifest::Start {};
        const auto start = pm.swap.has_value() ? PieceManifest::Start {} : pm.start;
        pd.defaultGain = juce::Decibels::decibelsToGain (pm.volumeDb + builtIn.volumeDb, -200.0f);
        pd.defaultTune = pm.tuneSemitones + builtIn.tuneSemitones;
        pd.defaultDecay = pm.decay * builtIn.decay;
        pd.startGain = juce::Decibels::decibelsToGain (start.volumeDb, -200.0f);
        pd.startTune = start.tuneSemitones;
        pd.startDecay = start.decay;
        pd.sampleRate = pm.swap.has_value() ? pm.swap->sampleRate : manifest.sampleRate;
        pd.micGain.fill (1.0f);

        KitPieceInfo pieceInfo;
        pieceInfo.id = pm.id;
        pieceInfo.name = pm.name;
        pieceInfo.role = pm.role;
        pieceInfo.defaultChokeNote = pm.chokeNote;
        pieceInfo.startVolumeDb = start.volumeDb;
        pieceInfo.startTune = start.tuneSemitones;
        pieceInfo.startDecay = start.decay;
        pieceInfo.firstArticulation = pd.firstArticulation;
        pieceInfo.slot = pm.slot;

        pieceInfo.recordedIn = pm.swap.has_value() ? pm.swap->kitId : manifest.isPreset ? manifest.baseId : manifest.id;

        if (pm.swap.has_value())
        {
            pd.micGain = pm.swap->micGain;
            pieceInfo.swappedFrom = pm.swap->kitName;
            pieceInfo.swappedPiece = pm.swap->pieceName;

            if (! pm.swap->dropped.isEmpty())
                pieceInfo.swapNotes.add ("Not used: " + pm.swap->dropped.joinIntoString (", ") + " (" + kit->name
                                         + " has no mic to match)");
        }

        if (pm.swapProblem.isNotEmpty())
            pieceInfo.swapNotes.add (pm.swapProblem);

        for (const auto& am : pm.articulations)
        {
            auto& artInfo = pieceInfo.articulations.emplace_back();
            artInfo.id = am.id;
            artInfo.name = am.name;

            // Openness and strike position: the kit's own values, else what the name means.
            const auto idString = am.id.toStdString();
            auto& traits = artInfo.traits;
            traits = articulations::defaultTraits (pm.role, idString);

            if (am.openness.has_value())
            {
                traits.openness = *am.openness;
                traits.stroke = am.stroke.value_or (traits.stroke == HatStroke::none ? HatStroke::tip : traits.stroke);
            }
            else if (am.stroke.has_value() && traits.openness >= 0.0f)
            {
                traits.stroke = *am.stroke;
            }

            if (am.position.has_value())
                traits.position = *am.position;

            if (am.positionRange.has_value())
                std::tie (traits.positionLow, traits.positionHigh) = *am.positionRange;

            // The hits a position controller still changes when the kit has
            // no positional recordings: a drum's centre hit, a ride's bow.
            const auto standard = articulations::standardName (pm.role, idString);
            traits.emulatesPosition = (pm.role == PieceRole::ride && standard == "bow")
                                      || ((pm.role == PieceRole::snare || pm.role == PieceRole::tom || pm.role == PieceRole::floorTom)
                                          && (standard == "center" || standard == "hit"));

            if (pm.role != PieceRole::hihat)
                traits.openness = -1.0f;

            Articulation a {};
            a.piece = (int) pi;
            a.firstZone = (int) kit->zones.size();
            a.numZones = (int) am.zones.size();
            a.chokeGroupMask = chokeGroups.maskFor (am.chokeGroup, info.warnings);
            a.chokesMask = 0;
            a.openness = traits.openness;
            a.stroke = traits.openness >= 0.0f ? traits.stroke : HatStroke::none;

            for (const auto& g : am.chokes)
                a.chokesMask |= chokeGroups.maskFor (g, info.warnings);

            // A cymbal keeps ringing when it's hit again, on the bow, the
            // bell or the edge: the new hit adds to the old one. Some kits
            // (SFZ kits with "group=N off_by=N" on every cymbal region) make
            // each hit cut off the last, which sounds wrong as soon as you
            // play the bell over a ringing bow. So on cymbals only a grab can
            // silence the cymbal itself.
            //
            // The same kits make every hi-hat hit cut the last one, so
            // repeated half-open hits chop each other off. Real hats only
            // stop ringing when they close: a hi-hat hit only chokes its own
            // hits that rang more open than it leaves the hats, so a closed
            // hit or the pedal stops an open ring, and open hits ring on.
            //
            // Either way, chokes on other pieces apply as the kit says.
            a.hatLevel = articulations::hatChokeLevel (idString, traits.openness);
            a.selfChoke = pm.role == PieceRole::hihat                ? SelfChoke::moreOpen
                        : ! lithium::info (pm.role).isCymbal         ? SelfChoke::all
                        : articulations::isGrab (idString)           ? SelfChoke::all
                                                                     : SelfChoke::none;

            for (const auto& zm : am.zones)
            {
                Zone z {};
                z.velocityLow = zm.velocityLow;
                z.velocityHigh = zm.velocityHigh;
                z.gain = juce::Decibels::decibelsToGain (zm.gainDb, -200.0f);
                z.velocityRangeDb = zm.velocityRangeDb;
                z.firstHit = numHits;
                z.numHits = (int) zm.roundRobins.size();
                artInfo.zones.push_back ({ z.velocityLow, z.velocityHigh, z.numHits });

                for (const auto& hit : zm.roundRobins)
                {
                    for (const auto& s : hit.sources)
                        sources.push_back ({ numHits, s });

                    ++numHits;
                }

                kit->zones.push_back (z);
            }

            kit->articulations.push_back (a);
        }

        // The hits the hi-hat pedal plays by itself, and the ride's edge, by
        // their standard names. Only the kit's own ids count here (no
        // fallback): a kit without a foot splash plays none, rather than a
        // closed hit standing in for it.
        auto find = [&] (PieceRole role, std::string_view standard) -> int
        {
            if (pm.role != role)
                return -1;

            for (const auto match : articulations::matchesFor (role, standard))
            {
                if (match.empty())
                    break;

                // The aliases' last resorts (a closed hit for "pedal", the bow
                // for "edge", the chick for "splash") don't count.
                if (match == "closed" || match == "bow" || (standard == "splash" && match == "pedal"))
                    continue;

                for (size_t a = 0; a < pieceInfo.articulations.size(); ++a)
                    if (pieceInfo.articulations[a].id == juce::String (match.data(), match.size()))
                        return pd.firstArticulation + (int) a;
            }

            return -1;
        };

        pd.pedalArticulation = find (PieceRole::hihat, "pedal");
        pd.splashArticulation = find (PieceRole::hihat, "splash");
        pd.edgeArticulation = find (PieceRole::ride, "edge");

        buildLadders (pd, pieceInfo);
        kit->pieces.push_back (pd);
        info.pieces.push_back (std::move (pieceInfo));
    }

    // Choke groups that nothing chokes, or that nobody belongs to, are probably typos.
    for (const auto& [i, name] : juce::enumerate (chokeGroups.names))
    {
        const auto bit = 1u << i;
        bool member = false, choker = false;

        for (const auto& a : kit->articulations)
        {
            member = member || (a.chokeGroupMask & bit) != 0;
            choker = choker || (a.chokesMask & bit) != 0;
        }

        if (member != choker)
            info.warnings.add ("Choke group \"" + name + "\" is " + (member ? "never choked" : "empty"));
    }

    // Every distinct audio file, once. A multichannel file usually feeds
    // every mic of one hit.
    // A preset's audio lives in its base kit's folder, and a swapped-in
    // piece's in its own kit's.
    auto plan = std::make_shared<LoadPlan>();
    plan->numMics = kit->numMics;

    for (int i = 0; i < manifest.numSources(); ++i)
        plan->folders.push_back (manifest.folderOf (i));

    std::map<std::pair<int, juce::String>, size_t> fileIndex;
    std::vector<size_t> sourceFile;

    for (const auto& s : sources)
    {
        const auto [it, added] = fileIndex.try_emplace ({ s.source.source, s.source.file }, plan->files.size());

        if (added)
            plan->files.push_back ({ s.source.file, s.source.source, 0, {} });

        sourceFile.push_back (it->second);
    }

    // Pass 1: every file's header (in parallel; large kits have thousands).
    // Files already in the stream cache don't need opening: their entries
    // say everything the header would.
    // One cache pack per audio folder.
    const auto& streaming = options.streaming;
    const auto mayStream = streaming.mode != StreamingMode::never && streaming.cacheFolder != juce::File();
    std::vector<streamcache::Pack> packs;

    for (const auto& f : plan->folders)
        packs.push_back (mayStream ? streamcache::Pack::open (streaming.cacheFolder, f) : streamcache::Pack());

    std::vector<FileHeader> headers (plan->files.size());
    std::vector<size_t> uncached;

    for (size_t i = 0; i < plan->files.size(); ++i)
    {
        const auto& f = plan->files[i];

        if (const auto* e = mayStream ? packs[(size_t) f.source].find (f.path) : nullptr)
            headers[i] = { e->numChannels, e->numFrames, e->sourceBits };
        else
            uncached.push_back (i);
    }

    if (! runParallel (uncached.size(), options.shouldAbort, error,
                       [&] (size_t j, juce::AudioFormatManager& formats, juce::String& jobError)
                       {
                           const auto i = uncached[j];
                           const auto& f = plan->files[i];
                           return readHeader (formats, plan->folders[(size_t) f.source].getChildFile (f.path), f.path,
                                              manifest.sampleRateOf (f.source), headers[i], jobError);
                       }))
        return nullptr;

    setProgress (options, 0.1f);

    // Keep the source's precision: 16-bit kits stay 16-bit, anything deeper is held as 24-bit.
    int bits = 16;

    for (const auto& h : headers)
        bits = std::max (bits, h.bitsPerSample);

    // What the whole kit takes in RAM decides whether it streams.
    const auto bytesPerSample = (size_t) (bits <= 16 ? 2 : 3);
    std::size_t fullBytes = 0;

    for (const auto& [i, s] : juce::enumerate (sources))
        fullBytes += (size_t) std::max<std::int64_t> (0, headers[sourceFile[(size_t) i]].length - s.source.start)
                     * (size_t) manifest.mics[(size_t) s.source.mic].channels * bytesPerSample;

    auto stream = mayStream && (streaming.mode == StreamingMode::always || fullBytes > streaming.autoThresholdBytes);

    if (stream && ! uncached.empty())
    {
        // Check there's room for the cache before decoding gigabytes into it.
        std::int64_t needed = 0;

        for (const auto i : uncached)
            needed += headers[i].length * headers[i].numChannels * (headers[i].bitsPerSample <= 16 ? 2 : 3);

        streaming.cacheFolder.createDirectory();

        if (streaming.cacheFolder.getBytesFreeOnVolume() < needed + ((std::int64_t) 256 << 20))
        {
            info.warnings.add ("Not enough free disk space for the stream cache (needs "
                               + juce::File::descriptionOfSizeInBytes (needed) + "), so the kit is held in RAM");
            stream = false;
        }
    }

    // Streaming: decode whatever isn't cached yet into the cache. From then
    // on, the source files aren't read again.
    if (stream && ! uncached.empty())
    {
        setPhase (options, LoadPhase::caching);

        for (int source = 0; source < manifest.numSources(); ++source)
        {
            juce::StringArray paths;

            for (const auto i : uncached)
                if (plan->files[i].source == source)
                    paths.add (plan->files[i].path);

            if (paths.isEmpty())
                continue;

            // Progress runs across the sources in turn.
            const auto from = 0.1f + 0.75f * (float) source / (float) manifest.numSources();
            const auto span = 0.75f / (float) manifest.numSources();

            if (! streamcache::cacheFiles (streaming.cacheFolder, plan->folders[(size_t) source], paths,
                                           manifest.sampleRateOf (source), packs[(size_t) source], error, options.shouldAbort,
                                           [&] (float p) { setProgress (options, from + span * p); }))
                return nullptr;
        }
    }

    setPhase (options, LoadPhase::loading);

    auto layout = std::make_shared<SampleLayout> (bits);
    const auto maxStored = stream ? (std::int64_t) std::max (StreamingSampleSource::chunkFrames, streaming.preloadFrames)
                                  : std::numeric_limits<std::int64_t>::max();
    std::vector<StreamRegion> regions;
    kit->hitSamples.assign ((size_t) numHits * (size_t) kit->numMics, invalidSampleId);

    for (const auto& [i, s] : juce::enumerate (sources))
    {
        auto& file = plan->files[sourceFile[(size_t) i]];
        const auto& header = headers[sourceFile[(size_t) i]];
        const auto micChannels = manifest.mics[(size_t) s.source.mic].channels;
        auto channels = s.source.fileChannels;

        file.numChannels = header.numChannels;

        if (micChannels == 2 && header.numChannels == 1)
            channels = { 1, 1 };     // a mono file on a stereo mic plays centred
        else if (micChannels == 2 && channels[1] < 1)
            channels[1] = channels[0];   // so does a mono source on a stereo mic

        // Checked here, on the loader thread: a channel outside the file
        // would have the decoding threads read and write outside their
        // buffers, which takes the whole host down.
        for (int ch = 0; ch < micChannels; ++ch)
        {
            if (channels[(size_t) ch] < 1 || channels[(size_t) ch] > header.numChannels)
            {
                error = file.path + " has " + juce::String (header.numChannels) + " channels, but the kit reads channel "
                        + juce::String (channels[(size_t) ch]) + " of it for mic \"" + manifest.mics[(size_t) s.source.mic].id + "\"";
                return nullptr;
            }
        }

        if (s.source.start >= header.length)
        {
            error = file.path + " is shorter than its \"start\" offset of " + juce::String (s.source.start) + " frames";
            return nullptr;
        }

        const auto id = layout->add (s.source.mic, micChannels, header.length - s.source.start, maxStored);
        auto& slot = kit->hitSamples[(size_t) (s.hit * kit->numMics + s.source.mic)];

        if (slot != invalidSampleId)
            info.warnings.addIfNotAlreadyThere ("Some hits list the same mic twice; the last one is used");

        slot = id;
        const std::array<int, 2> fileChannels { channels[0] - 1, (micChannels == 2 ? channels[1] : channels[0]) - 1 };
        file.targets.push_back ({ s.source.mic, id, fileChannels, s.source.start });

        if (stream)
        {
            const auto* e = packs[(size_t) file.source].find (file.path);

            if (e == nullptr)
            {
                error = "The stream cache has no entry for " + file.path;
                return nullptr;
            }

            StreamRegion r;
            r.bytesPerSample = e->bytesPerSample;
            r.pack = file.source;

            for (int ch = 0; ch < micChannels; ++ch)
                r.offset[(size_t) ch] = e->offsetOf (fileChannels[(size_t) ch], s.source.start);

            regions.push_back (r);
        }
    }

    plan->layout = layout;

    std::array<int, maxMics> micChannels {};

    for (int m = 0; m < kit->numMics; ++m)
        micChannels[(size_t) m] = kit->mics[(size_t) m].numChannels;

    if (stream)
    {
        auto map = std::make_shared<StreamMap>();

        for (const auto& p : packs)
            map->dataFiles.push_back (p.getDataFile());

        map->regions = std::move (regions);
        plan->stream = std::move (map);
        plan->streamBufferBytes = StreamingSampleSource::bufferBytesFor (micChannels, kit->numMics, DrumEngine::numVoices);
    }

    // Pass 2: the audio of every mic that should be in RAM: decoded from the
    // source files, or for a streaming kit, its preloads read from the cache.
    std::array<std::unique_ptr<MicStore>, maxMics> stores;
    auto decodeOptions = options;
    decodeOptions.progressFrom = stream ? 0.85f : 0.1f;

    if (! decode (*plan, options.mics, stores, error, decodeOptions))
        return nullptr;

    std::unique_ptr<InMemorySampleSource> source;

    if (stream)
    {
        auto streamer = std::make_unique<StreamingSampleSource> (layout, plan->stream, micChannels, kit->numMics, DrumEngine::numVoices,
                                                                 options.streamStats != nullptr ? options.streamStats
                                                                                                : std::make_shared<StreamStats>());
        if (! streamer->isValid())
        {
            error = "Can't open the stream cache " + plan->stream->dataFiles.front().getFullPathName();
            return nullptr;
        }

        source = std::move (streamer);
    }
    else
    {
        source = std::make_unique<InMemorySampleSource> (layout);
    }

    for (int m = 0; m < kit->numMics; ++m)
        source->attachStore (m, std::move (stores[(size_t) m]));

    info.streaming = stream;
    info.preloadFrames = stream ? (int) maxStored : 0;
    info.fullMemoryBytes = fullBytes;
    info.streamBufferBytes = plan->streamBufferBytes;
    info.streamCache = stream ? packs.front().getFolder() : juce::File();

    // MIDI: the map the load was requested with (Lithium Standard, the kit's
    // own map, General MIDI or an e-kit map), resolved against this kit.
    info.kitMidiMap = manifest.midiMap;

    // Cymbals without a choke note of their own get a fixed one by role, the
    // same on every kit: crashes 1-3 on 85-87, rides 1-2 on 88-89, chinas
    // 1-2 on 90-91 and the splash on 92. Any more get the next free note up
    // to 95. (A note that plays something never chokes; see DrumEngine.)
    std::array<bool, 128> chokeTaken {};
    std::array<int, numPieceRoles> cymbalsSeen {};
    int splashesSeen = 0;

    for (const auto& pd : kit->pieces)
        if (juce::isPositiveAndBelow (pd.defaultChokeNote, 128))
            chokeTaken[(size_t) pd.defaultChokeNote] = true;

    for (size_t p = 0; p < kit->pieces.size(); ++p)
    {
        auto& pd = kit->pieces[p];

        if (pd.defaultChokeNote >= 0 || ! lithium::info (pd.role).isCymbal)   // (the parameter named info shadows the function)
            continue;

        auto note = -1;

        if (isSplash (info.pieces[p]))
        {
            note = splashesSeen++ == 0 ? 92 : -1;
        }
        else
        {
            const auto n = cymbalsSeen[(size_t) pd.role]++;

            if (pd.role == PieceRole::crash && n < 3)       note = 85 + n;
            else if (pd.role == PieceRole::ride && n < 2)   note = 88 + n;
            else if (pd.role == PieceRole::china && n < 2)  note = 90 + n;
        }

        if (note < 0 || chokeTaken[(size_t) note])
        {
            note = -1;

            for (int n = 93; n <= 95 && note < 0; ++n)
                if (! chokeTaken[(size_t) n])
                    note = n;
        }

        if (note >= 0)
        {
            pd.defaultChokeNote = note;
            chokeTaken[(size_t) note] = true;
        }
    }

    for (size_t p = 0; p < kit->pieces.size(); ++p)
        info.pieces[p].defaultChokeNote = kit->pieces[p].defaultChokeNote;

    // Resolved after the choke notes are known, so spare notes keep clear of them.
    kit->notes = resolveMidiMap (midiMap, info);

    kit->initialiseRoundRobins();

    // Which drums have anything on the mics that feed the drum channels.
    for (size_t p = 0; p < kit->pieces.size(); ++p)
    {
        const auto& pd = kit->pieces[p];
        bool found = false;

        for (int a = pd.firstArticulation; a < pd.firstArticulation + pd.numArticulations && ! found; ++a)
        {
            const auto& art = kit->articulations[(size_t) a];

            for (int z = art.firstZone; z < art.firstZone + art.numZones && ! found; ++z)
                for (int m = 0; m < kit->numMics && ! found; ++m)
                    found = kit->mics[(size_t) m].direct && kit->getSample (kit->zones[(size_t) z].firstHit, m) != invalidSampleId;
        }

        info.pieces[p].hasDirectSignal = found;
    }

    std::vector<PieceRole> roles;
    std::vector<bool> splash;

    for (size_t p = 0; p < kit->pieces.size(); ++p)
    {
        roles.push_back (kit->pieces[p].role);
        splash.push_back (isSplash (info.pieces[p]));
    }

    info.slotPiece = assignStageSlots (roles, splash);
    info.bitsPerSample = layout->bytesPerSample * 8;
    info.sampleRate = manifest.sampleRate;
    info.memoryBytes = plan->bytesFor (allMics);
    info.numSampleFiles = (int) plan->files.size();
    info.loadSeconds = (juce::Time::getMillisecondCounterHiRes() - startTime) / 1000.0;

    kit->samples = std::move (source);

    if (planOut != nullptr)
        *planOut = std::move (plan);

    setProgress (options, 1.0f);
    return kit;
}

//==============================================================================
KitLoader::KitLoader (KitHandoff& h)
    : juce::Thread ("Lithium kit loader"), handoff (h)
{
    startThread (juce::Thread::Priority::low);
}

KitLoader::~KitLoader()
{
    signalThreadShouldExit();
    wakeUp.signal();
    stopThread (10000);
}

void KitLoader::requestLoad (Request r)
{
    {
        // Bumping the generation makes any load in progress abort. It has to
        // happen under the same lock as the request, so the loader thread
        // always sees a request together with its matching generation.
        std::scoped_lock lock (mutex);
        request = std::move (r);
        ++generation;
    }

    wakeUp.signal();
}

std::optional<KitLoader::Result> KitLoader::takeResult()
{
    std::scoped_lock lock (mutex);
    return std::exchange (result, std::nullopt);
}

void KitLoader::setWantedMics (MicMask mask)
{
    if (wantedMics.exchange (mask) != mask)
        wakeUp.signal();
}

void KitLoader::run()
{
    while (! threadShouldExit())
    {
        // Wake on a request, or every 100 ms to delete what the audio thread has
        // retired and to catch up with mic mutes once the audio thread has
        // switched to a newly published kit.
        wakeUp.wait (100);
        handoff.collectGarbage();

        std::optional<Request> r;
        std::uint64_t myGeneration = 0;

        {
            std::scoped_lock lock (mutex);
            r = std::exchange (request, std::nullopt);
            myGeneration = generation.load();
        }

        if (r.has_value())
        {
            loadRequested (*r, myGeneration);
            continue;
        }

        while (! threadShouldExit() && generation.load() == myGeneration && updateMics())
            handoff.collectGarbage();
    }
}

void KitLoader::loadRequested (const Request& r, std::uint64_t myGeneration)
{
    // Shared by every plugin instance in the process, hence atomic.
    static std::atomic<std::uint64_t> nextSerial { 1 };

    loading = true;

    auto info = std::make_shared<KitInfo>();
    juce::String error;
    std::shared_ptr<const LoadPlan> plan;

    Options options;
    options.mics = r.mics;
    options.progress = &progress;
    options.phase = &phase;
    options.streaming = r.streaming;
    options.streamStats = r.streamStats;
    options.swaps = r.swaps;
    options.shouldAbort = [this, myGeneration] { return threadShouldExit() || generation.load() != myGeneration; };

    auto kit = loadKit (r.folder, r.midiMap, *info, error, options, &plan);

    loading = false;

    if (threadShouldExit() || generation.load() != myGeneration)
        return;    // superseded by a newer request; drop this result

    if (kit != nullptr)
    {
        kit->serial = nextSerial++;
        kit->resetToken = r.resetToken;
        info->serial = kit->serial;

        currentPlan = plan;
        currentSerial = kit->serial;
        loadedMics = r.mics & validMics (plan->numMics);
        memoryUsage = plan->bytesFor (loadedMics);

        handoff.publish (std::move (kit));
    }
    else
    {
        info = nullptr;
    }

    {
        std::scoped_lock lock (mutex);
        result = Result { r.folder, std::move (info), error, r.resetToken, r.swaps };
    }

    if (onLoadFinished)
        onLoadFinished();
}

bool KitLoader::updateMics()
{
    // Only once the audio thread plays the kit we published, so the stores
    // we send can't miss it.
    if (currentPlan == nullptr || handoff.getCurrentSerial() != currentSerial)
        return false;

    const auto wanted = wantedMics.load() & validMics (currentPlan->numMics);
    const auto diff = wanted ^ loadedMics;

    if (diff == 0)
        return false;

    const auto mic = std::countr_zero (diff);
    const auto bit = 1u << mic;
    std::unique_ptr<MicStore> store;

    if ((wanted & bit) != 0)
    {
        std::array<std::unique_ptr<MicStore>, maxMics> stores;
        juce::String error;
        const auto myGeneration = generation.load();

        Options options;
        options.shouldAbort = [this, myGeneration, bit]
        {
            // Give up if a kit load comes in, or the mic is muted again.
            return threadShouldExit() || generation.load() != myGeneration || (wantedMics.load() & bit) == 0;
        };

        if (! decode (*currentPlan, bit, stores, error, options))
        {
            // A real failure (rather than an abort) would repeat forever, so
            // treat the mic as loaded; it stays silent until the kit is reloaded.
            if (! options.shouldAbort())
                loadedMics |= bit;

            return false;
        }

        store = std::move (stores[(size_t) mic]);
    }

    if (! handoff.sendMicStore (currentSerial, mic, store))
        return false;    // queue full; try again on the next wake-up

    loadedMics ^= bit;
    memoryUsage = currentPlan->bytesFor (loadedMics);
    return true;
}

} // namespace lithium
