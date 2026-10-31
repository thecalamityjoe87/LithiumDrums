#include "KitLoader.h"

#include "KitManifest.h"

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

    /** Runs job(i, formats, error) for every i in [0, numJobs) on a pool of
        worker threads. Workers pull the next index from an atomic counter, so
        the load balances itself. Each worker has its own AudioFormatManager,
        because format readers aren't shared between threads. Stops at the
        first failure, or when shouldAbort() says so. */
    template <typename Job>
    bool runParallel (size_t numJobs, const std::function<bool()>& shouldAbort, juce::String& error, Job&& job)
    {
        std::atomic<size_t> next { 0 };
        std::atomic<bool> failed { false };
        std::mutex errorMutex;

        const auto numWorkers = (size_t) juce::jlimit (1, 8, (int) std::thread::hardware_concurrency() - 1);

        {
            // std::jthread joins in its destructor, so leaving this scope waits for every worker.
            std::vector<std::jthread> workers;

            for (size_t w = 0; w < std::min (numWorkers, std::max<size_t> (numJobs, 1)); ++w)
            {
                workers.emplace_back ([&]
                {
                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();

                    for (auto i = next++; i < numJobs && ! failed; i = next++)
                    {
                        if (shouldAbort && shouldAbort())
                        {
                            failed = true;
                            break;
                        }

                        juce::String jobError;

                        if (! job (i, formats, jobError))
                        {
                            std::scoped_lock lock (errorMutex);

                            if (error.isEmpty())
                                error = jobError;

                            failed = true;
                        }
                    }
                });
            }
        }

        if (failed && error.isEmpty())
            error = "Loading was cancelled";

        return ! failed;
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
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (plan.folder.getChildFile (f.path)));

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
} // namespace

//==============================================================================
std::size_t LoadPlan::bytesFor (MicMask mask) const noexcept
{
    std::size_t bytes = 0;

    for (int m = 0; m < numMics; ++m)
        if ((mask & (1u << m)) != 0)
            bytes += layout->storeSizes[(size_t) m];

    return bytes;
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
                            if (! decodeFile (formats, plan, *jobs[i], mask, stores, jobError))
                                return false;

                            setProgress (options, 0.1f + 0.9f * (float) ++done / (float) jobs.size());
                            return true;
                        });
}

