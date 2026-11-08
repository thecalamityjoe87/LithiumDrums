#include "MidiPanel.h"

#include "Theme.h"

namespace lithium
{

namespace
{
    constexpr int toolbarHeight = 32;
    constexpr int headerRowHeight = 30, rowHeight = 34;
    constexpr int monitorWidth = 300, keyboardWidth = 300;
    constexpr int chipWidth = 62, chipGap = 6;
    constexpr double flashMs = 220.0;
    constexpr double confirmMs = 3000.0;
    constexpr int velocityCardHeight = 168;

    juce::String noteChipText (int note)
    {
        return juce::String (note) + " " + noteName (note);
    }

    juce::String followLabel (Follow f)
    {
        return f == Follow::pedal ? "PEDAL" : "POSITION";
    }
}

//==============================================================================
/** Velocity Adjust for one articulation: a small bar that fills from the
    middle, with the amount on it. Drag (Shift
    for fine steps) or scroll; double-click for 0. */
class MidiPanel::AdjustSlider final : public ControlSlider
{
public:
    AdjustSlider()
    {
        setSliderStyle (juce::Slider::LinearHorizontal);
        setRange (-PieceVelocity::maxAdjust, PieceVelocity::maxAdjust, 1.0);
        setDoubleClickReturnValue (true, 0.0);
        setTooltip ("Velocity Adjust: makes just this articulation hit harder or softer, before the piece's curve. "
                    "Drag or scroll; double-click for 0.");
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto box = getLocalBounds().toFloat().reduced (0.5f);
        const auto value = (float) getValue();
        const auto centre = box.getCentreX();
        const auto end = centre + (box.getWidth() * 0.5f - 2.0f) * value / (float) PieceVelocity::maxAdjust;

        g.setColour (colours::panel);
        g.fillRoundedRectangle (box, 4.0f);

        if (! juce::exactlyEqual (value, 0.0f))
        {
            g.setColour (colours::accent.withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (std::min (centre, end), box.getY() + 2.0f,
                                                                    std::max (centre, end), box.getBottom() - 2.0f));
        }

        g.setColour (isMouseOverOrDragging() || hasKeyboardFocus (false) ? colours::accent.withAlpha (0.7f) : colours::borderStrong);
        g.drawRoundedRectangle (box, 4.0f, 1.0f);

        const auto amount = juce::roundToInt (value);
        g.setColour (amount != 0 ? colours::text : colours::textMuted);
        g.setFont (mono (11.5f, amount != 0 ? Weight::medium : Weight::regular));
        g.drawText (amount > 0 ? "+" + juce::String (amount) : juce::String (amount), getLocalBounds(), juce::Justification::centred, false);
    }
};

//==============================================================================
/** One line of the mapping table: a piece's name (click it to show its
    velocity curve), or one of its articulations with its Velocity Adjust,
    notes, follow toggle and Learn button. */
class MidiPanel::Row final : public juce::Component,
                              public juce::SettableTooltipClient
{
public:
    // A piece header.
    Row (juce::String pieceName, juce::Colour dot) : name (std::move (pieceName)), colour (dot), header (true)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("Click to show this piece's velocity curve");
    }

    // An articulation.
    Row (int articulationIndex, juce::String articulationName, Follow canFollow)
        : name (std::move (articulationName)), articulation (articulationIndex), followKind (canFollow), header (false)
    {
        if (followKind != Follow::none)
        {
            followButton.setButtonText (followLabel (followKind));
            followButton.setTooltip (followKind == Follow::pedal
                                       ? "Hits follow the hi-hat pedal: its position picks how open the hats play"
                                       : "Hits follow the strike position the e-kit sends: centre to edge");
            followButton.onClick = [this] { if (onFollow) onFollow (followButton.getToggleState() ? Follow::none : followKind); };
            addChildComponent (followButton);
        }

        learnButton.setTooltip ("Click, then hit a pad, or click a key on the keyboard: that note will play this");
        learnButton.onClick = [this] { if (onLearn) onLearn(); };
        addAndMakeVisible (learnButton);

        adjust.onValueChange = [this] { if (onAdjust) onAdjust (juce::roundToInt (adjust.getValue())); };
        addAndMakeVisible (adjust);
    }

    std::function<void()> onLearn, onAudition, onSelect;
    std::function<void (Follow)> onFollow;
    std::function<void (int)> onRemoveNote, onAdjust;

    void setAdjust (int amount)    { adjust.setValue (amount, juce::dontSendNotification); }

    /** A header: whether the velocity card shows this piece. */
    void setSelected (bool s)
    {
        if (s != selected)
        {
            selected = s;
            repaint();
        }
    }

    bool isHeader() const noexcept          { return header; }
    int getArticulation() const noexcept    { return articulation; }

    /** The notes that play this, and which of them are spare notes (given
        automatically, not part of the map). */
    void setNotes (std::vector<int> n, std::vector<int> spareNotes, bool following)
    {
        notes = std::move (n);
        spares = std::move (spareNotes);

        // Following is a property of the map's notes; spare notes never follow.
        followButton.setToggleState (following, juce::dontSendNotification);
        followButton.setVisible (followKind != Follow::none && notes.size() > spares.size());
        resized();
        repaint();
    }

    void setLearning (bool l)
    {
        learning = l;
        learnButton.setToggleState (l, juce::dontSendNotification);
        learnButton.setButtonText (l ? "Hit a pad" : "Learn");
        repaint();
    }

    void flash()
    {
        flashUntil = juce::Time::getMillisecondCounterHiRes() + flashMs;
        repaint();
    }

    bool isFlashing() const    { return flashUntil > 0.0; }

