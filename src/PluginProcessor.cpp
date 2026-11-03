#include "PluginProcessor.h"

#include "MidiMapLibrary.h"
#include "PluginEditor.h"
#include "StandaloneAudioFix.h"
#include "engine/StreamCache.h"
#include "grooves/GrooveMidi.h"

namespace lithium
{

namespace
{
    const juce::Identifier stateKitFolder { "kitFolder" };
    const juce::Identifier stateKitId { "kitId" };
    const juce::Identifier stateSelectedPiece { "selectedPiece" };
    const juce::Identifier stateMidiMap { "midiMap" };
    const juce::Identifier stateMidiMapModified { "midiMapModified" };
    const juce::Identifier statePresetFile { "presetFile" };
    const juce::Identifier stateRoomIrFile { "roomIrFile" };   // on the parameters' own state, so presets carry it
    const juce::Identifier stateFxAB { "fxAB" }, stateFxSlot { "slot" };
    const juce::Identifier grooveTempo { "tempo" }, grooveCustomBpm { "customBpm" }, grooveSpeed { "speed" }, grooveLoop { "loop" };

    constexpr double kitSwapFadeSeconds = 0.01;

    // A mic is unloaded once it has stayed muted this long, so quickly
    // toggling mute (or soloing around) doesn't reload gigabytes each time.
    constexpr double unloadAfterMutedMs = 2000.0;
}

juce::AudioProcessor::BusesProperties LithiumDrumsProcessor::createBuses()
{
    // A stereo main out, plus stereo aux outs for multi-output setups. The
    // aux outs are disabled until the host enables them. Any mic (and the
    // reverb return) can be routed to any aux out with its Output parameter;
    // several can share one, e.g. all the tom mics on "Aux 4".
    auto buses = BusesProperties().withOutput ("Main", juce::AudioChannelSet::stereo(), true);

    for (int i = 0; i < numAuxOuts; ++i)
        buses = buses.withOutput ("Aux " + juce::String (i + 1), juce::AudioChannelSet::stereo(), false);

    return buses;
}

LithiumDrumsProcessor::LithiumDrumsProcessor()
    : AudioProcessor (createBuses()),
      parameters (*this, nullptr, "LithiumDrums", params::createLayout (slotNames)),
      snapshot (parameters),
      loader (handoff),
      midiMap (midimaps::loadDefault (*settings))
{
    slotNames.set (nullptr);

    installStandaloneAudioFix();

    // Runs on the loader thread; AsyncUpdater hops over to the message thread.
    loader.onLoadFinished = [this] { triggerAsyncUpdate(); };

    // A new instance starts with the default kit. If the host then restores a
    // project, its kit request simply replaces this one.
    if (const auto folder = kitlibrary::findDefaultKit (settings->getKitsFolder()); folder != juce::File())
        loadKit (folder, true);

    if (const auto log = juce::SystemStats::getEnvironmentVariable ("LITHIUM_HIT_LOG", {}); log.isNotEmpty())
    {
        hitLogFile = juce::File::getCurrentWorkingDirectory().getChildFile (log).createOutputStream();

        if (hitLogFile != nullptr)
        {
            hitLogFile->setPosition (hitLogFile->getFile().getSize());   // append
            *hitLogFile << "\n" << juce::Time::getCurrentTime().toString (true, true, true, true) << ": Lithium Drums started\n";
            hitLogFile->flush();
            hitLogOn = true;
        }
    }

    updateRoomIr();
    startTimerHz (5);
}

LithiumDrumsProcessor::~LithiumDrumsProcessor()
{
    stopTimer();
}

bool LithiumDrumsProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (! layouts.inputBuses.isEmpty() && layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled())
        return false;

    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    for (int i = 1; i < layouts.outputBuses.size(); ++i)
    {
        const auto& set = layouts.outputBuses.getReference (i);

        if (! set.isDisabled() && set != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

void LithiumDrumsProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    currentSampleRate = sampleRate;
    engine.prepare (sampleRate);

    {
        // The IR loader hands the Room reverb new IRs from its own thread;
        // this keeps it from doing so while the reverb is being prepared.
        std::scoped_lock lock (irLoader.getEngineLock());
        mixer.prepare (sampleRate, std::max (64, maximumExpectedSamplesPerBlock));
    }

    loadMeasurer.reset (sampleRate, maximumExpectedSamplesPerBlock);
    updateLatency();

    // The audio thread is guaranteed not to be running here, so a kit that
    // arrived while playback was stopped can be swapped in straight away.
    swapInWaitingKit();
    handoff.applyMicUpdates();
    applyNoteTables();
}

void LithiumDrumsProcessor::swapInWaitingKit() noexcept
{
    if (handoff.pollIncoming())
    {
        if (auto* current = handoff.getCurrent())
            engine.stopAll (*current);

        handoff.commitIncoming();
        kitSwapPending = false;
    }
}

//==============================================================================
void LithiumDrumsProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const auto numSamples = buffer.getNumSamples();
    juce::AudioProcessLoadMeasurer::ScopedTimer loadTimer (loadMeasurer, numSamples);

    buffer.clear();

    // A new kit replaces the current one only once its voices have faded out,
    // so a kit change never clicks. Notes are ignored for those few ms.
    if (handoff.pollIncoming())
    {
        auto* current = handoff.getCurrent();

        if (! kitSwapPending && current != nullptr)
            engine.fadeOutAll (kitSwapFadeSeconds);

        kitSwapPending = true;

        if (current == nullptr || engine.getNumActiveVoices() == 0)
            swapInWaitingKit();
    }

    handoff.applyMicUpdates();
    applyNoteTables();

    auto* kit = handoff.getCurrent();
    const auto acceptNotes = kit != nullptr && ! kitSwapPending;

    snapshot.read (engineParams);

    // A kit chosen from the menu plays with neutral settings until the
    // message thread confirms it has reset the parameters for it, so the new
    // kit never plays (even briefly) through the previous kit's mix.
    if (kit != nullptr && kit->resetToken > confirmedResetToken.load (std::memory_order_acquire))
        engineParams.resetForKit (*kit);

    // Output pointers. getBusBuffer() only wraps the host's channel pointers
    // (no allocation); disabled buses come back with no channels.
    auto mainBus = getBusBuffer (buffer, false, 0);
    const std::array<float*, 2> mainOut { mainBus.getWritePointer (0), mainBus.getWritePointer (1) };

    Mixer::AuxOutputs auxOuts {};

    for (int i = 0; i < numAuxOuts && i + 1 < getBusCount (false); ++i)
    {
        auto bus = getBusBuffer (buffer, false, i + 1);

        if (bus.getNumChannels() == 2)
            auxOuts[(size_t) i] = { bus.getWritePointer (0), bus.getWritePointer (1) };
    }

    // Clicks on the UI play at the start of the block.
    AuditionRequest request;

    while (auditions.pop (request))
    {
        if (acceptNotes && request.note >= 0)
        {
            logHit (HitLogEntry::Source::key, request.note, request.velocity);
            engine.setTime (engineClock);
            engine.noteOn (*kit, request.note, request.velocity, engineParams);
        }
        else if (acceptNotes && juce::isPositiveAndBelow (request.piece, (int) kit->pieces.size()))
        {
            logHit (HitLogEntry::Source::click, request.piece, request.velocity);
            // The request was made against the kit the UI knew about; if that
            // has changed since, the clamp keeps the index valid.
            const auto& piece = kit->pieces[(size_t) request.piece];
            const auto articulation = juce::jlimit (0, piece.numArticulations - 1, request.articulation);
            engine.trigger (*kit, piece.firstArticulation + articulation, request.velocity, engineParams);
        }
    }

    // The groove preview's hits in this block. getPosition() fills in a
    // struct on the stack; nothing here allocates.
    GroovePlayer::Transport transport;

    if (auto* host = getPlayHead())
    {
        if (const auto info = host->getPosition())
        {
            if (const auto bpm = info->getBpm())
            {
                transport.hostBpm = engineParams.hostBpm = *bpm;
                hostBpm.store (*bpm, std::memory_order_relaxed);
            }

            if (const auto ppq = info->getPpqPosition())
                transport.hostPpq = *ppq;

            transport.hostPlaying = info->getIsPlaying();
        }
    }

    const auto numGrooveHits = groovePlayer.process (acceptNotes ? kit->serial : 0, transport, numSamples,
                                                     currentSampleRate, grooveHits);

    // Render up to each MIDI event (or groove hit), then apply it, so hits
    // land on the exact sample.
    int position = 0;
    int nextGrooveHit = 0;

    auto playGrooveHitsBefore = [&] (int time)
    {
        for (; nextGrooveHit < numGrooveHits && grooveHits[(size_t) nextGrooveHit].sampleOffset < time; ++nextGrooveHit)
        {
            const auto& hit = grooveHits[(size_t) nextGrooveHit];
            render (kit, position, hit.sampleOffset, mainOut, auxOuts);
            position = hit.sampleOffset;

            // The player only plays patterns made for this kit, but the
            // check costs nothing and guards the index.
            if (acceptNotes && juce::isPositiveAndBelow (hit.articulation, (int) kit->articulations.size()))
            {
                logHit (HitLogEntry::Source::groove, hit.articulation, hit.velocity);
                engine.trigger (*kit, hit.articulation, hit.velocity, engineParams);
            }
        }
    };

    for (const auto metadata : midi)
    {
        const auto eventTime = juce::jlimit (0, numSamples, metadata.samplePosition);
        playGrooveHitsBefore (eventTime);
        render (kit, position, eventTime, mainOut, auxOuts);
        position = eventTime;

        const auto message = metadata.getMessage();

        if (message.isNoteOn())
            logHit (HitLogEntry::Source::midiNote, message.getNoteNumber(), message.getVelocity());
        else if (message.isController())
            logHit (HitLogEntry::Source::midiController, message.getControllerNumber(), message.getControllerValue());

        // Drums are one-shots, so note-offs (and All Notes Off, which many hosts
        // send when the transport stops) leave ringing cymbals alone. Only All
        // Sound Off silences the kit.
        //
        // Messages are handled in the order they arrive, so the hi-hat
        // pedal's or a pad's position controller, which e-kit modules send
        // just before the note, is always latched before the note plays.
        engine.setTime (engineClock + eventTime);

        if (message.isAllSoundOff())
            engine.fadeOutAll (kitSwapFadeSeconds);
        else if (! acceptNotes)
            continue;
        else if (message.isNoteOn())
            engine.noteOn (*kit, message.getNoteNumber(), message.getVelocity(), engineParams, message.getChannel());
        else if (message.isAftertouch())
            engine.polyAftertouch (*kit, message.getNoteNumber(), message.getAfterTouchValue(), message.getChannel());
        else if (message.isController())
            engine.controlChange (*kit, message.getChannel(), message.getControllerNumber(), message.getControllerValue(), engineParams);
    }

    playGrooveHitsBefore (numSamples + 1);
    render (kit, position, numSamples, mainOut, auxOuts);
    engineClock += numSamples;

    if (hitLogOn)
        endHitLogBlock (mainBus.getMagnitude (0, numSamples), numSamples);
}

//==============================================================================
void LithiumDrumsProcessor::logHit (HitLogEntry::Source source, int a, int b, float peakDb) noexcept
{
    if (! hitLogOn)
        return;

    if (source != HitLogEntry::Source::midiController && source != HitLogEntry::Source::lateBlock)
        samplesSinceHit = 0;

    hitLog.push ({ samplesPlayed.load (std::memory_order_relaxed), source, a, b, peakDb });   // dropped if the queue is full
}

void LithiumDrumsProcessor::endHitLogBlock (float peak, int numSamples) noexcept
{
    // A block that came well after the previous one: the audio device ran
    // short (an xrun), which can click or repeat a scrap of old audio.
    // (Reading the clock is a cheap system call, no locks or allocation.)
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto blockMs = 1000.0 * numSamples / currentSampleRate;

    if (lastBlockMs > 0.0 && now - lastBlockMs > 2.0 * blockMs + 5.0)
        logHit (HitLogEntry::Source::lateBlock, juce::roundToInt (now - lastBlockMs), 0);

    lastBlockMs = now;

    // Sound after at least 0.3 s of silence, with no hit in the last 50 ms.
    const auto rate = (std::int64_t) currentSampleRate;

    if (peak > juce::Decibels::decibelsToGain (-50.0f) && quietSamples >= rate * 3 / 10 && samplesSinceHit > rate / 20)
        logHit (HitLogEntry::Source::noHit, 0, 0, juce::Decibels::gainToDecibels (peak));

    quietSamples = peak < juce::Decibels::decibelsToGain (-80.0f) ? quietSamples + numSamples : 0;
    samplesSinceHit += numSamples;
    samplesPlayed.fetch_add (numSamples, std::memory_order_relaxed);
}

void LithiumDrumsProcessor::writeHitLog()
{
    if (hitLogFile == nullptr)
        return;

    const auto info = getKitInfo();

    // Articulation and piece names, by the kit's global articulation index.
    auto articulationName = [&] (int index) -> juce::String
    {
        if (info != nullptr)
            for (const auto& p : info->pieces)
                if (juce::isPositiveAndBelow (index - p.firstArticulation, (int) p.articulations.size()))
                    return p.name + " / " + p.articulations[(size_t) (index - p.firstArticulation)].name;

        return "?";
    };

    auto noteName = [&] (int note)
    {
        std::scoped_lock lock (stateMutex);
        const auto target = noteTable.targets[(size_t) juce::jlimit (0, 127, note)].articulation;
        return target >= 0 ? articulationName (target) : juce::String ("nothing");
    };

    const auto playedNow = samplesPlayed.load (std::memory_order_relaxed);
    const auto nowMs = juce::Time::currentTimeMillis();
    HitLogEntry e;

    while (hitLog.pop (e))
    {
        using Source = HitLogEntry::Source;
        const auto when = juce::Time (nowMs - (juce::int64) (1000.0 * (double) (playedNow - e.sample) / currentSampleRate));
        juce::String text;

        switch (e.source)
        {
            case Source::midiNote:       text << "MIDI note " << e.a << " vel " << e.b << " -> " << noteName (e.a); break;
            case Source::midiController: text << "MIDI CC " << e.a << " = " << e.b; break;
            case Source::click:          text << "click: " << (info != nullptr && e.a < (int) info->pieces.size() ? info->pieces[(size_t) e.a].name : juce::String (e.a)) << " vel " << e.b; break;
            case Source::key:            text << "on-screen key " << e.a << " vel " << e.b << " -> " << noteName (e.a); break;
            case Source::groove:         text << "groove: " << articulationName (e.a) << " vel " << e.b; break;
            case Source::noHit:          text << "*** SOUND WITH NO HIT, peak " << juce::String (e.peakDb, 1) << " dB"; break;
            case Source::lateBlock:      text << "late audio block: " << e.a << " ms since the last"; break;
        }

        *hitLogFile << when.formatted ("%H:%M:%S.") << juce::String (when.getMilliseconds()).paddedLeft ('0', 3) << "  " << text << "\n";
    }

    hitLogFile->flush();
}

void LithiumDrumsProcessor::render (Kit* kit, int from, int to, std::array<float*, 2> mainOut,
                                    const Mixer::AuxOutputs& auxOuts) noexcept
{
    if (kit == nullptr)
        return;   // the buffer is already clear

    // The mic bus was sized in prepareToPlay(). Hosts occasionally send bigger
    // blocks than they announced, so render in chunks that always fit.
    while (from < to)
    {
        const auto n = std::min (to - from, mixer.getMaxBlockSize());

        mixer.clearBuses (n);
        engine.render (*kit, mixer.getBuses(), 0, n, engineParams);

        Mixer::AuxOutputs offsetAux {};

        for (size_t i = 0; i < auxOuts.size(); ++i)
            if (auxOuts[i][0] != nullptr)
                offsetAux[i] = { auxOuts[i][0] + from, auxOuts[i][1] + from };

        mixer.process (*kit, n, engineParams, { mainOut[0] + from, mainOut[1] + from }, offsetAux);
        from += n;
    }
}

void LithiumDrumsProcessor::applyNoteTables() noexcept
{
    // Only the newest table matters; older ones are overwritten.
    NoteTableUpdate update;

    while (noteTableUpdates.pop (update))
    {
        waitingNoteTable = update;
        noteTableWaiting = true;
    }

    auto* kit = handoff.getCurrent();

    if (! noteTableWaiting || kit == nullptr)
        return;

    // Serials only ever grow, so a table for an older kit will never be
    // needed again. One for a newer kit waits until that kit is swapped in.
    if (waitingNoteTable.kitSerial == kit->serial)
        kit->notes = waitingNoteTable.table;

    if (waitingNoteTable.kitSerial <= kit->serial)
        noteTableWaiting = false;
}

//==============================================================================
void LithiumDrumsProcessor::audition (int piece, int velocity, int articulation)
{
    auditions.push ({ piece, articulation, velocity });   // if the queue is full the click is dropped, which is harmless
}

void LithiumDrumsProcessor::playNote (int note, int velocity)
{
    if (juce::isPositiveAndBelow (note, 128))
        auditions.push ({ -1, 0, juce::jlimit (1, 127, velocity), note });
}

MicMask LithiumDrumsProcessor::currentlyUnmutedMics() const
{
    MicMask mask = 0;
    const auto mutes = snapshot.readMicMutes();

    for (size_t i = 0; i < mutes.size(); ++i)
        if (! mutes[i])
            mask |= 1u << i;

    return mask;
}

void LithiumDrumsProcessor::loadKit (const juce::File& folder, bool resetMixer)
{
    auto force = false;

    if (resetMixer)
    {
        // A kit from the menu is a fresh start: its own pieces, no preset.
        presetFile = juce::File();
        pendingPreset.reset();

        std::scoped_lock lock (stateMutex);
        force = ! pieceSwaps.empty();
        pieceSwaps.clear();
    }

    requestKit ({ folder, resetMixer ? KitSettings::reset : KitSettings::keep, force });
}

std::uint64_t LithiumDrumsProcessor::requestKit (const KitRequest& r)
{
    if (r.folder == juce::File())
        return 0;

    KitLoader::Request request;
    request.streaming = settings->getStreaming();
    request.streamStats = streamStats;
    const auto reset = r.settings != KitSettings::keep;

    {
        std::scoped_lock lock (stateMutex);

        // Asking for the kit that's already loaded (or loading) doesn't load
        // it again; it only updates whether the mixer should be reset.
        if (! r.force && r.folder == kitFolder && (kitLoadPending || (kitInfo != nullptr && kitInfo->folder == r.folder)))
        {
            resetWanted = reset && kitLoadPending;
            return 0;
        }

        kitFolder = r.folder;
        resetWanted = reset;

        request.folder = r.folder;
        request.resetToken = reset ? ++lastResetToken : 0;
        request.midiMap = midiMap;
        request.swaps = pieceSwaps;

        // Muted mics of a restored project aren't loaded at all. A kit chosen
        // from the menu starts unmuted, so everything loads.
        request.mics = r.mics.value_or (r.settings == KitSettings::reset ? allMics : currentlyUnmutedMics());
    }

    kitLoadPending = true;
    loader.requestLoad (request);
    sendChangeMessage();   // safe from any thread: the message is delivered asynchronously
    return request.resetToken;
}

juce::File LithiumDrumsProcessor::getKitFolder() const
{
    std::scoped_lock lock (stateMutex);
    return kitFolder;
}

void LithiumDrumsProcessor::setKitsFolder (const juce::File& folder)
{
    settings->setKitsFolder (folder);
    sendChangeMessage();
}

void LithiumDrumsProcessor::setStreamingMode (StreamingMode mode)
{
    settings->setStreamingMode (mode);
    reloadKit();
}

void LithiumDrumsProcessor::setPreloadFrames (int frames)
{
    settings->setPreloadFrames (frames);
    reloadKit();
}

int LithiumDrumsProcessor::clearStreamCache()
{
    // Deleting a pack that is open would pull the audio out from under a
    // playing kit (and fails on Windows), so those stay.
    const juce::SharedResourcePointer<DiskStreamer> streamer;
    return streamcache::clear (settings->getStreaming().cacheFolder, streamer->getPacksInUse());
}

void LithiumDrumsProcessor::handleAsyncUpdate()
{
    if (auto result = loader.takeResult())
        applyLoadResult (*result);

    applySelection();
}

void LithiumDrumsProcessor::applyLoadResult (const KitLoader::Result& result)
{
    std::shared_ptr<const KitInfo> info, previous;
    bool reset = false;

    {
        std::scoped_lock lock (stateMutex);
        previous = kitInfo;

        // Only the latest request counts; an older result that slipped
        // through is ignored.
        if (result.folder != kitFolder)
            return;

        kitLoadPending = false;

        if (result.info == nullptr)
        {
            kitError = result.error;
            pendingPreset.reset();

            // If an older kit is still playing, keep saving that one rather
            // than the folder that failed.
            if (kitInfo != nullptr)
                kitFolder = kitInfo->folder;

            sendChangeMessage();
            return;
        }

        kitInfo = info = result.info;
        loadedSwaps = result.swaps;
        kitError = {};
        reset = resetWanted && result.resetToken == lastResetToken;
        resetWanted = false;
        resetSelection = reset;
    }

    // A preset's settings go in once its kit is here; any other kit chosen
    // from the menu starts neutral.
    if (reset && pendingPreset != nullptr && pendingPreset->resetToken == result.resetToken)
    {
        parameters.replaceState (pendingPreset->contents.parameters.createCopy());
        applyPresetBaseline (pendingPreset->contents);
        pendingPreset.reset();
    }
    else if (reset)
    {
        params::resetForKit (parameters, *info);
    }
    else if (previous != nullptr && previous->folder == info->folder)
    {
        // The same kit with a slot filled or emptied: pieces may have moved,
        // so their settings move with them (matched by id).
        std::vector<int> from;
        auto moved = info->pieces.size() != previous->pieces.size();

        for (size_t i = 0; i < info->pieces.size(); ++i)
        {
            auto old = -1;

            for (size_t j = 0; j < previous->pieces.size() && old < 0; ++j)
                if (previous->pieces[j].id == info->pieces[i].id)
                    old = (int) j;

            from.push_back (old);
            moved = moved || old != (int) i;
        }

        if (moved)
            params::movePieces (parameters, *info, from);
    }

    // Whether or not anything was reset, the audio thread can now use the
    // parameters for this kit.
    if (result.resetToken != 0)
        confirmedResetToken.store (std::max (confirmedResetToken.load(), result.resetToken), std::memory_order_release);

    // The kit was built with the map as it was when the load was requested;
    // send it the map as it is now.
    sendNoteTable();

    // A groove that's previewing carries on, on the new kit.
    sendGroovePattern (false);

    // Slot parameters are named after the kit's pieces and mics; tell the
    // host to re-read the names.
    slotNames.set (info.get());
    updateHostDisplay (ChangeDetails().withParameterInfoChanged (true));
    sendChangeMessage();
}

void LithiumDrumsProcessor::applySelection()
{
    std::shared_ptr<const KitInfo> info;
    juce::String selectId;
    bool fresh = false;

    {
        std::scoped_lock lock (stateMutex);

        if (kitInfo == nullptr || kitLoadPending)
            return;   // wait for the kit; the saved selection stays pending

        info = kitInfo;
        selectId = std::exchange (pendingSelection, {});
        fresh = std::exchange (resetSelection, false);
    }

    // Select the saved piece, or keep the selection if it still fits, or
    // fall back to the snare (or the first piece).
    const auto& pieces = info->pieces;
    auto selected = fresh ? -1 : selection.getSelected();

    for (size_t i = 0; i < pieces.size(); ++i)
        if (selectId.isNotEmpty() && pieces[i].id == selectId)
            selected = (int) i;

    if (! juce::isPositiveAndBelow (selected, (int) pieces.size()))
    {
        selected = 0;

        for (size_t i = 0; i < pieces.size(); ++i)
        {
            if (pieces[i].role == PieceRole::snare)
            {
                selected = (int) i;
                break;
            }
        }
    }

    selection.select (selected);
}

void LithiumDrumsProcessor::timerCallback()
{
    writeHitLog();
    updateRoomIr();
    updateLatency();

    // Keep the loader's idea of which mics should be in RAM in line with the
    // mute buttons: unmuted mics load straight away, muted ones unload once
    // they've stayed muted for a moment.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto mutes = snapshot.readMicMutes();
    MicMask wanted = 0;

    for (size_t i = 0; i < mutes.size(); ++i)
    {
        if (! mutes[i])
            mutedSince[i] = 0.0;
        else if (mutedSince[i] <= 0.0)
            mutedSince[i] = now;

        if (! mutes[i] || now - mutedSince[i] < unloadAfterMutedMs)
            wanted |= 1u << i;
    }

    loader.setWantedMics (wanted);

    bool unsent = false;

    {
        std::scoped_lock lock (stateMutex);
        unsent = noteTableUnsent;
    }

    if (unsent)
        sendNoteTable();

    groovePlayer.collectGarbage();

    if (unsentGroovePattern != nullptr)
        groovePlayer.send (unsentGroovePattern);
}

//==============================================================================
void LithiumDrumsProcessor::previewGroove (std::shared_ptr<const grooves::Clip> clip, const juce::String& noteMapId, int clipId)
{
    grooveClip = std::move (clip);
    grooveNoteMap = noteMapId;
    grooveClipId = clipId;
    sendGroovePattern (true);
    groovePlayer.playing = grooveClip != nullptr;
}

void LithiumDrumsProcessor::sendGroovePattern (bool restart)
{
    const auto info = getKitInfo();

    if (grooveClip == nullptr || info == nullptr)
        return;

    // Resolving allocates, so it happens here; the audio thread gets a
    // finished pattern it only has to read.
    auto pattern = std::make_unique<GroovePattern>();
    pattern->lengthBeats = grooveClip->lengthBeats;
    pattern->bpm = grooveClip->bpm;
    pattern->kitSerial = info->serial;
    pattern->clipId = grooveClipId;
    pattern->restart = restart;

    for (const auto& h : grooves::resolve (*grooveClip, MidiMap::factoryMap (grooveNoteMap), *info))
    {
        // Hits pushed ahead of the first bar line play at the end of the
        // loop, just before it comes round again.
        const auto beat = h.beat < 0.0 ? h.beat + pattern->lengthBeats : h.beat;
        pattern->events.push_back ({ beat, (std::int16_t) h.articulation, (std::uint8_t) h.velocity });
    }

    std::stable_sort (pattern->events.begin(), pattern->events.end(),
                      [] (const auto& a, const auto& b) { return a.beat < b.beat; });

    // Only the newest pattern matters, so an unsent older one is simply replaced.
    unsentGroovePattern = std::move (pattern);
    groovePlayer.send (unsentGroovePattern);
}

juce::MidiFile LithiumDrumsProcessor::grooveToMidi (const grooves::Clip& clip, const juce::String& noteMapId) const
{
    const auto info = getKitInfo();
    return grooves::toMidiFile (clip, MidiMap::factoryMap (noteMapId), info.get(), getNoteTable());
}

//==============================================================================
void LithiumDrumsProcessor::setMidiMap (const MidiMap& map, bool modified)
{
    {
        std::scoped_lock lock (stateMutex);
        midiMap = map;
        midiMapModified = modified;
    }

    sendNoteTable();
}

void LithiumDrumsProcessor::sendNoteTable()
{
    {
        // Pushed under the state lock: a host may restore state on its own
        // thread while the UI edits the map, and the queue only allows one
        // thread to push at a time. The audio thread, which pops, never
        // takes this lock.
        std::scoped_lock lock (stateMutex);

        if (kitInfo != nullptr)
        {
            noteTable = resolveMidiMap (midiMap, *kitInfo);

            // The queue copies the table, so nothing is shared with the audio
            // thread. If it's full (the map changed many times within one
            // block), the timer sends the newest table a moment later.
            noteTableUnsent = ! noteTableUpdates.push ({ kitInfo->serial, noteTable });
        }
    }

    midiMapBroadcaster.sendChangeMessage();   // asynchronous, so fine from any thread
}

void LithiumDrumsProcessor::editMidiMap (const std::function<void (MidiMap&, const KitInfo&)>& edit)
{
    const auto info = getKitInfo();

    if (info == nullptr)
        return;

    auto map = editableCopy (getMidiMap(), *info);
    edit (map, *info);
    setMidiMap (map, true);
}

void LithiumDrumsProcessor::learnNote (int note, int articulation)
{
    editMidiMap ([=] (MidiMap& map, const KitInfo& info) { map.assign (entryForArticulation (info, articulation, note)); });
}

void LithiumDrumsProcessor::forgetNote (int note)
{
    editMidiMap ([=] (MidiMap& map, const KitInfo&) { map.remove (note); });
}

void LithiumDrumsProcessor::setFollow (int articulation, Follow follow)
{
    const auto table = getNoteTable();

    editMidiMap ([&] (MidiMap& map, const KitInfo&)
    {
        for (auto& e : map.entries)
            if (table.targets[(size_t) e.note].articulation == articulation)
                e.follow = follow;
    });
}

void LithiumDrumsProcessor::setControllers (const ControllerSettings& controllers)
{
    auto map = getMidiMap();
    map.controllers = controllers;
    setMidiMap (map, true);
}

//==============================================================================
void LithiumDrumsProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty (stateKitFolder, getKitFolder().getFullPathName(), nullptr);