//==============================================================================
std::unique_ptr<Kit> KitLoader::loadKit (const juce::File& folder, const MidiMap& generalMidi, KitInfo& info,
                                         juce::String& error, const Options& options,
                                         std::shared_ptr<const LoadPlan>* planOut)
{
    const auto startTime = juce::Time::getMillisecondCounterHiRes();
    setProgress (options, 0.0f);

    KitManifest manifest;

    if (auto r = KitManifest::parse (folder, manifest); r.failed())
    {
        error = r.getErrorMessage();
        return nullptr;
    }

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

    for (const auto& [i, m] : juce::enumerate (manifest.mics))
    {
        auto& mic = kit->mics[(size_t) i];
        mic.numChannels = m.channels;
        mic.role = m.role;
        mic.busChannel = kit->numBusChannels;
        mic.defaultGain = juce::Decibels::decibelsToGain (m.volumeDb, -200.0f) * (m.invertPolarity ? -1.0f : 1.0f);
        mic.defaultPan = m.pan;
        kit->numBusChannels += m.channels;

        info.mics.push_back ({ m.id, m.name, m.channels, m.role, m.volumeDb, m.pan });
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
        pd.defaultGain = juce::Decibels::decibelsToGain (pm.volumeDb, -200.0f);
        pd.defaultTune = pm.tuneSemitones;
        pd.defaultDecay = pm.decay;

        KitPieceInfo pieceInfo { pm.id, pm.name, pm.role, {}, -1 };

        for (const auto& am : pm.articulations)
        {
            pieceInfo.articulations.add (am.name);

            Articulation a {};
            a.piece = (int) pi;
            a.firstZone = (int) kit->zones.size();
            a.numZones = (int) am.zones.size();
            a.chokeGroupMask = chokeGroups.maskFor (am.chokeGroup, info.warnings);
            a.chokesMask = 0;

            for (const auto& g : am.chokes)
                a.chokesMask |= chokeGroups.maskFor (g, info.warnings);

            for (const auto& zm : am.zones)
            {
                Zone z {};
                z.velocityLow = zm.velocityLow;
                z.velocityHigh = zm.velocityHigh;
                z.gain = juce::Decibels::decibelsToGain (zm.gainDb, -200.0f);
                z.velocityRangeDb = zm.velocityRangeDb;
                z.firstHit = numHits;
                z.numHits = (int) zm.roundRobins.size();

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
    // A preset's audio lives in its base kit's folder.
    auto plan = std::make_shared<LoadPlan>();
    plan->folder = manifest.audioFolder;
    plan->numMics = kit->numMics;

    std::map<juce::String, size_t> fileIndex;
    std::vector<size_t> sourceFile;

    for (const auto& s : sources)
    {
        const auto [it, added] = fileIndex.try_emplace (s.source.file, plan->files.size());

        if (added)
            plan->files.push_back ({ s.source.file, 0, {} });

        sourceFile.push_back (it->second);
    }

    // Pass 1: read every file's header (in parallel; large kits have thousands).
    std::vector<FileHeader> headers (plan->files.size());

    if (! runParallel (plan->files.size(), options.shouldAbort, error,
                       [&] (size_t i, juce::AudioFormatManager& formats, juce::String& jobError)
                       {
                           const auto& f = plan->files[i];
                           return readHeader (formats, plan->folder.getChildFile (f.path), f.path, manifest.sampleRate, headers[i], jobError);
                       }))
        return nullptr;

    setProgress (options, 0.1f);

    // Keep the source's precision: 16-bit kits stay 16-bit, anything deeper is held as 24-bit.
    int bits = 16;

    for (const auto& h : headers)
        bits = std::max (bits, h.bitsPerSample);

    auto layout = std::make_shared<SampleLayout> (bits);
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

        for (int ch = 0; ch < micChannels; ++ch)
        {
            if (channels[(size_t) ch] > header.numChannels)
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

        const auto id = layout->add (s.source.mic, micChannels, header.length - s.source.start);
        auto& slot = kit->hitSamples[(size_t) (s.hit * kit->numMics + s.source.mic)];

        if (slot != invalidSampleId)
            info.warnings.addIfNotAlreadyThere ("Some hits list the same mic twice; the last one is used");

        slot = id;
        file.targets.push_back ({ s.source.mic, id, { channels[0] - 1, (micChannels == 2 ? channels[1] : channels[0]) - 1 },
                                  s.source.start });
    }

    plan->layout = layout;

    // Pass 2: decode the audio of every mic that should be in RAM.
    std::array<std::unique_ptr<MicStore>, maxMics> stores;

    if (! decode (*plan, options.mics, stores, error, options))
        return nullptr;

    auto source = std::make_unique<InMemorySampleSource> (layout);

    for (int m = 0; m < kit->numMics; ++m)
        source->attachStore (m, std::move (stores[(size_t) m]));

    // MIDI: the kit's own map if it has one, otherwise General MIDI resolved by role.
    std::vector<int> mappedNotes;

    auto articulationFor = [&] (int piece, const juce::String& id)
    {
        const auto& pd = kit->pieces[(size_t) piece];
        const auto& arts = manifest.pieces[(size_t) piece].articulations;

        for (int a = 0; a < pd.numArticulations; ++a)
            if (arts[(size_t) a].id == id)
                return pd.firstArticulation + a;

        return pd.firstArticulation;   // fall back to the piece's first articulation
    };

    if (! manifest.midiMap.empty())
    {
        for (const auto& e : manifest.midiMap)
        {
            kit->noteMap[(size_t) e.note].articulation = articulationFor (manifest.findPiece (e.piece), e.articulation);
            mappedNotes.push_back (e.note);
        }
    }
    else
    {
        for (const auto& e : generalMidi.entries)
        {
            int match = -1, seen = 0;

            // The nth piece of the role, or the last one if the kit has fewer.
            for (size_t p = 0; p < kit->pieces.size() && seen <= e.nth; ++p)
            {
                if (kit->pieces[p].role == e.role)
                {
                    match = (int) p;
                    ++seen;
                }
            }

            if (match >= 0)
            {
                kit->noteMap[(size_t) e.note].articulation = articulationFor (match, e.articulation);
                mappedNotes.push_back (e.note);
            }
        }
    }

    // Cymbals without a choke note of their own get the next free notes from
    // 85 up, above the General MIDI drum range.
    int nextChokeNote = 85;

    for (auto& pd : kit->pieces)
        if (pd.defaultChokeNote >= 0)
            mappedNotes.push_back (pd.defaultChokeNote);

    for (auto& pd : kit->pieces)
    {
        if (pd.defaultChokeNote >= 0 || ! lithium::info (pd.role).isCymbal)   // (the parameter named info shadows the function)
            continue;

        while (nextChokeNote < 128 && std::find (mappedNotes.begin(), mappedNotes.end(), nextChokeNote) != mappedNotes.end())
            ++nextChokeNote;

        if (nextChokeNote < 128)
            pd.defaultChokeNote = nextChokeNote++;
    }

    for (size_t p = 0; p < kit->pieces.size(); ++p)
        info.pieces[p].defaultChokeNote = kit->pieces[p].defaultChokeNote;

    kit->initialiseRoundRobins();

    std::vector<PieceRole> roles;

    for (const auto& p : kit->pieces)
        roles.push_back (p.role);

    info.slotPiece = assignStageSlots (roles);
    info.bitsPerSample = layout->bytesPerSample * 8;
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
KitLoader::KitLoader (KitHandoff& h, MidiMap map)
    : juce::Thread ("Lithium kit loader"), handoff (h), generalMidi (std::move (map))
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
    options.shouldAbort = [this, myGeneration] { return threadShouldExit() || generation.load() != myGeneration; };

    auto kit = loadKit (r.folder, generalMidi, *info, error, options, &plan);

    loading = false;

    if (threadShouldExit() || generation.load() != myGeneration)
        return;    // superseded by a newer request; drop this result

    if (kit != nullptr)
    {
        kit->serial = nextSerial++;
        kit->resetToken = r.resetToken;

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
        result = Result { r.folder, std::move (info), error, r.resetToken };
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