    void stopFlashIfDone()
    {
        if (flashUntil > 0.0 && juce::Time::getMillisecondCounterHiRes() > flashUntil)
        {
            flashUntil = 0.0;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        auto area = getLocalBounds();

        if (header)
        {
            g.setColour (colour);
            g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ 10.0f, (float) area.getCentreY() + 2.0f }));
            g.setColour (selected ? colours::accent : colours::text);
            drawTrackedText (g, name.toUpperCase(), label (14.0f, Weight::semibold), 0.1f,
                             area.withTrimmedLeft (22).toFloat().withTrimmedTop (4.0f), juce::Justification::centredLeft);
            g.setColour (colours::border);
            g.fillRect (area.removeFromBottom (1));
            return;
        }

        const auto box = area.reduced (0, 2).toFloat();

        if (learning || isFlashing() || isMouseOver (true))
        {
            g.setColour (learning ? colours::raisedHigh : colours::raised);
            g.fillRoundedRectangle (box, 5.0f);
        }

        if (learning || isFlashing())
        {
            g.setColour (colours::accent.withAlpha (learning ? 1.0f : 0.6f));
            g.drawRoundedRectangle (box.reduced (0.5f), 5.0f, 1.0f);
        }

        g.setColour (colours::text);
        g.setFont (body (14.0f));
        g.drawText (name, getNameArea(), juce::Justification::centredLeft, true);

        if (notes.empty())
        {
            g.setColour (colours::textMuted.withAlpha (0.6f));
            g.setFont (mono (13.0f));
            g.drawText ("-", getChipArea(), juce::Justification::centredLeft, false);
        }

        if (chips.size() < notes.size())
        {
            g.setColour (colours::textMuted);
            g.setFont (mono (12.0f));
            g.drawText ("+" + juce::String (notes.size() - chips.size()), overflow, juce::Justification::centredLeft, false);
        }

        for (const auto& [rect, note] : chips)
        {
            const auto r = rect.toFloat();

            if (isSpare (note))
            {
                // Spare notes are dimmer, and can't be removed: Learn replaces them.
                g.setColour (colours::border);
                g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
                g.setColour (colours::textMuted);
                g.setFont (mono (12.5f));
                g.drawText (noteChipText (note), rect, juce::Justification::centred, false);
                continue;
            }

            const auto hover = rect.contains (getMouseXYRelative()) && isMouseOver();
            g.setColour (hover ? colours::warning.withAlpha (0.2f) : colours::panel);
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (hover ? colours::warning : colours::borderStrong);
            g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
            g.setColour (hover ? colours::warning : colours::text);
            g.setFont (mono (12.5f, Weight::medium));
            g.drawText (noteChipText (note), rect, juce::Justification::centred, false);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (6, 5);
        adjust.setBounds (area.withWidth (adjustWidth).withSizeKeepingCentre (adjustWidth, 20));
        learnButton.setBounds (area.removeFromRight (learnWidth));
        area.removeFromRight (8);
        followButton.setBounds (area.removeFromRight (followWidth));

        chips.clear();
        auto chipArea = getChipArea();

        for (const auto n : notes)
        {
            // More notes than fit: the rest show as "+2", and in the tooltip and on the keyboard.
            if (chipArea.getWidth() < chipWidth + (n != notes.back() ? chipWidth / 2 : 0))
                break;

            chips.emplace_back (chipArea.removeFromLeft (chipWidth).reduced (0, 2), n);
            chipArea.removeFromLeft (chipGap);
        }

        overflow = chipArea.withWidth (chipWidth / 2);
    }

    juce::String getTooltipText() const
    {
        if (notes.empty())
            return "Not played by any note. Click Learn, then hit a pad.";

        juce::StringArray names;

        for (const auto n : notes)
            names.add (noteChipText (n) + (isSpare (n) ? " (spare)" : ""));

        auto text = "Played by " + names.joinIntoString (", ") + ". Click a note to remove it; click the name to hear it and find its keys.";

        if (! spares.empty())
            text << "\nThe map doesn't reach this, so it has a spare note of its own. Learn a note to replace it.";

        return text;
    }

    void mouseMove (const juce::MouseEvent&) override     { repaint(); }
    void mouseEnter (const juce::MouseEvent&) override    { repaint(); }
    void mouseExit (const juce::MouseEvent&) override     { repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (header)
        {
            if (onSelect)
                onSelect();

            return;
        }

        for (const auto& [rect, note] : chips)
        {
            if (rect.contains (e.getPosition()))
            {
                if (onRemoveNote && ! isSpare (note))
                    onRemoveNote (note);

                return;
            }
        }

        if (getNameArea().contains (e.getPosition()) && onAudition)
            onAudition();
    }

private:
    // The adjust slider on the left, then the name.
    static constexpr int adjustWidth = 54, nameLeft = 6 + adjustWidth + 8;
    static constexpr int nameWidth = 130, learnWidth = 76, followWidth = 80;

    juce::Rectangle<int> getNameArea() const     { return getLocalBounds().withTrimmedLeft (nameLeft).withWidth (nameWidth); }
    bool isSpare (int note) const                { return std::find (spares.begin(), spares.end(), note) != spares.end(); }

    juce::Rectangle<int> getChipArea() const
    {
        return getLocalBounds().withTrimmedLeft (nameLeft + nameWidth + 8).withTrimmedRight (6 + learnWidth + 8 + followWidth + 12).reduced (0, 5);
    }

    const juce::String name;
    const juce::Colour colour;
    const int articulation = -1;
    const Follow followKind = Follow::none;
    const bool header;

    std::vector<int> notes, spares;
    std::vector<std::pair<juce::Rectangle<int>, int>> chips;
    juce::Rectangle<int> overflow;
    bool learning = false, selected = false;
    double flashUntil = 0.0;
    juce::TextButton followButton, learnButton { "Learn" };
    AdjustSlider adjust;
};