    // The id finds the kit again if its folder moves (a new kits folder, or
    // a re-import into a content pack).
    if (const auto info = getKitInfo(); info != nullptr && info->folder == getKitFolder())
        state.setProperty (stateKitId, info->id, nullptr);

    // The selection is saved by piece id, which stays right if the kit's
    // piece order ever changes.
    const auto selected = selection.getSelected();

    if (const auto info = getKitInfo(); info != nullptr && juce::isPositiveAndBelow (selected, (int) info->pieces.size()))
        state.setProperty (stateSelectedPiece, info->pieces[(size_t) selected].id, nullptr);

    // The whole map goes into the project, not just its name, so the project
    // plays the same on a machine without that saved map.
    {
        std::scoped_lock lock (stateMutex);
        state.appendChild (swaps::toValueTree (pieceSwaps), nullptr);
        state.setProperty (statePresetFile, presetFile.getFullPathName(), nullptr);
        state.setProperty (stateMidiMap, midiMap.toJson(), nullptr);
        state.setProperty (stateMidiMapModified, midiMapModified, nullptr);

        // FX A/B: which slot is live, and the other one's settings. Kept out
        // of the parameters' state, so presets don't carry it.
        juce::ValueTree ab (stateFxAB);
        ab.setProperty (stateFxSlot, fxSlot, nullptr);

        if (const auto& other = fxSlots[(size_t) (1 - fxSlot)]; other.isValid())
            ab.appendChild (other.createCopy(), nullptr);

        state.appendChild (ab, nullptr);

        auto grooves = grooveState.createCopy();
        grooves.setProperty (grooveTempo, (int) groovePlayer.tempoMode.load(), nullptr);
        grooves.setProperty (grooveCustomBpm, groovePlayer.customBpm.load(), nullptr);
        grooves.setProperty (grooveSpeed, groovePlayer.speed.load(), nullptr);
        grooves.setProperty (grooveLoop, groovePlayer.loop.load(), nullptr);
        state.appendChild (grooves, nullptr);
    }

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void LithiumDrumsProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    const auto savedFolder = state[stateKitFolder].toString();
    const auto savedId = state[stateKitId].toString();
    const auto selected = state[stateSelectedPiece].toString();
    const auto savedMap = state[stateMidiMap].toString();
    const auto savedMapModified = (bool) state[stateMidiMapModified];

