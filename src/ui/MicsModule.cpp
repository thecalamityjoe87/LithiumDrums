#include "MicsModule.h"

#include "KitArt.h"
#include "LithiumAssets.h"
#include "engine/MicEmulation.h"
#include "SlotGrid.h"
#include "Theme.h"

#include <map>
#include <optional>

namespace lithium
{

namespace
{
    // The per-drum level parameter for each mic role (see Parameters.cpp).
    constexpr std::array<std::string_view, numMicRoles> sendIds { "send_close", "send_overheads", "send_room" };
    constexpr std::array<const char*, numMicRoles> roleNames { "Close", "Overheads", "Room" };
    constexpr std::array<const char*, numMicRoles> roleTooltips { "How loud the drum is in its close mics",
                                                                  "How loud the drum is in the overhead mics",
                                                                  "How loud the drum is in the room mics" };

    constexpr int knobWidth = 64, knobHeight = 66;   // a labelled knob: name, dial (theme::size::knob), value
    constexpr float roomHeight = 0.72f;              // the drum view's room stands, as a share of the card's height
    constexpr int maxPerRow = 12;                    // the mic view's knobs per row before it wraps to two
    constexpr float emulatedTagWidth = 60.0f;        // the EMULATED tag at its narrowest: its word and a little padding

    size_t roleIndex (MicRole r) noexcept  { return static_cast<size_t> (r); }

    /** "Mics · ", the card's title before the channel's name. JUCE treats a
        plain char* as ASCII, so the middle dot goes through CharPointer_UTF8. */
    juce::String titlePrefix()             { return juce::String (juce::CharPointer_UTF8 ("Mics \xc2\xb7 ")); }

    bool isPlate (PieceRole role)          { return role == PieceRole::hihat || info (role).isCymbal; }

    /** The box a drum's picture is fitted into in the drum view, by role:
        the renders' shapes (cymbals wide and flat, the kick tall). */
    juce::Point<float> pictureBoxSize (PieceRole role)
    {
        switch (role)
        {
            case PieceRole::kick:     return { 144.0f, 144.0f };
            case PieceRole::snare:    return { 150.0f, 130.0f };
            case PieceRole::tom:      return { 144.0f, 130.0f };
            case PieceRole::floorTom: return { 136.0f, 154.0f };
            case PieceRole::hihat:    return { 166.0f, 127.0f };
            case PieceRole::crash:
            case PieceRole::ride:
            case PieceRole::china:    return { 210.0f, 108.0f };
            case PieceRole::other:    break;
        }

        return { 136.0f, 136.0f };
    }

    /** Where the mics aim on a drum's render (see SlotGrid::pictureFor), as
        a share of the picture's width and height: the batter head of a shell
        drum, the front head of the kick, the plate of a cymbal. */
    juce::Point<float> aimOnPicture (PieceRole role)
    {
        switch (role)
        {
            case PieceRole::kick:     return { 0.50f, 0.50f };
            case PieceRole::snare:
            case PieceRole::tom:      return { 0.50f, 0.07f };
            case PieceRole::floorTom: return { 0.50f, 0.06f };
            case PieceRole::hihat:    return { 0.50f, 0.74f };
            case PieceRole::crash:    return { 0.45f, 0.22f };
            case PieceRole::ride:     return { 0.50f, 0.24f };
            case PieceRole::china:    return { 0.50f, 0.28f };
            case PieceRole::other:    break;
        }

        return { 0.50f, 0.35f };
    }

    /** Where the mics aim on a drum's stage layer (its rectangle in
        layout.json), as a share of its width and height. */
    juce::Point<float> aimOnStage (PieceRole role)
    {
        switch (role)
        {
            case PieceRole::kick:     return { 0.50f, 0.55f };
            case PieceRole::snare:
            case PieceRole::tom:
            case PieceRole::floorTom: return { 0.50f, 0.12f };
            case PieceRole::hihat:    return { 0.50f, 0.25f };
            case PieceRole::crash:
            case PieceRole::ride:
            case PieceRole::china:    return { 0.50f, 0.40f };
            case PieceRole::other:    break;
        }

        return { 0.50f, 0.40f };
    }

    /** The picture placed in `box`, keeping its shape: on the box's bottom
        edge, or its top for cymbals (whose renders end partway down the stand). */
    juce::Rectangle<float> placePicture (const juce::Image& picture, juce::Rectangle<float> box, bool alignTop)
    {
        if (! picture.isValid())
            return box;

        const juce::RectanglePlacement placement (juce::RectanglePlacement::xMid
                                                  | (alignTop ? juce::RectanglePlacement::yTop : juce::RectanglePlacement::yBottom));
        return placement.appliedTo (picture.getBounds().toFloat(), box);
    }

    /** The picture shrunk once to the size it's drawn at (twice that, for
        high-DPI screens), rather than resampled from the full render on every
        repaint. The renders are up to about 1000 px across. */
    juce::Image scaledFor (const juce::Image& picture, juce::Rectangle<float> area)
    {
        if (! picture.isValid() || area.isEmpty())
            return {};

        return picture.rescaled (juce::roundToInt (area.getWidth() * 2.0f), juce::roundToInt (area.getHeight() * 2.0f),
                                 juce::Graphics::highResamplingQuality);
    }


    /** A mic render (assets/mic-renders, made by assets/blender-mics.py)
        and the points mics.json gives for it, as shares of its width and
        height: each mic's front (where its pickup line starts), the stand's
        foot on the floor, and the close mic's clip (which sits on the rim). */
    struct MicArt
    {
        juce::Point<float> size;                  // in render pixels
        float pxPerMetre = 1.0f;                  // the render's scale
        std::vector<juce::Point<float>> tips;
        std::optional<juce::Point<float>> foot, clip;
    };

    const MicArt& micArt (const char* name)   // a plain char*: a temporary String here makes GCC warn about the returned reference
    {
        // Read once. Only plain values are kept (the images come from
        // kitart::load(), which juce::ImageCache keeps), so nothing of JUCE's
        // outlives it at shutdown.
        static const auto all = []
        {
            std::map<juce::String, MicArt> result;
            // The parsed var is kept in a variable: getDynamicObject() points into it.
            const auto json = juce::JSON::parse (juce::String::fromUTF8 (LithiumAssets::mics_json, LithiumAssets::mics_jsonSize));

            auto point = [] (const juce::var& v) -> std::optional<juce::Point<float>>
            {
                if (! v.isArray() || v.size() < 2)
                    return std::nullopt;

                return juce::Point<float> ((float) (double) v[0], (float) (double) v[1]);
            };

            if (const auto* object = json.getDynamicObject())
                for (const auto& entry : object->getProperties())
                {
                    MicArt art;
                    art.size = point (entry.value["size"]).value_or (juce::Point<float> (1.0f, 1.0f));
                    art.pxPerMetre = (float) (double) entry.value["pxPerMetre"];

                    if (const auto* tips = entry.value["tips"].getArray())
                        for (const auto& t : *tips)
                            if (const auto tip = point (t))
                                art.tips.push_back (*tip);

                    art.foot = point (entry.value["foot"]);
                    art.clip = point (entry.value["clip"]);
                    result[entry.name.toString()] = std::move (art);
                }

            return result;
        }();

        static const MicArt none;
        const auto it = all.find (juce::String (name));
        return it != all.end() ? it->second : none;
    }