//==============================================================================
/** One controller in the input monitor: its name, CC number and a meter of
    where it is now. */
class MidiPanel::ControllerRow final : public juce::Component
{
public:
    ControllerRow (juce::String t, juce::String lowEnd, juce::String highEnd)
        : title (std::move (t)), low (std::move (lowEnd)), high (std::move (highEnd))
    {
        ccBox.addItem ("Off", 1);

        for (int cc = 0; cc < 128; ++cc)
            ccBox.addItem ("CC " + juce::String (cc), cc + 2);

        ccBox.onChange = [this] { if (onControllerChanged) onControllerChanged (ccBox.getSelectedId() - 2); };
        addAndMakeVisible (ccBox);
    }

    std::function<void (int)> onControllerChanged;   // -1 = off

    void setController (int cc)         { ccBox.setSelectedId (cc + 2, juce::dontSendNotification); }

    void setValue (float v)
    {
        if (std::abs (v - value) > 0.001f)
        {
            value = v;
            repaint();
        }
    }

    juce::Rectangle<int> getExtraArea() const    { return getLocalBounds().removeFromTop (24).withTrimmedRight (92).removeFromRight (62); }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        auto area = getLocalBounds();
        auto top = area.removeFromTop (24);

        g.setColour (colours::textMuted);
        drawTrackedText (g, title.toUpperCase(), label (12.5f, Weight::semibold), 0.12f, top.toFloat(), juce::Justification::centredLeft);

        area.removeFromTop (8);
        auto bar = area.removeFromTop (10).toFloat();
        g.setColour (colours::panel);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (colours::border);
        g.drawRoundedRectangle (bar.reduced (0.5f), 3.0f, 1.0f);

        if (value >= 0.0f)
        {
            g.setColour (colours::accent);
            g.fillRoundedRectangle (bar.withWidth (std::max (6.0f, bar.getWidth() * value)), 3.0f);
        }

        auto ends = area.removeFromTop (18);
        g.setFont (body (11.5f));
        g.setColour (colours::textMuted);
        g.drawText (low, ends, juce::Justification::centredLeft, false);
        g.drawText (high, ends, juce::Justification::centredRight, false);

        if (value < 0.0f)
            g.drawText ("not received", ends, juce::Justification::centred, false);
        else
        {
            g.setColour (colours::text);
            g.setFont (mono (11.5f));
            g.drawText (juce::String (juce::roundToInt (value * 100.0f)) + "%", ends, juce::Justification::centred, false);
        }
    }

    void resized() override
    {
        ccBox.setBounds (getLocalBounds().removeFromTop (24).removeFromRight (84));
    }

private:
    const juce::String title, low, high;
    juce::ComboBox ccBox;
    float value = -1.0f;
};

