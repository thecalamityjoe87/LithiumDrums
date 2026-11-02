#include "KitPage.h"

#include "Theme.h"

namespace lithium
{

//==============================================================================
/** The selected piece, over the stage's bottom left corner: its name, the
    model it plays (click to swap it), its volume, mute, solo and play, and
    a way to the Edit tab. With no kit, it says why and what to do. The
    controls are re-attached to whichever piece is selected. */
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
