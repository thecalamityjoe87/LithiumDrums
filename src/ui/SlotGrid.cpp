#include "SlotGrid.h"

#include "KitArt.h"
#include "Theme.h"
#include "engine/MidiMapping.h"

namespace lithium
{

namespace
{
    /** The render of a percussion piece, found by its name; null if there's none. */
    const char* percussionPicture (const juce::String& words)
    {
        // A word in the piece's id or name, and the picture it means.
        static constexpr std::pair<const char*, const char*> byWord[] {
            { "cowbell", "cowbell" }, { "chime", "chimes" }, { "gong", "gong" },
            { "bell tree", "belltree" }, { "bucket", "bucket" }, { "clap", "clap" },
            { "clave", "claves" }, { "maraca", "maraca" }, { "shaker", "shaker" },
            { "snap", "snap" }, { "stack", "stack" }, { "stick", "stickclick" },
            { "stomp", "stomp" },
        };

        for (const auto& [word, picture] : byWord)
            if (words.contains (word))
                return picture;

        return nullptr;
    }

    /** The rendered picture of a piece (assets/kit-renders/slot-*.png: each
        piece of the stage kit rendered alone, plus the percussion that isn't
        on the stage). The nth rack tom or crash alternates between the
        stage's two, so neighbouring cards differ. Null for percussion there's
        no render of. */
    juce::Image pictureFor (const KitPieceInfo& piece, int nth)
    {
        const char* name = nullptr;
        const auto role = piece.role;
        const auto words = (piece.id + " " + piece.name).toLowerCase();

        // Some kits play hand percussion from a drum's slot (Cobblestone's
        // cajon is its kick, its large conga its floor tom), so these go by
        // what they are, not the role they fill.
        if (words.contains ("cajon"))
            return kitart::slotPicture (piece.recordedIn, "cajon");

        if (words.contains ("conga"))
            return kitart::slotPicture (piece.recordedIn, words.contains ("large") || words.contains ("low") ? "conga-large" : "conga-small");

        if (words.contains ("tambourine"))
            return kitart::slotPicture (piece.recordedIn, "tambourine");

        switch (role)
        {
            case PieceRole::kick:     name = "kick"; break;
            case PieceRole::snare:    name = "snare"; break;
            case PieceRole::hihat:    name = "hihat"; break;
            case PieceRole::tom:      name = nth % 2 == 0 ? "tom1" : "tom2"; break;
            case PieceRole::floorTom: name = "floor"; break;
            case PieceRole::crash:    name = isSplash (piece) ? "splash" : nth % 2 == 0 ? "crash" : "crash2"; break;
            case PieceRole::ride:     name = "ride"; break;
            case PieceRole::china:    name = "china"; break;
            case PieceRole::other:
                name = percussionPicture (words.replaceCharacter ('_', ' '));
                break;
        }

        if (name == nullptr)
            return {};

        // In the finish of the kit the piece's recordings come from.
        return kitart::slotPicture (piece.recordedIn, name);
    }

    bool isCymbal (PieceRole role) noexcept
    {
        return role == PieceRole::crash || role == PieceRole::ride || role == PieceRole::china;
    }

    static_assert (SlotGrid::columns * SlotGrid::rows == kitslots::numSlots);

    constexpr int labelHeight = 18, modelHeight = 24, buttonRow = 22, faderWidth = 18;