//==============================================================================
MidiPanel::MidiPanel (LithiumDrumsProcessor& p) : OverlayPanel ("MIDI Mapping", 1240, 770), processor (p), edrums (p)
{
    for (auto* b : { &mappingView, &edrumsView })
    {
        b->setRadioGroupId (4);
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }

    mappingView.setTooltip ("Which note plays what");
    edrumsView.setTooltip ("Set up an e-kit: the hi-hat pedal, positional sensing, and a monitor of the MIDI coming in");
    mappingView.setToggleState (true, juce::dontSendNotification);
    mappingView.onClick = [this] { showEdrums (false); };
    edrumsView.onClick = [this] { showEdrums (true); };
    addChildComponent (edrums);

    keyboard.onPlay = [this] (int note, int velocity)
    {
        // A key works like a pad: while a row is learning, it takes the note.
        if (learning >= 0)
        {
            processor.learnNote (note, learning);
            selectArticulation (learning);
            startLearning (-1);
        }

        processor.playNote (note, velocity);
    };
    addAndMakeVisible (keyboard);

    mapBox.setTooltip ("The MIDI map: which note plays what. E-kit maps follow the hi-hat pedal and positional sensing.");
    mapBox.onChange = [this] { chooseMap (mapBox.getSelectedId() - 1); };

    saveButton.setTooltip ("Save this map under a name, to use it in other projects");
    revertButton.setTooltip ("Throw away the changes and go back to the map as it was saved");
    deleteButton.setTooltip ("Delete this saved map");
    defaultButton.setTooltip ("New Lithium Drums instances start with this map");

    saveButton.onClick = [this] { showNameEditor (true); };
    revertButton.onClick = [this]
    {
        if (auto map = midimaps::find (processor.getMidiMap().id))
            processor.setMidiMap (*map, false);
    };
    deleteButton.onClick = [this]
    {
        // Deleting a file can't be undone, so the first click only arms it.
        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (now > deleteArmedUntil)
        {
            deleteArmedUntil = now + confirmMs;
            updateToolbar();
            return;
        }

        deleteArmedUntil = 0.0;
        const auto map = processor.getMidiMap();

        if (midimaps::remove (map.id))
        {
            // Keep playing it, as an unsaved map.
            auto copy = map;
            copy.id = {};
            processor.setMidiMap (copy, true);
        }

        refresh();
    };
    defaultButton.onClick = [this]
    {
        settings->setDefaultMidiMap (processor.getMidiMap().toJson());
        defaultSavedUntil = juce::Time::getMillisecondCounterHiRes() + confirmMs;
        updateToolbar();
    };

    nameEditor.setTextToShowWhenEmpty ("Name of the map", theme::colours::textMuted);
    nameEditor.setFont (theme::body (14.0f));
    nameEditor.onReturnKey = [this] { saveAs(); };
    nameEditor.onEscapeKey = [this] { showNameEditor (false); };
    confirmSave.onClick = [this] { saveAs(); };
    cancelSave.onClick = [this] { showNameEditor (false); };

    for (auto* c : std::initializer_list<juce::Component*> { &mapBox, &saveButton, &revertButton, &deleteButton, &defaultButton })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &nameEditor, &confirmSave, &cancelSave })
        addChildComponent (c);

    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (8);
    viewport.getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, theme::colours::borderStrong);
    addAndMakeVisible (viewport);

    controllers[0] = std::make_unique<ControllerRow> ("Hi-hat pedal", "closed", "open");
    controllers[1 + (size_t) PositionSource::snare] = std::make_unique<ControllerRow> ("Snare position", "centre", "edge");
    controllers[1 + (size_t) PositionSource::ride] = std::make_unique<ControllerRow> ("Ride position", "bell side", "edge");
    controllers[1 + (size_t) PositionSource::toms] = std::make_unique<ControllerRow> ("Tom position", "centre", "edge");
    controllers[1 + (size_t) PositionSource::hihat] = std::make_unique<ControllerRow> ("Hi-hat position", "bell side", "edge");

    controllers[0]->onControllerChanged = [this] (int cc) { setControllers ([cc] (auto& c) { c.hihatPedal = cc; }); };

    for (size_t i = 0; i < (size_t) numPositionSources; ++i)
        controllers[i + 1]->onControllerChanged = [this, i] (int cc) { setControllers ([cc, i] (auto& c) { c.position[i] = cc; }); };

    for (auto& c : controllers)
        addAndMakeVisible (*c);

    invertButton.setClickingTogglesState (true);
    invertButton.setTooltip ("For pedals that send 0 when closed and 127 when open");
    invertButton.onClick = [this] { setControllers ([this] (auto& c) { c.pedalInverted = invertButton.getToggleState(); }); };
    addAndMakeVisible (invertButton);

    // The velocity card.
    curveEditor.onChange = [this] (const VelocityCurve& curve)
    {
        editPieceVelocity (velocityPiece, [&curve] (PieceVelocity& pv) { pv.curve = curve; });
    };
    addAndMakeVisible (curveEditor);

    for (size_t i = 0; i < velocityCurvePresets.size(); ++i)
        curvePresets.addItem (velocityCurvePresets[i].name, (int) i + 1);

    curvePresets.setTooltip ("Ready-made curves to start from: Softer needs harder hits for the same level, Harder makes light hits louder");
    curvePresets.onChange = [this]
    {
        if (const auto i = curvePresets.getSelectedId() - 1; juce::isPositiveAndBelow (i, (int) velocityCurvePresets.size()))
            editPieceVelocity (velocityPiece, [i] (PieceVelocity& pv) { pv.curve = velocityCurvePresets[(size_t) i].make(); });
    };
    addAndMakeVisible (curvePresets);

    globalVelocity.setSliderStyle (juce::Slider::LinearHorizontal);
    globalVelocity.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    globalVelocity.setRange (-1.0, 1.0, 0.01);
    globalVelocity.setDoubleClickReturnValue (true, 0.0);
    globalVelocity.setColour (juce::Slider::trackColourId, theme::colours::accent);
    globalVelocity.setColour (juce::Slider::backgroundColourId, theme::colours::background);
    globalVelocity.setTooltip ("Global Velocity, for every piece and articulation. Softer: the hardest hit plays at 90. "
                               "Harder: the softest hit plays at 60. In the middle, the whole range is used. Double-click for the middle.");
    globalVelocity.onValueChange = [this]
    {
        auto velocity = processor.getMidiMap().velocity;
        velocity.global = (float) globalVelocity.getValue();
        processor.setVelocity (velocity);
    };
    addAndMakeVisible (globalVelocity);

    processor.addChangeListener (this);
    processor.getMidiMapBroadcaster().addChangeListener (this);
}

MidiPanel::~MidiPanel()
{
    processor.getMidiMapBroadcaster().removeChangeListener (this);
    processor.removeChangeListener (this);
}

void MidiPanel::showEdrums (bool show)
{
    showingEdrums = show;
    startLearning (-1);
    edrums.stopListening();
    setTitle (show ? "E-Drums" : "MIDI Mapping");
    edrums.setVisible (show);

    for (auto* c : std::initializer_list<juce::Component*> { &viewport, &keyboard, &invertButton, &curveEditor, &curvePresets, &globalVelocity })
        c->setVisible (! show);

    for (auto& c : controllers)
        c->setVisible (! show);

    repaint();
}

void MidiPanel::refresh()
{
    maps = midimaps::scan();
    mapBox.clear (juce::dontSendNotification);

    for (size_t i = 0; i < maps.size(); ++i)
    {
        if (i == MidiMap::factoryMaps().size())
            mapBox.addSeparator();

        mapBox.addItem (maps[i].map.name, (int) i + 1);
    }

    if (shownKit != processor.getKitInfo())
        rebuildRows();

    updateRows();
    updateToolbar();
}

void MidiPanel::visibilityChanged()
{
    // Only poll the input while the panel is showing.
    if (isVisible())
    {
        const auto& feedback = processor.getFeedback();
        seenNote = feedback.lastNote.load();

        for (size_t p = 0; p < seenHits.size(); ++p)
            seenHits[p] = feedback.pieceHits[p].load();

        // What came in while the panel was closed is old news.
        MidiMonitorEvent stale;

        while (processor.popMonitorEvent (stale)) {}

        edrums.refresh();
        startTimerHz (30);
    }
    else
    {
        stopTimer();
        startLearning (-1);
        edrums.stopListening();
        showNameEditor (false);
    }
}

