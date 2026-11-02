#pragma once

#include "EdrumsView.h"
#include "InfoPanels.h"
#include "MidiKeyboard.h"
#include "MidiMapLibrary.h"

namespace lithium
{

/** MIDI mapping: which map plays the kit, what each of the kit's
    articulations is played by, and what's coming in.

    The toolbar picks a map (Kit default, General MIDI, the e-kit maps and
    the user's saved ones) and saves, reverts, deletes or makes default the
    current one. Below it, three columns, laid out like Addictive Drums 2's:

     - the input monitor: the last note and what it played, and the pedal
       and position controllers with their CC numbers;
     - every articulation of the loaded kit with its notes: Learn assigns
       the next note that comes in (or the next key clicked), a note chip
       removes its note, and the follow button makes the articulation's
       notes follow the hi-hat pedal or the strike position;
     - the keyboard: every note labelled with what it plays.

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
    bool showingEdrums = false;
    double deleteArmedUntil = 0.0, defaultSavedUntil = 0.0;

    // The mapping table
    juce::Viewport viewport;
    juce::Component list;
    std::vector<std::unique_ptr<Row>> rows;
    int learning = -1;                      // the articulation waiting for a note, -1 = none
    int selected = -1;                      // the articulation whose keys are outlined

    MidiKeyboard keyboard;
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
