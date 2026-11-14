#pragma once

#include "InfoPanels.h"
#include "grooves/GrooveImporter.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lithium
{

/** Importing a folder of MIDI files as a groove pack (grooves::importMidiFolder),
    as one card over the UI that moves through its steps:

    - the form: the pack's name and the MIDI map its notes are written in,
      with a warning (and Import becoming Replace) when a pack of that name
      is already installed;
    - importing, while the files are read and copied on a background thread;
    - the summary: how many grooves, fills and styles came in, the styles as
      chips (click one to see it on the Grooves page), and the files left out;
    - or what went wrong.

    The Grooves page picks the folder (a file chooser, not drawn here) and
    the main view shows this card. */
class GrooveImportPanel final : public OverlayPanel, private juce::Timer
{
public:
    GrooveImportPanel();
    ~GrooveImportPanel() override;

    /** Starts over with the form, for this folder. */
    void showForm (const juce::File& folder);

    /** A pack was written: the Grooves page should read the folder again and show it. */
    std::function<void (const juce::File& packFolder)> onImported;

    /** From the summary: show only the new pack's grooves, or one of its styles. */
    std::function<void (const juce::String& packName)> onShowPack;
    std::function<void (const juce::String& styleKey)> onShowStyle;

    /** From a failed import: pick another folder. */
    std::function<void()> onChooseFolder;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override;

private:
    enum class Step { form, importing, done, failed };

    struct StyleCount
    {
        juce::String key, name;
        int count = 0;
        juce::Rectangle<float> bounds;     // where its chip is, once laid out
    };

    class LeftOutList;

    void layoutContent (juce::Rectangle<int>) override;
    void timerCallback() override;
    void setStep (Step);
    void updateReplaceWarning();
    void primaryClicked();
    void startImport();
    void finished (juce::Result, const grooves::ImportedPack&, std::vector<StyleCount>);
    juce::String mapName (const juce::String& id) const;

    Step step = Step::form;
    juce::File folder;
    bool replacing = false;

    // The maps a pack can be read through: the factory maps, but not Kit
    // default, which depends on the kit loaded. (id, name), in menu order.
    std::vector<std::pair<juce::String, juce::String>> maps;

    juce::TextEditor nameField;
    juce::ComboBox mapBox;
    juce::TextButton primaryButton;

    // The form's areas, worked out in layoutContent for paint.
    juce::Rectangle<int> messageArea, nameLabelArea, mapLabelArea, hintArea, warningArea;

    // The summary.
    grooves::ImportedPack imported;
    juce::String importedMap;
    std::vector<StyleCount> styles;
    int hiddenStyles = 0;                  // chips that didn't fit
    juce::Rectangle<float> moreBounds;     // where "+N more" goes
    int hoveredStyle = -1;
    juce::String error;
    juce::Rectangle<int> countsArea, stylesArea, leftOutLabelArea;
    std::unique_ptr<LeftOutList> leftOut;
    juce::Viewport leftOutView;

    // The importing step's moving bar.
    double barPhase = 0.0;

    // The import runs here, off the message thread. Declared last so it's
    // destroyed first: its destructor waits for a running import, whose
    // result then finds the panel gone (SafePointer) and is dropped.
    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withThreadName ("Groove import").withNumberOfThreads (1) };
};

} // namespace lithium
