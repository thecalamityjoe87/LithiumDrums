#include "EditPage.h"

#include "KitArt.h"

#include "SlotGrid.h"
#include "Theme.h"

namespace lithium
{

namespace
{
    constexpr int margin = 16, gap = 12;
    constexpr int cardHeight = 152, cardPadding = 10, cardControlsWidth = 300, topHeight = cardHeight + 18, rowOneHeight = 236, rowTwoHeight = 300;
    constexpr int micTopExtra = 40;   // a mic channel's top band (its card, transient and saturation) is this much taller

    juce::Rectangle<int> contentOf (const juce::Component& module)
    {
        return module.getLocalBounds().reduced (12, 0).withTrimmedTop (32).withTrimmedBottom (10);
    }

    juce::String noteLabel (int note)
    {
        return juce::String (note) + " (" + noteName (note) + ")";
    }
}

//==============================================================================
//==============================================================================
/** The shape of each hit's volume: the attack's ramp, the hold, then the
    decay (or the recording's own fade, at Natural). Reads the parameters
    on a timer, like the EQ graph. */
class EditPage::EnvelopeGraph final : public juce::Component, private juce::Timer
{
public:
    explicit EnvelopeGraph (juce::AudioProcessorValueTreeState& s) : state (s)   { startTimerHz (15); }

    void setPiece (int piece, juce::Colour c)
    {
        attack = state.getParameter (params::pieceParamId (piece, params::attack));
        hold = state.getParameter (params::pieceParamId (piece, params::hold));
        decay = state.getParameter (params::pieceParamId (piece, "decay"));
        colour = c;
        timerCallback();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        const auto bounds = getLocalBounds().toFloat();
        g.setColour (colours::background);
        g.fillRoundedRectangle (bounds, cornerRadius);

        if (attack == nullptr)
            return;

        const auto area = bounds.reduced (26.0f, 14.0f).withTrimmedBottom (8.0f);
        const auto a = shown[0] <= 0.15f ? 0.0f : shown[0] / 1000.0f;
        const auto h = shown[1] / 1000.0f;
        const auto natural = shown[2] >= 99.9f;

        // The same decay law as the engine: a time constant from 30 ms to 4 s.
        const auto tau = 0.03f * std::pow (4.0f / 0.03f, shown[2] / 100.0f);
        const auto seconds = natural ? std::max (1.0f, std::max (a, h) * 3.0f)
                                     : juce::jlimit (0.1f, 8.0f, (std::max (a, h) + 4.6f * tau) * 1.1f);

        auto level = [&] (float t)
        {
            const auto rise = a > 0.0f ? std::min (1.0f, t / a) : 1.0f;
            return rise * (natural || t < h ? 1.0f : std::exp (-(t - h) / tau));
        };

        auto xFor = [&] (float t) { return juce::jmap (t, 0.0f, seconds, area.getX(), area.getRight()); };
        auto yFor = [&] (float v) { return juce::jmap (v, 0.0f, 1.0f, area.getBottom(), area.getY()); };

        // A time grid in round steps.
        const auto step = seconds > 4.0f ? 1.0f : seconds > 1.5f ? 0.5f : seconds > 0.6f ? 0.2f : 0.05f;
        g.setFont (mono (10.0f));

        for (auto t = 0.0f; t <= seconds + 0.001f; t += step)
        {
            g.setColour (colours::border);
            g.fillRect (juce::Rectangle<float> (xFor (t), area.getY(), 1.0f, area.getHeight()));
            g.setColour (colours::textMuted);
            g.drawText (t < 1.0f ? juce::String (juce::roundToInt (t * 1000.0f)) + " ms" : juce::String (t, 1) + " s",
                        juce::Rectangle<float> (xFor (t) - 24.0f, area.getBottom() + 1.0f, 48.0f, 12.0f), juce::Justification::centred);
        }

        juce::Path curve;
        curve.startNewSubPath (xFor (0.0f), yFor (level (0.0f)));

        for (int i = 1; i <= 240; ++i)
        {
            const auto t = seconds * (float) i / 240.0f;
            curve.lineTo (xFor (t), yFor (level (t)));
        }

        auto fill = curve;
        fill.lineTo (area.getRight(), area.getBottom());
        fill.lineTo (area.getX(), area.getBottom());
        fill.closeSubPath();
        g.setColour (colour.withAlpha (0.14f));
        g.fillPath (fill);
        g.setColour (colour);
        g.strokePath (curve, juce::PathStrokeType (2.0f));

        if (natural)
        {
            g.setColour (colours::textMuted);
            g.setFont (body (12.5f));
            g.drawText ("Natural decay: the recording's own fade", area.reduced (8.0f), juce::Justification::topRight, false);
        }
    }

private:
    void timerCallback() override
    {
        if (attack == nullptr)
            return;

        const std::array<float, 3> now { attack->convertFrom0to1 (attack->getValue()), hold->convertFrom0to1 (hold->getValue()),
                                         decay->convertFrom0to1 (decay->getValue()) };

        if (! std::equal (now.begin(), now.end(), shown.begin(), [] (float x, float y) { return juce::exactlyEqual (x, y); }))
        {
            shown = now;
            repaint();
        }
    }

