#pragma once

#include "KitLibrary.h"
#include "Parameters.h"
#include "Presets.h"
#include "SelectionModel.h"
#include "engine/DrumEngine.h"
#include "engine/GroovePlayer.h"
#include "engine/IrLoader.h"
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
        overhead or room mic's), and the FX page's selected module. Kept here
        so they survive closing the editor. Message thread. */
    params::ChannelId selectedMixerChannel { true, 0 };
    int selectedFxModule = 3;   // fxpresets::Module: the EQ

    /** Whether the Kit tab shows the piece slots rather than the stage. Message thread. */
    bool kitSlotsView = false;
    double getCpuLoadPercent() const                                 { return loadMeasurer.getLoadAsPercentage(); }

    /** Loads a kit. With resetMixer (choosing a kit from the menu), the
        piece and mixer settings go back to neutral for the new kit, and
        swapped pieces and the current preset are let go of; without it
        (restoring a project), they're kept. */
    void loadKit (const juce::File& folder, bool resetMixer);

    /** Loads the current kit again, keeping its settings (after the
        streaming settings change). */
    void reloadKit()                                                 { requestKit ({ getKitFolder(), KitSettings::keep, true }); }

    juce::File getKitFolder() const;
    std::shared_ptr<const KitInfo> getKitInfo() const                { std::scoped_lock lock (stateMutex); return kitInfo; }
    bool isKitLoading() const noexcept                               { return kitLoadPending.load(); }
    float getKitLoadProgress() const noexcept                        { return loader.getProgress(); }
    LoadPhase getKitLoadPhase() const noexcept                       { return loader.getPhase(); }
    juce::String getKitError() const                                 { return kitError; }

    /** Applies a finished kit load now, rather than when the message loop
        gets to it. For the tests, which don't run a message loop. */
    void applyPendingKitLoad()                                       { handleUpdateNowIfNeeded(); }

    /** Asks for the Room IR the parameters choose now, rather than on the
        next timer tick. For tools that don't run a message loop. */
    void requestRoomIr()                                             { updateRoomIr(); }

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
    // Piece swapping (message thread): playing a piece with another kit's
    // recordings of that kind of drum. See engine/PieceSwap.h.

    PieceSwaps getPieceSwaps() const                                 { std::scoped_lock lock (stateMutex); return pieceSwaps; }

    /** Swaps a piece (swap.piece, by id) for another kit's, or with an empty
        sourceKit puts the kit's own back. The kit reloads in the background
        with its settings kept; the old one plays until the new one is ready. */
    void setPieceSwap (const PieceSwap&);

    //==============================================================================
    // Presets (message thread). See Presets.h.

    /** Loads a preset: its kit and swapped pieces in the background (the
        current kit plays on until they're ready), then its settings. */
    juce::Result loadPreset (const juce::File&);

    /** Saves everything as a preset, which then becomes the current one. */
    juce::Result savePreset (const juce::File&);

    /** The preset loaded (or saved) last, if the kit hasn't been changed
        from the menu since. */
    juce::File getPresetFile() const                                 { return presetFile; }

    /** After a rename. */
    void setPresetFile (const juce::File& f)                         { presetFile = f; sendChangeMessage(); }

    /** Whether anything differs from the preset as it was loaded or saved. */
    bool isPresetModified() const;

    /** Whether a preset's kit is still loading. */
    bool isPresetLoading() const noexcept                            { return pendingPreset != nullptr; }

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

    /** The MIDI monitor: every MIDI message the engine got, and what it did.
        One consumer (the MIDI panel), on the message thread. */
    bool popMonitorEvent (MidiMonitorEvent& e) noexcept              { return engine.popMonitorEvent (e); }

    //==============================================================================
    // The FX page (message thread).

    /** The host's tempo, as of the last audio block (120 if it has none). Any thread. */
    double getHostBpm() const noexcept                               { return hostBpm.load (std::memory_order_relaxed); }

    /** The Room reverb's IR loader: what's loaded, for the UI. */
    const IrLoader& getIrLoader() const noexcept                     { return irLoader; }

    /** Plays a WAV, FLAC or AIFF file of the user's own in the Room reverb.
        The file's path is kept in the parameters' state, so presets and
        projects keep it. */
    void setRoomIrFile (const juce::File&);
    juce::File getRoomIrFile() const;

    /** A/B comparison of the whole FX page: two sets of FX settings, one of
        them live. Switching keeps the one left and brings the other back. */
    int getFxSlot() const noexcept                                   { return fxSlot; }
    void selectFxSlot (int slot);

    /** Copies the live settings into the other slot. */
    void copyFxSlot();

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
    enum class KitSettings
    {
        keep,      // restoring a project, or reloading: the settings stay
        reset,     // chosen from the menu: neutral settings for the new kit
        preset     // a preset's kit: its settings, once the kit has arrived
    };

    struct KitRequest
    {
        juce::File folder;
        KitSettings settings = KitSettings::keep;
        bool force = false;                  // load it even if it's the kit that's loaded
        std::optional<MicMask> mics = std::nullopt;   // the mics to load straight away (default: the unmuted ones)
    };

    /** Returns the request's reset token (0 if it has none, or nothing was requested). */
    std::uint64_t requestKit (const KitRequest&);
    void applyPresetBaseline (const presets::Contents&);
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
    void updateRoomIr();
    void updateLatency();
    juce::ValueTree captureFx() const;
    void applyFx (const juce::ValueTree&);

    struct AuditionRequest
    {
        int piece;
        int articulation;            // within the piece
        int velocity;
        int note = -1;               // >= 0: play this MIDI note instead of the piece
    };

    /** For tracking down sounds nobody played: with LITHIUM_HIT_LOG=<file> in
        the environment, every hit and where it came from, every MIDI
        message, any sound that starts with no hit behind it, and any audio
        block that came late are written to that file. The audio thread only
        queues them; the timer writes them. */
    struct HitLogEntry
    {
        enum class Source : std::uint8_t { midiNote, midiController, click, key, groove, noHit, lateBlock };

        std::int64_t sample = 0;     // when, in samples since playback started
        Source source = Source::midiNote;
        int a = 0, b = 0;            // note and velocity, controller and value, piece or articulation and velocity...
        float peakDb = 0.0f;         // noHit: how loud
    };

    void logHit (HitLogEntry::Source, int a, int b, float peakDb = 0.0f) noexcept;
    void endHitLogBlock (float peak, int numSamples) noexcept;
    void writeHitLog();

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
    IrLoader irLoader { mixer.getRoomReverb() };   // after the mixer, whose reverb it loads into
    std::int64_t engineClock = 0;                   // samples rendered, for the engine's timing of the pedal
    std::atomic<double> hostBpm { 120.0 };
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

    // The hit log (see HitLogEntry). The counters are the audio thread's;
    // samplesPlayed is atomic so the timer can turn sample times into clock times.
    bool hitLogOn = false;
    SpscQueue<HitLogEntry, 1024> hitLog;
    std::unique_ptr<juce::FileOutputStream> hitLogFile;     // message thread
    std::atomic<std::int64_t> samplesPlayed { 0 };
    std::int64_t samplesSinceHit = 0, quietSamples = 0;
    double lastBlockMs = 0.0;

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

    PieceSwaps pieceSwaps;                   // what the kit is asked to play with
    PieceSwaps loadedSwaps;                  // what kitInfo was loaded with

    // Presets. Message thread only.
    struct PendingPreset
    {
        std::uint64_t resetToken = 0;
        presets::Contents contents;
    };

    juce::File presetFile;
    std::unique_ptr<PendingPreset> pendingPreset;    // waiting for its kit
    juce::String presetKitId;
    PieceSwaps presetSwaps;
    std::vector<std::pair<juce::RangedAudioParameter*, float>> presetValues;   // as the preset set them

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

    // FX A/B (message thread): the slot not live holds its settings.
    std::array<juce::ValueTree, 2> fxSlots;
    int fxSlot = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (LithiumDrumsProcessor)   // for callbacks that may outlive it
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LithiumDrumsProcessor)
};

} // namespace lithium