    /** About how wide a drum is, in metres, by role: with the drum's picture
        on the card, that gives the card's scale for the mics around it. */
    float drumWidthMetres (PieceRole role)
    {
        switch (role)
        {
            case PieceRole::kick:     return 0.56f;
            case PieceRole::snare:    return 0.36f;
            case PieceRole::tom:      return 0.30f;
            case PieceRole::floorTom: return 0.40f;
            case PieceRole::hihat:    return 0.36f;
            case PieceRole::crash:
            case PieceRole::china:    return 0.46f;
            case PieceRole::ride:     return 0.53f;
            case PieceRole::other:    break;
        }

        return 0.30f;
    }

    juce::Rectangle<float> boundsOf (const std::vector<juce::Point<float>>& points);

    /** Where a render's visible pixels are, as shares of its width and
        height: the renders have transparent margins of different sizes, so
        an outline drawn round their whole image isn't even round what shows.
        Measured once per image (they're juce::ImageCache'd). */
    juce::Rectangle<float> opaqueShare (const juce::Image& image, float topShare)
    {
        static std::map<juce::String, juce::Rectangle<float>> measured;   // message thread only
        const auto key = juce::String::toHexString ((juce::pointer_sized_int) image.getPixelData().get()) + juce::String (topShare);

        if (const auto it = measured.find (key); it != measured.end())
            return it->second;

        int x0 = image.getWidth(), y0 = image.getHeight(), x1 = -1, y1 = -1;
        const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);

        for (int y = 0; y < juce::roundToInt ((float) data.height * topShare); ++y)
            for (int x = 0; x < data.width; ++x)
                if (data.getPixelColour (x, y).getAlpha() > 60)
                {
                    x0 = std::min (x0, x); x1 = std::max (x1, x);
                    y0 = std::min (y0, y); y1 = std::max (y1, y);
                }

        const auto w = (float) std::max (1, image.getWidth()), h = (float) std::max (1, image.getHeight());
        const auto share = x1 < 0 ? juce::Rectangle<float> (0.0f, 0.0f, 1.0f, 1.0f)
                                  : juce::Rectangle<float> ((float) x0 / w, (float) y0 / h, (float) (x1 - x0 + 1) / w, (float) (y1 - y0 + 1) / h);
        return measured[key] = share;
    }

    /** Around a room pair's two mics (not their stands, whose legs and
        cable spread across the floor), the same margin on every side: like
        the overheads' outline, which is round their heads. The mics are in
        the top quarter of the render. */
    juce::Rectangle<float> standsOutline (const auto& placed)
    {
        if (! placed.image.isValid())
            return placed.area;

        constexpr float micsShare = 0.25f;
        return placed.area.getProportion (opaqueShare (placed.image, micsShare)).expanded (8.0f);
    }

    /** The box around some points. (Rectangle::getUnion() skips empty
        rectangles, so it can't grow a box from single points.) */
    juce::Rectangle<float> boundsOf (const std::vector<juce::Point<float>>& points)
    {
        if (points.empty())
            return {};

        auto x0 = points[0].x, y0 = points[0].y, x1 = x0, y1 = y0;

        for (const auto& p : points)
        {
            x0 = std::min (x0, p.x); x1 = std::max (x1, p.x);
            y0 = std::min (y0, p.y); y1 = std::max (y1, p.y);
        }

        return { x0, y0, x1 - x0, y1 - y0 };
    }

    juce::Point<float> middleOf (const std::vector<juce::Point<float>>& points)
    {
        juce::Point<float> sum;

        for (const auto& p : points)
            sum += p;

        return points.empty() ? sum : sum / (float) points.size();
    }

    /** What a mic hears of a drum: a soft cone of light from the mic to the
        drum, brightest at the mic and fading out toward the drum; fainter as
        the level drops, gone at 0. */
    void drawPickup (juce::Graphics& g, juce::Point<float> from, juce::Point<float> to, float spread, juce::Colour colour, float level)
    {
        const auto direction = to - from;
        const auto length = direction.getDistanceFromOrigin();

        if (level <= 0.0f || length < 1.0f)
            return;

        const auto across = juce::Point<float> (-direction.y, direction.x) * (spread / length);
        juce::Path cone;
        cone.addTriangle (from, to + across, to - across);

        juce::ColourGradient light (colour.withAlpha (0.30f * level), from, colour.withAlpha (0.0f), to, false);
        light.addColour (0.55, colour.withAlpha (0.12f * level));
        g.setGradientFill (light);
        g.fillPath (cone);
    }

    void drawPicture (juce::Graphics& g, const juce::Image& scaled, juce::Rectangle<float> area, bool lit)
    {
        if (! scaled.isValid())
            return;

        g.setOpacity (lit ? 1.0f : 0.35f);
        g.drawImage (scaled, area);
        g.setOpacity (1.0f);
    }

    /** The base stage layer's solid pixels' bounds, in render pixels: its
        stands and pedals, without the soft floor shadow that spreads across
        the whole frame. Measured once (it's the same for every kit). */
    juce::Rectangle<int> solidBoundsOfBase()
    {
        static const auto bounds = []
        {
            const auto base = kitart::load ("base.png");

            if (! base.isValid())
                return juce::Rectangle<int>();

            constexpr int solid = 200;   // alpha; the shadow stays well under this
            int x0 = base.getWidth(), y0 = base.getHeight(), x1 = -1, y1 = -1;
            const juce::Image::BitmapData data (base, juce::Image::BitmapData::readOnly);

            for (int y = 0; y < data.height; ++y)
                for (int x = 0; x < data.width; ++x)
                    if (data.getPixelColour (x, y).getAlpha() > solid)
                    {
                        x0 = std::min (x0, x); x1 = std::max (x1, x);
                        y0 = std::min (y0, y); y1 = std::max (y1, y);
                    }

            return x1 < 0 ? juce::Rectangle<int>() : juce::Rectangle<int> (x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        }();

        return bounds;
    }

    /** The base stage layer (stands and pedals) without the soft floor
        shadow the render spreads across its whole frame: on the card, with no
        room around the kit, the shadow would show as a dark box. A copy, so
        the cached original (the Kit tab's) is left as it is. */
    juce::Image withoutShadow (const juce::Image& base)
    {
        if (! base.isValid())
            return {};

        auto result = base.createCopy();
        constexpr int shadow = 140;   // alpha: the shadow stays under this, the stands are well over it
        const juce::Image::BitmapData data (result, juce::Image::BitmapData::readWrite);

        for (int y = 0; y < data.height; ++y)
            for (int x = 0; x < data.width; ++x)
                if (data.getPixelColour (x, y).getAlpha() < shadow)
                    data.setPixelColour (x, y, juce::Colours::transparentBlack);

        return result;
    }

    void drawNote (juce::Graphics& g, const juce::String& note, juce::Rectangle<float> area)
    {
        g.setColour (theme::colours::textMuted);
        g.setFont (theme::body (theme::type::small));
        g.drawFittedText (note, area.toNearestInt(), juce::Justification::centredTop, 2, 0.9f);
    }
}

//==============================================================================
MicsModule::MicsModule (juce::AudioProcessorValueTreeState& s) : Module ("Mics"), state (s)
{
    reset.setTooltip ("Put these levels back to 100%");
    reset.onClick = [this] { resetLevels(); };
    addAndMakeVisible (reset);

    // Emulation. The buttons only flip the parameter: the timer sees the
    // change and rebuilds the view, as it does for one made by the host.
    for (const auto role : { MicRole::overheads, MicRole::room })
    {
        auto& b = emulateButtons[roleIndex (role)];
        b.setName (role == MicRole::room ? "Emulate room" : "Emulate overheads");
        b.onClick = [this, role] { params::setEmulating (state, role, ! params::isEmulating (state, role)); };
        addChildComponent (b);
    }

    emulationOff.setName ("Emulated mics on or off");
    emulationOff.onClick = [this]
    {
        if (micIsEmulated)
            params::setEmulating (state, micRole, ! params::isEmulating (state, micRole));
    };
    addChildComponent (emulationOff);

    // The overheads' and room's own channels are a drum menu away; these go there.
    // ("Edit overheads": the user's choice over "Configure" and "Go to".)
    for (const auto role : { MicRole::overheads, MicRole::room })
    {
        auto& b = configureButtons[roleIndex (role)];
        b.setName (role == MicRole::room ? "Edit room" : "Edit overheads");
        b.setButtonText (role == MicRole::room ? "Edit room" : "Edit overheads");
        b.setTooltip (role == MicRole::room ? "Open the room mics' channel: every drum's level in them, and their sound"
                                            : "Open the overheads' channel: every drum's level in them, and their sound");
        b.onClick = [this, role]
        {
            if (onOpenMic != nullptr && configureSlots[roleIndex (role)] >= 0)
                onOpenMic (configureSlots[roleIndex (role)]);
        };
        addChildComponent (b);
    }

    roomChoice.addItemList (emulation::roomNames(), 1);
    roomChoice.setTooltip ("The room the emulated room mics are in");
    roomAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, params::emulatedRoomIr, roomChoice);
    addChildComponent (roomChoice);