    juce::AudioProcessorValueTreeState& state;
    juce::RangedAudioParameter *attack = nullptr, *hold = nullptr, *decay = nullptr;
    std::array<float, 3> shown {};
    juce::Colour colour;
};

//==============================================================================
/** The drum's picture in the piece card, in a well edged in its group's
    colour. Like a piece on the stage or a slot card, clicking it plays it
    (the articulation shown), and it lights up briefly when the drum is hit. */
class EditPage::PiecePicture final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    std::function<void()> onClick;

    void setPiece (juce::Image picture, juce::Colour groupColour, const juce::String& name)
    {
        image = std::move (picture);
        colour = groupColour;
        setTitle (name);   // for screen readers
        setTooltip (name + ": click to play");
        repaint();
    }

    void flash()
    {
        flashUntil = juce::Time::getMillisecondCounterHiRes() + theme::motion::hitFlashMs;
        startTimerHz (60);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (theme::colours::background);
        g.fillRoundedRectangle (bounds, 6.0f);

        // The hit flash: the group colour, fading out (as on a slot card).
        if (const auto left = (float) ((flashUntil - juce::Time::getMillisecondCounterHiRes()) / theme::motion::hitFlashMs); left > 0.0f)
        {
            g.setColour (colour.withAlpha (0.16f * std::min (1.0f, left)));
            g.fillRoundedRectangle (bounds, 6.0f);
        }

        g.setColour (colour.withAlpha (isMouseOver() ? 0.9f : 0.55f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

        // drawImage is modulated by the current colour's alpha, so go back to opaque.
        g.setColour (juce::Colours::black);

        if (image.isValid())
            g.drawImage (image, getLocalBounds().reduced (8).toFloat(), juce::RectanglePlacement::centred);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onClick)
            onClick();

        flash();   // at once, before the audio thread reports the hit
    }

    void mouseEnter (const juce::MouseEvent&) override   { repaint(); }
    void mouseExit (const juce::MouseEvent&) override    { repaint(); }

private:
    void timerCallback() override
    {
        repaint();

        // One more repaint after the flash is over, to clear it.
        if (juce::Time::getMillisecondCounterHiRes() > flashUntil + 20.0)
            stopTimer();
    }

    juce::Image image;
    juce::Colour colour;
    double flashUntil = 0.0;
};

//==============================================================================
EditPage::EditPage (LithiumDrumsProcessor& p)
    : processor (p),
      picture (std::make_unique<PiecePicture>()),
      model (p),
      envelope (std::make_unique<EnvelopeGraph> (p.getParameters())),
      transient (p.getParameters(), TwoKnobModule::Kind::transient),
      saturation (p.getParameters(), TwoKnobModule::Kind::saturation),
      mics (p.getParameters()),
      comp (p.getParameters())
{
    // Piece bar
    previous.onClick = [this] { stepPiece (-1); };
    next.onClick = [this] { stepPiece (1); };
    pieceBox.setTooltip ("The drum to edit");
    articulationBox.setTooltip ("The articulation to show");
    articulationBox.onChange = [this]
    {
        if (const auto id = articulationBox.getSelectedId(); id > 0)
            showArticulation (id - 1);
    };
    pieceBox.onChange = [this]
    {
        const auto id = pieceBox.getSelectedId();

        if (id > micMenuId)
            showMic (id - micMenuId - 1);
        else if (id > 0 && id - 1 == processor.getSelection().getSelected())
            showPiece (id - 1);   // back from a mic channel to the drum that's still selected
        else if (id > 0)
            processor.getSelection().select (id - 1);
    };

    // The picture and Strike do the same: play the articulation shown.
    auto playShown = [this]
    {
        if (piece >= 0)
            processor.audition (piece, LithiumDrumsProcessor::auditionVelocity, articulation);
    };

    strike.setTooltip ("Strike this articulation at velocity " + juce::String (LithiumDrumsProcessor::auditionVelocity));
    strike.onClick = playShown;
    picture->onClick = playShown;
    // The Mics card: its drum is the one shown, so it plays as Strike does;
    // another drum on the kit (overheads and room views) plays as on the stage.
    mics.onStrike = [this, playShown] (int drum)
    {
        if (drum == piece)
            playShown();
        else
            processor.audition (drum, LithiumDrumsProcessor::auditionVelocity);
    };
    picture->setMouseCursor (juce::MouseCursor::PointingHandCursor);

    crossfade.attach (processor.getParameters(), params::velocityCrossfade);
    mute.setColour (juce::TextButton::buttonOnColourId, theme::colours::textMuted);
    choke.setTooltip ("Choke note: a note that chokes (grabs) this drum, e.g. from an e-kit's cymbal pad");
    chokeLabel.setFont (theme::label (11.0f));
    chokeLabel.setColour (juce::Label::textColourId, theme::colours::textMuted);
    chokeLabel.setJustificationType (juce::Justification::centredRight);
    chokeLabel.setBorderSize ({});

    for (auto* c : std::initializer_list<juce::Component*> { picture.get(), &previous, &next, &pieceBox, &model, &strike, &crossfade, &mute, &solo,
                                                             &chokeLabel, &choke })
        addAndMakeVisible (c);

    addChildComponent (articulationBox);

    // Shaping
    tune.setBipolar (true);
    pitchEnv.setBipolar (true);
    dynamics.setBipolar (true);
    mics.onOpenMic = [this] (int m) { showMic (m); };

    // What each card does, behind its "?", for new users.
    velocityModule.setHelp ("Velocity", {
        "How loud this drum is, and how much its level follows how hard you hit it.",
        "Volume: the drum's level everywhere it's heard: in its own mics, the overheads and the room.",
        "Dynamics: turn it up for a bigger difference between soft and hard hits, or down to even them out. "
        "The hardest hits stay as they are." });
    pitchModule.setHelp ("Pitch", {
        "Tune: raises or lowers the drum's pitch, up to 12 semitones (an octave) either way. "
        "Down makes a drum deeper and longer, up makes it tighter and higher.",
        "Bend: starts each hit higher or lower than its pitch and glides it back, like the pitch drop of a "
        "hard-hit tom. Bend time: how long the glide takes." });
    toneModule.setHelp ("Tone", {
        "Low cut takes away the low end below the frequency you set: boom and rumble. "
        "High cut takes away the top above it: sizzle and air.",
        "Both work on every mic of the drum, so they change it in the overheads and room too. "
        "Turned all the way open, they do nothing." });
    envelopeModule.setHelp ("Envelope", {
        "The shape of each hit over time, drawn in the graph.",
        "Attack fades the start of the hit in, softening the stick. Hold is how long the hit then plays untouched. "
        "Decay shortens the ring after that, like putting a gel or a towel on the drum; at Natural it rings out "
        "as it was recorded." });
    zonesModule.setHelp ("Zones", {
        "Each drum was recorded at several strengths, from soft to hard. The map shows which recording plays "
        "at each velocity (how hard a hit is).",
        "Click the map to hear that velocity. Crossfade blends neighbouring recordings, so there's no jump in "
        "the sound from one to the next." });

    velocityModule.addAndMakeVisible (volume);
    velocityModule.addAndMakeVisible (dynamics);
    pitchModule.addAndMakeVisible (tune);
    pitchModule.addAndMakeVisible (pitchEnv);
    pitchModule.addAndMakeVisible (pitchEnvTime);
    toneModule.addAndMakeVisible (toneHighPass);
    toneModule.addAndMakeVisible (toneLowPass);
    envelopeModule.addAndMakeVisible (*envelope);

    for (auto* k : { &attack, &hold, &decay })
        envelopeModule.addAndMakeVisible (k);

    // Zones
    zoneMap.setCompact (true);
    zoneMap.onAudition = [this] (int velocity)
    {
        if (piece >= 0)
            processor.audition (piece, velocity, articulation);
    };
    zonesModule.addAndMakeVisible (zoneMap);

    for (auto* c : std::initializer_list<juce::Component*> { &velocityModule, &pitchModule, &toneModule, &envelopeModule,
                                                             &mics, &comp, &transient, &saturation,
                                                             &zonesModule })
        addAndMakeVisible (c);

    processor.addChangeListener (this);
    processor.getMidiMapBroadcaster().addChangeListener (this);
    processor.getSelection().addListener (this);
    updateKitInfo();
    startTimerHz (30);
}

EditPage::~EditPage()
{
    processor.getSelection().removeListener (this);
    processor.getMidiMapBroadcaster().removeChangeListener (this);
    processor.removeChangeListener (this);
}

const KitPieceInfo* EditPage::currentPiece() const
{
    if (shownKit == nullptr || ! juce::isPositiveAndBelow (piece, (int) shownKit->pieces.size()))
        return nullptr;

    return &shownKit->pieces[(size_t) piece];
}

void EditPage::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &processor.getMidiMapBroadcaster())
        showArticulation (articulation);   // only the notes in the zone strip's title change
    else
        updateKitInfo();
}