    const auto savedPreset = state[statePresetFile].toString();

    for (const auto& id : { stateKitFolder, stateKitId, stateSelectedPiece, stateMidiMap, stateMidiMapModified, statePresetFile })
        state.removeProperty (id, nullptr);

    // Swapped pieces: projects from before piece swapping have none.
    auto swapsChanged = false;

    {
        auto savedSwaps = swaps::fromValueTree (state.getChildWithName (swaps::treeType));
        state.removeChild (state.getChildWithName (swaps::treeType), nullptr);

        std::scoped_lock lock (stateMutex);
        swapsChanged = savedSwaps != pieceSwaps;
        pieceSwaps = std::move (savedSwaps);
    }

    // FX A/B. (Hosts may restore on another thread; the slots are the
    // message thread's, so they're set there.)
    if (auto ab = state.getChildWithName (stateFxAB); ab.isValid())
    {
        state.removeChild (ab, nullptr);
        const auto slot = juce::jlimit (0, 1, (int) ab[stateFxSlot]);
        const auto other = ab.getChild (0).createCopy();

        juce::MessageManager::callAsync ([this, safe = juce::WeakReference<LithiumDrumsProcessor> (this), slot, other]
        {
            if (safe == nullptr)
                return;

            std::scoped_lock lock (stateMutex);
            fxSlot = slot;
            fxSlots[(size_t) slot] = {};
            fxSlots[(size_t) (1 - slot)] = other;
        });
    }