    // The picture follows the knobs, host automation included. Like the
    // other graphs, it polls the parameters on a timer rather than listening:
    // hosts can set parameters from the audio thread, and a listener would
    // run (and repaint) there.
    startTimerHz (30);
}

MicsModule::~MicsModule() = default;

std::array<bool, numMicRoles> MicsModule::emulationState() const
{
    return { false, params::isEmulating (state, MicRole::overheads), params::isEmulating (state, MicRole::room) };
}

void MicsModule::show (std::shared_ptr<const KitInfo> k, params::ChannelId channel)
{
    // Destroying the knobs also destroys their parameter attachments.
    entries.clear();
    hovered = -1;
    drumPicture = {};
    overlay = {};
    view = View::none;
    kit = std::move (k);
    shownChannel = channel;
    shownEmulation = emulationState();
    micIsEmulated = false;

    for (auto& b : emulateButtons)
        b.setVisible (false);

    emulationOff.setVisible (false);
    roomChoice.setVisible (false);
    configureSlots = { -1, -1, -1 };

    for (auto& b : configureButtons)
        b.setVisible (false);

    auto addEntry = [this] (int p, MicRole role, juce::String name, juce::String tooltip, bool available, juce::String note)
    {
        Entry e;
        e.piece = p;
        e.parameterId = params::pieceParamId (p, sendIds[roleIndex (role)]);
        e.level = state.getRawParameterValue (e.parameterId);
        e.available = available;
        e.note = std::move (note);
        e.knob = std::make_unique<Knob> (std::move (name));
        e.knob->attach (state, e.parameterId);
        e.knob->setTooltip (tooltip);
        e.knob->setArcColour (theme::groupColour (info (kit->pieces[(size_t) p].role).group));
        e.knob->setEnabled (available);
        addAndMakeVisible (*e.knob);
        entries.push_back (std::move (e));
    };

    if (kit == nullptr || ! channel.isValid())
    {
        setTitle ("Mics");
    }
    else if (channel.isPiece && juce::isPositiveAndBelow (channel.slot, (int) kit->pieces.size()))
    {
        view = View::drum;
        const auto& drum = kit->pieces[(size_t) channel.slot];
        setTitle (titlePrefix() + drum.name);

        // The drum's render in the kit's finish, as on the Kit tab's slot cards.
        drumPicture.piece = channel.slot;
        drumPicture.full = SlotGrid::pictureFor (*kit, channel.slot);

        std::array<bool, numMicRoles> kitHas {};

        for (const auto& m : kit->mics)
            if (! m.emulated)
                kitHas[roleIndex (m.role)] = true;

        const std::array<juce::String, numMicRoles> missing { "No close mics in this kit", "No overheads in this kit", "No room mics in this kit" };

        for (size_t r = 0; r < (size_t) numMicRoles; ++r)
        {
            const auto role = static_cast<MicRole> (r);
            auto available = kitHas[r];
            auto note = available ? juce::String() : missing[r];
            auto emulated = false;

            // A drum no close mic recorded has a silent channel (see KitPieceInfo::hasDirectSignal).
            if (role == MicRole::close && available && ! drum.hasDirectSignal)
            {
                available = false;
                note = "Not on the close mics";
            }

            // Mics the kit doesn't have, but can emulate: an Emulate button,
            // or, once they're on, their knob, marked as emulated.
            if (! kitHas[r] && kit->emulatedSlot (role) >= 0)
            {
                auto& button = emulateButtons[r];
                emulated = shownEmulation[r];
                available = emulated;
                note = emulated ? juce::String() : missing[r];
                button.setButtonText (emulated ? "Turn off" : "Emulate");
                button.setTooltip (emulated ? juce::String ("Turn the emulated ") + (role == MicRole::room ? "room mics" : "overheads") + " off"
                                   : role == MicRole::room ? juce::String ("This kit was recorded without room mics. Emulate a pair from its other mics, "
                                                                           "in a built-in room. You can turn them off at any time.")
                                                           : juce::String ("This kit was recorded without overheads. Emulate a pair over the kit from "
                                                                           "its close mics. You can turn them off at any time."));
                button.setVisible (true);
            }

            addEntry (channel.slot, role, roleNames[r], roleTooltips[r], available, note);
            entries.back().emulated = emulated;

            // Edit overheads / Edit room: the first overhead or room mic with a channel of its
            // own (a kit of one stereo pair sends it to the drums' channels instead).
            if (role != MicRole::close)
            {
                for (size_t m = 0; m < kit->mics.size() && configureSlots[r] < 0; ++m)
                    if (kit->mics[m].role == role && ! kit->mics[m].direct && params::hasMicChannel (state, *kit, (int) m))
                        configureSlots[r] = (int) m;

                configureButtons[r].setVisible (configureSlots[r] >= 0);
            }

            if (emulated)
                entries.back().knob->setTooltip (juce::String (roleTooltips[r]) + " (emulated: this kit wasn't recorded with them)");
        }

        if (std::none_of (entries.begin(), entries.end(), [] (const Entry& e) { return e.available; }))
            overlay = drum.name + " isn't on any of this kit's mics.";
    }
    else if (! channel.isPiece && juce::isPositiveAndBelow (channel.slot, (int) kit->mics.size()))
    {
        view = View::mic;
        const auto& mic = kit->mics[(size_t) channel.slot];
        micRole = mic.role;
        micIsEmulated = mic.emulated;
        setTitle (titlePrefix() + mic.name + (mic.emulated ? " (emulated)" : ""));

        if (micIsEmulated)
        {
            emulationOff.setButtonText (shownEmulation[roleIndex (micRole)] ? "Turn off" : "Emulate");
            emulationOff.setTooltip (shownEmulation[roleIndex (micRole)] ? "Turn these emulated mics off" : "Turn these emulated mics on");
            emulationOff.setVisible (true);
            roomChoice.setVisible (micRole == MicRole::room && shownEmulation[roleIndex (micRole)]);

            if (! shownEmulation[roleIndex (micRole)])
                overlay = juce::String ("This kit was recorded without ") + (micRole == MicRole::room ? "room mics" : "overheads")
                          + ", and the emulated ones are switched off.";
        }

        if (kitPicture.builtFor != kit)
            buildKitPicture();

        for (size_t p = 0; p < kit->pieces.size(); ++p)
        {
            const auto& drum = kit->pieces[p];
            const auto available = micRole != MicRole::close || drum.hasDirectSignal;

            // The knob draws no name: paintMicView() fits the drum's name to its column.
            addEntry ((int) p, micRole, {}, "How loud the " + drum.name.toLowerCase() + " is in the " + mic.name.toLowerCase(),
                      available, available ? juce::String() : "Not on the close mics");
        }

        if (entries.empty() && overlay.isEmpty())
            overlay = "This kit has no drums.";
    }

    for (auto& e : entries)
    {
        e.knob->setVisible (overlay.isEmpty());

        // The mic view draws a line to the drum whose knob is under the mouse
        // (see mouseEnter()); true: the knob's slider inside it reports too.
        if (view == View::mic)
            e.knob->addMouseListener (this, true);
        e.shownLevel = levelOf (e);
    }

    reset.setVisible (view != View::none && overlay.isEmpty());
    resized();
    repaint();
}