void MidiPanel::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (! isVisible())
        return;

    if (source != &processor.getMidiMapBroadcaster() && shownKit != processor.getKitInfo())
    {
        startLearning (-1);
        rebuildRows();
    }

    updateRows();
    updateToolbar();
    edrums.refresh();
}

//==============================================================================
void MidiPanel::rebuildRows()
{
    rows.clear();
    shownKit = processor.getKitInfo();
    selected = -1;   // articulation indices belong to the kit
    velocityPiece = -1;

    if (shownKit != nullptr)
    {
        // The velocity card starts on the piece selected on the Kit page.
        const auto kitSelection = processor.getSelection().getSelected();
        velocityPiece = juce::isPositiveAndBelow (kitSelection, (int) shownKit->pieces.size()) ? kitSelection : 0;

        for (const auto& [p, piece] : juce::enumerate (shownKit->pieces))
        {
            auto header = std::make_unique<Row> (piece.name, theme::groupColour (info (piece.role).group));
            header->onSelect = [this, pieceIndex = (int) p] { selectPiece (pieceIndex); };
            rows.push_back (std::move (header));

            for (const auto& [a, art] : juce::enumerate (piece.articulations))
            {
                const auto index = piece.firstArticulation + (int) a;
                const auto follow = piece.canFollow ((int) a, Follow::pedal)      ? Follow::pedal
                                  : piece.canFollow ((int) a, Follow::position)   ? Follow::position
                                                                                  : Follow::none;
                auto row = std::make_unique<Row> (index, art.name, follow);
                const auto pieceIndex = (int) p, local = (int) a;

                row->onLearn = [this, index] { startLearning (learning == index ? -1 : index); };
                row->onAudition = [this, pieceIndex, local, index]
                {
                    processor.audition (pieceIndex, LithiumDrumsProcessor::auditionVelocity, local);
                    selectArticulation (index);
                    selectPiece (pieceIndex);
                };
                row->onAdjust = [this, pieceIndex, index] (int amount)
                {
                    editPieceVelocity (pieceIndex, [this, index, amount] (PieceVelocity& pv) { setVelocityAdjust (pv, *shownKit, index, amount); });
                };
                row->onRemoveNote = [this] (int note) { processor.forgetNote (note); };
                row->onFollow = [this, index] (Follow f) { processor.setFollow (index, f); };
                rows.push_back (std::move (row));
            }
        }
    }

    for (auto& r : rows)
        list.addAndMakeVisible (*r);

    resized();
}

void MidiPanel::updateRows()
{
    const auto table = processor.getNoteTable();
    const auto map = processor.getMidiMap();

    for (auto& r : rows)
    {
        if (r->isHeader())
            continue;

        if (shownKit != nullptr)
        {
            const auto piece = pieceAndArticulation (*shownKit, r->getArticulation()).first;
            const auto* pv = findPieceVelocity (map.velocity, *shownKit, piece);
            r->setAdjust (pv != nullptr ? velocityAdjustFor (*pv, *shownKit, r->getArticulation()) : 0);
        }

        std::vector<int> notes, spares;
        bool following = false;

        for (int n = 0; n < 128; ++n)
        {
            if (const auto& t = table.targets[(size_t) n]; t.articulation == r->getArticulation())
            {
                notes.push_back (n);
                following = following || t.follow != Follow::none;

                if (t.spare)
                    spares.push_back (n);
            }
        }

        r->setNotes (std::move (notes), std::move (spares), following);
        r->setTooltip (r->getTooltipText());
    }

    updateKeyboard (table);
    updateVelocity();

    controllers[0]->setController (map.controllers.hihatPedal);
    invertButton.setToggleState (map.controllers.pedalInverted, juce::dontSendNotification);

    for (size_t i = 0; i < (size_t) numPositionSources; ++i)
        controllers[i + 1]->setController (map.controllers.position[i]);
}

void MidiPanel::updateToolbar()
{
    const auto map = processor.getMidiMap();
    const auto it = std::find_if (maps.begin(), maps.end(), [&] (const auto& m) { return m.map.id == map.id; });

    if (it != maps.end())
        mapBox.setSelectedId ((int) (it - maps.begin()) + 1, juce::dontSendNotification);
    else
        mapBox.setText (map.name.isNotEmpty() ? map.name : juce::String ("Unsaved map"), juce::dontSendNotification);

    const auto now = juce::Time::getMillisecondCounterHiRes();
    revertButton.setEnabled (processor.isMidiMapModified() && it != maps.end());
    deleteButton.setEnabled (it != maps.end() && ! it->factory);
    deleteButton.setButtonText (now < deleteArmedUntil ? "Really delete?" : "Delete");
    defaultButton.setButtonText (now < defaultSavedUntil ? "Saved as default" : "Set as default");
    repaint (modifiedTag);
}

void MidiPanel::chooseMap (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) maps.size()))
        return;

    startLearning (-1);
    processor.setMidiMap (maps[(size_t) index].map, false);
}

void MidiPanel::setControllers (const std::function<void (ControllerSettings&)>& change)
{
    auto c = processor.getMidiMap().controllers;
    change (c);
    processor.setControllers (c);
}

//==============================================================================
void MidiPanel::selectPiece (int piece)
{
    if (piece == velocityPiece)
        return;

    velocityPiece = piece;
    curveEditor.showHit (-1, -1);
    updateVelocity();
}