    // The Grooves page's state isn't a parameter; it's kept apart from them.
    if (auto grooves = state.getChildWithName (grooveState.getType()); grooves.isValid())
    {
        state.removeChild (grooves, nullptr);
        groovePlayer.tempoMode = (GrooveTempo) juce::jlimit (0, 2, (int) grooves.getProperty (grooveTempo, 0));
        groovePlayer.customBpm = juce::jlimit (20.0f, 400.0f, (float) grooves.getProperty (grooveCustomBpm, 120.0f));
        groovePlayer.speed = juce::jlimit (0.5f, 2.0f, (float) grooves.getProperty (grooveSpeed, 1.0f));
        groovePlayer.loop = (bool) grooves.getProperty (grooveLoop, true);

        for (const auto& id : { grooveTempo, grooveCustomBpm, grooveSpeed, grooveLoop })
            grooves.removeProperty (id, nullptr);

        std::scoped_lock lock (stateMutex);
        grooveState.copyPropertiesFrom (grooves, nullptr);
    }

    parameters.replaceState (state);

    // Projects saved before MIDI maps existed keep the instance's map.
    if (MidiMap map; savedMap.isNotEmpty() && MidiMap::parse (savedMap, map).wasOk())
        setMidiMap (map, savedMapModified);

    {
        std::scoped_lock lock (stateMutex);
        pendingSelection = selected;
    }