void MicsModule::buildKitPicture()
{
    // The Kit tab's stage, flattened into one picture and cropped to the
    // kit: its layers (layout.json), back to front, each drum in the finish
    // of the kit its recordings come from, and a slot without a drum faded,
    // as KitStage draws them. Built once per kit; the layers are juce::ImageCache'd.
    kitPicture = {};
    kitPicture.builtFor = kit;
    kitPicture.pieceRects.assign (kit->pieces.size(), {});

    const auto json = juce::JSON::parse (juce::String::fromUTF8 (LithiumAssets::layout_json, LithiumAssets::layout_jsonSize));
    const auto* order = json["drawOrderBackToFront"].getArray();

    if (order == nullptr)
        return;

    auto rectOf = [&json] (const juce::String& name)
    {
        const auto& r = json["pieces"][juce::Identifier (name)];
        return juce::Rectangle<int> ((int) r["x"], (int) r["y"], (int) r["w"], (int) r["h"]);
    };

    // The crop: the whole kit, stands and all. That's every drum slot's
    // rectangle plus the base layer's solid pixels (its stands and pedals;
    // the faint floor shadow around them doesn't count), with a margin.
    juce::Rectangle<int> crop = solidBoundsOfBase();

    for (const auto& name : *order)
        if (slotFromRenderKey (name.toString().toStdString()).has_value())
            crop = crop.isEmpty() ? rectOf (name.toString()) : crop.getUnion (rectOf (name.toString()));

    if (crop.isEmpty())
        return;

    crop = crop.expanded (16);

    // A slot without a drum is drawn in the kit's own finish: that of its
    // pieces that weren't swapped in (see KitStage::showFinishes).
    juce::String ownKit;

    for (const auto& p : kit->pieces)
        if (p.swappedFrom.isEmpty())
        {
            ownKit = p.recordedIn;
            break;
        }

    juce::Image image (juce::Image::ARGB, crop.getWidth(), crop.getHeight(), true);

    {
        // A Graphics drawing into an image must be gone before the image is
        // used, hence the scope.
        juce::Graphics g (image);

        for (const auto& nameVar : *order)
        {
            const auto name = nameVar.toString();
            const auto rect = rectOf (name);
            const auto slot = slotFromRenderKey (name.toStdString());
            const auto piece = slot.has_value() ? kit->slotPiece[(size_t) index (*slot)] : -1;
            const auto kitId = piece >= 0 ? kit->pieces[(size_t) piece].recordedIn : ownKit;

            // A finish's layer only counts if it's cut to the same rectangle.
            // Made once per card (it's the same for every kit), held by the card
            // rather than a static, so it's gone before JUCE shuts down.
            if (! slot.has_value() && ! cleanBase.isValid())
                cleanBase = withoutShadow (kitart::load (name + ".png"));

            auto layer = slot.has_value() ? kitart::stageLayer (kitId, name) : cleanBase;

            if (! layer.isValid() || layer.getBounds() != rect.withZeroOrigin())
                layer = kitart::load (name + ".png");

            g.setOpacity (slot.has_value() && piece < 0 ? 0.3f : 1.0f);
            g.drawImageAt (layer, rect.getX() - crop.getX(), rect.getY() - crop.getY());

            if (juce::isPositiveAndBelow (piece, (int) kit->pieces.size()))
                kitPicture.pieceRects[(size_t) piece] = rect.translated (-crop.getX(), -crop.getY()).toFloat();

            if (slot.has_value())
                kitPicture.hitLayers.push_back ({ piece, rect.translated (-crop.getX(), -crop.getY()), layer });
        }
    }

    kitPicture.full = image;
}

float MicsModule::levelOf (const Entry& e) const
{
    return e.level != nullptr ? juce::jlimit (0.0f, 1.0f, e.level->load() / 100.0f) : 0.0f;
}

void MicsModule::timerCallback()
{
    // Emulation switched on or off (here, on another page, or by the host):
    // the view is built again, even while hidden, so it's right when shown.
    if (kit != nullptr && emulationState() != shownEmulation)
    {
        show (kit, shownChannel);
        return;
    }

    if (! isShowing())
        return;

    auto changed = false;

    for (auto& e : entries)
    {
        const auto level = levelOf (e);

        if (std::abs (level - e.shownLevel) > 0.001f)
        {
            e.shownLevel = level;
            changed = true;
        }
    }

    if (changed)
        repaint();
}

void MicsModule::resetLevels()
{
    for (const auto& e : entries)
    {
        if (! e.available)
            continue;

        if (auto* p = state.getParameter (e.parameterId))
        {
            // A gesture around the change, so the host records it as one edit.
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        }
    }
}

//==============================================================================
MicsModule::PlacedMics MicsModule::placeMics (const juce::String& name, float scale, Anchor anchor, juce::Point<float> at)
{
    const auto& art = micArt (name.toRawUTF8());
    PlacedMics placed;
    placed.image = kitart::load (name + ".png");

    if (! placed.image.isValid())
        return placed;

    const auto share = anchor == Anchor::clip ? art.clip.value_or (juce::Point<float> (0.5f, 0.5f))
                     : anchor == Anchor::foot ? art.foot.value_or (juce::Point<float> (0.5f, 1.0f))
                                              : middleOf (art.tips);
    const auto w = art.size.x * scale, h = art.size.y * scale;
    placed.area = { at.x - share.x * w, at.y - share.y * h, w, h };
    placed.scaled = scaledFor (placed.image, placed.area);

    for (const auto& t : art.tips)
        placed.tips.push_back (placed.area.getRelativePoint (t.x, t.y));

    return placed;
}

void MicsModule::drawMics (juce::Graphics& g, const PlacedMics& placed)
{
    if (! placed.scaled.isValid())
        return;

    if (juce::approximatelyEqual (placed.splitOffset, 0.0f))
    {
        g.drawImage (placed.scaled, placed.area);
        return;
    }

    // Each half of the pair (one mic each), moved out by splitOffset (in, if
    // it's negative). This
    // drawImage overload takes whole pixels for both rectangles.
    const auto& image = placed.scaled;
    const auto sourceHalf = image.getWidth() / 2;
    const auto dest = placed.area.toNearestInt();
    const auto destHalf = dest.getWidth() / 2;
    const auto offset = juce::roundToInt (placed.splitOffset);
    g.drawImage (image, dest.getX() - offset, dest.getY(), destHalf, dest.getHeight(), 0, 0, sourceHalf, image.getHeight());
    g.drawImage (image, dest.getX() + destHalf + offset, dest.getY(), dest.getWidth() - destHalf, dest.getHeight(),
                 sourceHalf, 0, image.getWidth() - sourceHalf, image.getHeight());
}