void MidiPanel::updateVelocity()
{
    const auto map = processor.getMidiMap();
    const auto hasPiece = shownKit != nullptr && juce::isPositiveAndBelow (velocityPiece, (int) shownKit->pieces.size());
    const auto* pv = hasPiece ? findPieceVelocity (map.velocity, *shownKit, velocityPiece) : nullptr;
    const auto curve = pv != nullptr ? pv->curve : VelocityCurve {};

    curveEditor.setEnabled (hasPiece);
    curvePresets.setEnabled (hasPiece);
    globalVelocity.setEnabled (shownKit != nullptr);
    curveEditor.setCurve (curve, map.velocity.global);
    globalVelocity.setValue (map.velocity.global, juce::dontSendNotification);

    // The preset the curve is, or "Custom" once it's been edited.
    const auto preset = std::find_if (velocityCurvePresets.begin(), velocityCurvePresets.end(), [&] (const auto& p) { return p.make() == curve; });

    if (preset != velocityCurvePresets.end())
        curvePresets.setSelectedId ((int) (preset - velocityCurvePresets.begin()) + 1, juce::dontSendNotification);
    else
        curvePresets.setText ("Custom", juce::dontSendNotification);

    // The list's header for the piece shows it's the one on the card.
    int piece = -1;

    for (auto& r : rows)
        if (r->isHeader())
            r->setSelected (++piece == velocityPiece);

    repaint (velocityCard);
}

void MidiPanel::editPieceVelocity (int piece, const std::function<void (PieceVelocity&)>& change)
{
    if (shownKit == nullptr || ! juce::isPositiveAndBelow (piece, (int) shownKit->pieces.size()))
        return;

    auto velocity = processor.getMidiMap().velocity;
    PieceVelocity pv;

    if (const auto* saved = findPieceVelocity (velocity, *shownKit, piece))
        pv = *saved;

    change (pv);
    setPieceVelocity (velocity, *shownKit, piece, std::move (pv));
    processor.setVelocity (velocity);
}

void MidiPanel::startLearning (int articulation)
{
    learning = articulation;

    for (auto& r : rows)
        if (! r->isHeader())
            r->setLearning (r->getArticulation() == learning);

    keyboard.setHighlighted (notesOf (learning >= 0 ? learning : selected));
}

void MidiPanel::selectArticulation (int articulation)
{
    selected = articulation;
    const auto notes = notesOf (selected);
    keyboard.setHighlighted (notes);

    if (! notes.empty())
        keyboard.scrollToShow (notes.front());
}

std::vector<int> MidiPanel::notesOf (int articulation) const
{
    std::vector<int> notes;

    if (articulation < 0)
        return notes;

    const auto table = processor.getNoteTable();

    for (int n = 0; n < 128; ++n)
        if (table.targets[(size_t) n].articulation == articulation)
            notes.push_back (n);

    return notes;
}

void MidiPanel::updateKeyboard (const NoteTable& table)
{
    std::array<MidiKeyboard::Key, 128> keys {};

    if (shownKit != nullptr)
    {
        for (int n = 0; n < 128; ++n)
        {
            const auto& target = table.targets[(size_t) n];
            const auto [p, a] = pieceAndArticulation (*shownKit, target.articulation);

            if (p < 0)
                continue;

            const auto& piece = shownKit->pieces[(size_t) p];
            const auto& art = piece.articulations[(size_t) a];
            auto& key = keys[(size_t) n];
            key.label = piece.name + " " + art.name;
            key.colour = theme::groupColour (info (piece.role).group);
            key.spare = target.spare;
            key.detail = piece.name + ", " + art.name
                         + (target.spare ? " (spare note: the map doesn't reach it)"
                            : target.follow == Follow::pedal ? " (follows the pedal)"
                            : target.follow == Follow::position ? " (follows the position)" : "");
        }

        // Cymbal choke notes play nothing, but they're notes the kit uses.
        for (const auto& piece : shownKit->pieces)
        {
            if (const auto c = piece.defaultChokeNote; juce::isPositiveAndBelow (c, 128) && keys[(size_t) c].label.isEmpty())
            {
                keys[(size_t) c].label = piece.name + " choke";
                keys[(size_t) c].colour = theme::groupColour (info (piece.role).group).withMultipliedSaturation (0.4f);
                keys[(size_t) c].detail = "chokes " + piece.name;
            }
        }
    }

    keyboard.setKeys (keys);
    keyboard.setHighlighted (notesOf (learning >= 0 ? learning : selected));

    // Show where the map's notes are when the map or kit changes, but not
    // after every edit, which would jump around while learning.
    const auto scrolledFor = processor.getMidiMap().id + "/" + (shownKit != nullptr ? juce::String ((juce::int64) shownKit->serial) : juce::String());

    if (scrolledFor != keyboardScrolledFor)
    {
        keyboardScrolledFor = scrolledFor;
        keyboard.scrollToMapped();
    }
}

void MidiPanel::showNameEditor (bool show)
{
    for (auto* c : std::initializer_list<juce::Component*> { &saveButton, &revertButton, &deleteButton, &defaultButton })
        c->setVisible (! show);

    for (auto* c : std::initializer_list<juce::Component*> { &nameEditor, &confirmSave, &cancelSave })
        c->setVisible (show);

    if (show)
    {
        const auto map = processor.getMidiMap();
        nameEditor.setText (midimaps::isFactory (map.id) ? map.name + " (mine)" : map.name, false);
        nameEditor.selectAll();
        nameEditor.grabKeyboardFocus();
    }
    else if (isShowing())
    {
        grabKeyboardFocus();   // so Escape still closes the panel
    }
}

void MidiPanel::saveAs()
{
    MidiMap saved;

    if (const auto r = midimaps::save (processor.getMidiMap(), nameEditor.getText(), saved); r.failed())
    {
        nameEditor.setTextToShowWhenEmpty (r.getErrorMessage(), theme::colours::warning);
        nameEditor.clear();
        return;
    }

    showNameEditor (false);
    processor.setMidiMap (saved, false);
    refresh();
}