    if (const auto folder = kitlibrary::resolveSavedKit (savedFolder, savedId, settings->getKitsFolder()); folder != juce::File())
        requestKit ({ folder, KitSettings::keep, swapsChanged });

    // The preset the project was using, to tell whether it has been changed
    // since. (Hosts may restore state off the message thread; the preset
    // fields are the message thread's, so this hops over.)
    juce::MessageManager::callAsync ([this, safe = juce::WeakReference<LithiumDrumsProcessor> (this), savedPreset]
    {
        if (safe == nullptr)
            return;

        pendingPreset.reset();
        presetFile = juce::File::isAbsolutePath (savedPreset) ? juce::File (savedPreset) : juce::File();
        presetValues.clear();

        if (presets::Contents contents; presetFile.existsAsFile() && presets::read (presetFile, contents).wasOk())
            applyPresetBaseline (contents);

        sendChangeMessage();
    });

    triggerAsyncUpdate();   // applies the saved selection once the kit is there
}

//==============================================================================
void LithiumDrumsProcessor::setPieceSwap (const PieceSwap& swap)
{
    {
        std::scoped_lock lock (stateMutex);
        auto swaps = pieceSwaps;
        swaps::set (swaps, swap);

        if (swaps == pieceSwaps)
            return;

        pieceSwaps = std::move (swaps);
    }

    requestKit ({ getKitFolder(), KitSettings::keep, true });
}