void MicsModule::resized()
{
    layoutHeaderButtons (nullptr, &reset);

    // Turn off (or Emulate) beside Reset in the header.
    emulationOff.setBounds (reset.getBounds().withWidth (70).translated (-76, 0));
    const auto c = getContentArea().toFloat();
    floorY = c.getBottom() - 4.0f;
    const auto half = (float) knobWidth * 0.5f;

    if (view == View::drum && entries.size() == (size_t) numMicRoles)
    {
        // The drum a little left of centre (on the floor, or up on its stand
        // for a cymbal); its close mic clipped to its rim (the kick's on a low
        // stand in front of it); the overheads hanging above it; the room pair
        // standing off to the right.
        const auto role = kit->pieces[(size_t) drumPicture.piece].role;
        const auto plate = isPlate (role);
        // Cymbals low enough to leave the overheads room above them; the
        // hi-hat (its plates near the bottom of its picture) higher.
        const auto plateTop = role == PieceRole::hihat ? c.getY() + 96.0f : c.getY() + 150.0f;
        const auto base = pictureBoxSize (role);

        // Drawn a little under the renders' size, so the scene sits in the card with room around it;
        // a cymbal only as far as it fits between its top and the floor, and a drum only as far as
        // leaves the overheads room above it.
        const auto grow = juce::jmin (0.85f, plate ? (floorY - plateTop) / base.y : (c.getHeight() - 96.0f) / base.y);

        const auto size = base * grow;
        const auto boxTop = plate ? plateTop : floorY - size.y;
        const auto box = juce::Rectangle<float> (c.getX() + c.getWidth() * 0.47f - size.x * 0.5f, boxTop, size.x, size.y);
        drumPicture.area = placePicture (drumPicture.full, box, plate);
        drumPicture.scaled = scaledFor (drumPicture.full, drumPicture.area);
        const auto aimShare = aimOnPicture (role);
        drumPicture.aim = drumPicture.area.getRelativePoint (aimShare.x, aimShare.y);
        const auto& drum = drumPicture.area;

        // The card's scale around the drum: its picture's width over its real width.
        const auto pxPerMetre = drum.getWidth() / drumWidthMetres (role);

        auto& closeMic = drumMics[roleIndex (MicRole::close)];

        if (role == PieceRole::kick)
        {
            // On the floor, the mic's front just short of the kick's shell.
            // Drawn smaller than the drum's scale (the render's mic is bigger
            // than life), so it doesn't crowd the kick in the card.
            constexpr float kickMicSize = 0.6f;
            const auto& art = micArt ("mics-kick");
            const auto scale = pxPerMetre / art.pxPerMetre * kickMicSize;
            const auto tip = art.tips.empty() ? juce::Point<float> (1.0f, 0.2f) : art.tips.front();
            const auto foot = art.foot.value_or (juce::Point<float> (0.5f, 1.0f));
            const auto footX = drum.getX() - 4.0f - (tip.x - foot.x) * art.size.x * scale;
            closeMic = placeMics ("mics-kick", scale, Anchor::foot, { footX, floorY });
        }
        else
        {
            if (plate)
            {
                // A cymbal's or the hi-hat's: on a tall boom stand like the
                // room mics', at their scale, the mic's front just off the
                // plate's near edge. If that would put the stand's feet below
                // the card's floor, it's drawn just small enough to stand on it.
                const auto& roomArt = micArt ("mics-room");
                const auto& art = micArt ("mics-cymbal");
                const auto gap = role == PieceRole::hihat ? juce::Point<float> (32.0f, 26.0f)    // the hi-hat's mic backed off a little more
                                                          : juce::Point<float> (10.0f, 18.0f);
                const auto front = juce::Point<float> (drum.getX() - gap.x, drumPicture.aim.y - gap.y);
                const auto tip = art.tips.empty() ? juce::Point<float> (1.0f, 0.2f) : art.tips.front();
                const auto foot = art.foot.value_or (juce::Point<float> (0.5f, 1.0f));
                const auto cardPxPerMetre = c.getHeight() * roomHeight / roomArt.size.y * roomArt.pxPerMetre;
                const auto onFloor = (floorY - front.y) / std::max (1.0f, (foot.y - tip.y) * art.size.y);
                closeMic = placeMics ("mics-cymbal", std::min (cardPxPerMetre / art.pxPerMetre, onFloor), Anchor::tips, front);
            }
            else
            {
                // A drum's: held off to its left, its front pointing across at the head.
                const auto& art = micArt ("mics-close");
                constexpr float micSize = 0.8f;   // a little under the drum's scale, so it doesn't crowd the head
                closeMic = placeMics ("mics-close", pxPerMetre / art.pxPerMetre * micSize, Anchor::tips,
                                      { drum.getX() - 36.0f, drumPicture.aim.y - 10.0f });
            }
        }

        {
            // About half the drum's width apart, so their cones (straight
            // down) just overlap over it; their cables run up off the card.
            const auto& art = micArt ("mics-overheads");
            const auto spread = art.tips.size() == 2 ? (art.tips[1].x - art.tips[0].x) * art.size.x : art.size.x;
            // The mics are drawn bigger than that spacing alone would make
            // them, with each half of the pair moved back in so the gap stays
            // the same (see drawMics()).
            const auto apart = juce::jlimit (60.0f, 150.0f, drum.getWidth() * 0.5f) * 1.05f;
            constexpr float micScale = 1.7f;
            const auto tipsY = std::min (c.getY() + 72.0f, drum.getY() - 24.0f);
            auto& pair = drumMics[roleIndex (MicRole::overheads)];
            pair = placeMics ("mics-overheads", apart * micScale / spread, Anchor::tips, { drumPicture.aim.x, tipsY });
            pair.splitOffset = 0.5f * apart * (1.0f - micScale);   // negative: in

            if (pair.tips.size() == 2)
            {
                pair.tips[0].x -= pair.splitOffset;
                pair.tips[1].x += pair.splitOffset;
            }
        }

        {
            // Three-quarters of the card's height, standing a little to the
            // drum's right (no further right than the card allows).
            const auto& art = micArt ("mics-room");
            const auto scale = c.getHeight() * roomHeight / art.size.y;
            const auto foot = art.foot.value_or (juce::Point<float> (0.5f, 1.0f));
            const auto footX = std::min (c.getRight() - 110.0f, drum.getRight() + 80.0f + foot.x * art.size.x * scale);
            drumMics[roleIndex (MicRole::room)] = placeMics ("mics-room", scale, Anchor::foot, { footX, floorY });
        }

        // Each knob beside its mics: left of the close mic (above the kick's
        // and a cymbal's, clear of their stands),
        // right of the overheads, top right of the room pair.
        const auto& oh = drumMics[roleIndex (MicRole::overheads)];
        const auto& room = drumMics[roleIndex (MicRole::room)];
        const auto closeTip = closeMic.tips.empty() ? drumPicture.aim : closeMic.tips.front();
        const auto closeKnob = role == PieceRole::kick ? juce::Point<float> (std::max (c.getX() + half, closeTip.x - 70.0f), closeTip.y - 74.0f)
                             : plate                ? juce::Point<float> (std::max (c.getX() + half, closeTip.x - 95.0f), closeTip.y - 50.0f)   // above its boom
                                                    : juce::Point<float> (std::max (c.getX() + half, closeMic.area.getX() - half - 6.0f), closeMic.area.getCentreY() - 6.0f);
        // The room knob at the top right of the room mics: beside the
        // right-hand mic's head, kept inside the card.
        auto roomKnob = juce::Point<float> (room.area.getX() - half + 10.0f, c.getY() + c.getHeight() * 0.45f);

        if (! room.tips.empty())
        {
            const auto rightMost = *std::max_element (room.tips.begin(), room.tips.end(), [] (auto a, auto b) { return a.x < b.x; });
            roomKnob = { std::min (c.getRight() - half, rightMost.x + half + 28.0f),   // clear of the stand, with its Turn off button under it
                         std::max (c.getY() + knobHeight * 0.5f, rightMost.y - 6.0f) };
        }

        const std::array<juce::Point<float>, numMicRoles> knobCentres {
            closeKnob,
            juce::Point<float> (oh.area.getRight() + oh.splitOffset + half + 8.0f, std::max (c.getY() + knobHeight * 0.5f, middleOf (oh.tips).y - 30.0f)),
            roomKnob };

        for (size_t r = 0; r < entries.size(); ++r)
        {
            entries[r].knob->setBounds (juce::Rectangle<float> ((float) knobWidth, (float) knobHeight).withCentre (knobCentres[r]).toNearestInt());

            // Under the knob: below its note while it has one ("No room
            // mics in this kit"), right under it once the mics are emulated.
            auto& button = emulateButtons[r];
            const auto knob = entries[r].knob->getBounds().toFloat();
            const auto below = entries[r].emulated ? 6.0f : 40.0f;
            button.setBounds (juce::Rectangle<float> (78.0f, 22.0f)
                                  .withCentre ({ knob.getCentreX(), knob.getBottom() + below + 11.0f })
                                  .constrainedWithin (c)
                                  .toNearestInt());

            // Edit overheads / Edit room under the knob, or under Turn off.
            const auto configureTop = button.isVisible() ? (float) button.getBottom() + 6.0f : knob.getBottom() + 6.0f;
            configureButtons[r].setBounds (juce::Rectangle<float> (static_cast<MicRole> (r) == MicRole::room ? 84.0f : 112.0f, 22.0f)
                                               .withCentre ({ knob.getCentreX(), configureTop + 11.0f })
                                               .constrainedWithin (c)
                                               .toNearestInt());
        }
    }
    else if (view == View::mic && ! entries.empty())
    {
        // The knobs along the bottom (two rows once there are more than
        // fit); the kit over them, with the overheads hanging over it, or the
        // room pair standing at its right.
        const auto n = (int) entries.size();
        const auto rows = n > maxPerRow ? 2 : 1;
        const auto perRow = (n + rows - 1) / rows;
        const auto columnWidth = c.getWidth() / (float) perRow;
        auto rowTop = c.getBottom() - (float) (rows * knobHeight + (rows - 1) * 4);
        const auto knobsTop = rowTop;

        for (int row = 0, first = 0; row < rows; ++row)
        {
            const auto count = std::min (perRow, n - first);
            auto x = c.getX() + (c.getWidth() - (float) count * columnWidth) * 0.5f;

            for (int i = first; i < first + count; ++i, x += columnWidth)
                entries[(size_t) i].knob->setBounds (juce::Rectangle<float> (x, rowTop, columnWidth, (float) knobHeight).toNearestInt());

            first += count;
            rowTop += (float) knobHeight + 4.0f;
        }

        // An emulated pair says so along the top, with its room choice.
        auto top = c.getY();

        if (micIsEmulated)
        {
            roomChoice.setBounds (juce::Rectangle<float> (c.getX() + 8.0f, c.getY() + 4.0f, 150.0f, 22.0f)
                                      .translated (emulatedTagWidth + 8.0f, 0.0f).toNearestInt());
            top += 30.0f;
        }

        // The overheads hang above the kit, so they get a strip of the card
        // over it for themselves and their cables.
        const auto room = micRole == MicRole::room;
        top += room ? 4.0f : 78.0f;
        const auto space = juce::Rectangle<float> (c.getX(), top, c.getWidth(), knobsTop - 8.0f - top);
        // The room pair a little shorter than the kit picture, so it reads
        // as mics in front of the kit rather than towering over it.
        const auto& roomArt = micArt ("mics-room");
        const auto roomScale = space.getHeight() * 0.62f / roomArt.size.y;
        const auto roomWidth = roomArt.size.x * roomScale;
        constexpr float gap = 16.0f;

        kitPicture.area = kitPicture.full.isValid()
                              ? juce::RectanglePlacement (juce::RectanglePlacement::centred)
                                    .appliedTo (kitPicture.full.getBounds().toFloat(), room ? space.withTrimmedRight (roomWidth + gap) : space)
                              : juce::Rectangle<float>();

        if (room)   // the kit and the room pair centred together
            kitPicture.area.setX (c.getCentreX() - (kitPicture.area.getWidth() + gap + roomWidth) * 0.5f);

        kitPicture.scaled = scaledFor (kitPicture.full, kitPicture.area);

        if (room)
        {
            const auto foot = roomArt.foot.value_or (juce::Point<float> (0.5f, 1.0f));
            kitMics = placeMics ("mics-room", roomScale, Anchor::foot,
                                 { kitPicture.area.getRight() + gap + foot.x * roomWidth,
                                   kitPicture.area.isEmpty() ? space.getBottom() : kitPicture.area.getBottom() });
        }
        else
        {
            const auto& art = micArt ("mics-overheads");
            const auto spread = art.tips.size() == 2 ? (art.tips[1].x - art.tips[0].x) * art.size.x : art.size.x;
            // Small, hanging above the cymbals in the strip over the kit, with
            // some of their cables showing: a fixed size and place, whatever
            // the kit's size. The render's pair is drawn small, then each half
            // moved out so the mics are pairWidth apart (see drawMics()).
            constexpr float pairWidth = 190.0f, frontsDown = 66.0f;   // between the two mics' fronts; from the content's top
            constexpr float micScale = 0.42f;                         // of the size that would make the render pairWidth wide
            kitMics = placeMics ("mics-overheads", pairWidth * micScale / spread, Anchor::tips,
                                 { kitPicture.area.getCentreX(), c.getY() + frontsDown });
            kitMics.splitOffset = 0.5f * pairWidth * (1.0f - micScale);

            if (kitMics.tips.size() == 2)
            {
                kitMics.tips[0].x -= kitMics.splitOffset;
                kitMics.tips[1].x += kitMics.splitOffset;
            }
        }
    }
}

