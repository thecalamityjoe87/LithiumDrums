#include "ModelPicker.h"

#include "LithiumLookAndFeel.h"
#include "Theme.h"
#include "engine/PieceSwap.h"

namespace lithium
{

namespace
{
    const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));   // a middle dot, in UTF-8
}

juce::String modelText (const KitInfo& kit, const KitPieceInfo& piece)
{
    if (piece.swappedFrom.isNotEmpty())
        return piece.swappedFrom + dot + piece.swappedPiece;

    // The kit's own: named the same way, so each piece shows the recording it
    // plays (and matches the picker's "This kit" row), not just the kit.
    return kit.credits.name + dot + piece.name;
}

juce::Component* findOverlayParent (juce::Component& c)
{
    // The main view is the editor's child; the editor scales it as a whole.
    for (auto* p = &c; p != nullptr; p = p->getParentComponent())
        if (dynamic_cast<juce::AudioProcessorEditor*> (p->getParentComponent()) != nullptr)
            return p;

    return c.getTopLevelComponent();
}

//==============================================================================
ModelButton::ModelButton (LithiumDrumsProcessor& p) : juce::Button ("Model"), processor (p)
{
    setTooltip ("The recording this piece plays. Click to swap in the same kind of piece from another kit.");
}

void ModelButton::setPiece (std::shared_ptr<const KitInfo> info, int index)
{
    kit = std::move (info);
    piece = index;
    setEnabled (kit != nullptr && juce::isPositiveAndBelow (piece, (int) kit->pieces.size())
                && kit->pieces[(size_t) piece].slot >= 0);

    // The whole model name (the button may be too narrow for it), and any notes.
    if (isEnabled())
    {
        const auto& p = kit->pieces[(size_t) piece];
        auto tip = modelText (*kit, p) + "\nClick to swap in the same kind of piece from another kit.";

        for (const auto& note : p.swapNotes)
            tip << "\n" << note;

        setTooltip (tip);
    }

    repaint();
}

void ModelButton::clicked()
{
    if (kit != nullptr && juce::isPositiveAndBelow (piece, (int) kit->pieces.size()))
        ModelPicker::show (processor, kit->pieces[(size_t) piece].slot, *this);
}

void ModelButton::paintButton (juce::Graphics& g, bool isMouseOver, bool isDown)
{
    using namespace theme;
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (isDown ? colours::raisedHigh.brighter (0.05f) : (isMouseOver ? colours::raisedHigh : colours::background.withAlpha (0.5f)));
    g.fillRoundedRectangle (bounds, cornerRadius);
    g.setColour (isMouseOver ? colours::borderStrong : colours::border);
    g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);

    if (hasKeyboardFocus (false))
        LithiumLookAndFeel::drawFocusRing (g, bounds, cornerRadius);

    if (kit == nullptr || ! juce::isPositiveAndBelow (piece, (int) kit->pieces.size()))
        return;

    const auto& p = kit->pieces[(size_t) piece];
    auto area = bounds.reduced (8.0f, 0.0f);

    // The chevron, as on drop-downs.
    const auto cx = area.getRight() - 4.0f, cy = area.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
    chevron.lineTo (cx, cy + 2.0f);
    chevron.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (colours::textMuted);
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    area.removeFromRight (14.0f);

    // A swapped piece is marked with a dot; one with notes, in the warning colour.
    if (p.swappedFrom.isNotEmpty() || ! p.swapNotes.isEmpty())
    {
        g.setColour (p.swapNotes.isEmpty() ? colours::accent : colours::warning);
        g.fillEllipse (area.removeFromLeft (6.0f).withSizeKeepingCentre (6.0f, 6.0f));
        area.removeFromLeft (6.0f);
    }

    g.setColour (isEnabled() ? colours::text : colours::textMuted);
    g.setFont (body (type::small, Weight::medium));
    g.drawText (modelText (*kit, p), area, juce::Justification::centredLeft, true);
}

//==============================================================================
ModelPicker::ModelPicker (LithiumDrumsProcessor& p, int s) : processor (p), slot (s)
{
    list.setModel (this);
    list.setRowHeight (26);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    list.getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, theme::colours::borderStrong);
    list.getViewport()->setScrollBarThickness (8);
    addAndMakeVisible (list);

    previous.onClick = [this] { step (-1); };
    next.onClick = [this] { step (1); };
    addAndMakeVisible (previous);
    addAndMakeVisible (next);

    ambience.setClickingTogglesState (true);
    ambience.setTooltip ("On: the piece brings its own overhead and room sound, mixed into this kit's overhead and room channels.\n"
                         "Off: only its close mics are used, so it sits in this kit's room only through its own channel.");
    ambience.onClick = [this]
    {
        if (const auto swap = swaps::find (processor.getPieceSwaps(), key()); swap.has_value() && ! swap->removed)
        {
            auto changed = *swap;
            changed.ambience = ambience.getToggleState();
            processor.setPieceSwap (changed);
        }
    };
    addAndMakeVisible (ambience);

    processor.addChangeListener (this);
    rebuild();
    setSize (width, height);

    if (const auto row = currentRow(); row >= 0)
        list.scrollToEnsureRowIsOnscreen (row);
}

