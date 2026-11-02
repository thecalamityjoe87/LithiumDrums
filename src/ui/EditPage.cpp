#include "EditPage.h"

#include "Theme.h"

namespace lithium
{

namespace
{
    constexpr int margin = 16, gap = 12;
    constexpr int barHeight = 40, rowOneHeight = 236, rowTwoHeight = 300;

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
/** One of the piece's mic sends: a name, a slider and its value. */
class EditPage::SendRow final : public juce::Component
{
public:
    SendRow (juce::String n, juce::String tip) : name (std::move (n))
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setTooltip (tip);
        slider.onValueChange = [this] { repaint(); };
        addAndMakeVisible (slider);
    }

    void attach (juce::AudioProcessorValueTreeState& state, const juce::String& id)
    {
        attachment.reset();
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, id, slider);
        slider.setDoubleClickReturnValue (true, 100.0);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        auto area = getLocalBounds().toFloat();
        g.setColour (colours::text);
        g.setFont (body (13.5f, Weight::medium));
        g.drawText (name, area.removeFromLeft (78.0f), juce::Justification::centredLeft, false);
        g.setFont (mono (11.5f));
        g.drawText (slider.getTextFromValue (slider.getValue()), area.removeFromRight (42.0f), juce::Justification::centredRight, false);
    }

    void resized() override   { slider.setBounds (getLocalBounds().withTrimmedLeft (78).withTrimmedRight (48)); }

private:
    const juce::String name;
    ControlSlider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

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
EditPage::EditPage (LithiumDrumsProcessor& p)
    : processor (p),
      model (p),
      envelope (std::make_unique<EnvelopeGraph> (p.getParameters())),
      eq (p.getParameters()),
      comp (p.getParameters()),
      transient (p.getParameters(), TwoKnobModule::Kind::transient),
      saturation (p.getParameters(), TwoKnobModule::Kind::saturation),
      routing (p.getParameters(), true)
{
    routing.onOpenReturn = [this] (params::FxReturn r) { if (onOpenReturn) onOpenReturn (r); };

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
        if (const auto id = pieceBox.getSelectedId(); id > 0)
            processor.getSelection().select (id - 1);
    };

    play.setTooltip ("Play this articulation at velocity " + juce::String (LithiumDrumsProcessor::auditionVelocity));
    play.onClick = [this]
    {
        if (piece >= 0)
            processor.audition (piece, LithiumDrumsProcessor::auditionVelocity, articulation);
    };

    crossfade.attach (processor.getParameters(), params::velocityCrossfade);
    mute.setColour (juce::TextButton::buttonOnColourId, theme::colours::textMuted);

    for (auto* c : std::initializer_list<juce::Component*> { &previous, &next, &pieceBox, &model, &play, &crossfade, &mute, &solo })
        addAndMakeVisible (c);

    addChildComponent (articulationBox);

    // Shaping
    tune.setBipolar (true);
    pitchEnv.setBipolar (true);
    dynamics.setBipolar (true);
    sends[0] = std::make_unique<SendRow> ("Close", "How loud the drum is in its close mics");
    sends[1] = std::make_unique<SendRow> ("Overheads", "How loud the drum is in the overhead mics");
    sends[2] = std::make_unique<SendRow> ("Room", "How loud the drum is in the room mics");
    choke.setTooltip ("A note that chokes (grabs) this drum, e.g. from an e-kit's cymbal pad");
    chokeLabel.setFont (theme::label (11.0f));
    chokeLabel.setColour (juce::Label::textColourId, theme::colours::textMuted);
    chokeLabel.setBorderSize ({});

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

    for (auto& s : sends)
        micsModule.addAndMakeVisible (*s);

    micsModule.addAndMakeVisible (chokeLabel);
    micsModule.addAndMakeVisible (choke);

    // Zones
    zoneMap.setCompact (true);
    zoneMap.onAudition = [this] (int velocity)
    {
        if (piece >= 0)
            processor.audition (piece, velocity, articulation);
    };
    zonesModule.addAndMakeVisible (zoneMap);