bool MidiPanel::keyPressed (const juce::KeyPress& key)
{
    // Escape first stops learning, and only then closes the panel.
    if (key == juce::KeyPress::escapeKey && learning >= 0)
    {
        startLearning (-1);
        return true;
    }

    return OverlayPanel::keyPressed (key);
}

//==============================================================================
void MidiPanel::timerCallback()
{
    const auto& feedback = processor.getFeedback();

    // The monitor, and the E-Drums view's Learn and Calibrate, see every message.
    for (MidiMonitorEvent e; processor.popMonitorEvent (e);)
        edrums.handle (e);

    edrums.tick();

    // A new note: show it, and if a row is learning, give it the note.
    if (const auto bits = feedback.lastNote.load(); bits != seenNote)
    {
        seenNote = bits;

        if (const auto n = EngineFeedback::unpackNote (bits))
        {
            const auto [note, velocity] = *n;
            lastNoteText = noteChipText (note) + "   vel " + juce::String (velocity);
            lastTargetText = "not mapped";

            if (shownKit != nullptr)
            {
                const auto table = processor.getNoteTable();
                const auto target = table.targets[(size_t) note];
                const auto [p, a] = pieceAndArticulation (*shownKit, target.articulation);

                // What the velocity settings made of it (without the edge
                // compensation, which depends on where the pad was hit).
                if (p >= 0)
                {
                    const auto out = table.velocityFor (note, p, velocity);

                    if (out != velocity)
                        lastNoteText << juce::String (juce::CharPointer_UTF8 (" \xe2\x86\x92 ")) << out;

                    if (p == velocityPiece)
                        curveEditor.showHit (velocity, out);
                }

                if (p >= 0)
                    lastTargetText = shownKit->pieces[(size_t) p].name + ", " + shownKit->pieces[(size_t) p].articulations[(size_t) a].name
                                     + (target.spare ? " (spare note)"
                                        : target.follow == Follow::pedal ? " (follows the pedal)"
                                        : target.follow == Follow::position ? " (follows the position)" : "");
            }

            if (learning >= 0)
            {
                processor.learnNote (note, learning);
                selectArticulation (learning);
                startLearning (-1);
            }

            keyboard.flash (note);
            keyboard.scrollToShow (note);
            repaint (lastNoteArea);
        }
    }

    // Flash the rows of what just played, which with a following note is
    // the step the pedal or position picked.
    if (shownKit != nullptr)
    {
        for (size_t p = 0; p < shownKit->pieces.size() && p < seenHits.size(); ++p)
        {
            const auto hits = feedback.pieceHits[p].load (std::memory_order_acquire);

            if (hits == seenHits[p])
                continue;

            seenHits[p] = hits;

            if (const auto hit = LastHit::unpack (feedback.lastHit[p].load (std::memory_order_relaxed)))
                for (auto& r : rows)
                    if (! r->isHeader() && r->getArticulation() == hit->articulation)
                        r->flash();
        }
    }

    for (auto& r : rows)
        r->stopFlashIfDone();

    keyboard.tick();

    controllers[0]->setValue (feedback.hihatOpenness.load (std::memory_order_relaxed));

    for (size_t i = 0; i < (size_t) numPositionSources; ++i)
        controllers[i + 1]->setValue (feedback.position[i].load (std::memory_order_relaxed));

    const auto now = juce::Time::getMillisecondCounterHiRes();

    if ((deleteArmedUntil > 0.0 && now > deleteArmedUntil) || (defaultSavedUntil > 0.0 && now > defaultSavedUntil))
    {
        deleteArmedUntil = defaultSavedUntil = 0.0;
        updateToolbar();
    }
}