juce::Result LithiumDrumsProcessor::loadPreset (const juce::File& file)
{
    presets::Contents contents;

    if (const auto r = presets::read (file, contents); r.failed())
        return r;

    const auto folder = kitlibrary::resolveSavedKit (contents.kitPath, contents.kitId, settings->getKitsFolder());

    if (folder == juce::File() || ! folder.getChildFile ("kit.json").existsAsFile())
        return juce::Result::fail ("This preset plays " + (contents.kitName.isNotEmpty() ? contents.kitName : contents.kitId)
                                   + ", which isn't installed. Download it from the kit browser (Browse), then load the preset again.");

    presetFile = file;

    // The same kit, with the same pieces: just the settings, at once. (The
    // mixer and EQ smooth their changes, so this doesn't click.)
    const auto info = getKitInfo();
    bool sameKit = false;

    {
        std::scoped_lock lock (stateMutex);
        sameKit = ! kitLoadPending && info != nullptr && info->folder == folder && loadedSwaps == contents.swaps
                  && pieceSwaps == contents.swaps;
    }

    if (sameKit)
    {
        pendingPreset.reset();
        parameters.replaceState (contents.parameters.createCopy());
        applyPresetBaseline (contents);
        sendChangeMessage();
        return juce::Result::ok();
    }

    // Another kit: it loads in the background, with only the mics the
    // preset has unmuted, and the settings go in when it arrives. Until
    // then the current kit plays on as it was.
    MicMask mics = 0;

    for (int m = 0; m < maxMics; ++m)
    {
        const auto mute = contents.parameters.getChildWithProperty ("id", params::micParamId (m, "mute"));

        if (! mute.isValid() || ! (bool) mute["value"])
            mics |= 1u << m;
    }

    {
        std::scoped_lock lock (stateMutex);
        pieceSwaps = contents.swaps;
    }

    auto pending = std::make_unique<PendingPreset>();
    pending->contents = std::move (contents);
    pendingPreset = std::move (pending);
    pendingPreset->resetToken = requestKit ({ folder, KitSettings::preset, true, mics });
    sendChangeMessage();
    return juce::Result::ok();
}