void EditPage::selectedPieceChanged (int p)
{
    showPiece (p);
}

std::vector<int> EditPage::micChannels() const
{
    // The mics with channels of their own: the overheads and room (a close
    // mic only feeds the drums' channels).
    std::vector<int> result;

    if (shownKit != nullptr)
        for (size_t m = 0; m < shownKit->mics.size(); ++m)
            if (! shownKit->mics[m].direct && params::hasMicChannel (processor.getParameters(), *shownKit, (int) m))
                result.push_back ((int) m);

    return result;
}

void EditPage::stepPiece (int delta)
{
    // Through the drums, then the mic channels, and round.
    if (shownKit == nullptr || shownKit->pieces.empty())
        return;

    const auto drums = (int) shownKit->pieces.size();
    const auto channels = micChannels();
    const auto n = drums + (int) channels.size();
    auto at = piece >= 0 ? piece : 0;

    if (micChannel >= 0)
        if (const auto it = std::find (channels.begin(), channels.end(), micChannel); it != channels.end())
            at = drums + (int) (it - channels.begin());

    const auto to = (at + delta + n) % n;

    if (to >= drums)
        showMic (channels[(size_t) (to - drums)]);
    else if (to == processor.getSelection().getSelected())
        showPiece (to);
    else
        processor.getSelection().select (to);
}