ModelPicker::~ModelPicker()
{
    processor.removeChangeListener (this);
    list.setModel (nullptr);
}

void ModelPicker::show (LithiumDrumsProcessor& processor, int slot, juce::Component& target)
{
    auto* parent = findOverlayParent (target);

    if (parent == nullptr || ! juce::isPositiveAndBelow (slot, kitslots::numSlots))
        return;

    // A CallOutBox owns its content and deletes it when it's dismissed
    // (a click outside it, or Escape).
    auto picker = std::make_unique<ModelPicker> (processor, slot);
    juce::CallOutBox::launchAsynchronously (std::move (picker), parent->getLocalArea (&target, target.getLocalBounds()), parent);
}

juce::String ModelPicker::key() const
{
    if (shownKit != nullptr && shownKit->slotOwners[(size_t) slot].id.isNotEmpty())
        return shownKit->slotOwners[(size_t) slot].id;

    return kitslots::addedPieceId (slot);
}

const KitPieceInfo* ModelPicker::currentPiece (int* index) const
{
    if (shownKit == nullptr)
        return nullptr;

    for (size_t i = 0; i < shownKit->pieces.size(); ++i)
    {
        if (shownKit->pieces[i].id == key())
        {
            if (index != nullptr)
                *index = (int) i;

            return &shownKit->pieces[i];   // valid as long as shownKit holds this KitInfo
        }
    }

    return nullptr;
}

void ModelPicker::rebuild()
{
    rows.clear();
    shownKit = processor.getKitInfo();
    const auto info = shownKit;

    if (info == nullptr)
    {
        title = {};
        list.updateContent();
        repaint();
        return;
    }

    const auto& slotInfo = kitslots::all()[(size_t) slot];
    const auto& owner = info->slotOwners[(size_t) slot];
    const auto* piece = currentPiece();
    title = (piece != nullptr ? piece->name : juce::String (slotInfo.name)).toUpperCase() + dot + "MODEL";

    rows.push_back ({ true, "This kit", {}, {} });

    if (owner.id.isNotEmpty())
        rows.push_back ({ false, info->credits.name + dot + owner.name, {}, {} });

    rows.push_back ({ false, "Empty", {}, {}, true });

    // Every installed kit's pieces that fit the slot (presets reuse their
    // kits' recordings, so only kits are listed).
    const auto library = kitlibrary::scan (processor.getKitsFolder());
    auto models = 0;

    for (const auto& kit : library.kits)
    {
        if (kit.summary.isPreset)
            continue;

        bool headed = false;

        for (const auto& p : kit.summary.pieces)
        {
            // This kit's own piece in the slot is a row already.
            if (! kitslots::accepts (slotInfo.kind, p.role, kitslots::isSplash (p.role, p.id, p.name))
                || (kit.summary.id == info->id && p.id == owner.id))
                continue;

            if (! headed)
            {
                rows.push_back ({ true, kit.credits().name, {}, {} });
                headed = true;
            }

            rows.push_back ({ false, p.name.isNotEmpty() ? p.name : p.id, kit.summary.id, p.id });
            ++models;
        }
    }

    // What didn't come along, or why the swap couldn't be made.
    notes = piece != nullptr ? piece->swapNotes.joinIntoString ("\n") : juce::String();
    notesAreProblems = piece != nullptr && piece->swapNotes.size() > 0 && ! piece->swapNotes[0].startsWith ("Not used");

    if (piece == nullptr)
        notes = "This slot is empty. Choose a piece from any installed kit to fill it.";

    if (models == 0)
        notes = "No other installed kit has a piece for this slot. Download more kits from Browse in the header.";

    if (processor.isKitLoading())
        notes = "Loading...";

    const auto swap = swaps::find (processor.getPieceSwaps(), key());
    const auto swapped = swap.has_value() && ! swap->removed;
    ambience.setToggleState (! swapped || swap->ambience, juce::dontSendNotification);
    ambience.setEnabled (swapped);

    list.updateContent();
    list.repaint();
    repaint();
}