juce::Result LithiumDrumsProcessor::savePreset (const juce::File& file)
{
    if (presets::isFactory (file))
        return juce::Result::fail ("Factory presets can't be overwritten. Save it under another name.");

    const auto info = getKitInfo();

    if (info == nullptr)
        return juce::Result::fail ("Load a kit before saving a preset.");

    presets::Contents contents;
    contents.kitId = info->id;
    contents.kitPath = info->folder.getFullPathName();
    contents.kitName = info->credits.name;
    contents.swaps = getPieceSwaps();
    contents.parameters = parameters.copyState();

    if (const auto r = presets::write (file, contents); r.failed())
        return r;

    presetFile = file;
    pendingPreset.reset();
    applyPresetBaseline (contents);
    sendChangeMessage();
    return juce::Result::ok();
}

void LithiumDrumsProcessor::applyPresetBaseline (const presets::Contents& contents)
{
    // What "unmodified" means: the preset's kit, its swaps and every
    // parameter value it holds.
    presetKitId = contents.kitId;
    presetSwaps = contents.swaps;
    presetValues.clear();

    for (const auto& child : contents.parameters)
        if (auto* p = parameters.getParameter (child["id"].toString()))
            presetValues.emplace_back (p, p->convertTo0to1 ((float) child["value"]));
}