void EditPage::updateKitInfo()
{
    const auto info = processor.getKitInfo();
    const std::array<bool, 2> emulating { params::isEmulating (processor.getParameters(), MicRole::overheads),
                                          params::isEmulating (processor.getParameters(), MicRole::room) };

    if (info == shownKit && emulating == shownEmulation)
        return;

    // The same kit with an emulated pair switched on or off: only the menu
    // changes (and the page leaves a mic channel that's gone).
    if (info == shownKit && info != nullptr)
    {
        shownEmulation = emulating;
        rebuildMenu();

        if (micChannel >= 0 && ! params::hasMicChannel (processor.getParameters(), *info, micChannel))
            showPiece (processor.getSelection().getSelected());
        else
            pieceBox.setSelectedId (micChannel >= 0 ? micMenuId + micChannel + 1 : piece + 1, juce::dontSendNotification);

        return;
    }

    shownEmulation = emulating;

    // The zone map points into the old KitInfo, so let go of that first.
    zoneMap.setArticulation (nullptr, {});
    shownKit = info;
    rebuildMenu();
    piece = -1;
    micChannel = -1;
    showPiece (processor.getSelection().getSelected());
}

void EditPage::rebuildMenu()
{
    pieceBox.clear (juce::dontSendNotification);

    if (shownKit == nullptr)
        return;

    for (size_t i = 0; i < shownKit->pieces.size(); ++i)
        pieceBox.addItem (shownKit->pieces[i].name, (int) i + 1);

    if (const auto channels = micChannels(); ! channels.empty())
    {
        pieceBox.addSectionHeading ("Mic channels");

        for (const auto m : channels)
        {
            const auto& mic = shownKit->mics[(size_t) m];
            pieceBox.addItem (mic.name + (mic.emulated ? " (emulated)" : ""), micMenuId + m + 1);
        }
    }
}

