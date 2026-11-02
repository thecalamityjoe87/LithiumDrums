#include "KitPage.h"

#include "Theme.h"

namespace lithium
{

//==============================================================================
/** The selected piece, over the stage's bottom left corner: its name, the
    model it plays (click to swap it), its volume, mute, solo and play, and
    a way to the Edit tab. With no kit, it says why and what to do. The
    controls are re-attached to whichever piece is selected. */
/** Beside the hi-hat on the stage: how open the e-kit's pedal has the
    hats, and lights that flash as notes and pedal messages come in, for
    setting up an e-kit. KitPage shows it once MIDI has arrived. */
class KitPage::HihatIndicator final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    static constexpr int width = 112, height = 66;

    explicit HihatIndicator (LithiumDrumsProcessor& p) : processor (p)
    {
        setTooltip ("The hi-hat pedal: how open it has the hats now, and MIDI activity. Set the pedal up under MIDI > E-Drums.");
        startTimerHz (30);
    }

    /** Called (once) when the first note or pedal message arrives. */
    std::function<void()> onFirstActivity;

    bool hasSeenActivity() const noexcept    { return seenNote != 0 || seenPedal != 0; }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (colours::panel.withAlpha (0.88f));
        g.fillRoundedRectangle (bounds, cornerRadius);
        g.setColour (colours::border);
        g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);

        auto area = getLocalBounds().reduced (space::sm, space::xs + 2);
        auto top = area.removeFromTop (14);
        g.setColour (colours::textMuted);
        drawTrackedText (g, "HI-HAT", label (type::caption), 0.12f, top.toFloat(), juce::Justification::centredLeft);
        g.setColour (openness >= 0.0f ? colours::text : colours::textMuted);
        g.setFont (mono (type::caption));
        g.drawText (openness < 0.0f   ? juce::String ("-")
                    : openness < 0.05f ? juce::String ("closed")
                                       : juce::String (juce::roundToInt (openness * 100.0f)) + "% open",
                    top, juce::Justification::centredRight, false);

        // Openness, closed at the left.
        area.removeFromTop (space::xs);
        const auto bar = area.removeFromTop (6).toFloat();
        g.setColour (colours::background);
        g.fillRoundedRectangle (bar, 3.0f);

        if (openness >= 0.0f)
        {
            g.setColour (colours::groupHats);
            g.fillRoundedRectangle (bar.withWidth (std::max (6.0f, bar.getWidth() * openness)), 3.0f);
        }

        // The activity lights.
        area.removeFromTop (space::sm);
        const auto now = juce::Time::getMillisecondCounterHiRes();

        auto light = [&] (juce::Rectangle<int> cell, const juce::String& text, bool lit)
        {
            g.setColour (lit ? colours::accent : colours::border);
            g.fillEllipse (cell.removeFromLeft (8).toFloat().withSizeKeepingCentre (6.0f, 6.0f));
            cell.removeFromLeft (space::xs);
            g.setColour (lit ? colours::text : colours::textMuted);
            drawTrackedText (g, text, label (type::caption), 0.08f, cell.toFloat(), juce::Justification::centredLeft);
        };

        light (area.removeFromLeft (area.getWidth() / 2), "NOTE", now < noteUntil);
        light (area, "PEDAL", now < pedalUntil);
    }

private:
    void timerCallback() override
    {
        const auto& fb = processor.getFeedback();
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto wasActive = hasSeenActivity();
        auto changed = false;

        if (const auto n = fb.lastNote.load (std::memory_order_relaxed); n != seenNote)
        {
            seenNote = n;
            noteUntil = now + theme::motion::hitFlashMs;
            changed = true;
        }

        if (const auto p = fb.pedalMessages.load (std::memory_order_relaxed); p != seenPedal)
        {
            seenPedal = p;
            pedalUntil = now + theme::motion::hitFlashMs;
            changed = true;
        }

        if (const auto o = fb.hihatOpenness.load (std::memory_order_relaxed); std::abs (o - openness) > 0.002f)
        {
            openness = o;
            changed = true;
        }

        if (! wasActive && hasSeenActivity() && onFirstActivity)
            onFirstActivity();

        // Repaint while a light is on, and once more as it goes out.
        const auto lit = now < std::max (noteUntil, pedalUntil);

        if (changed || lit != wasLit)
        {
            wasLit = lit;
            repaint();
        }
    }

    LithiumDrumsProcessor& processor;
    std::uint32_t seenNote = 0, seenPedal = 0;
    double noteUntil = 0.0, pedalUntil = 0.0;
    float openness = -1.0f;
    bool wasLit = false;
};