juce::Point<float> MicsModule::aimOnKit (int piece) const
{
    const auto& r = kitPicture.pieceRects[(size_t) piece];
    const auto share = aimOnStage (kit->pieces[(size_t) piece].role);
    const auto scale = kitPicture.area.getWidth() / (float) kitPicture.full.getWidth();
    return kitPicture.area.getPosition() + r.getRelativePoint (share.x, share.y) * scale;
}

void MicsModule::paint (juce::Graphics& g)
{
    Module::paint (g);

    if (overlay.isNotEmpty() || view == View::none)
    {
        g.setColour (theme::colours::textMuted);
        g.setFont (theme::body (theme::type::body));
        g.drawFittedText (overlay.isNotEmpty() ? overlay : juce::String ("Select a drum, or an overhead or room channel."),
                          getContentArea().reduced (theme::space::lg), juce::Justification::centred, 3);
        return;
    }

    if (view == View::drum && entries.size() == (size_t) numMicRoles)
        paintDrumView (g);
    else if (view == View::mic)
        paintMicView (g);
}

void MicsModule::paintDrumView (juce::Graphics& g)
{
    using namespace theme;
    const auto colour = groupColour (info (kit->pieces[(size_t) drumPicture.piece].role).group);
    const auto head = drumPicture.aim;
    const auto c = getContentArea().toFloat();
    const auto spread = drumPicture.area.getWidth() * 0.28f;

    const auto& closeEntry = entries[roleIndex (MicRole::close)];
    const auto& overheadEntry = entries[roleIndex (MicRole::overheads)];
    const auto& roomEntry = entries[roleIndex (MicRole::room)];
    const auto& closeMic = drumMics[roleIndex (MicRole::close)];
    const auto& overheads = drumMics[roleIndex (MicRole::overheads)];
    const auto& room = drumMics[roleIndex (MicRole::room)];

    // The cables run off the top of the card's content, not into its title.
    g.saveState();
    g.reduceClipRegion (getContentArea());

    // Back to front: the overhead and room pickups, the drum over their ends,
    // the close mic's pickup over the drum, then the mics.
    // The overheads face straight down, so their light falls straight down
    // (to the drum's height) rather than angling in at it.
    if (overheadEntry.available)
        for (const auto& tip : overheads.tips)
            drawPickup (g, tip, { tip.x, head.y }, spread, colour, levelOf (overheadEntry));

    if (roomEntry.available && ! room.tips.empty())
        drawPickup (g, middleOf (room.tips), head, spread, colour, levelOf (roomEntry));

    drawPicture (g, drumPicture.scaled, drumPicture.area, true);

    if (closeEntry.available && ! closeMic.tips.empty())
    {
        // The kick's mic points straight ahead along its own axis (level,
        // tilted up 4 degrees in its render: assets/blender-mics.py), so its
        // light follows that line to the drum's middle, rather than angling
        // off to the head's centre.
        const auto tip = closeMic.tips.front();
        auto to = head;

        if (kit->pieces[(size_t) drumPicture.piece].role == PieceRole::kick)
        {
            constexpr float kickMicTilt = 4.0f * juce::MathConstants<float>::pi / 180.0f;
            const auto reach = std::max (20.0f, head.x - tip.x);
            to = tip + juce::Point<float> (reach, -reach * std::tan (kickMicTilt));
        }

        drawPickup (g, tip, to, spread * 0.5f, colour, levelOf (closeEntry));
    }

    // An emulated pair is drawn faded inside a dashed outline: there are
    // no such mics in the recording.
    if (roomEntry.emulated)
    {
        g.setOpacity (0.6f);
        g.drawImage (room.scaled, room.area);
        g.setOpacity (1.0f);
        drawEmulatedOutline (g, standsOutline (room));
    }
    else
    {
        drawPicture (g, room.scaled, room.area, roomEntry.available);
    }

    drawPicture (g, closeMic.scaled, closeMic.area, closeEntry.available);
    g.setOpacity (overheadEntry.emulated ? 0.6f : overheadEntry.available ? 1.0f : 0.35f);
    drawMics (g, overheads);
    g.setOpacity (1.0f);

    if (overheadEntry.emulated && ! overheads.tips.empty())
    {
        // Around the two mics' heads (the pair's render also has their cables).
        const auto heads = boundsOf (overheads.tips);
        drawEmulatedOutline (g, heads.expanded (14.0f, 0.0f).withTop (heads.getY() - 44.0f).withBottom (heads.getY() + 10.0f));
    }

    g.restoreState();

    // EMULATED over each emulated pair's knob.
    for (const auto* e : { &overheadEntry, &roomEntry })
        if (e->emulated)
        {
            // As wide as the knob's name under it (drawn the same way, see
            // Knob::paint), so the two line up; wider only if the name is short.
            const auto knob = e->knob->getBounds().toFloat();
            const auto nameWidth = getTrackedTextWidth (juce::String (roleNames[e == &roomEntry ? 2 : 1]).toUpperCase(),
                                                        label (type::caption), 0.08f);
            const auto tagWidth = getTrackedTextWidth ("EMULATED", label (type::caption), 0.08f) + 10.0f;
            drawEmulatedTag (g, { knob.getCentreX(), knob.getY() - 9.0f }, std::max (nameWidth, tagWidth));
        }

    // Top left, clear of the mics: how the drum's sound divides between its three mics.
    {
        const auto bar = juce::Rectangle<float> (c.getX() + 8.0f, c.getY() + 26.0f, 216.0f, 12.0f);
        std::array<float, numMicRoles> shares {};
        auto total = 0.0f;

        for (size_t r = 0; r < shares.size(); ++r)
        {
            shares[r] = entries[r].available ? levelOf (entries[r]) : 0.0f;
            total += shares[r];
        }

        g.setColour (colours::textMuted);
        g.setFont (label (type::caption));
        g.drawText ("MIC BALANCE", bar.translated (0.0f, -20.0f).withHeight (14.0f), juce::Justification::centredLeft, false);

        g.setColour (colours::background);
        g.fillRoundedRectangle (bar, 3.0f);

        auto x = bar.getX();
        const auto cell = bar.getWidth() / (float) numMicRoles;

        for (size_t r = 0; r < shares.size(); ++r)
        {
            const auto share = total > 0.0f ? shares[r] / total : 0.0f;
            const auto width = bar.getWidth() * share;

            if (width >= 1.0f)
            {
                g.setColour (colour.withAlpha (r == 0 ? 1.0f : r == 1 ? 0.65f : 0.35f));
                g.fillRect (juce::Rectangle<float> (x, bar.getY(), width, bar.getHeight()));
                x += width;
            }

            g.setColour (colours::textMuted);
            g.setFont (mono (10.0f));
            constexpr std::array<const char*, numMicRoles> shortNames { "CLOSE", "OH", "ROOM" };
            g.drawText (juce::String (shortNames[r]) + " " + juce::String (juce::roundToInt (share * 100.0f)) + "%",
                        juce::Rectangle<float> (bar.getX() + cell * (float) r, bar.getBottom() + 6.0f, cell, 14.0f), juce::Justification::centredLeft, false);
        }
    }

    // Notes for what this drum or kit doesn't have, under each knob.
    for (const auto& e : entries)
        if (! e.available && e.note.isNotEmpty())
        {
            const auto knob = e.knob->getBounds().toFloat();
            auto area = juce::Rectangle<float> (110.0f, 32.0f).withCentre ({ knob.getCentreX(), knob.getBottom() + 18.0f });
            area = area.constrainedWithin (c);
            drawNote (g, e.note, area);
        }
}

