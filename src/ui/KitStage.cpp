#include "KitStage.h"

#include "LithiumAssets.h"
#include "Theme.h"

namespace lithium
{

namespace
{
    constexpr double flashDurationMs = 240.0;
    constexpr int hitAlphaThreshold = 48;     // pixels fainter than this (soft shadows, antialiasing) don't count

    juce::Image loadPng (const juce::String& name)
    {
        int size = 0;

        if (const auto* data = LithiumAssets::getNamedResource ((name + "_png").toRawUTF8(), size))
            return juce::ImageFileFormat::loadFrom (data, (size_t) size);

        jassertfalse;   // a render (room, or a layer named in layout.json) has no matching PNG in the binary data
        return {};
    }

    // A brightened copy of a piece, faded in over it when it is hit.
    juce::Image makeFlashImage (const juce::Image& src)
    {
        auto out = src.createCopy();
        const juce::Image::BitmapData data (out, juce::Image::BitmapData::readWrite);
        const auto tint = juce::Colours::white.interpolatedWith (theme::colours::accent, 0.35f);

        for (int y = 0; y < data.height; ++y)
        {
            for (int x = 0; x < data.width; ++x)
            {
                const auto c = data.getPixelColour (x, y);

                if (c.getAlpha() > 0)
                    data.setPixelColour (x, y, c.interpolatedWith (tint.withAlpha (c.getAlpha()), 0.6f));
            }
        }

        return out;
    }

    // A soft glow following the piece's alpha edge: its alpha mask, blurred
    // and tinted with the accent colour. The piece is first copied into a
    // padded image, because the blur can't spread past the image it works on.
    juce::Image makeGlowImage (const juce::Image& src, int padding)
    {
        juce::Image padded (juce::Image::ARGB, src.getWidth() + 2 * padding, src.getHeight() + 2 * padding, true);

        {
            juce::Graphics g (padded);
            g.drawImageAt (src, padding, padding);
        }

        juce::Image glow (juce::Image::ARGB, padded.getWidth(), padded.getHeight(), true);

        {
            juce::Graphics g (glow);
            // A wide, soft halo (drawn twice to build up strength) plus a tight rim.
            const juce::DropShadow halo (theme::colours::accent.withAlpha (0.85f), padding * 3 / 4, {});
            halo.drawForImage (g, padded);
            halo.drawForImage (g, padded);
            juce::DropShadow (theme::colours::accent, padding / 5, {}).drawForImage (g, padded);
        }

        return glow;
    }
}

//==============================================================================
class KitStage::OverlayCard final : public juce::Component
{
public:
    OverlayCard()
    {
        editButton.setButtonText ("Edit piece");
        addAndMakeVisible (editButton);
    }