class KitPage::PieceCard final : public juce::Component, private SelectionModel::Listener
{
public:
    explicit PieceCard (LithiumDrumsProcessor& p) : processor (p), model (p)
    {
        using namespace theme::colours;

        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setDoubleClickReturnValue (true, 0.0);
        volume.getProperties().set ("unityValue", 0.0);
        volume.onValueChange = [this] { repaint (readoutArea); };

        mute.setClickingTogglesState (true);
        mute.setColour (juce::TextButton::buttonOnColourId, textMuted);
        solo.setClickingTogglesState (true);
        play.onClick = [this]
        {
            if (const auto piece = processor.getSelection().getSelected(); piece >= 0)
                processor.audition (piece, LithiumDrumsProcessor::auditionVelocity);
        };
        editButton.onClick = [this] { if (onEdit) onEdit(); };

        for (auto* c : std::initializer_list<juce::Component*> { &model, &volume, &mute, &solo, &play, &editButton })
            addAndMakeVisible (c);

        processor.getSelection().addListener (this);
    }

    ~PieceCard() override
    {
        processor.getSelection().removeListener (this);
    }

    std::function<void()> onEdit;

    void setKit (std::shared_ptr<const KitInfo> info)
    {
        kit = std::move (info);
        selectedPieceChanged (processor.getSelection().getSelected());
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

        g.setColour (colours::panel.withAlpha (0.9f));
        g.fillRoundedRectangle (bounds, cornerRadius + 2.0f);
        g.setColour (colours::borderStrong);
        g.drawRoundedRectangle (bounds, cornerRadius + 2.0f, 1.0f);

        auto area = getLocalBounds().reduced (space::lg, space::md).toFloat();
        auto top = area.removeFromTop (14.0f);
        const auto* p = piece();

        if (p != nullptr)
        {
            g.setColour (groupColour (info (p->role).group));
            g.fillEllipse (top.removeFromLeft (8.0f).withSizeKeepingCentre (8.0f, 8.0f));
            top.removeFromLeft (8.0f);
        }

        g.setColour (colours::textMuted);
        drawTrackedText (g, "SELECTED PIECE", label (12.0f), 0.1f, top, juce::Justification::centredLeft);
        area.removeFromTop (4.0f);

        g.setColour (colours::text);
        g.setFont (body (20.0f, Weight::semibold));

        if (p != nullptr)
        {
            g.drawText (p->name, area.removeFromTop (26.0f), juce::Justification::centredLeft, true);

            g.setColour (colours::textMuted);
            drawTrackedText (g, "VOLUME", label (type::caption), 0.1f, readoutArea.toFloat(), juce::Justification::centredLeft);

            const auto db = volume.getValue();
            g.setColour (colours::text);
            g.setFont (mono (type::caption));
            g.drawText ((db <= params::minVolumeDb + 0.05 ? juce::String ("-inf") : (db > 0.05 ? "+" : "") + juce::String (db, 1)) + " dB",
                        readoutArea, juce::Justification::centredRight, false);
            return;
        }

        // No piece: say why, and what to do about it.
        const auto loading = processor.isKitLoading();
        const auto error = processor.getKitError();
        g.drawText (loading ? "Loading kit..." : error.isNotEmpty() ? "The kit couldn't be loaded" : "No kit loaded",
                    area.removeFromTop (26.0f), juce::Justification::centredLeft, true);

        if (! loading)
        {
            g.setColour (colours::textMuted);
            g.setFont (body (type::small));
            g.drawFittedText (error.isNotEmpty() ? error + "\nPick another kit from the preset menu, or press Browse to download or re-install it."
                                                 : "Choose a kit from the preset menu at the top, or press Browse to download one.",
                              area.toNearestInt(), juce::Justification::topLeft, 6, 1.0f);
        }
    }

    void resized() override
    {
        using namespace theme;
        auto area = getLocalBounds().reduced (space::lg, space::md);
        area.removeFromTop (14 + 4 + 26 + space::sm);
        model.setBounds (area.removeFromTop (size::button));
        area.removeFromTop (space::md);
        readoutArea = area.removeFromTop (16);
        area.removeFromTop (space::xxs);
        volume.setBounds (area.removeFromTop (20).expanded (2, 0));

        area.removeFromTop (space::md);
        auto buttons = area.removeFromTop (size::button);
        editButton.setBounds (buttons.removeFromRight (96));
        buttons.removeFromRight (space::sm);
        const auto w = (buttons.getWidth() - 2 * space::xs) / 3;
        mute.setBounds (buttons.removeFromLeft (w));
        buttons.removeFromLeft (space::xs);
        solo.setBounds (buttons.removeFromLeft (w));
        buttons.removeFromLeft (space::xs);
        play.setBounds (buttons);
    }

