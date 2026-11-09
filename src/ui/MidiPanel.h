#pragma once

#include "EdrumsView.h"
#include "InfoPanels.h"
#include "MidiKeyboard.h"
#include "VelocityCurveEditor.h"
#include "MidiMapLibrary.h"

namespace lithium
{

/** MIDI mapping: which map plays the kit, what each of the kit's
    articulations is played by, and what's coming in.

    The toolbar picks a map (Kit default, General MIDI, the e-kit maps and
    the user's saved ones) and saves, reverts, deletes or makes default the
    current one. Below it, three columns:

     - the input monitor: the last note and what it played, and the pedal
       and position controllers with their CC numbers;
     - every articulation of the loaded kit with its notes: Learn assigns
       the next note that comes in (or the next key clicked), a note chip
       removes its note, and the follow button makes the articulation's
       notes follow the hi-hat pedal or the strike position;
     - the keyboard: every note labelled with what it plays.

    Above the articulations, the velocity card: the curve of the piece last
    clicked in the list, with ready-made curves, and the global velocity for
    every piece. Each articulation row has its Velocity Adjust slider on the
    left. All of it is saved in the map and applies to notes that come in as
    MIDI (and the keyboard here), not to grooves or clicks on the kit.

    The E-Drums view (the switch in the toolbar) sets up the e-kit's hi-hat
    pedal and positional pads, and has the MIDI monitor (see EdrumsView). */
class MidiPanel final : public OverlayPanel,
                        private juce::ChangeListener,
                        private juce::Timer
{
public:
    explicit MidiPanel (LithiumDrumsProcessor&);
    ~MidiPanel() override;

    /** Re-reads the saved maps. Called each time the panel opens. */
    void refresh();

    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class Row;
    class ControllerRow;
    class AdjustSlider;

    void layoutContent (juce::Rectangle<int>) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void visibilityChanged() override;

    void rebuildRows();
    void updateRows();
    void updateToolbar();
    void chooseMap (int index);
    void startLearning (int articulation);
    void selectArticulation (int articulation);
    void updateKeyboard (const NoteTable&);
    std::vector<int> notesOf (int articulation) const;
    void showNameEditor (bool);
    void saveAs();
    void setControllers (const std::function<void (ControllerSettings&)>&);
    void showEdrums (bool);

    // The velocity card
    void selectPiece (int piece);
    void updateVelocity();
    void editPieceVelocity (int piece, const std::function<void (PieceVelocity&)>&);

    LithiumDrumsProcessor& processor;
    juce::SharedResourcePointer<Settings> settings;
    std::shared_ptr<const KitInfo> shownKit;
    std::vector<midimaps::Item> maps;

    // Toolbar
    juce::ComboBox mapBox;
    juce::TextButton saveButton { "Save as..." }, revertButton { "Revert" }, deleteButton { "Delete" },
        defaultButton { "Set as default" };
    juce::TextEditor nameEditor;
    juce::TextButton confirmSave { "Save" }, cancelSave { "Cancel" };
    juce::Rectangle<int> modifiedTag;
    juce::TextButton mappingView { "Mapping" }, edrumsView { "E-Drums" };
    EdrumsView edrums;

    // Help for new users: what a map is (beside the map menu), and the Input
    // and Velocity cards (in their top-right corners).
    HelpButton mapHelp { "MIDI maps", {
        "A MIDI map says which drum each MIDI note plays. Pick your e-kit's map from this menu, General MIDI for "
        "MIDI files and keyboards, or Lithium Standard, which plays the same on every kit.",
        "The list shows every sound of the loaded kit with the notes that play it. Click Learn on a row, then hit a "
        "pad or click a key, to give it that note; click a note to take it away. PEDAL and POSITION make a hit follow "
        "the hi-hat pedal or where the pad was struck. The slider at the left of a row makes that sound hit harder "
        "or softer.",
        "The keyboard on the right shows what each note plays; click a key to hear it, further right for harder.",
        "Save as... keeps your changes as a map of your own, Revert throws them away, and Set as default makes new "
        "instances start with this map." } };
    HelpButton inputHelp { "Input", {
        "The last note that came in and what it played, so you can check each pad.",
        "Below it, the controllers an e-kit sends: the hi-hat pedal, and where each pad was struck. Pick the "
        "controller (CC) your module sends for each; its bar moves when it arrives. Invert is for pedals that send "
        "0 when closed. The E-Drums tab has the full setup for these." } };
    HelpButton velocityHelp { "Velocity", {
        "Changes how hard your hits come in, to suit your pads, before they pick a sound.",
        "The curve is for the drum last clicked in the list: velocity in across, velocity out up. Drag a dot to "
        "move it, click to add one, and double-click one to remove it. Curve offers Softer and Harder to start from.",
        "Global works on every drum: towards Softer the hardest hits play quieter, towards Harder the softest hits "
        "play louder.",
        "Only MIDI notes go through these: hits from your e-kit or DAW, and clicks on the keyboard here." } };
    bool showingEdrums = false;
    double deleteArmedUntil = 0.0, defaultSavedUntil = 0.0;

    // The mapping table
    juce::Viewport viewport;
    juce::Component list;
    std::vector<std::unique_ptr<Row>> rows;
    int learning = -1;                      // the articulation waiting for a note, -1 = none
    int selected = -1;                      // the articulation whose keys are outlined

    MidiKeyboard keyboard;

    // The velocity card
    VelocityCurveEditor curveEditor;
    juce::ComboBox curvePresets;
    ControlSlider globalVelocity;
    juce::Rectangle<int> velocityCard, velocityTitle, presetsLabel, globalLabel, globalMarks, globalRange;
    int velocityPiece = -1;                 // the piece whose curve shows, -1 = none
    juce::String keyboardScrolledFor;       // the map and kit the keyboard last scrolled to

    // The input monitor
    std::array<std::unique_ptr<ControllerRow>, 1 + numPositionSources> controllers;   // pedal, then positions
    juce::TextButton invertButton { "Invert" };
    juce::Rectangle<int> monitorArea, lastNoteArea, hintArea;
    std::uint32_t seenNote = 0;
    juce::String lastNoteText, lastTargetText;
    std::array<std::uint32_t, maxPieces> seenHits {};
};

} // namespace lithium