    // Below this a card puts its model button and buttons on one row.
    constexpr int roomyCardHeight = 196;
}

//==============================================================================
juce::Image SlotGrid::pictureFor (const KitInfo& info, int pieceIndex)
{
    const auto& piece = info.pieces[(size_t) pieceIndex];
    int nth = 0;

    for (const auto& other : info.pieces)
        nth += other.role == piece.role && other.slot < piece.slot ? 1 : 0;

    return lithium::pictureFor (piece, nth);
}

//==============================================================================
class SlotGrid::Card final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Card (LithiumDrumsProcessor& p, const std::shared_ptr<const KitInfo>& info, int slotIndex)
        : processor (p), slot (slotIndex), model (p)
    {
        using namespace theme;

        for (size_t i = 0; i < info->pieces.size(); ++i)
            if (info->pieces[i].slot == slot)
                piece = (int) i;

        if (piece < 0)
        {
            // An empty slot: a click opens the picker to fill it.
            name = kitslots::all()[(size_t) slot].name;
            setTitle (name + ", empty");
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTooltip (name + " is empty. Click to fill it with a piece from any installed kit.");
            return;
        }

        const auto& pieceInfo = info->pieces[(size_t) piece];
        name = pieceInfo.name;
        role = pieceInfo.role;

        // The nth of its kind on the page, so neighbouring toms and crashes
        // show different renders.
        picture = SlotGrid::pictureFor (*info, piece);
        colour = groupColour (lithium::info (role).group);
        model.setPiece (info, piece);
        addAndMakeVisible (model);

        volume.setSliderStyle (juce::Slider::LinearVertical);
        volume.setDoubleClickReturnValue (true, 0.0);
        volume.getProperties().set ("unityValue", 0.0);
        volume.setColour (juce::Slider::trackColourId, colour);
        volume.setPopupDisplayEnabled (true, false, nullptr);
        volume.setTooltip (name + " volume (double-click for 0 dB)");
        addAndMakeVisible (volume);

        mute.setClickingTogglesState (true);
        mute.setTooltip ("Mute " + name);
        mute.setColour (juce::TextButton::buttonOnColourId, colours::textMuted);
        solo.setClickingTogglesState (true);
        solo.setTooltip ("Solo " + name);

        for (auto* b : std::initializer_list<juce::Button*> { &mute, &solo })
            addAndMakeVisible (b);

        auto& state = p.getParameters();
        volumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, params::pieceParamId (piece, "volume"), volume);
        muteAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, params::pieceParamId (piece, "mute"), mute);
        soloAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, params::pieceParamId (piece, "solo"), solo);

        setTitle (name);   // for screen readers
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip (name + ": click to play");
    }

    /** The piece in the slot (an index into the kit's pieces), or -1 if it's empty. */
    int getPiece() const noexcept    { return piece; }

    void setSelected (bool s)
    {
        if (s != selected)
        {
            selected = s;
            repaint();
        }
    }

    void flash()
    {
        flashUntil = juce::Time::getMillisecondCounterHiRes() + theme::motion::hitFlashMs;
        repaint();
    }

    /** Repaints while the flash fades; false once it has. */
    bool updateFlash()
    {
        const auto active = juce::Time::getMillisecondCounterHiRes() < flashUntil + 20.0;

        if (active || flashShown)
            repaint();

        flashShown = active;
        return active;
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (piece < 0)
        {
            ModelPicker::show (processor, slot, *this);
            return;
        }

        // Like a piece on the stage: a click selects it and plays it. (The
        // card's own controls take their clicks before they get here.)
        processor.getSelection().select (piece);
        processor.audition (piece, LithiumDrumsProcessor::auditionVelocity);
        flash();   // at once, before the audio thread reports the hit
    }

    void mouseEnter (const juce::MouseEvent&) override   { if (piece < 0) repaint(); }
    void mouseExit (const juce::MouseEvent&) override    { if (piece < 0) repaint(); }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

        if (piece < 0)
        {
            paintEmpty (g, bounds);
            return;
        }

        g.setColour (selected ? colours::raisedHigh : colours::raised);
        g.fillRoundedRectangle (bounds, cornerRadius + 2.0f);

        // The hit flash: the group colour, fading out.
        const auto left = (float) ((flashUntil - juce::Time::getMillisecondCounterHiRes()) / motion::hitFlashMs);

        if (left > 0.0f)
        {
            g.setColour (colour.withAlpha (0.16f * std::min (1.0f, left)));
            g.fillRoundedRectangle (bounds, cornerRadius + 2.0f);
        }

        g.setColour (selected ? colours::accent : colours::border);
        g.drawRoundedRectangle (bounds, cornerRadius + 2.0f, selected ? 1.5f : 1.0f);

        auto area = getLocalBounds().reduced (space::md, space::sm);
        g.setColour (colours::text);
        drawTrackedText (g, name.toUpperCase(), label (13.5f, Weight::semibold), 0.1f,
                         area.removeFromTop (labelHeight).toFloat(), juce::Justification::centred);

        // The piece's render, scaled down to fit (never up, so it stays sharp).
        if (picture.isValid())
        {
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            const auto iconArea = getIconArea().toFloat().reduced (2.0f);

            // Cymbals are all drawn at the scale that fits a crash, so they
            // keep their real sizes: a splash looks smaller than a crash, a
            // ride a little bigger (clamped to the card). Everything else
            // fills the picture area.
            if (const auto crash = kitart::load ("slot-crash.png"); isCymbal (role) && crash.isValid())
            {
                const auto scale = std::min (iconArea.getWidth() / (float) crash.getWidth(), iconArea.getHeight() / (float) crash.getHeight());
                const auto size = juce::Rectangle<float> ((float) picture.getWidth() * scale, (float) picture.getHeight() * scale);
                g.drawImage (picture, size.withCentre (iconArea.getCentre()).constrainedWithin (iconArea).withCentre (iconArea.getCentre()),
                             juce::RectanglePlacement::centred);
            }
            else
            {
                g.drawImage (picture, iconArea, juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
            }
        }
    }

    void resized() override
    {
        using namespace theme;

        if (piece < 0)
            return;

        auto area = getLocalBounds().reduced (space::md, space::sm);
        area.removeFromTop (labelHeight + space::xs);
        auto buttons = area.removeFromBottom (buttonRow);
        area.removeFromBottom (space::sm);

        // A roomy card has the model on a row of its own; a compact one
        // shares the bottom row with the buttons.
        if (getHeight() >= roomyCardHeight)
        {
            model.setBounds (area.removeFromBottom (modelHeight));
            area.removeFromBottom (space::sm);
        }
        else
        {
            model.setBounds (buttons.removeFromLeft (buttons.getWidth() - 2 * buttonRow - 2 * space::xs));
            buttons.removeFromLeft (space::xs);
        }

        volume.setBounds (area.removeFromRight (faderWidth));

        const auto w = (buttons.getWidth() - space::xs) / 2;
        mute.setBounds (buttons.removeFromLeft (w));
        buttons.removeFromLeft (space::xs);
        solo.setBounds (buttons);
    }