void EditPage::showPiece (int p)
{
    piece = p;
    micChannel = -1;
    const auto* info = currentPiece();

    for (auto* c : getChildren())
        c->setVisible (info != nullptr);

    picture->setMouseCursor (juce::MouseCursor::PointingHandCursor);

    if (info == nullptr)
    {
        zoneMap.setArticulation (nullptr, {});
        repaint();
        return;
    }

    pieceBox.setSelectedId (p + 1, juce::dontSendNotification);
    picture->setPiece (SlotGrid::pictureFor (*shownKit, p), theme::groupColour (lithium::info (info->role).group), info->name);
    model.setPiece (shownKit, p);

    auto& state = processor.getParameters();
    auto attachKnob = [&] (Knob& k, std::string_view name) { k.attach (state, params::pieceParamId (p, name)); };
    attachKnob (volume, "volume");
    attachKnob (dynamics, params::dynamics);
    attachKnob (tune, "tune");
    attachKnob (pitchEnv, params::pitchEnv);
    attachKnob (pitchEnvTime, params::pitchEnvTime);
    attachKnob (toneHighPass, params::toneHighPass);
    attachKnob (toneLowPass, params::toneLowPass);
    attachKnob (attack, params::attack);
    attachKnob (hold, params::hold);
    attachKnob (decay, "decay");
    mute.attach (state, params::pieceParamId (p, "mute"));
    solo.attach (state, params::pieceParamId (p, "solo"));
    choke.attach (state, params::pieceParamId (p, "choke_note"));
    mute.setTooltip ("Mute this drum");
    solo.setTooltip ("Solo this drum");

    const auto colour = theme::groupColour (lithium::info (info->role).group);

    for (auto* k : { &volume, &dynamics, &tune, &pitchEnv, &pitchEnvTime, &toneHighPass, &toneLowPass, &attack, &hold, &decay })
        k->setArcColour (colour);

    envelope->setPiece (p, colour);

    // The drum's channel: the same parameters as its strip in the Mixer.
    const params::ChannelId channel { true, p };
    mics.show (shownKit, channel);
    comp.attach (channel);
    transient.attach (channel);
    saturation.attach (channel);

    // A drum with nothing on the close mics has a silent channel; say so
    // (see paintOverChildren) rather than offer controls that do nothing.
    for (auto* c : std::initializer_list<juce::Component*> { &comp, &transient, &saturation })
        c->setEnabled (info->hasDirectSignal);

    articulationBox.clear (juce::dontSendNotification);

    for (size_t a = 0; a < info->articulations.size(); ++a)
    {
        articulationBox.addItem (info->articulations[a].name, (int) a + 1);
    }

    seenHits = processor.getFeedback().pieceHits[(size_t) p].load();
    lastHitText = {};
    showArticulation (0);
    resized();
    repaint();
}

void EditPage::showMic (int m)
{
    if (shownKit == nullptr || ! juce::isPositiveAndBelow (m, (int) shownKit->mics.size()) || shownKit->mics[(size_t) m].direct)
        return;

    const auto& mic = shownKit->mics[(size_t) m];
    piece = -1;
    micChannel = m;
    zoneMap.setArticulation (nullptr, {});
    lastHitText = {};

    // What a drum has and a mic channel doesn't (the articulations, zones,
    // the per-hit shaping, Strike and choke) is hidden; the rest is the
    // channel's.
    for (auto* c : getChildren())
        c->setVisible (false);

    for (auto* c : std::initializer_list<juce::Component*> { picture.get(), &previous, &next, &pieceBox, &mute, &solo,
                                                             &transient, &saturation, &mics, &comp })
        c->setVisible (true);

    pieceBox.setSelectedId (micMenuId + m + 1, juce::dontSendNotification);

    // The mics' render: the overheads hanging (just their lower part, the
    // mics and plugs, not the long cables), the room pair on their stands.
    auto image = kitart::load (mic.role == MicRole::room ? "mics-room.webp" : mic.role == MicRole::overheads ? "mics-overheads.webp" : "mics-close.webp");

    if (mic.role == MicRole::overheads && image.isValid())
    {
        image = image.getClippedImage (image.getBounds().withTrimmedTop (juce::roundToInt (image.getHeight() * 0.72f)));

        // The two mics hang at the render's edges, wide apart; in this small
        // picture they're drawn closer together (each mic's strip, with a
        // smaller gap between), so they come out bigger.
        const auto strip = juce::roundToInt (image.getWidth() * 0.16f), gapWidth = juce::roundToInt (image.getWidth() * 0.18f);
        juce::Image closer (juce::Image::ARGB, 2 * strip + gapWidth, image.getHeight(), true);

        {
            juce::Graphics g (closer);   // gone before `closer` is used
            g.drawImage (image, 0, 0, strip, image.getHeight(), 0, 0, strip, image.getHeight());
            g.drawImage (image, strip + gapWidth, 0, strip, image.getHeight(), image.getWidth() - strip, 0, strip, image.getHeight());
        }

        image = closer;
    }

    picture->setPiece (image, theme::colours::textMuted, mic.name + (mic.emulated ? " (emulated)" : ""));
    picture->setMouseCursor (juce::MouseCursor::NormalCursor);   // nothing to play

    const params::ChannelId channel { false, m };
    auto& state = processor.getParameters();
    mute.attach (state, channel.param ("mute"));
    solo.attach (state, channel.param ("solo"));
    mute.setTooltip ("Mute " + mic.name);
    solo.setTooltip ("Solo " + mic.name);
    mics.show (shownKit, channel);
    comp.attach (channel);
    transient.attach (channel);
    saturation.attach (channel);

    for (auto* c : std::initializer_list<juce::Component*> { &comp, &transient, &saturation })
        c->setEnabled (true);

    resized();
    repaint();
}