void MicsModule::paintMicView (juce::Graphics& g)
{
    using namespace theme;

    if (micIsEmulated)
    {
        // Along the top: EMULATED, the room choice (a component), and what
        // the pair is made from.
        const auto c = getContentArea().toFloat();
        drawEmulatedTag (g, { c.getX() + 8.0f + emulatedTagWidth * 0.5f, c.getY() + 15.0f }, emulatedTagWidth);

        auto fromClose = false, fromOverheads = false;

        for (size_t m = 0; m < kit->mics.size(); ++m)
            if (((kit->emulationSources >> m) & 1u) != 0)
                (kit->mics[m].role == MicRole::close ? fromClose : fromOverheads) = true;

        const juce::String sources = fromClose && fromOverheads ? "close mics and overheads" : fromOverheads ? "overheads" : "close mics";
        const auto made = juce::String ("Made from the ") + sources + (micRole == MicRole::room ? ", in this room" : "");
        auto text = c.withHeight (30.0f).withTrimmedLeft (8.0f + emulatedTagWidth + 8.0f);

        if (roomChoice.isVisible())
            text.removeFromLeft (158.0f);

        g.setColour (colours::textMuted);
        g.setFont (body (type::small));
        g.drawText (made, text, juce::Justification::centredLeft, true);
    }

    if (kitPicture.scaled.isValid())
        g.drawImage (kitPicture.scaled, kitPicture.area);   // straight on the card, no frame

    // Only the drum whose knob is under the mouse (or being turned) gets its
    // cone of light from the mics: cones to every drum at once bury the kit. Drums with no place on the stage (extras beyond its
    // slots) only have their knob.
    g.saveState();
    g.reduceClipRegion (getContentArea());   // the overheads' cables run off the top

    if (juce::isPositiveAndBelow (hovered, (int) entries.size()) && ! kitMics.tips.empty())
    {
        const auto& e = entries[(size_t) hovered];

        if (e.available && ! kitPicture.pieceRects[(size_t) e.piece].isEmpty() && kitPicture.full.isValid())
        {
            // A cone from each mic of the pair to the drum, about as wide as
            // the drum where it lands.
            const auto colour = groupColour (info (kit->pieces[(size_t) e.piece].role).group);
            const auto drumWidth = kitPicture.pieceRects[(size_t) e.piece].getWidth() * kitPicture.area.getWidth() / (float) kitPicture.full.getWidth();
            const auto drum = aimOnKit (e.piece);

            for (const auto& tip : kitMics.tips)
                drawPickup (g, tip, drum, drumWidth * 0.45f, colour, std::max (0.35f, levelOf (e)));
        }
    }

    g.setOpacity (micIsEmulated ? 0.6f : 1.0f);
    drawMics (g, kitMics);
    g.setOpacity (1.0f);

    if (micIsEmulated && kitMics.scaled.isValid())
    {
        // The room pair's stands, or the overheads' heads.
        if (micRole == MicRole::room)
            drawEmulatedOutline (g, standsOutline (kitMics));
        else if (! kitMics.tips.empty())
        {
            const auto heads = boundsOf (kitMics.tips);
            drawEmulatedOutline (g, heads.expanded (22.0f, 0.0f).withTop (heads.getY() - 30.0f).withBottom (heads.getY() + 8.0f));
        }
    }

    g.restoreState();

    // Each drum's name where the knob would put its own (see Knob::getNameArea),
    // squeezed to fit narrow columns; the hovered one brighter.
    g.setFont (label (type::caption));

    for (size_t i = 0; i < entries.size(); ++i)
    {
        const auto& e = entries[i];
        const auto knob = e.knob->getBounds();
        g.setColour (! e.available       ? colours::textMuted.withMultipliedAlpha (0.6f)
                     : (int) i == hovered ? colours::text
                                          : colours::textMuted);
        g.drawFittedText (kit->pieces[(size_t) e.piece].name.toUpperCase(), knob.withHeight (14).reduced (2, 0), juce::Justification::centred, 1, 0.7f);
    }
}

