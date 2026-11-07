#pragma once

#include "PluginProcessor.h"
#include "grooves/GrooveLibrary.h"
#include "packs/PackInstaller.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lithium
{

/** The Grooves tab: recorded MIDI grooves to preview through the kit and
    drag into the host.

    Styles run across the top as chips; the grooves (whole recorded takes)
    are listed on the left with search, type and tempo filters; the selected
    groove's phrases and fills are cards on the right, with the selected
    card's every hit in a detail pane under them. Clicking a card (or the
    detail pane) plays it, in time with the host; dragging it (or the handle
    in the transport bar) drops it into the host as a MIDI file, in the notes
    of the current MIDI map.

    The groove library is read from the grooves folder the first time the
    tab is shown, and again with Rescan. With no grooves installed, the
    catalog's groove pack is shown greyed out, with a Download button. */
class GroovesPage final : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    explicit GroovesPage (LithiumDrumsProcessor&);
    ~GroovesPage() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

    /** What can be selected and played: a phrase, a fill or the whole take. */
    struct Part
    {
        enum class Kind { whole, phrase, fill };

        Kind kind = Kind::phrase;
        int index = 0;

        bool operator== (const Part&) const = default;
    };

    //==============================================================================
    // For the groove controls on the Kit tab's stage. They work this page's
    // selection rather than keeping one of their own, so the two always agree.

    /** What the stage card shows: the part playing, or else the one Play would play. */
    struct NowPlaying
    {
        juce::String groove, part;            // "Funk 4", "Bars 5-8"; empty with no grooves
        double lengthBeats = 0.0, beatsPerBar = 4.0;
        double position = -1.0;               // beats into the part while it plays, else -1
        float bpm = 0.0f;                     // what it plays at, or would
        bool playing = false, hostSynced = false;
        bool hasPrevious = false, hasNext = false;   // another tile to step to
    };

    /** Reads the groove library, if it hasn't been yet (it's read the first
        time it's needed). True if it was read now. */
    bool scanIfNeeded()
    {
        if (scanned)
            return false;

        rescan();
        return true;
    }
    bool hasGrooves() const noexcept          { return library.numGrooves() > 0; }

    NowPlaying nowPlaying() const;

    /** Plays the selected tile, or stops what plays. */
    void togglePlay();

    /** Selects the previous (-1) or next (+1) tile, in the order the page
        shows them, and plays it if a groove is playing. */
    void stepPart (int delta);

private:
    class StyleChips;
    class GrooveList;
    class TileGrid;
    class DetailPane;
    class TempoField;
    class DragHandle;

    struct GrooveRef
    {
        int pack, groove;

        bool operator== (const GrooveRef&) const = default;
    };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    const packs::CatalogPack* missingGroovePack() const;
    void rescan();
    void applyFilter();
    bool passesFilter (const grooves::Pack&, const grooves::Groove&) const;
    void selectGroove (GrooveRef, bool keepPlaying);
    void selectPart (Part, bool play);
    void updateTransportButtons();
    void showDetail();
    std::array<std::optional<PieceRole>, 128> roleTable() const;

    const grooves::Pack* selectedPack() const;
    const grooves::Groove* selectedGroove() const;
    const grooves::Groove* grooveAt (GrooveRef) const;
    std::vector<Part> partsShown() const;
    float tempoFor (const grooves::Groove*) const;
    std::shared_ptr<const grooves::Clip> makeClip (Part) const;
    juce::File writeMidi (Part) const;
    void saveMidi();
    juce::String describe (Part) const;

    LithiumDrumsProcessor& processor;
    grooves::Library library;
    bool scanned = false;

    std::vector<GrooveRef> filtered;          // the grooves the list shows
    GrooveRef selected { -1, -1 };
    Part selectedPart;

    // The selected groove's hits, and its fills', read when it's selected.
    std::vector<grooves::Hit> takeHits;
    std::vector<std::vector<grooves::Hit>> fillHits;

    // The part playing, and the clip id it was sent with, to match the
    // player's feedback against.
    GrooveRef playingGroove { -1, -1 };
    Part playingPart;
    int playingClipId = -1, nextClipId = 0;

    juce::String styleFilter;                 // a style key, or empty for all
    juce::TextEditor search;
    juce::TextButton beatsButton { "Beats" }, fillsButton { "Fills" };
    juce::Slider tempoRange { juce::Slider::TwoValueHorizontal, juce::Slider::NoTextBox };
    juce::TextButton rescanButton { "Rescan" };
    juce::SharedResourcePointer<packs::Installer> installer;
    juce::TextButton downloadButton;

    std::unique_ptr<StyleChips> chips;
    std::unique_ptr<GrooveList> list;
    juce::Viewport tileView;
    std::unique_ptr<TileGrid> tiles;
    std::unique_ptr<DetailPane> detail;

    // Transport.
    juce::TextButton playButton { "Play" }, loopButton { "Loop" };
    juce::TextButton hostTempoButton { "Host" }, grooveTempoButton { "Groove" }, customTempoButton { "Custom" };
    juce::TextButton halfButton, normalButton, doubleButton;
    std::unique_ptr<TempoField> tempoField;
    juce::TextButton saveButton { "Save MIDI..." };
    std::unique_ptr<DragHandle> dragHandle;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> leftArea, rightArea, transportArea, emptyArea, statusArea, grooveHeaderArea;
    juce::String status, statusText;   // a problem to report; what the status line shows
};

} // namespace lithium