void EditPage::showArticulation (int a)
{
    const auto* info = currentPiece();

    if (info == nullptr || ! juce::isPositiveAndBelow (a, (int) info->articulations.size()))
        return;

    articulation = a;

    articulationBox.setSelectedId (a + 1, juce::dontSendNotification);

    const auto& art = info->articulations[(size_t) a];
    zoneMap.setArticulation (&art, theme::groupColour (lithium::info (info->role).group));

    // The notes that play it with the current MIDI map.
    juce::StringArray notes;
    const auto table = processor.getNoteTable();

    for (int n = 0; n < 128; ++n)
        if (table.targets[(size_t) n].articulation == info->firstArticulation + a)
            notes.add (noteLabel (n));

    // (juce::String reads plain char strings as ASCII, so the UTF-8 dot is marked as such.)
    zonesModule.setTitle ("Zones" + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + art.name + "   MIDI " + (notes.isEmpty() ? juce::String ("not mapped") : notes.joinIntoString (", ")));
    repaint();
}

void EditPage::flashPlayedDrums()
{
    // Every drum that played since the last tick lights up on the Mics card,
    // whatever played it (MIDI, a groove, Strike), as on the Kit tab's stage.
    // The card ignores drums it doesn't show.
    const auto& hits = processor.getFeedback().pieceHits;
    const auto count = shownKit != nullptr ? std::min (shownKit->pieces.size(), hits.size()) : size_t (0);

    if (shownKit.get() != micsSeenKit || micsSeenHits.size() != count)
    {
        // A new kit: start counting from now, so nothing lights up for hits from before.
        micsSeenKit = shownKit.get();
        micsSeenHits.resize (count);

        for (size_t i = 0; i < count; ++i)
            micsSeenHits[i] = hits[i].load (std::memory_order_relaxed);

        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        if (const auto h = hits[i].load (std::memory_order_relaxed); h != micsSeenHits[i])
        {
            micsSeenHits[i] = h;

            if (mics.isShowing())
                mics.flash ((int) i);
        }
    }
}

void EditPage::timerCallback()
{
    // An emulated pair switched on or off changes the drum menu.
    if (shownKit != nullptr)
        updateKitInfo();

    flashPlayedDrums();

    if (micChannel >= 0)
    {
        comp.pushReduction (MixerFeedback::take (processor.getMixerFeedback().micReductionDb[(size_t) micChannel]));
        return;
    }

    const auto* info = currentPiece();

    if (info == nullptr)
        return;

    comp.pushReduction (MixerFeedback::take (processor.getMixerFeedback().pieceReductionDb[(size_t) piece]));

    // The engine bumps the piece's counter on every hit, after storing what played.
    const auto& feedback = processor.getFeedback();
    const auto hits = feedback.pieceHits[(size_t) piece].load (std::memory_order_acquire);

    if (hits == seenHits)
        return;

    seenHits = hits;
    picture->flash();
    const auto hit = LastHit::unpack (feedback.lastHit[(size_t) piece].load (std::memory_order_relaxed));

    if (! hit.has_value())
        return;

    const auto local = hit->articulation - info->firstArticulation;

    if (! juce::isPositiveAndBelow (local, (int) info->articulations.size()))
        return;   // from a kit that has since been replaced

    if (local != articulation)
        showArticulation (local);

    zoneMap.showHit (hit->zone, hit->roundRobin, hit->velocity);
    lastHitText = "v" + juce::String (hit->velocity) + "  zone " + juce::String (hit->zone + 1)
                + "  rr " + juce::String (hit->roundRobin + 1);
    zonesModule.repaint();
    repaint (zonesModule.getBounds());
}