void MicsModule::drawEmulatedOutline (juce::Graphics& g, juce::Rectangle<float> around)
{
    juce::Path outline, dashed;
    outline.addRoundedRectangle (around, 6.0f);
    const float dashes[] { 5.0f, 4.0f };
    juce::PathStrokeType (1.2f).createDashedStroke (dashed, outline, dashes, 2);
    g.setColour (theme::colours::accent.withAlpha (0.75f));
    g.fillPath (dashed);
}

void MicsModule::drawEmulatedTag (juce::Graphics& g, juce::Point<float> centre, float width)
{
    // Tracked text (a knob's name, and the word in the tag) has its extra
    // spacing after every letter, the last one too, so the letters sit left
    // of the middle of the space it takes, by half a letter's spacing. The
    // pill is centred on the letters, not on that space, so it lines up
    // with the name under it; the word is laid out the same way as the name.
    const auto font = theme::label (theme::type::caption);
    constexpr float tracking = 0.08f;
    const auto inkShift = 0.5f * tracking * font.getHeight();
    const auto textArea = juce::Rectangle<float> (width, 16.0f).withCentre (centre);
    const auto pill = textArea.translated (-inkShift, 0.0f);

    g.setColour (theme::colours::accent.withAlpha (0.16f));
    g.fillRoundedRectangle (pill, 8.0f);
    g.setColour (theme::colours::accent.withAlpha (0.8f));
    g.drawRoundedRectangle (pill.reduced (0.5f), 8.0f, 1.0f);
    g.setColour (theme::colours::accent);
    theme::drawTrackedText (g, "EMULATED", font, tracking, textArea, juce::Justification::centred);
}

//==============================================================================
// The knobs' mouse events reach here too: show() registers this module as a
// mouse listener on each mic-view knob (and its slider inside), so it can tell
// which drum to draw a line to without the Knob class knowing about it.
int MicsModule::entryFor (const juce::Component* c) const
{
    for (size_t i = 0; i < entries.size(); ++i)
        if (c == entries[i].knob.get() || entries[i].knob->isParentOf (c))
            return (int) i;

    return -1;
}

void MicsModule::setHovered (int entry)
{
    if (entry != hovered)
    {
        hovered = entry;
        repaint();
    }
}

void MicsModule::mouseEnter (const juce::MouseEvent& e)
{
    if (! e.mods.isAnyMouseButtonDown())
        setHovered (entryFor (e.eventComponent));
}

void MicsModule::mouseExit (const juce::MouseEvent& e)
{
    // Turning a knob keeps its line even when the drag leaves the knob.
    if (! e.mods.isAnyMouseButtonDown() && entryFor (e.eventComponent) == hovered)
        setHovered (-1);
}

int MicsModule::drumAt (juce::Point<float> p) const
{
    if (overlay.isNotEmpty() || kit == nullptr)
        return -1;

    if (view == View::drum)
        return drumPicture.area.contains (p) ? drumPicture.piece : -1;

    if (view != View::mic || ! kitPicture.full.isValid() || ! kitPicture.area.contains (p))
        return -1;

    // From the card to the picture's pixels, then the frontmost layer that's
    // solid there (soft shadows and antialiased edges don't count).
    constexpr int hitAlphaThreshold = 48;
    const auto scale = kitPicture.area.getWidth() / (float) kitPicture.full.getWidth();
    const auto pixel = ((p - kitPicture.area.getPosition()) / scale).toInt();

    for (auto it = kitPicture.hitLayers.rbegin(); it != kitPicture.hitLayers.rend(); ++it)
    {
        const auto local = pixel - it->rect.getPosition();

        if (it->image.getBounds().contains (local) && it->image.getPixelAt (local.x, local.y).getAlpha() >= hitAlphaThreshold)
            return juce::isPositiveAndBelow (it->piece, (int) kit->pieces.size()) ? it->piece : -1;
    }

    return -1;
}

void MicsModule::mouseDown (const juce::MouseEvent& e)
{
    // The knobs send us their mouse events too (for the hover line): only a
    // click on the card itself, over a drum, strikes it.
    if (e.eventComponent != this || ! onStrike)
        return;

    if (const auto piece = drumAt (e.position); piece >= 0)
        onStrike (piece);
}

void MicsModule::mouseMove (const juce::MouseEvent& e)
{
    if (e.eventComponent == this)
        setMouseCursor (drumAt (e.position) >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void MicsModule::mouseUp (const juce::MouseEvent&)
{
    // The drag is over: keep the line only if the mouse is still on that knob.
    if (juce::isPositiveAndBelow (hovered, (int) entries.size()) && ! entries[(size_t) hovered].knob->isMouseOver (true))
        setHovered (-1);
}

} // namespace lithium