    void setPiece (const KitPieceInfo* p)
    {
        if (p != nullptr)
        {
            name = p->name;
            group = info (p->role).group;
            detail = juce::String (p->articulations.size()) + (p->articulations.size() == 1 ? " articulation" : " articulations");
        }
        else
        {
            name = "No piece";
            group = PieceGroup::other;
            detail = "No kit loaded";
        }

        editButton.setEnabled (p != nullptr);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

        g.setColour (colours::panel.withAlpha (0.88f));
        g.fillRoundedRectangle (bounds, 8.0f);
        g.setColour (colours::borderStrong);
        g.drawRoundedRectangle (bounds, 8.0f, 1.0f);

        auto area = getLocalBounds().reduced (16, 14).toFloat();

        auto top = area.removeFromTop (14.0f);
        g.setColour (groupColour (group));
        g.fillEllipse (top.removeFromLeft (8.0f).withSizeKeepingCentre (8.0f, 8.0f));
        top.removeFromLeft (8.0f);
        g.setColour (colours::textMuted);
        drawTrackedText (g, "SELECTED PIECE", label (12.0f), 0.1f, top, juce::Justification::centredLeft);

        area.removeFromTop (4.0f);
        g.setColour (colours::text);
        g.setFont (body (20.0f, Weight::semibold));
        g.drawText (name, area.removeFromTop (26.0f), juce::Justification::centredLeft, true);

        g.setColour (colours::textMuted);
        g.setFont (mono (12.5f));
        g.drawText (detail, area.removeFromTop (18.0f), juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        editButton.setBounds (getLocalBounds().reduced (16, 14).removeFromBottom (26).removeFromLeft (96));
    }

    juce::TextButton editButton;

private:
    juce::String name, detail;
    PieceGroup group = PieceGroup::other;
};

//==============================================================================
KitStage::KitStage (LithiumDrumsProcessor& p) : processor (p), card (std::make_unique<OverlayCard>())
{
    loadRenders();

    slotPiece.fill (-1);

    card->editButton.onClick = [this] { if (onEditPiece) onEditPiece(); };
    addAndMakeVisible (*card);

    processor.getSelection().addListener (this);
    selectedPieceChanged (processor.getSelection().getSelected());

    // Rendering ten large images with high-quality resampling is expensive,
    // so the stage is cached as an image. Repainting just a flashing piece's
    // area only redraws that part of the cache.
    setBufferedToImage (true);
    setOpaque (true);
    startTimerHz (60);
}

KitStage::~KitStage()
{
    processor.getSelection().removeListener (this);
}

void KitStage::loadRenders()
{
    const auto json = juce::JSON::parse (juce::String::fromUTF8 (LithiumAssets::layout_json, LithiumAssets::layout_jsonSize));
    renderScale = (float) (double) json["renderScale"];
    glowPadding = juce::roundToInt (24.0f * renderScale);
    room = loadPng ("room");   // painted by tools/roomgen through the same camera as the kit

    const auto* order = json["drawOrderBackToFront"].getArray();
    jassert (order != nullptr && renderScale > 0.0f);

    for (const auto& nameVar : *order)
    {
        const auto name = nameVar.toString();
        const auto& rect = json["pieces"][juce::Identifier (name)];

        Layer layer;
        layer.slot = slotFromRenderKey (name.toStdString());
        layer.image = loadPng (name);
        layer.renderRect = { (int) rect["x"], (int) rect["y"], (int) rect["w"], (int) rect["h"] };

        // The PNG's own size is the truth; the layout rect just positions it.
        jassert (layer.image.getWidth() == layer.renderRect.getWidth() && layer.image.getHeight() == layer.renderRect.getHeight());

        if (layer.slot.has_value())
        {
            layer.flash = makeFlashImage (layer.image);
            layer.glow = makeGlowImage (layer.image, glowPadding);
        }

        layers.push_back (std::move (layer));
    }
}

void KitStage::resized()
{
    // layout.json is in render pixels; divide by renderScale for 1x stage
    // coordinates, then by how much the stage is scaled relative to 1x.
    const auto toStage = (float) getWidth() / (float) theme::windowWidth / renderScale;

    for (auto& l : layers)
    {
        l.bounds = l.renderRect.toFloat() * toStage;
        l.glowBounds = l.renderRect.expanded (glowPadding).toFloat() * toStage;
    }

    card->setBounds (20, 20, 260, 118);
}

void KitStage::setKitInfo (std::shared_ptr<const KitInfo> info)
{
    kitInfo = std::move (info);

    if (kitInfo != nullptr)
        slotPiece = kitInfo->slotPiece;
    else
        slotPiece.fill (-1);

    // Start each slot's hit counter from now, so a kit change doesn't flash everything.
    for (auto& l : layers)
        if (const auto piece = pieceIn (l); piece >= 0)
            l.lastHitCount = processor.getFeedback().pieceHits[(size_t) piece].load();

    updateCard();
    repaint();
}

void KitStage::updateCard()
{
    const auto selected = processor.getSelection().getSelected();
    const auto valid = kitInfo != nullptr && juce::isPositiveAndBelow (selected, (int) kitInfo->pieces.size());
    card->setPiece (valid ? &kitInfo->pieces[(size_t) selected] : nullptr);
}

int KitStage::pieceIn (const Layer& l) const noexcept
{
    return l.slot.has_value() ? slotPiece[(size_t) index (*l.slot)] : -1;
}

//==============================================================================
void KitStage::paint (juce::Graphics& g)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (room, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);