//==============================================================================
void EditPage::paint (juce::Graphics& g)
{
    using namespace theme;

    g.fillAll (colours::background);
    g.setColour (colours::panel);
    const auto bandHeight = topHeight + (micChannel >= 0 ? micTopExtra : 0);
    g.fillRect (getLocalBounds().removeFromTop (bandHeight));
    g.setColour (colours::border);
    g.fillRect (0, bandHeight, getWidth(), 1);

    if (currentPiece() != nullptr || micChannel >= 0)
    {
        // The piece card (the picture is a child: PiecePicture).
        g.setColour (colours::raised);
        g.fillRoundedRectangle (pieceCardArea.toFloat(), 8.0f);
        g.setColour (colours::border);
        g.drawRoundedRectangle (pieceCardArea.toFloat().reduced (0.5f), 8.0f, 1.0f);
    }
    else
    {
        g.setColour (colours::textMuted);
        g.setFont (body (15.0f));
        g.drawText (shownKit == nullptr ? "Load a kit to edit its drums." : "Choose a drum.", getLocalBounds(), juce::Justification::centred, false);
    }
}

void EditPage::paintOverChildren (juce::Graphics& g)
{
    using namespace theme;

    if (micChannel >= 0 && shownKit != nullptr)
    {
        // The dot in front of the menu, and what these mics are, where a drum
        // has its model and articulations.
        g.setColour (colours::textMuted);
        g.fillEllipse (pieceBox.getBounds().toFloat().withWidth (26.0f).withSizeKeepingCentre (8.0f, 8.0f).translated (4.0f, 0.0f));
        const auto role = shownKit->mics[(size_t) micChannel].role;
        g.setFont (body (13.5f));
        g.drawText (role == MicRole::room ? "Room mics: the kit from across the room" : "Overhead mics: the whole kit from above",
                    juce::Rectangle<int> (pieceBox.getX(), pieceBox.getBottom() + 6, pieceBox.getRight() - pieceBox.getX() + 30, 28),
                    juce::Justification::centredLeft, true);
        return;
    }

    const auto* info = currentPiece();

    if (info == nullptr)
        return;

    // The group dot in front of the piece menu.
    g.setColour (groupColour (lithium::info (info->role).group));
    g.fillEllipse (pieceBox.getBounds().toFloat().withWidth (26.0f).withSizeKeepingCentre (8.0f, 8.0f).translated (4.0f, 0.0f));

    // The last hit, in the zone strip's header.
    if (lastHitText.isNotEmpty())
    {
        g.setColour (colours::text);
        g.setFont (mono (11.0f));
        g.drawText ("LAST HIT  " + lastHitText, zonesModule.getBounds().reduced (12, 0).withTrimmedRight (zonesModule.getHelpWidth()).withHeight (32).translated (0, 1),
                    juce::Justification::centredRight, false);
    }

    if (! info->hasDirectSignal)
    {
        const auto row = comp.getBounds().toFloat();
        g.setColour (colours::background.withAlpha (0.72f));
        g.fillRoundedRectangle (row, 8.0f);
        g.setColour (colours::text);
        g.setFont (body (15.0f, Weight::semibold));
        g.drawText ("No close mic on this drum", row.withTrimmedBottom (row.getHeight() * 0.5f - 4.0f).withTrimmedTop (row.getHeight() * 0.5f - 22.0f),
                    juce::Justification::centred, false);
        g.setColour (colours::textMuted);
        g.setFont (body (13.5f));
        g.drawText ("This kit only recorded it in the overheads and room, so its channel has nothing to process.",
                    row.withTrimmedTop (row.getHeight() * 0.5f + 2.0f).withHeight (40.0f).reduced (40.0f, 0.0f),
                    juce::Justification::centred, true);
    }
}