int ModelPicker::currentRow() const
{
    const auto swap = swaps::find (processor.getPieceSwaps(), key());
    const auto ownsSlot = shownKit != nullptr && shownKit->slotOwners[(size_t) slot].id.isNotEmpty();

    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto& r = rows[i];
        const auto own = ! r.heading && ! r.empty && r.kitId.isEmpty();

        // No swap: the kit's own piece, or for a slot the kit has nothing in, Empty.
        const auto current = ! swap.has_value() ? (ownsSlot ? own : r.empty)
                           : swap->removed      ? r.empty
                                                : ! r.heading && r.kitId == swap->sourceKit && r.pieceId == swap->sourcePiece;

        if (current)
            return (int) i;
    }

    return -1;
}

void ModelPicker::choose (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()) || rows[(size_t) row].heading || row == currentRow())
        return;

    const auto& r = rows[(size_t) row];
    const auto old = swaps::find (processor.getPieceSwaps(), key());
    const auto ownsSlot = shownKit != nullptr && shownKit->slotOwners[(size_t) slot].id.isNotEmpty();

    // Empty takes the kit's own piece out; for a slot it has nothing in,
    // that's just no swap at all. The kit's own row is no swap either.
    PieceSwap swap { key(), r.kitId, r.pieceId, ! old.has_value() || old->ambience };
    swap.removed = r.empty && ownsSlot;
    processor.setPieceSwap (swap);
    auditionWhenLoaded = ! r.empty;
    rebuild();
}

void ModelPicker::step (int delta)
{
    auto row = currentRow();

    for (size_t tries = 0; tries < rows.size(); ++tries)
    {
        row = (row + delta + (int) rows.size()) % (int) rows.size();

        if (! rows[(size_t) row].heading)
            break;
    }

    choose (row);
    list.scrollToEnsureRowIsOnscreen (row);
}

void ModelPicker::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rebuild();

    // Play the new model once it's in, so it can be heard straight away.
    if (int index = -1; auditionWhenLoaded && ! processor.isKitLoading() && currentPiece (&index) != nullptr)
    {
        auditionWhenLoaded = false;
        processor.audition (index, LithiumDrumsProcessor::auditionVelocity);
    }
}

void ModelPicker::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool)
{
    using namespace theme;

    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;

    const auto& r = rows[(size_t) row];
    auto area = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);

    if (r.heading)
    {
        g.setColour (colours::textMuted);
        drawTrackedText (g, r.text.toUpperCase(), label (type::caption), 0.1f, area.reduced (8.0f, 0.0f).withTrimmedTop (6.0f),
                         juce::Justification::centredLeft);
        return;
    }

    const auto current = row == currentRow();
    const auto hover = list.getRowContainingPosition (list.getMouseXYRelative().x, list.getMouseXYRelative().y) == row
                       && list.isMouseOver (true);

    if (current || hover)
    {
        g.setColour (current ? colours::raisedHigh : colours::raised);
        g.fillRoundedRectangle (area.reduced (2.0f, 1.0f), cornerRadius - 2.0f);
    }

    auto text = area.reduced (10.0f, 0.0f);

    if (current)
    {
        g.setColour (processor.isKitLoading() ? colours::textMuted : colours::accent);
        g.fillEllipse (text.removeFromLeft (7.0f).withSizeKeepingCentre (7.0f, 7.0f));
        text.removeFromLeft (8.0f);
    }
    else
    {
        text.removeFromLeft (15.0f);
    }

    g.setColour (current ? colours::text : colours::text.withAlpha (0.85f));
    g.setFont (body (13.5f, current ? Weight::semibold : Weight::regular));
    g.drawText (r.text, text, juce::Justification::centredLeft, true);
}

void ModelPicker::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    choose (row);
}

void ModelPicker::paint (juce::Graphics& g)
{
    using namespace theme;

    g.setColour (colours::text);
    drawTrackedText (g, title, label (type::heading, Weight::bold), 0.08f, { 12.0f, 8.0f, (float) getWidth() - 90.0f, 26.0f },
                     juce::Justification::centredLeft);

    g.setColour (colours::border);
    g.fillRect (12, list.getBottom() + 4, getWidth() - 24, 1);

    g.setColour (notesAreProblems ? colours::warning : colours::textMuted);
    g.setFont (body (type::small));
    g.drawFittedText (notes, juce::Rectangle<int> (12, ambience.getBottom() + 6, getWidth() - 24, getHeight() - ambience.getBottom() - 10),
                      juce::Justification::topLeft, 3, 1.0f);
}

void ModelPicker::resized()
{
    using namespace theme;
    auto area = getLocalBounds().reduced (space::md, space::sm);
    auto top = area.removeFromTop (26);
    next.setBounds (top.removeFromRight (24));
    top.removeFromRight (space::xs);
    previous.setBounds (top.removeFromRight (24));
    area.removeFromTop (space::sm);

    area.removeFromBottom (52);   // the notes
    ambience.setBounds (area.removeFromBottom (size::button));
    area.removeFromBottom (space::md);
    list.setBounds (area);
}

} // namespace lithium