    const auto selected = processor.getSelection().getSelected();

    for (const auto& l : layers)
    {
        const auto piece = pieceIn (l);
        const auto dimmed = l.slot.has_value() && piece < 0;

        if (piece >= 0 && piece == selected)
        {
            g.setOpacity (1.0f);
            g.drawImage (l.glow, l.glowBounds);
        }

        g.setOpacity (dimmed ? 0.3f : 1.0f);
        g.drawImage (l.image, l.bounds);

        if (l.flashLevel > 0.0f)
        {
            g.setOpacity (l.flashLevel * 0.8f);
            g.drawImage (l.flash, l.bounds);
        }
    }

    g.setOpacity (1.0f);
}

std::optional<StageSlot> KitStage::getSlotAt (juce::Point<float> position) const
{
    const auto toRender = renderScale * (float) theme::windowWidth / (float) getWidth();

    for (auto it = layers.rbegin(); it != layers.rend(); ++it)
    {
        if (! it->slot.has_value() || ! it->bounds.contains (position))
            continue;

        const auto pixel = (position * toRender).toInt() - it->renderRect.getPosition();

        if (it->image.getBounds().contains (pixel)
            && it->image.getPixelAt (pixel.x, pixel.y).getAlpha() >= hitAlphaThreshold)
            return it->slot;
    }

    return std::nullopt;
}

std::optional<int> KitStage::getPieceAt (juce::Point<float> position) const
{
    // A dimmed slot (no piece) still blocks what's behind it: the click lands
    // on something visibly in front, it just does nothing.
    if (const auto slot = getSlotAt (position))
        if (const auto piece = slotPiece[(size_t) index (*slot)]; piece >= 0)
            return piece;

    return std::nullopt;
}

void KitStage::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (getPieceAt (e.position).has_value() ? juce::MouseCursor::PointingHandCursor
                                                     : juce::MouseCursor::NormalCursor);
}

void KitStage::mouseDown (const juce::MouseEvent& e)
{
    if (const auto piece = getPieceAt (e.position))
    {
        processor.getSelection().select (*piece);
        processor.audition (*piece, LithiumDrumsProcessor::auditionVelocity);
        startFlash (*piece);   // immediate feedback, even before the audio thread reports the hit
    }
}

KitStage::Layer* KitStage::layerFor (int piece)
{
    for (auto& l : layers)
        if (piece >= 0 && pieceIn (l) == piece)
            return &l;

    return nullptr;
}

void KitStage::startFlash (int piece)
{
    if (auto* l = layerFor (piece))
    {
        l->flashStartMs = juce::Time::getMillisecondCounterHiRes();
        l->flashLevel = 1.0f;
        repaint (l->bounds.getSmallestIntegerContainer().expanded (2));
    }
}

void KitStage::selectedPieceChanged (int)
{
    updateCard();
    repaint();
}

void KitStage::timerCallback()
{
    // Hits from MIDI (and auditions) arrive as counter changes from the audio thread.
    const auto& hits = processor.getFeedback().pieceHits;

    for (auto& l : layers)
    {
        const auto piece = pieceIn (l);

        if (piece < 0)
            continue;

        const auto count = hits[(size_t) piece].load (std::memory_order_relaxed);

        if (count != l.lastHitCount)
        {
            l.lastHitCount = count;
            startFlash (piece);
        }
    }

    const auto now = juce::Time::getMillisecondCounterHiRes();

    for (auto& l : layers)
    {
        if (l.flashLevel <= 0.0f)
            continue;

        l.flashLevel = (float) juce::jmax (0.0, 1.0 - (now - l.flashStartMs) / flashDurationMs);
        repaint (l.bounds.getSmallestIntegerContainer().expanded (2));
    }
}

} // namespace lithium