private:
    juce::Rectangle<int> getIconArea() const
    {
        // Between the label and the controls, clear of the fader on either side.
        return { theme::space::md + faderWidth, volume.getY(), getWidth() - 2 * (theme::space::md + faderWidth), volume.getHeight() };
    }

    void paintEmpty (juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        using namespace theme;
        const auto hover = isMouseOver (true);

        // A dashed outline: a place for a piece rather than a piece.
        juce::Path outline, dashed;
        outline.addRoundedRectangle (bounds, cornerRadius + 2.0f);
        const float dashes[] { 5.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (hover ? colours::raised : colours::raised.withAlpha (0.35f));
        g.fillPath (outline);
        g.setColour (hover ? colours::accent : colours::borderStrong);
        g.fillPath (dashed);

        auto area = getLocalBounds().reduced (space::md, space::sm);
        g.setColour (colours::textMuted);
        drawTrackedText (g, name.toUpperCase(), label (13.5f, Weight::semibold), 0.1f,
                         area.removeFromTop (labelHeight).toFloat(), juce::Justification::centred);

        // A plus over "Add".
        const auto centre = area.toFloat().getCentre();
        g.setColour (hover ? colours::accent : colours::textMuted);
        g.fillRect (juce::Rectangle<float> (18.0f, 2.0f).withCentre (centre.translated (0.0f, -10.0f)));
        g.fillRect (juce::Rectangle<float> (2.0f, 18.0f).withCentre (centre.translated (0.0f, -10.0f)));
        g.setFont (body (type::small, Weight::medium));
        g.drawText ("Add", juce::Rectangle<float> (area.toFloat().getWidth(), 18.0f).withCentre (centre.translated (0.0f, 14.0f)),
                    juce::Justification::centred, false);
    }

    LithiumDrumsProcessor& processor;
    const int slot;
    int piece = -1;
    juce::String name;
    PieceRole role = PieceRole::other;
    juce::Image picture;
    juce::Colour colour;
    bool selected = false, flashShown = false;
    double flashUntil = 0.0;

    ModelButton model;
    ControlSlider volume;
    juce::TextButton mute { "M" }, solo { "S" };

    // After the controls they're attached to (destroyed first). Only a slot
    // with a piece in it has them.
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAttachment, soloAttachment;
};

//==============================================================================
SlotGrid::SlotGrid (LithiumDrumsProcessor& p) : processor (p)
{
    processor.getSelection().addListener (this);
    startTimerHz (30);
}

SlotGrid::~SlotGrid()
{
    processor.getSelection().removeListener (this);
}

void SlotGrid::setKit (std::shared_ptr<const KitInfo> info, juce::Rectangle<int> area)
{
    kit = std::move (info);
    cards.clear();

    if (kit != nullptr)
    {
        for (int slot = 0; slot < kitslots::numSlots; ++slot)
        {
            cards.push_back (std::make_unique<Card> (processor, kit, slot));
            addAndMakeVisible (*cards.back());
        }

        for (size_t i = 0; i < kit->pieces.size(); ++i)
            seenHits[i] = processor.getFeedback().pieceHits[i].load();

        enableKeyboardNavigation (*this);
    }

    selectedPieceChanged (processor.getSelection().getSelected());
    layout (area);
}

void SlotGrid::layout (juce::Rectangle<int> area)
{
    using namespace theme;
    setSize (area.getWidth(), area.getHeight());

    // Always the same grid, whatever the kit: every slot in its place, the
    // cards sharing the page out evenly.
    const auto cardWidth = (area.getWidth() - space::sm * (columns - 1)) / columns;
    const auto cardHeight = (area.getHeight() - space::sm * (rows - 1)) / rows;

    for (size_t k = 0; k < cards.size(); ++k)
    {
        const auto row = (int) k / columns, column = (int) k % columns;
        cards[k]->setBounds (column * (cardWidth + space::sm), row * (cardHeight + space::sm), cardWidth, cardHeight);
    }
}

void SlotGrid::selectedPieceChanged (int piece)
{
    for (auto& c : cards)
        c->setSelected (c->getPiece() >= 0 && c->getPiece() == piece);
}

void SlotGrid::timerCallback()
{
    // Only the cards that were hit, or are fading, repaint; nothing does
    // while the grid is hidden (the Stage view, or another tab).
    const auto& feedback = processor.getFeedback();
    const auto showing = isShowing();

    for (auto& c : cards)
    {
        const auto i = (size_t) c->getPiece();

        if (c->getPiece() < 0)
            continue;

        const auto hits = feedback.pieceHits[i].load (std::memory_order_acquire);

        if (hits != seenHits[i])
        {
            seenHits[i] = hits;

            if (showing)
                c->flash();
        }

        if (showing)
            c->updateFlash();
    }
}

} // namespace lithium