    static constexpr int width = 280, height = 194;

private:
    class PlayButton final : public juce::Button
    {
    public:
        PlayButton() : juce::Button ("Play") {}

        void paintButton (juce::Graphics& g, bool isMouseOver, bool isDown) override
        {
            getLookAndFeel().drawButtonBackground (g, *this, {}, isMouseOver, isDown);
            const auto c = getLocalBounds().toFloat().getCentre();
            juce::Path triangle;
            triangle.addTriangle (c.x - 4.0f, c.y - 5.5f, c.x - 4.0f, c.y + 5.5f, c.x + 6.0f, c.y);
            g.setColour (isMouseOver ? theme::colours::accent : theme::colours::textMuted);
            g.fillPath (triangle);
        }
    };

    const KitPieceInfo* piece() const
    {
        const auto selected = processor.getSelection().getSelected();
        return kit != nullptr && juce::isPositiveAndBelow (selected, (int) kit->pieces.size()) ? &kit->pieces[(size_t) selected] : nullptr;
    }

    void selectedPieceChanged (int selected) override
    {
        // The old attachments let go of the controls before the new ones take them.
        volumeAttachment.reset();
        muteAttachment.reset();
        soloAttachment.reset();

        const auto* p = piece();

        for (auto* c : std::initializer_list<juce::Component*> { &model, &volume, &mute, &solo, &play, &editButton })
            c->setVisible (p != nullptr);

        if (p != nullptr)
        {
            auto& state = processor.getParameters();
            volumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, params::pieceParamId (selected, "volume"), volume);
            muteAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, params::pieceParamId (selected, "mute"), mute);
            soloAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, params::pieceParamId (selected, "solo"), solo);
            model.setPiece (kit, selected);
            volume.setColour (juce::Slider::trackColourId, theme::groupColour (info (p->role).group));
            volume.setTooltip (p->name + " volume (double-click for 0 dB, Shift-drag for fine steps)");
            mute.setTooltip ("Mute " + p->name);
            solo.setTooltip ("Solo " + p->name);
            play.setTooltip ("Play " + p->name);
            editButton.setTooltip ("Shape " + p->name + " and its channel on the Edit tab");
        }

        repaint();
    }

    LithiumDrumsProcessor& processor;
    std::shared_ptr<const KitInfo> kit;
    juce::Rectangle<int> readoutArea;

    ModelButton model;
    ControlSlider volume;
    juce::TextButton mute { "M" }, solo { "S" }, editButton { "Edit piece" };
    PlayButton play;

    // After the controls they're attached to (destroyed first).
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAttachment, soloAttachment;
};

//==============================================================================
KitPage::KitPage (LithiumDrumsProcessor& p)
    : processor (p), stage (p), card (std::make_unique<PieceCard> (p)), slots (p)
{
    addAndMakeVisible (stage);

    card->onEdit = [this] { if (onEditPiece) onEditPiece(); };
    addAndMakeVisible (*card);

    hihat = std::make_unique<HihatIndicator> (p);
    hihat->onFirstActivity = [this] { resized(); };
    addChildComponent (*hihat);

    slotView.setViewedComponent (&slots, false);   // not owned: a member
    slotView.setScrollBarsShown (true, false);
    slotView.setScrollBarThickness (8);
    slotView.getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, theme::colours::borderStrong);
    addChildComponent (slotView);

    // Stage | Slots, as one segmented switch.
    for (auto* b : { &stageButton, &slotsButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (3);
        addAndMakeVisible (b);
    }

    stageButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    slotsButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    stageButton.setTooltip ("The kit on stage: click a piece to select and play it");
    slotsButton.setTooltip ("A slot per piece: swap pieces in from your other kits to build your own");
    stageButton.onClick = [this] { if (stageButton.getToggleState()) showSlots (false); };
    slotsButton.onClick = [this] { if (slotsButton.getToggleState()) showSlots (true); };

    processor.addChangeListener (this);
    updateKitInfo();
    showSlots (processor.kitSlotsView);
}

KitPage::~KitPage()
{
    processor.removeChangeListener (this);
}

void KitPage::showSlots (bool s)
{
    // Kept in the processor, so the view survives closing the window.
    processor.kitSlotsView = s;
    (s ? slotsButton : stageButton).setToggleState (true, juce::dontSendNotification);
    stage.setVisible (! s);
    card->setVisible (! s);
    hihat->setVisible (false);   // resized() shows it where it belongs
    slotView.setVisible (s);
    resized();
    repaint();
}

void KitPage::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateKitInfo();
    card->repaint();   // loading and error messages
}