bool LithiumDrumsProcessor::isPresetModified() const
{
    if (presetFile == juce::File() || pendingPreset != nullptr || isKitLoading())
        return false;

    const auto info = getKitInfo();

    if (info == nullptr || (presetKitId.isNotEmpty() && info->id != presetKitId))
        return true;

    auto sorted = [] (PieceSwaps s)
    {
        std::sort (s.begin(), s.end(), [] (const PieceSwap& a, const PieceSwap& b) { return a.piece < b.piece; });
        return s;
    };

    if (sorted (getPieceSwaps()) != sorted (presetSwaps))
        return true;

    // Normalised values, so one tolerance suits every range.
    return std::any_of (presetValues.begin(), presetValues.end(), [] (const auto& pv)
    {
        return std::abs (pv.first->getValue() - pv.second) > 1.0e-4f;
    });
}

//==============================================================================
void LithiumDrumsProcessor::updateRoomIr()
{
    // The IR choice and Decay are parameters, but changing them means
    // loading or re-trimming an IR, which the loader's thread does; it
    // ignores requests for what it already has.
    const auto choice = juce::roundToInt (parameters.getRawParameterValue (params::roomIr)->load());
    const auto decay = parameters.getRawParameterValue (params::roomDecay)->load() / 100.0f;
    const auto custom = choice >= (int) irs::builtIn().size();

    IrLoader::Request request;
    request.builtIn = custom ? -1 : choice;
    request.file = custom ? getRoomIrFile() : juce::File();
    request.decay = decay;

    // A custom choice with no file yet (picked from the menu before choosing
    // one) keeps the first built-in IR until a file is chosen.
    if (custom && request.file == juce::File())
        request.builtIn = 0;

    irLoader.request (request);
}

void LithiumDrumsProcessor::updateLatency()
{
    // Hosts are told about latency from the message thread, never the audio
    // thread. It only changes when the limiter's lookahead is switched (or
    // the sample rate changes), and hosts re-align the track when it does.
    if (const auto latency = mixer.getLatencySamples (snapshot.readLookahead()); latency != getLatencySamples())
        setLatencySamples (latency);
}

void LithiumDrumsProcessor::setRoomIrFile (const juce::File& file)
{
    parameters.state.setProperty (stateRoomIrFile, file.getFullPathName(), nullptr);

    if (auto* p = parameters.getParameter (params::roomIr))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) irs::builtIn().size()));
        p->endChangeGesture();
    }

    updateRoomIr();
}

juce::File LithiumDrumsProcessor::getRoomIrFile() const
{
    const auto path = parameters.state[stateRoomIrFile].toString();
    return juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
}

juce::ValueTree LithiumDrumsProcessor::captureFx() const
{
    juce::ValueTree tree ("fx");

    // AudioProcessor's own list of parameters (getParameters() on this class is the APVTS).
    for (auto* p : AudioProcessor::getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p); ranged != nullptr && params::isFxParameter (ranged->getParameterID()))
        {
            juce::ValueTree child ("PARAM");
            child.setProperty ("id", ranged->getParameterID(), nullptr);
            child.setProperty ("value", ranged->getValue(), nullptr);   // normalised
            tree.appendChild (child, nullptr);
        }
    }

    tree.setProperty (stateRoomIrFile, parameters.state[stateRoomIrFile], nullptr);
    return tree;
}

void LithiumDrumsProcessor::applyFx (const juce::ValueTree& tree)
{
    parameters.state.setProperty (stateRoomIrFile, tree[stateRoomIrFile], nullptr);

    for (const auto& child : tree)
    {
        if (auto* p = parameters.getParameter (child["id"].toString()))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost ((float) child["value"]);
            p->endChangeGesture();
        }
    }

    updateRoomIr();
    updateLatency();
}

void LithiumDrumsProcessor::selectFxSlot (int slot)
{
    slot = juce::jlimit (0, 1, slot);

    if (slot == fxSlot)
        return;

    // The other slot starts as a copy of this one, the first time it's used.
    // (The slots are under the state lock, as hosts may save from other threads.)
    auto leaving = captureFx();
    juce::ValueTree coming;

    {
        std::scoped_lock lock (stateMutex);
        fxSlots[(size_t) fxSlot] = std::move (leaving);
        coming = fxSlots[(size_t) slot];
        fxSlot = slot;
    }

    if (coming.isValid())
        applyFx (coming);
}

void LithiumDrumsProcessor::copyFxSlot()
{
    auto live = captureFx();
    std::scoped_lock lock (stateMutex);
    fxSlots[(size_t) (1 - fxSlot)] = std::move (live);
}

juce::AudioProcessorEditor* LithiumDrumsProcessor::createEditor()
{
    return new LithiumDrumsEditor (*this);
}

} // namespace lithium

// JUCE's plugin wrappers call this to create the processor.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new lithium::LithiumDrumsProcessor();
}