void EditPage::resized()
{
    // Piece card: [picture]  < [piece] >
    //                        [model]
    //                        [articulation v] CHOKE [note v]
    //                        [Strike] [M] [S]
    const auto topExtra = micChannel >= 0 ? micTopExtra : 0;
    const auto thisCardHeight = cardHeight + topExtra;
    pieceCardArea = juce::Rectangle<int> (margin, 9, cardPadding * 2 + (thisCardHeight - 2 * cardPadding) + 12 + cardControlsWidth, thisCardHeight);
    auto card = pieceCardArea.reduced (cardPadding);
    picture->setBounds (card.removeFromLeft (card.getHeight()));
    card.removeFromLeft (12);

    constexpr int controlHeight = 28, controlGap = 6;
    auto nextRow = [&]
    {
        auto r = card.removeFromTop (controlHeight);
        card.removeFromTop (controlGap);
        return r;
    };

    auto selector = nextRow();
    previous.setBounds (selector.removeFromLeft (30));
    next.setBounds (selector.removeFromRight (30));
    pieceBox.setBounds (selector.reduced (4, 0));

    // The menu's text starts after the group dot drawn over it.
    if (auto* text = dynamic_cast<juce::Label*> (pieceBox.getChildren().getFirst()))
        text->setBorderSize ({ 1, 24, 1, 5 });

    model.setBounds (nextRow());

    // The articulation menu (always a menu, however many there are), then
    // the choke note's menu at the end of the row.
    auto articulations = nextRow();
    choke.setBounds (articulations.removeFromRight (78));
    articulations.removeFromRight (4);
    chokeLabel.setBounds (articulations.removeFromRight (44));
    articulations.removeFromRight (8);
    articulationBox.setVisible (currentPiece() != nullptr);
    articulationBox.setBounds (articulations);

    auto actions = card.removeFromTop (controlHeight);
    solo.setBounds (actions.removeFromRight (40));
    actions.removeFromRight (4);
    mute.setBounds (actions.removeFromRight (40));
    actions.removeFromRight (8);
    strike.setBounds (actions);

    if (micChannel >= 0)
    {
        // A mic channel: its transient shaper and saturation where the zones
        // are; below, the whole-kit mics card beside its compressor, both
        // the height of the two rows.
        auto top = juce::Rectangle<int> (pieceCardArea.getRight() + gap, pieceCardArea.getY(), getWidth() - pieceCardArea.getRight() - gap - margin, thisCardHeight);
        transient.setBounds (top.removeFromLeft ((top.getWidth() - gap) / 2));
        top.removeFromLeft (gap);
        saturation.setBounds (top);

        auto rows = juce::Rectangle<int> (margin, topHeight + topExtra + gap, getWidth() - 2 * margin, rowOneHeight + gap + rowTwoHeight - topExtra);
        comp.setBounds (rows.removeFromRight (500));
        rows.removeFromRight (gap);
        mics.setBounds (rows);
        return;
    }

    // Row one: shaping each hit.
    auto row = juce::Rectangle<int> (margin, topHeight + gap, getWidth() - 2 * margin, rowOneHeight);

    // (How loud the drum is in each mic is set in the Mixer's Mics card.)
    auto column = row.removeFromRight (196);   // room for the title, On and "?" in each header; the Envelope card gives it up
    transient.setBounds (column.removeFromTop ((column.getHeight() - gap) / 2));
    column.removeFromTop (gap);
    saturation.setBounds (column);
    row.removeFromRight (gap);

    for (auto [module, width] : { std::pair { &velocityModule, 150 }, { &pitchModule, 222 }, { &toneModule, 150 }, { &envelopeModule, 0 } })
    {
        module->setBounds (width > 0 ? row.removeFromLeft (width) : row);
        row.removeFromLeft (gap);
    }

    {
        auto area = contentOf (velocityModule);
        volume.setBounds (area.removeFromTop (area.getHeight() / 2).reduced (6, 2));
        dynamics.setBounds (area.reduced (6, 2));
    }

    {
        auto area = contentOf (pitchModule);
        const auto half = area.getHeight() / 2;
        tune.setBounds (area.removeFromTop (half).withSizeKeepingCentre (100, half - 4));
        pitchEnv.setBounds (area.removeFromLeft (area.getWidth() / 2).reduced (6, 2));
        pitchEnvTime.setBounds (area.reduced (6, 2));
    }

    {
        auto area = contentOf (toneModule);
        toneHighPass.setBounds (area.removeFromTop (area.getHeight() / 2).reduced (6, 2));
        toneLowPass.setBounds (area.reduced (6, 2));
    }

    {
        auto area = contentOf (envelopeModule);
        auto knobs = area.removeFromBottom (66);
        area.removeFromBottom (6);
        envelope->setBounds (area);
        const auto w = knobs.getWidth() / 3;

        for (auto* k : { &attack, &hold, &decay })
            k->setBounds (knobs.removeFromLeft (w).reduced (8, 0));
    }

    // Row two: the drum's channel.
    row = juce::Rectangle<int> (margin, topHeight + gap + rowOneHeight + gap, getWidth() - 2 * margin, rowTwoHeight);
    comp.setBounds (row.removeFromRight (500));
    row.removeFromRight (gap);
    mics.setBounds (row);

    // The zones sit beside the piece card.
    zonesModule.setBounds (pieceCardArea.getRight() + gap, pieceCardArea.getY(), getWidth() - pieceCardArea.getRight() - gap - margin, cardHeight);
    zoneMap.setBounds (zonesModule.getLocalBounds().reduced (8, 0).withTrimmedTop (30).withTrimmedBottom (6));

    // The crossfade switch in the zones header, left of the last hit.
    crossfade.setBounds (zonesModule.getBounds().getRight() - 12 - zonesModule.getHelpWidth() - 280 - 106, zonesModule.getY() + 4, 106, 24);
}

} // namespace lithium