void KitPage::updateKitInfo()
{
    // Hold our own reference while using it, in case the processor swaps it.
    const auto info = processor.getKitInfo();

    if (info == shownKit)
        return;

    shownKit = info;
    stage.setKitInfo (info);
    card->setKit (info);
    slots.setKit (info, slotView.getLocalBounds().withWidth (slotView.getMaximumVisibleWidth()));
    resized();   // the hi-hat indicator follows the kit's hi-hat
    repaint();
}

void KitPage::paint (juce::Graphics& g)
{
    using namespace theme;

    if (! processor.kitSlotsView)
        return;   // the stage covers everything

    g.fillAll (colours::background);
    g.setColour (colours::textMuted);
    const auto title = "PIECES" + (shownKit != nullptr ? juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + shownKit->credits.name.toUpperCase()
                                                       : juce::String());
    drawTrackedText (g, title, label (12.5f), 0.1f, { (float) space::page, 0.0f, 600.0f, (float) slotView.getY() },
                     juce::Justification::centredLeft);
    g.setFont (body (type::small));
    g.drawText ("Click a model to swap in a piece from another kit, or Add to fill an empty slot.",
                juce::Rectangle<int> (space::page, 0, slotView.getWidth() - 2 * 76 - space::lg, slotView.getY()),
                juce::Justification::centredRight, true);

    if (shownKit == nullptr)
    {
        g.setFont (body (type::body));
        g.drawText (processor.isKitLoading() ? "Loading kit..." : "No kit loaded. Choose one from the preset menu, or press Browse to download one.",
                    slotView.getBounds(), juce::Justification::centred, true);
    }
}

void KitPage::resized()
{
    using namespace theme;

    stage.setBounds (getLocalBounds());
    // Bottom left: only floor and stand feet there, which can't be clicked anyway.
    card->setBounds (space::page, getHeight() - space::page - PieceCard::height, PieceCard::width, PieceCard::height);

    // The hi-hat indicator, just right of the hats' rod (the middle of the
    // hi-hat layer, which is the cymbals and the top of the rod), a little
    // above the cymbals. Only on the stage, with a hi-hat, once some MIDI has come in.
    const auto hatBounds = stage.getSlotBounds (StageSlot::hihat);
    const auto hasHat = shownKit != nullptr && shownKit->slotPiece[(size_t) index (StageSlot::hihat)] >= 0;
    hihat->setVisible (! processor.kitSlotsView && hasHat && hatBounds.has_value() && hihat->hasSeenActivity());

    if (hatBounds.has_value())
    {
        const auto x = juce::jlimit (space::page, getWidth() - HihatIndicator::width - space::page,
                                     juce::roundToInt (hatBounds->getCentreX()) + space::md);
        const auto y = std::max (space::md + size::button + space::md, juce::roundToInt (hatBounds->getY()) - HihatIndicator::height / 2 + space::xs);
        hihat->setBounds (x, y, HihatIndicator::width, HihatIndicator::height);
    }

    // The view switch, top right.
    auto switchArea = juce::Rectangle<int> (getWidth() - space::page - 2 * 76, space::md, 2 * 76, size::button);
    stageButton.setBounds (switchArea.removeFromLeft (76));
    slotsButton.setBounds (switchArea);

    const auto slotsTop = space::md + size::button + space::md;
    slotView.setBounds (getLocalBounds().withTrimmedTop (slotsTop).reduced (space::page, 0).withTrimmedBottom (space::md));

    if (slots.getWidth() != slotView.getMaximumVisibleWidth())
        slots.setKit (shownKit, slotView.getLocalBounds().withWidth (slotView.getMaximumVisibleWidth()));
}

//==============================================================================
ComingSoonPage::ComingSoonPage (juce::String t, juce::String d) : title (std::move (t)), description (std::move (d)) {}

void ComingSoonPage::paint (juce::Graphics& g)
{
    using namespace theme;

    g.fillAll (colours::background);

    auto area = getLocalBounds().toFloat().withSizeKeepingCentre (600.0f, 110.0f);

    g.setColour (colours::text);
    drawTrackedText (g, title.toUpperCase(), label (34.0f, Weight::bold), 0.12f,
                     area.removeFromTop (44.0f), juce::Justification::centred);

    g.setColour (colours::accent);
    drawTrackedText (g, "COMING SOON", label (14.0f), 0.2f, area.removeFromTop (26.0f), juce::Justification::centred);

    g.setColour (colours::textMuted);
    g.setFont (body (15.0f));
    g.drawText (description, area.removeFromTop (30.0f), juce::Justification::centred, true);
}

} // namespace lithium