//==============================================================================
void MidiPanel::paint (juce::Graphics& g)
{
    OverlayPanel::paint (g);
    using namespace theme;

    if (processor.isMidiMapModified())
    {
        g.setColour (colours::accent);
        drawTrackedText (g, "MODIFIED", label (12.0f, Weight::semibold), 0.12f, modifiedTag.toFloat(), juce::Justification::centredLeft);
    }

    if (showingEdrums)
        return;

    // The monitor column.
    g.setColour (colours::raised);
    g.fillRoundedRectangle (monitorArea.toFloat(), 8.0f);
    g.setColour (colours::border);
    g.drawRoundedRectangle (monitorArea.toFloat().reduced (0.5f), 8.0f, 1.0f);

    auto area = lastNoteArea;
    g.setColour (colours::textMuted);
    drawTrackedText (g, "INPUT", label (12.5f, Weight::semibold), 0.12f, area.removeFromTop (22).toFloat(), juce::Justification::centredLeft);

    g.setColour (colours::text);
    g.setFont (mono (18.0f, Weight::medium));
    g.drawText (lastNoteText.isNotEmpty() ? lastNoteText : juce::String ("-"), area.removeFromTop (30), juce::Justification::centredLeft, true);
    g.setColour (colours::textMuted);
    g.setFont (body (13.5f));
    g.drawFittedText (lastNoteText.isNotEmpty() ? lastTargetText : juce::String ("Play a pad or key to see what it plays."),
                      area.removeFromTop (36), juce::Justification::topLeft, 2, 0.9f);

    g.setFont (body (12.5f));
    g.drawFittedText ("Learn: click Learn on a row, then hit the pad or click a key. Hits that follow the pedal or the "
                      "strike position play their own sound until the e-kit sends that controller.",
                      hintArea, juce::Justification::topLeft, 4, 1.0f);

    // The velocity card.
    g.setColour (colours::raised);
    g.fillRoundedRectangle (velocityCard.toFloat(), 8.0f);
    g.setColour (colours::border);
    g.drawRoundedRectangle (velocityCard.toFloat().reduced (0.5f), 8.0f, 1.0f);

    const auto pieceName = shownKit != nullptr && juce::isPositiveAndBelow (velocityPiece, (int) shownKit->pieces.size())
                             ? shownKit->pieces[(size_t) velocityPiece].name : juce::String();
    g.setColour (colours::textMuted);
    drawTrackedText (g, "VELOCITY", label (12.5f, Weight::semibold), 0.12f, velocityTitle.toFloat(), juce::Justification::centredLeft);
    g.setColour (colours::text);
    drawTrackedText (g, pieceName.toUpperCase(), label (14.0f, Weight::semibold), 0.1f,
                     velocityTitle.toFloat().withTrimmedLeft (84.0f), juce::Justification::centredLeft);

    g.setColour (colours::textMuted);
    g.setFont (body (12.5f));
    g.drawText ("Curve", presetsLabel, juce::Justification::centredLeft, false);
    drawTrackedText (g, "GLOBAL", label (12.0f, Weight::semibold), 0.12f, globalLabel.toFloat(), juce::Justification::centredLeft);

    g.setFont (body (11.5f));
    g.drawText ("Softer", globalMarks, juce::Justification::centredLeft, false);
    g.drawText ("Default", globalMarks, juce::Justification::centred, false);
    g.drawText ("Harder", globalMarks, juce::Justification::centredRight, false);

    const auto global = (float) globalVelocity.getValue();
    g.setColour (colours::text);
    g.setFont (mono (12.0f));
    g.drawText ("plays " + juce::String (juce::roundToInt (applyGlobalVelocity (1.0f, global))) + " to "
                    + juce::String (juce::roundToInt (applyGlobalVelocity (127.0f, global))),
                globalRange, juce::Justification::centredLeft, false);

    if (shownKit == nullptr)
    {
        g.setFont (body (15.0f));
        g.drawText ("Load a kit to map it.", viewport.getBounds(), juce::Justification::centred, false);
    }
}

void MidiPanel::layoutContent (juce::Rectangle<int> area)
{
    auto toolbar = area.removeFromTop (toolbarHeight);
    area.removeFromTop (16);

    mapBox.setBounds (toolbar.removeFromLeft (260));
    toolbar.removeFromLeft (10);
    modifiedTag = toolbar.removeFromLeft (80);
    mappingView.setBounds (toolbar.removeFromLeft (96));
    edrumsView.setBounds (toolbar.removeFromLeft (96));
    edrums.setBounds (area);

    for (auto* b : { &defaultButton, &deleteButton, &revertButton, &saveButton })
    {
        b->setBounds (toolbar.removeFromRight (b == &defaultButton ? 132 : 104));
        toolbar.removeFromRight (8);
    }

    auto editorRow = juce::Rectangle<int> (saveButton.getX(), mapBox.getY(), defaultButton.getRight() - saveButton.getX(), toolbarHeight);
    cancelSave.setBounds (editorRow.removeFromRight (90));
    editorRow.removeFromRight (8);
    confirmSave.setBounds (editorRow.removeFromRight (90));
    editorRow.removeFromRight (8);
    nameEditor.setBounds (editorRow);

    // The monitor on the left, the articulations in the middle and the
    // keyboard on the right, next to the rows it maps.
    monitorArea = area.removeFromLeft (monitorWidth);
    area.removeFromLeft (20);
    keyboard.setBounds (area.removeFromRight (keyboardWidth));
    area.removeFromRight (20);

    // The velocity card above the list: the curve on the left, the piece's
    // name, its preset curves and the global velocity on the right.
    velocityCard = area.removeFromTop (velocityCardHeight);
    area.removeFromTop (10);
    viewport.setBounds (area);

    {
        auto card = velocityCard.reduced (14, 10);
        curveEditor.setBounds (card.removeFromLeft (250));
        card.removeFromLeft (18);

        velocityTitle = card.removeFromTop (24);
        card.removeFromTop (8);
        auto row = card.removeFromTop (theme::size::button);
        presetsLabel = row.removeFromLeft (46);
        curvePresets.setBounds (row.withWidth (std::min (row.getWidth(), 150)));
        card.removeFromTop (16);
        auto globalTitle = card.removeFromTop (18);
        globalLabel = globalTitle.removeFromLeft (70);
        globalRange = globalTitle;
        card.removeFromTop (2);
        globalVelocity.setBounds (card.removeFromTop (20));
        globalMarks = card.removeFromTop (16);
    }

    auto monitor = monitorArea.reduced (18, 16);
    lastNoteArea = monitor.removeFromTop (92);
    monitor.removeFromTop (14);

    // The pedal, then the pads in their display order.
    controllers[0]->setBounds (monitor.removeFromTop (64));
    monitor.removeFromTop (14);

    for (const auto source : positionDisplayOrder)
    {
        controllers[1 + (size_t) source]->setBounds (monitor.removeFromTop (64));
        monitor.removeFromTop (14);
    }

    invertButton.setBounds (controllers[0]->getExtraArea() + controllers[0]->getPosition());
    hintArea = monitor;

    const auto width = viewport.getWidth() - 12;   // room for the scroll bar
    int y = 0;

    for (auto& r : rows)
    {
        const auto h = r->isHeader() ? headerRowHeight : rowHeight;
        r->setBounds (0, y + (r->isHeader() && y > 0 ? 8 : 0), width, h);
        y = r->getBottom();
    }

    list.setSize (width, y);
}

} // namespace lithium