    for (auto* c : std::initializer_list<juce::Component*> { &velocityModule, &pitchModule, &toneModule, &envelopeModule,
                                                             &micsModule, &eq, &comp, &transient, &saturation, &routing,
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

void EditPage::stepPiece (int delta)
{
    if (shownKit == nullptr || shownKit->pieces.empty())
        return;

    const auto n = (int) shownKit->pieces.size();
    processor.getSelection().select (((piece < 0 ? 0 : piece) + delta + n) % n);
}

void EditPage::updateKitInfo()
{
    const auto info = processor.getKitInfo();

    if (info == shownKit)
        return;

    // The zone map points into the old KitInfo, so let go of that first.
    zoneMap.setArticulation (nullptr, {});
    shownKit = info;
    pieceBox.clear (juce::dontSendNotification);

    if (info != nullptr)
        for (size_t i = 0; i < info->pieces.size(); ++i)
            pieceBox.addItem (info->pieces[i].name, (int) i + 1);

    piece = -1;
    showPiece (processor.getSelection().getSelected());
}

void EditPage::showPiece (int p)
{
    piece = p;
    tabs.clear();
    const auto* info = currentPiece();

    for (auto* c : getChildren())
        c->setVisible (info != nullptr);

    if (info == nullptr)
    {
        zoneMap.setArticulation (nullptr, {});
        repaint();
        return;
    }

    pieceBox.setSelectedId (p + 1, juce::dontSendNotification);
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
    sends[0]->attach (state, params::pieceParamId (p, "send_close"));
    sends[1]->attach (state, params::pieceParamId (p, "send_overheads"));
    sends[2]->attach (state, params::pieceParamId (p, "send_room"));
    choke.attach (state, params::pieceParamId (p, "choke_note"));
    mute.attach (state, params::pieceParamId (p, "mute"));
    solo.attach (state, params::pieceParamId (p, "solo"));

    const auto colour = theme::groupColour (lithium::info (info->role).group);

    for (auto* k : { &volume, &dynamics, &tune, &pitchEnv, &pitchEnvTime, &toneHighPass, &toneLowPass, &attack, &hold, &decay })
        k->setArcColour (colour);

    envelope->setPiece (p, colour);

    // The drum's channel: the same parameters as its strip in the Mixer.
    const params::ChannelId channel { true, p };
    eq.setSampleRate (processor.getSampleRate());
    eq.attach (channel);
    comp.attach (channel);
    transient.attach (channel);
    saturation.attach (channel);
    routing.attach (channel);

    // A drum with nothing on the close mics has a silent channel; say so
    // (see paintOverChildren) rather than offer controls that do nothing.
    for (auto* c : std::initializer_list<juce::Component*> { &eq, &comp, &transient, &saturation, &routing })
        c->setEnabled (info->hasDirectSignal);

    articulationBox.clear (juce::dontSendNotification);

    for (size_t a = 0; a < info->articulations.size(); ++a)
    {
        articulationBox.addItem (info->articulations[a].name, (int) a + 1);
        auto tab = std::make_unique<juce::TextButton> (info->articulations[a].name);
        tab->setRadioGroupId (1);
        tab->setTooltip ("Show " + info->articulations[a].name);
        tab->onClick = [this, a] { showArticulation ((int) a); };
        addAndMakeVisible (*tab);
        tabs.push_back (std::move (tab));
    }

    seenHits = processor.getFeedback().pieceHits[(size_t) p].load();
    lastHitText = {};
    showArticulation (0);
    resized();
    repaint();
}

void EditPage::showArticulation (int a)
{
    const auto* info = currentPiece();

    if (info == nullptr || ! juce::isPositiveAndBelow (a, (int) info->articulations.size()))
        return;

    articulation = a;

    for (size_t i = 0; i < tabs.size(); ++i)
        tabs[i]->setToggleState ((int) i == a, juce::dontSendNotification);

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

void EditPage::timerCallback()
{
    const auto* info = currentPiece();

    if (info == nullptr)
        return;

    // The engine bumps the piece's counter on every hit, after storing what played.
    const auto& feedback = processor.getFeedback();
    const auto hits = feedback.pieceHits[(size_t) piece].load (std::memory_order_acquire);

    if (hits == seenHits)
        return;

    seenHits = hits;
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
    g.fillRect (getLocalBounds().removeFromTop (barHeight + 18));
    g.setColour (colours::border);
    g.fillRect (0, barHeight + 18, getWidth(), 1);

    if (currentPiece() == nullptr)
    {
        g.setColour (colours::textMuted);
        g.setFont (body (15.0f));
        g.drawText (shownKit == nullptr ? "Load a kit to edit its drums." : "Choose a drum.", getLocalBounds(), juce::Justification::centred, false);
    }
}

void EditPage::paintOverChildren (juce::Graphics& g)
{
    using namespace theme;

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
        g.drawText ("LAST HIT  " + lastHitText, zonesModule.getBounds().reduced (12, 0).withHeight (32).translated (0, 1),
                    juce::Justification::centredRight, false);
    }

    if (! info->hasDirectSignal)
    {
        const auto row = eq.getBounds().getUnion (routing.getBounds()).toFloat();
        g.setColour (colours::background.withAlpha (0.72f));
        g.fillRoundedRectangle (row, 8.0f);
        g.setColour (colours::text);
        g.setFont (body (15.0f, Weight::semibold));
        g.drawText ("No close mic on this drum", row.withTrimmedBottom (row.getHeight() * 0.5f - 4.0f).withTrimmedTop (row.getHeight() * 0.5f - 22.0f),
                    juce::Justification::centred, false);
        g.setColour (colours::textMuted);
        g.setFont (body (13.5f));
        g.drawText ("This kit only recorded it in the overheads and room, so its channel has nothing to process. "
                    "Shape it with the controls above, and the overhead and room channels in the Mixer.",
                    row.withTrimmedTop (row.getHeight() * 0.5f + 2.0f).withHeight (20.0f).reduced (40.0f, 0.0f),
                    juce::Justification::centred, true);
    }
}

void EditPage::resized()
{
    // Piece bar: < > [piece] [articulations...]            [Crossfade] [Play] [M] [S]
    auto bar = juce::Rectangle<int> (margin, 9, getWidth() - 2 * margin, barHeight);
    previous.setBounds (bar.removeFromLeft (30).reduced (0, 5));
    bar.removeFromLeft (4);
    next.setBounds (bar.removeFromLeft (30).reduced (0, 5));
    bar.removeFromLeft (8);
    pieceBox.setBounds (bar.removeFromLeft (190).reduced (0, 5));
    bar.removeFromLeft (8);
    model.setBounds (bar.removeFromLeft (220).reduced (0, 5));

    // The menu's text starts after the group dot drawn over it.
    if (auto* text = dynamic_cast<juce::Label*> (pieceBox.getChildren().getFirst()))
        text->setBorderSize ({ 1, 24, 1, 5 });

    bar.removeFromLeft (20);

    auto right = bar.removeFromRight (310);
    solo.setBounds (right.removeFromRight (34).reduced (0, 6));
    right.removeFromRight (4);
    mute.setBounds (right.removeFromRight (34).reduced (0, 6));
    right.removeFromRight (12);
    play.setBounds (right.removeFromRight (80).reduced (0, 6));
    right.removeFromRight (8);
    crossfade.setBounds (right.removeFromRight (106).reduced (0, 6));

    // One tab per articulation, or a menu if there are too many to fit.
    std::vector<int> widths;
    int total = 0;

    for (auto& tab : tabs)
    {
        widths.push_back ((int) theme::getTrackedTextWidth (tab->getButtonText().toUpperCase(), theme::label (14.0f), 0.06f) + 26);
        total += widths.back() + 6;
    }

    const auto useMenu = total > bar.getWidth();
    articulationBox.setVisible (useMenu && currentPiece() != nullptr);

    if (useMenu)
        articulationBox.setBounds (bar.removeFromLeft (220).reduced (0, 5));

    for (size_t i = 0; i < tabs.size(); ++i)
    {
        tabs[i]->setVisible (! useMenu);
        tabs[i]->setBounds (bar.removeFromLeft (widths[i]).reduced (0, 6));
        bar.removeFromLeft (6);
    }

    // Row one: shaping each hit.
    auto row = juce::Rectangle<int> (margin, barHeight + 18 + gap, getWidth() - 2 * margin, rowOneHeight);

    for (auto [module, width] : { std::pair { &velocityModule, 150 }, { &pitchModule, 222 }, { &toneModule, 150 },
                                  { &envelopeModule, 420 }, { &micsModule, 0 } })
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

    {
        auto area = contentOf (micsModule);
        area.removeFromTop (4);

        for (auto& s : sends)
        {
            s->setBounds (area.removeFromTop (28));
            area.removeFromTop (8);
        }

        auto bottom = area.removeFromBottom (26);
        choke.setBounds (bottom);
        area.removeFromBottom (4);
        chokeLabel.setBounds (area.removeFromBottom (16));
    }

    // Row two: the drum's channel.
    row = juce::Rectangle<int> (margin, barHeight + 18 + gap + rowOneHeight + gap, getWidth() - 2 * margin, rowTwoHeight);
    eq.setBounds (row.removeFromLeft (470));
    row.removeFromLeft (gap);
    comp.setBounds (row.removeFromLeft (300));
    row.removeFromLeft (gap);
    auto column = row.removeFromLeft (156);
    transient.setBounds (column.removeFromTop ((column.getHeight() - gap) / 2));
    column.removeFromTop (gap);
    saturation.setBounds (column);
    row.removeFromLeft (gap);
    routing.setBounds (row);

    // The zone strip fills the rest.
    const auto zonesTop = row.getBottom() + gap;
    zonesModule.setBounds (margin, zonesTop, getWidth() - 2 * margin, getHeight() - zonesTop - 12);
    zoneMap.setBounds (zonesModule.getLocalBounds().reduced (8, 0).withTrimmedTop (30).withTrimmedBottom (6));
}

} // namespace lithium
