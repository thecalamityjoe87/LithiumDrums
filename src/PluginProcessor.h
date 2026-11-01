#pragma once

#include "KitLibrary.h"
#include "Parameters.h"
#include "SelectionModel.h"
#include "engine/DrumEngine.h"
#include "engine/GroovePlayer.h"
#include "engine/KitHandoff.h"
#include "engine/KitLoader.h"
#include "engine/MidiMapping.h"
#include "engine/Mixer.h"
#include "engine/SpscQueue.h"
#include "grooves/GrooveLibrary.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <mutex>

namespace lithium
{

/** The plugin's audio side and the owner of everything that outlives the
    editor window: parameters, the loaded kit and the selection.

    Threads:
      - The audio thread calls processBlock() only.
      - The kit loader thread builds kits, loads and unloads muted mics, and
        deletes what the audio thread retires.
      - Everything else (UI, state, kit requests) is the message thread.
    The only ways the audio thread talks to anyone are lock-free: parameter
    atomics, the KitHandoff, the audition and note table queues, the
    GroovePlayer's queues and atomics, EngineFeedback, MixerFeedback and one
    atomic counter for kit-switch resets. */
class LithiumDrumsProcessor final : public juce::AudioProcessor,
                                    public juce::ChangeBroadcaster,
                                    private juce::AsyncUpdater,
                                    private juce::Timer
{
public:
    LithiumDrumsProcessor();
    ~LithiumDrumsProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                          { return true; }

    const juce::String getName() const override              { return JucePlugin_Name; }
    bool acceptsMidi() const override                        { return true; }
    bool producesMidi() const override                       { return false; }
    bool isMidiEffect() const override                       { return false; }
    double getTailLengthSeconds() const override             { return 0.0; }

    int getNumPrograms() override                            { return 1; }
    int getCurrentProgram() override                         { return 0; }
    void setCurrentProgram (int) override                    {}
    const juce::String getProgramName (int) override         { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // For the UI (message thread). A ChangeBroadcaster message is sent
    // whenever the kit, or its loading status, changes.

    juce::AudioProcessorValueTreeState& getParameters() noexcept     { return parameters; }
    SelectionModel& getSelection() noexcept                          { return selection; }
    const EngineFeedback& getFeedback() const noexcept               { return feedback; }
    MixerFeedback& getMixerFeedback() noexcept                       { return mixerFeedback; }

    /** The mixer channel shown in the Mixer page's inspector (a drum's, or an
        overhead or room mic's), or the reverb. Kept here so they survive
        closing the editor. Message thread. */
    params::ChannelId selectedMixerChannel { true, 0 };
    bool reverbSelected = false;
    double getCpuLoadPercent() const                                 { return loadMeasurer.getLoadAsPercentage(); }

    /** Loads a kit. With resetMixer (choosing a kit from the menu), the
        piece and mixer settings go back to neutral for the new kit; without
        it (restoring a project), they're kept. */
    void loadKit (const juce::File& folder, bool resetMixer)        { requestKit (folder, resetMixer, false); }

    /** Loads the current kit again, keeping its settings (after the
        streaming settings change). */
    void reloadKit()                                                 { requestKit (getKitFolder(), false, true); }

    juce::File getKitFolder() const;
    std::shared_ptr<const KitInfo> getKitInfo() const                { std::scoped_lock lock (stateMutex); return kitInfo; }
    bool isKitLoading() const noexcept                               { return kitLoadPending.load(); }
    float getKitLoadProgress() const noexcept                        { return loader.getProgress(); }
    LoadPhase getKitLoadPhase() const noexcept                       { return loader.getPhase(); }
    juce::String getKitError() const                                 { return kitError; }

    /** Applies a finished kit load now, rather than when the message loop
        gets to it. For the tests, which don't run a message loop. */
    void applyPendingKitLoad()                                       { handleUpdateNowIfNeeded(); }

    /** Bytes of sample data in RAM right now (muted mics are unloaded). */
    std::size_t getSampleMemoryUsage() const noexcept                { return loader.getMemoryUsage(); }

    juce::File getKitsFolder() const                                 { return settings->getKitsFolder(); }
    void setKitsFolder (const juce::File&);

    /** Disk reads and dropouts of this instance's streaming kits. */
    const StreamStats& getStreamStats() const noexcept               { return *streamStats; }

    StreamingSettings getStreamingSettings() const                   { return settings->getStreaming(); }

    /** These save the setting and reload the kit, so it takes effect at once. */
    void setStreamingMode (StreamingMode);
    void setPreloadFrames (int);

    /** Deletes the stream cache, except the packs that are streaming right
        now (in any instance in this process). Returns the number deleted. */
    int clearStreamCache();

    /** Plays one of a piece's articulations (0 = its first), as if hit from MIDI. */
    void audition (int piece, int velocity, int articulation = 0);

    //==============================================================================
    // The MIDI map (message thread). Every change is resolved against the
    // current kit and sent to the audio thread at once; listeners of
    // getMidiMapBroadcaster() hear about it.

    MidiMap getMidiMap() const                                       { std::scoped_lock lock (stateMutex); return midiMap; }

    /** Whether the map differs from the saved (or factory) map it came from. */
    bool isMidiMapModified() const                                   { std::scoped_lock lock (stateMutex); return midiMapModified; }

    void setMidiMap (const MidiMap&, bool modified);

    /** What each note plays on the current kit. */
    NoteTable getNoteTable() const                                   { std::scoped_lock lock (stateMutex); return noteTable; }

    /** MIDI learn: makes a note play one of the kit's articulations (an
        index into its flattened articulations), instead of what it played. */
    void learnNote (int note, int articulation);
    void forgetNote (int note);

    /** Makes the notes that play an articulation follow the pedal or a
        position controller, or not. */
    void setFollow (int articulation, Follow);

    void setControllers (const ControllerSettings&);

    juce::ChangeBroadcaster& getMidiMapBroadcaster() noexcept        { return midiMapBroadcaster; }

    /** Plays a MIDI note as if it came in from MIDI: through the map, with
        chokes and the pedal and position following. For the on-screen keyboard. */
    void playNote (int note, int velocity);

    static constexpr int auditionVelocity = 90;

    //==============================================================================
    // The groove preview (message thread). The Grooves page picks a clip;
    // the processor resolves it against the current kit, and again whenever
    // the kit changes, and the GroovePlayer plays it on the audio thread.

    /** Plays a clip from the top, through the pack's note map. clipId is
        anything the caller wants to recognise it by in the player's feedback. */
    void previewGroove (std::shared_ptr<const grooves::Clip>, const juce::String& noteMapId, int clipId);
    void stopGroove()                                                { groovePlayer.playing = false; }
    bool isGroovePlaying() const noexcept                            { return groovePlayer.playing.load(); }

    /** The settings atomics (loop, tempo, speed) and what's playing. */
    GroovePlayer& getGroovePlayer() noexcept                         { return groovePlayer; }

    /** A clip as a MIDI file, in the notes that play it with the current MIDI map and kit. */
    juce::MidiFile grooveToMidi (const grooves::Clip&, const juce::String& noteMapId) const;

    /** What the Grooves page shows (pack, groove, phrase), kept here so it
        survives closing the editor and is saved with the project. Behind
        the state lock, as hosts may save from other threads. */
    juce::var getGrooveSetting (const juce::Identifier& id) const    { std::scoped_lock lock (stateMutex); return grooveState[id]; }
    void setGrooveSetting (const juce::Identifier& id, const juce::var& v)  { std::scoped_lock lock (stateMutex); grooveState.setProperty (id, v, nullptr); }

private:
    void requestKit (const juce::File& folder, bool resetMixer, bool force);
    void handleAsyncUpdate() override;
    void applyLoadResult (const KitLoader::Result&);
    void applySelection();
    void timerCallback() override;
    void swapInWaitingKit() noexcept;
    void render (Kit*, int from, int to, std::array<float*, 2> mainOut, const Mixer::AuxOutputs&) noexcept;
    MicMask currentlyUnmutedMics() const;
    static BusesProperties createBuses();

    /** Edits the map: an editable copy (see editableCopy()) goes to the
        function, and the result becomes the current, modified map. */
    void editMidiMap (const std::function<void (MidiMap&, const KitInfo&)>&);
    void sendNoteTable();
    void applyNoteTables() noexcept;
    void sendGroovePattern (bool restart);

    struct AuditionRequest
    {
        int piece;
        int articulation;            // within the piece
        int velocity;
        int note = -1;               // >= 0: play this MIDI note instead of the piece
    };

    /** A note table for one kit, sent when the map changes (or a kit
        arrives). Plain data, copied through the queue. */
    struct NoteTableUpdate
    {
        std::uint64_t kitSerial = 0;
        NoteTable table;
    };

    // Slot names are referenced by the parameters, so they're declared first
    // (members are constructed in order and destroyed in reverse).
    params::SlotNames slotNames;
    juce::AudioProcessorValueTreeState parameters;
    params::Snapshot snapshot;
    SelectionModel selection;
    juce::SharedResourcePointer<Settings> settings;   // one Settings object shared by every instance

    // Audio-thread state.
    EngineParams engineParams;
    EngineFeedback feedback;
    MixerFeedback mixerFeedback;
    DrumEngine engine { feedback };
    Mixer mixer { &mixerFeedback };
    bool kitSwapPending = false;
    juce::AudioProcessLoadMeasurer loadMeasurer;

    // The newest note table, kept until the kit it's for is playing: it can
    // arrive while that kit is still waiting to be swapped in.
    NoteTableUpdate waitingNoteTable;
    bool noteTableWaiting = false;

    // Shared between threads, lock-free. Declaration order matters: the
    // loader thread uses the handoff, so the handoff must be destroyed after
    // the loader.
    KitHandoff handoff;
    KitLoader loader;
    SpscQueue<AuditionRequest, 32> auditions;
    SpscQueue<NoteTableUpdate, 4> noteTableUpdates;
    std::shared_ptr<StreamStats> streamStats = std::make_shared<StreamStats>();
    std::atomic<bool> kitLoadPending { false };
    std::atomic<std::uint64_t> confirmedResetToken { 0 };   // see Kit::resetToken
    GroovePlayer groovePlayer;
    std::array<GroovePlayer::Hit, 256> grooveHits {};       // one block's worth, allocated up front
    double currentSampleRate = 44100.0;                     // from prepareToPlay

    // Message thread, but some hosts save and restore state on other
    // threads, hence the mutex.
    mutable std::mutex stateMutex;
    juce::File kitFolder;
    std::shared_ptr<const KitInfo> kitInfo;
    juce::String pendingSelection;           // piece id to select once the kit arrives
    std::uint64_t lastResetToken = 0;
    bool resetWanted = false;
    bool resetSelection = false;

    MidiMap midiMap;
    bool midiMapModified = false;
    NoteTable noteTable;                     // midiMap resolved for kitInfo
    bool noteTableUnsent = false;            // the queue was full; the timer tries again

    // Message thread only.
    juce::String kitError;
    juce::ChangeBroadcaster midiMapBroadcaster;
    std::array<double, maxMics> mutedSince {};   // ms timestamps, 0 = not muted

    // The clip being previewed, kept to resolve it again for a new kit.
    std::shared_ptr<const grooves::Clip> grooveClip;
    juce::String grooveNoteMap;
    int grooveClipId = -1;
    std::unique_ptr<GroovePattern> unsentGroovePattern;   // the queue was full; the timer tries again
    juce::ValueTree grooveState { "grooves" };            // under stateMutex

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LithiumDrumsProcessor)
};

} // namespace lithium
