#include "KitStage.h"

#include "KitArt.h"
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
KitStage::KitStage (LithiumDrumsProcessor& p) : processor (p)
{
    loadRenders();

    slotPiece.fill (-1);

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
        layer.renderKey = name;
        layer.image = loadPng (name);
        layer.renderRect = { (int) rect["x"], (int) rect["y"], (int) rect["w"], (int) rect["h"] };

        // The PNG's own size is the truth; the layout rect just positions it.
        jassert (layer.image.getWidth() == layer.renderRect.getWidth() && layer.image.getHeight() == layer.renderRect.getHeight());

        if (layer.slot.has_value())
        {
            layer.flash = makeFlashImage (layer.image);
            layer.glow = makeGlowImage (layer.image, glowPadding);
            layer.looks[{}] = { layer.image, layer.glow, layer.flash };   // the default finish
        }

        layers.push_back (std::move (layer));
    }
}

void KitStage::showFinishes()
{
    // Each piece in the finish of the kit its recordings come from, so a kit
    // with swapped pieces shows them as they are: MuldjordKit's black snare
    // among DRSKit's orange toms. A finish's layers have the same size and
    // place as the default's, so only the pictures change.
    // An empty spot (a kit with one rack tom) is faded in the kit's own
    // finish: that of its pieces that weren't swapped in (for a preset, its
    // base kit's).
    juce::String ownKit;

    if (kitInfo != nullptr)
        for (const auto& p : kitInfo->pieces)
            if (p.swappedFrom.isEmpty())
            {
                ownKit = p.recordedIn;
                break;
            }

    for (auto& l : layers)
    {
        if (! l.slot.has_value())
            continue;

        const auto piece = pieceIn (l);
        const auto kitId = kitInfo == nullptr ? juce::String()
                         : piece >= 0         ? kitInfo->pieces[(size_t) piece].recordedIn
                                              : ownKit;
        // Kits without a finish of their own (and pieces a finish doesn't
        // cover, like the cymbals) use the default.
        const auto own = kitart::load ("finish-" + kitart::finishFor (kitId) + "-" + l.renderKey + ".png");
        const auto finish = own.isValid() && own.getBounds() == l.renderRect.withZeroOrigin() ? kitart::finishFor (kitId) : juce::String();
        auto look = l.looks.find (finish);

        if (look == l.looks.end())
            look = l.looks.emplace (finish, Layer::Look { own, makeGlowImage (own, glowPadding), makeFlashImage (own) }).first;

        l.image = look->second.image;
        l.glow = look->second.glow;
        l.flash = look->second.flash;
    }
}

void KitStage::resized()
{
    // The renders are of a 1280 x 744 stage, the Kit tab's size. It's scaled
    // to cover the whole component, centred, so a component of another shape
    // crops the room's edges rather than squashing the kit.
    const auto cover = std::max ((float) getWidth() / (float) theme::windowWidth, (float) getHeight() / (float) theme::stageHeight);
    stageOrigin = { ((float) getWidth() - (float) theme::windowWidth * cover) * 0.5f,
                    ((float) getHeight() - (float) theme::stageHeight * cover) * 0.5f };

    // Then zoomed in on the kit, which stays where it was on the page; the
    // room's edges go past the sides. (The renders are 1.5x, so they stay
    // sharp at this zoom.)
    const auto focus = stageOrigin + kitCentre * cover;
    stageScale = cover * kitZoom;
    stageOrigin = focus - kitCentre * stageScale;

    // layout.json is in render pixels; divide by renderScale for 1x stage
    // coordinates, then scale and place them like the stage.
    const auto toStage = stageScale / renderScale;

    for (auto& l : layers)
    {
        l.bounds = (l.renderRect.toFloat() * toStage) + stageOrigin;
        l.glowBounds = (l.renderRect.expanded (glowPadding).toFloat() * toStage) + stageOrigin;
    }
}

void KitStage::setKitInfo (std::shared_ptr<const KitInfo> info)
{
    kitInfo = std::move (info);

    if (kitInfo != nullptr)
        slotPiece = kitInfo->slotPiece;
    else
        slotPiece.fill (-1);

    showFinishes();

    // Start each slot's hit counter from now, so a kit change doesn't flash everything.
    for (auto& l : layers)
        if (const auto piece = pieceIn (l); piece >= 0)
            l.lastHitCount = processor.getFeedback().pieceHits[(size_t) piece].load();

    repaint();
}

int KitStage::pieceIn (const Layer& l) const noexcept
{
    return l.slot.has_value() ? slotPiece[(size_t) index (*l.slot)] : -1;
}

//==============================================================================
void KitStage::paint (juce::Graphics& g)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (room, juce::Rectangle<float> (stageOrigin.x, stageOrigin.y, (float) theme::windowWidth * stageScale,
                                               (float) theme::stageHeight * stageScale), juce::RectanglePlacement::stretchToFit);

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
    const auto toRender = renderScale / stageScale;

    for (auto it = layers.rbegin(); it != layers.rend(); ++it)
    {
        if (! it->slot.has_value() || ! it->bounds.contains (position))
            continue;

        const auto pixel = ((position - stageOrigin) * toRender).toInt() - it->renderRect.getPosition();

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
