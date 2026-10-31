#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace lithium
{

namespace
{
    const juce::Identifier stateKitFolder { "kitFolder" };
    const juce::Identifier stateKitId { "kitId" };
    const juce::Identifier stateSelectedPiece { "selectedPiece" };

    constexpr double kitSwapFadeSeconds = 0.01;

    // A mic is unloaded once it has stayed muted this long, so quickly
    // toggling mute (or soloing around) doesn't reload gigabytes each time.
    constexpr double unloadAfterMutedMs = 2000.0;
}

juce::AudioProcessor::BusesProperties LithiumDrumsProcessor::createBuses()
{
    // A stereo main out, plus one stereo direct out per mic slot (up to
    // Mixer::numDirectOuts) for multi-output setups. The direct outs are
    // disabled until the host enables them, and a mic only uses its direct
    // out when its Output parameter says so; otherwise it stays in the main mix.
    auto buses = BusesProperties().withOutput ("Main", juce::AudioChannelSet::stereo(), true);

    for (int i = 0; i < Mixer::numDirectOuts; ++i)
        buses = buses.withOutput ("Mic " + juce::String (i + 1), juce::AudioChannelSet::stereo(), false);

    return buses;
}

LithiumDrumsProcessor::LithiumDrumsProcessor()
    : AudioProcessor (createBuses()),
      parameters (*this, nullptr, "LithiumDrums", params::createLayout (slotNames)),
      snapshot (parameters),
      loader (handoff, MidiMap::generalMidi())
{
    slotNames.set (nullptr);

    // Runs on the loader thread; AsyncUpdater hops over to the message thread.
    loader.onLoadFinished = [this] { triggerAsyncUpdate(); };

    // A new instance starts with the default kit. If the host then restores a
    // project, its kit request simply replaces this one.
    if (const auto folder = kitlibrary::findDefaultKit (settings->getKitsFolder()); folder != juce::File())
        loadKit (folder, true);

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
    engine.prepare (sampleRate);
    mixer.prepare (sampleRate, std::max (64, maximumExpectedSamplesPerBlock));
    loadMeasurer.reset (sampleRate, maximumExpectedSamplesPerBlock);

    // The audio thread is guaranteed not to be running here, so a kit that
    // arrived while playback was stopped can be swapped in straight away.
    swapInWaitingKit();
    handoff.applyMicUpdates();
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

    Mixer::DirectOutputs directOuts {};

    for (int i = 0; i < Mixer::numDirectOuts && i + 1 < getBusCount (false); ++i)
    {
        auto bus = getBusBuffer (buffer, false, i + 1);

        if (bus.getNumChannels() == 2)
            directOuts[(size_t) i] = { bus.getWritePointer (0), bus.getWritePointer (1) };
    }

    // Clicks on the UI play at the start of the block.
    AuditionRequest request;

    while (auditions.pop (request))
    {
        if (acceptNotes && juce::isPositiveAndBelow (request.piece, (int) kit->pieces.size()))
            engine.trigger (*kit, kit->pieces[(size_t) request.piece].firstArticulation, request.velocity, engineParams);
    }

    // Render up to each MIDI event, then apply it, so hits land on the exact sample.
    int position = 0;

    for (const auto metadata : midi)
    {
        const auto eventTime = juce::jlimit (0, numSamples, metadata.samplePosition);
        render (kit, position, eventTime, mainOut, directOuts);
        position = eventTime;

        const auto message = metadata.getMessage();

        // Drums are one-shots, so note-offs (and All Notes Off, which many hosts
        // send when the transport stops) leave ringing cymbals alone. Only All
        // Sound Off silences the kit.
        if (message.isAllSoundOff())
            engine.fadeOutAll (kitSwapFadeSeconds);
        else if (! acceptNotes)
            continue;
        else if (message.isNoteOn())
            engine.noteOn (*kit, message.getNoteNumber(), message.getVelocity(), engineParams);
        else if (message.isAftertouch())
            engine.polyAftertouch (*kit, message.getNoteNumber(), message.getAfterTouchValue());
    }

    render (kit, position, numSamples, mainOut, directOuts);
}

void LithiumDrumsProcessor::render (Kit* kit, int from, int to, std::array<float*, 2> mainOut,
                                    const Mixer::DirectOutputs& directOuts) noexcept
{
    if (kit == nullptr)
        return;   // the buffer is already clear

    // The mic bus was sized in prepareToPlay(). Hosts occasionally send bigger
    // blocks than they announced, so render in chunks that always fit.
    while (from < to)
    {
        const auto n = std::min (to - from, mixer.getMaxBlockSize());

        mixer.clearMicBus (n);
        engine.render (*kit, mixer.getMicBus(), 0, n, engineParams);

        Mixer::DirectOutputs offsetDirect {};

        for (size_t i = 0; i < directOuts.size(); ++i)
            if (directOuts[i][0] != nullptr)
                offsetDirect[i] = { directOuts[i][0] + from, directOuts[i][1] + from };

        mixer.process (*kit, n, engineParams, { mainOut[0] + from, mainOut[1] + from }, offsetDirect);
        from += n;
    }
}

//==============================================================================
void LithiumDrumsProcessor::audition (int piece, int velocity)
{
    auditions.push ({ piece, velocity });   // if the queue is full the click is dropped, which is harmless
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
    KitLoader::Request request;

    {
        std::scoped_lock lock (stateMutex);

        // Asking for the kit that's already loaded (or loading) doesn't load
        // it again; it only updates whether the mixer should be reset.
        if (folder == kitFolder && (kitLoadPending || (kitInfo != nullptr && kitInfo->folder == folder)))
        {
            resetWanted = resetMixer && kitLoadPending;
            return;
        }

        kitFolder = folder;
        resetWanted = resetMixer;

        request.folder = folder;
        request.resetToken = resetMixer ? ++lastResetToken : 0;

        // Muted mics of a restored project aren't loaded at all. A kit chosen
        // from the menu starts unmuted, so everything loads.
        request.mics = resetMixer ? allMics : currentlyUnmutedMics();
    }

    kitLoadPending = true;
    loader.requestLoad (request);
    sendChangeMessage();   // safe from any thread: the message is delivered asynchronously
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

void LithiumDrumsProcessor::handleAsyncUpdate()
{
    if (auto result = loader.takeResult())
        applyLoadResult (*result);

    applySelection();
}

void LithiumDrumsProcessor::applyLoadResult (const KitLoader::Result& result)
{
    std::shared_ptr<const KitInfo> info;
    bool reset = false;

    {
        std::scoped_lock lock (stateMutex);

        // Only the latest request counts; an older result that slipped
        // through is ignored.
        if (result.folder != kitFolder)
            return;

        kitLoadPending = false;

        if (result.info == nullptr)
        {
            kitError = result.error;

            // If an older kit is still playing, keep saving that one rather
            // than the folder that failed.
            if (kitInfo != nullptr)
                kitFolder = kitInfo->folder;

            sendChangeMessage();
            return;
        }

        kitInfo = info = result.info;
        kitError = {};
        reset = resetWanted && result.resetToken == lastResetToken;
        resetWanted = false;
        resetSelection = reset;
    }

    if (reset)
        params::resetForKit (parameters, *info);

    // Whether or not anything was reset, the audio thread can now use the
    // parameters for this kit.
    if (result.resetToken != 0)
        confirmedResetToken.store (std::max (confirmedResetToken.load(), result.resetToken), std::memory_order_release);

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
    state.removeProperty (stateKitFolder, nullptr);
    state.removeProperty (stateKitId, nullptr);
    state.removeProperty (stateSelectedPiece, nullptr);

    parameters.replaceState (state);

    {
        std::scoped_lock lock (stateMutex);
        pendingSelection = selected;
    }

    if (const auto folder = kitlibrary::resolveSavedKit (savedFolder, savedId, settings->getKitsFolder()); folder != juce::File())
        loadKit (folder, false);

    triggerAsyncUpdate();   // applies the saved selection once the kit is there
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
