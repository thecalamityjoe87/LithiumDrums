#include "MixerPage.h"

#include "Theme.h"

namespace lithium
{

namespace
{
    constexpr int inspectorHeight = 300;
    constexpr int bankTop = inspectorHeight + 12;

    juce::String formatFaderDb (double db)
    {
        return db <= params::minVolumeDb + 0.05 ? juce::String ("-inf") : (db > 0.05 ? "+" : "") + juce::String (db, 1);
    }

    juce::String roleName (PieceRole r)
    {
        return { info (r).displayName.data(), info (r).displayName.size() };
    }
}

//==============================================================================
MixerStrip::MixerStrip (LithiumDrumsProcessor& p, Kind k, int s, const KitInfo* kit)
    : kind (k), slot (s), meter (k == Kind::master ? 2 : 1)
{
    using namespace theme::colours;
    auto& state = p.getParameters();

    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    fader.getProperties().set ("unityValue", 0.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.onValueChange = [this] { repaint (getReadoutArea()); };
    addAndMakeVisible (fader);
    addAndMakeVisible (meter);

    auto attachFader = [&] (const juce::String& id)
    {
        faderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, id, fader);
    };

    auto addChannelControls = [&] (params::ChannelId channel)
    {
        attachFader (channel.fader());
        pan.attach (state, channel.param ("pan"));
        pan.setBipolar (true);
        eq.attach (state, channel.param (params::eqOn));
        comp.attach (state, channel.param (params::compOn));
        output.attach (state, channel.param (params::output));

        for (auto* c : std::initializer_list<juce::Component*> { &pan, &eq, &comp, &mute, &solo, &output })
            addAndMakeVisible (c);
    };

    switch (kind)
    {
        case Kind::piece:
        {
            const auto& piece = kit->pieces[(size_t) slot];
            name = piece.name;
            dot = theme::groupColour (info (piece.role).group);
            silent = ! piece.hasDirectSignal;
            tag = silent ? "No close mic" : roleName (piece.role);
            addChannelControls ({ true, slot });
            mute.attach (state, params::pieceParamId (slot, "mute"));
            solo.attach (state, params::pieceParamId (slot, "solo"));
            fader.setColour (juce::Slider::trackColourId, dot);
            break;
        }

        case Kind::mic:
        {
            const auto& mic = kit->mics[(size_t) slot];
            name = mic.name;
            tag = juce::String (mic.role == MicRole::room ? "Room" : "Overhead") + (mic.numChannels > 1 ? juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 St")) : juce::String());
            addChannelControls ({ false, slot });
            mute.attach (state, params::micParamId (slot, "mute"));
            solo.attach (state, params::micParamId (slot, "solo"));
            fader.setColour (juce::Slider::trackColourId, accent);
            break;
        }

        case Kind::trim:
        {
            const auto& mic = kit->mics[(size_t) slot];
            name = mic.name;
            tag = "Close trim";
            attachFader (params::micParamId (slot, "volume"));
            pan.attach (state, params::micParamId (slot, "pan"));
            pan.setBipolar (true);
            mute.attach (state, params::micParamId (slot, "mute"));
            solo.attach (state, params::micParamId (slot, "solo"));
            fader.setColour (juce::Slider::trackColourId, textMuted);
            meter.setVisible (false);   // a trim has no output of its own to meter

            for (auto* c : std::initializer_list<juce::Component*> { &pan, &mute, &solo })
                addAndMakeVisible (c);

            break;
        }

        case Kind::reverb:
            name = "Reverb";
            tag = "Return";
            attachFader (params::reverbReturn);
            mute.attach (state, params::reverbMute);
            output.attach (state, params::reverbOutput);
            fader.setColour (juce::Slider::trackColourId, groupToms);
            addAndMakeVisible (mute);
            addAndMakeVisible (output);
            break;

        case Kind::master:
            name = "Master";
            tag = "Main out";
            attachFader (params::masterVolume);
            fader.setColour (juce::Slider::trackColourId, textMuted);
            break;
    }

    fader.setTooltip (kind == Kind::trim ? name + ": how loud this close mic is in the drum channels"
                                         : name + " level (double-click for 0 dB)");
    mute.setColour (juce::TextButton::buttonOnColourId, textMuted);
    output.setTooltip ("Output: the main mix, or an aux out for mixing in the host");

    for (auto* b : { &eq, &comp })
        b->setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.85f));

    if (silent)
        setTooltip ("In this kit, nothing of this drum was recorded on a close mic: it's all in the overheads and room, "
                    "so its channel stays silent.");
}

void MixerStrip::setSelected (bool s)
{
    if (s != selected)
    {
        selected = s;
        repaint();
    }
}

void MixerStrip::pushLevels (float left, float right)
{
    meter.push (left, right);
}

void MixerStrip::mouseDown (const juce::MouseEvent&)
{
    if (onSelect)
        onSelect();
}

juce::Rectangle<int> MixerStrip::getReadoutArea() const
{
    // Just above the buttons (see resized).
    return { 4, getHeight() - 6 - 52 - 18, getWidth() - 8, 16 };
}

void MixerStrip::paint (juce::Graphics& g)
{
    using namespace theme;

    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (selected ? colours::raisedHigh : (kind == Kind::trim ? colours::panel : colours::raised));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (selected ? colours::accent : colours::border);
    g.drawRoundedRectangle (bounds, 6.0f, selected ? 1.5f : 1.0f);

    auto top = getLocalBounds().reduced (6, 6);

    if (! dot.isTransparent())
    {
        g.setColour (dot);
        g.fillEllipse (top.removeFromTop (8).withSizeKeepingCentre (6, 6).toFloat());
    }
    else
    {
        top.removeFromTop (8);
    }

    g.setColour (silent ? colours::textMuted : colours::text);
    g.setFont (body (12.5f, Weight::semibold));
    g.drawFittedText (name, top.removeFromTop (28), juce::Justification::centred, 2, 0.85f);

    g.setColour (silent ? colours::warning.withAlpha (0.8f) : colours::textMuted);
    drawTrackedText (g, tag.toUpperCase(), label (10.0f), 0.04f, top.removeFromTop (14).toFloat(), juce::Justification::centred);

    g.setColour (colours::text);
    g.setFont (mono (11.0f));
    g.drawText (formatFaderDb (fader.getValue()), getReadoutArea(), juce::Justification::centred, false);
}

void MixerStrip::resized()
{
    auto area = getLocalBounds().reduced (6);
    area.removeFromTop (54);   // dot, name and tag

    // The same rows are kept free on every strip, so all the faders line up.
    auto toggles = area.removeFromTop (18);
    eq.setBounds (toggles.removeFromLeft ((toggles.getWidth() - 4) / 2));
    toggles.removeFromLeft (4);
    comp.setBounds (toggles);
    area.removeFromTop (6);
    pan.setBounds (area.removeFromTop (32).withSizeKeepingCentre (32, 32));
    area.removeFromTop (8);

    output.setBounds (area.removeFromBottom (22));
    area.removeFromBottom (6);
    auto buttons = area.removeFromBottom (24);

    if (kind == Kind::reverb)
    {
        mute.setBounds (buttons);
    }
    else
    {
        mute.setBounds (buttons.removeFromLeft ((buttons.getWidth() - 4) / 2));
        buttons.removeFromLeft (4);
        solo.setBounds (buttons);
    }

    area.removeFromBottom (20);   // the dB readout
    const auto meterWidth = kind == Kind::master ? 10 : 6;
    auto column = area.withSizeKeepingCentre (std::min (area.getWidth(), 28 + 4 + meterWidth), area.getHeight());
    meter.setBounds (column.removeFromRight (meterWidth).reduced (0, 8));
    column.removeFromRight (4);
    fader.setBounds (column);
}

//==============================================================================
/** The reverb's envelope: silence for the pre-delay, then a fall of 60 dB
    over the decay time. */
class MixerInspector::DecayGraph final : public juce::Component, private juce::Timer
{
public:
    explicit DecayGraph (juce::AudioProcessorValueTreeState& s)
        : decay (s.getParameter (params::reverbDecay)), preDelay (s.getParameter (params::reverbPreDelay))
    {
        startTimerHz (15);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        const auto bounds = getLocalBounds().toFloat();
        g.setColour (colours::background);
        g.fillRoundedRectangle (bounds, cornerRadius);

        const auto area = bounds.reduced (10.0f, 8.0f).withTrimmedBottom (12.0f);
        const auto seconds = std::max (1.0f, std::ceil ((shownDecay + shownPreDelay) * 1.25f));
        auto xFor = [&] (float s) { return juce::jmap (s, 0.0f, seconds, area.getX(), area.getRight()); };
        auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (-60.0f, 0.0f, db), 0.0f, -60.0f, area.getY(), area.getBottom()); };

        g.setFont (mono (10.0f));

        for (int s = 0; s <= (int) seconds; ++s)
        {
            const auto x = xFor ((float) s);
            g.setColour (colours::border);
            g.fillRect (juce::Rectangle<float> (x, area.getY(), 1.0f, area.getHeight()));
            g.setColour (colours::textMuted);
            g.drawText (juce::String (s) + " s", juce::Rectangle<float> (x - 20.0f, area.getBottom() + 1.0f, 40.0f, 12.0f),
                        juce::Justification::centred);
        }

        juce::Path env;
        env.startNewSubPath (xFor (0.0f), area.getBottom());
        env.lineTo (xFor (shownPreDelay), area.getBottom());
        env.lineTo (xFor (shownPreDelay), yFor (0.0f));
        env.lineTo (xFor (shownPreDelay + shownDecay), yFor (-60.0f));
        env.closeSubPath();

        g.setColour (colours::groupToms.withAlpha (0.18f));
        g.fillPath (env);
        g.setColour (colours::groupToms);
        g.drawLine ({ xFor (shownPreDelay), yFor (0.0f), xFor (shownPreDelay + shownDecay), yFor (-60.0f) }, 2.0f);

        g.setColour (colours::text);
        g.setFont (mono (11.0f));
        g.drawText ("RT60 " + juce::String (shownDecay, 2) + " s", area.reduced (6.0f), juce::Justification::topRight, false);
    }

private:
    void timerCallback() override
    {
        const auto d = decay->convertFrom0to1 (decay->getValue());
        const auto p = preDelay->convertFrom0to1 (preDelay->getValue()) / 1000.0f;

        if (! juce::exactlyEqual (d, shownDecay) || ! juce::exactlyEqual (p, shownPreDelay))
        {
            shownDecay = d;
            shownPreDelay = p;
            repaint();
        }
    }

    juce::RangedAudioParameter *decay, *preDelay;
    float shownDecay = 0.0f, shownPreDelay = 0.0f;
};

//==============================================================================
MixerInspector::MixerInspector (LithiumDrumsProcessor& p)
    : processor (p),
      eq (p.getParameters()),
      comp (p.getParameters()),
      transient (p.getParameters(), TwoKnobModule::Kind::transient),
      saturation (p.getParameters(), TwoKnobModule::Kind::saturation),
      routing (p.getParameters(), false),
      decay (std::make_unique<DecayGraph> (p.getParameters()))
{
    auto& state = p.getParameters();

    for (auto* k : { &size, &decayTime, &preDelay, &damping, &lowCut, &width, &returnLevel })
        k->setArcColour (theme::colours::groupToms);

    size.attach (state, params::reverbSize);
    decayTime.attach (state, params::reverbDecay);
    preDelay.attach (state, params::reverbPreDelay);
    damping.attach (state, params::reverbDamping);
    lowCut.attach (state, params::reverbLowCut);
    width.attach (state, params::reverbWidth);
    returnLevel.attach (state, params::reverbReturn);
    returnMute.attach (state, params::reverbMute);
    returnOutput.attach (state, params::reverbOutput);
    returnMute.setColour (juce::TextButton::buttonOnColourId, theme::colours::textMuted);
    returnOutput.setTooltip ("The main mix, or an aux out for mixing in the host");

    for (auto* c : std::initializer_list<juce::Component*> { &eq, &comp, &transient, &saturation, &routing, &room, &returnModule })
        addChildComponent (c);

    for (auto* c : std::initializer_list<juce::Component*> { decay.get(), &size, &decayTime, &preDelay, &damping, &lowCut, &width })
        room.addAndMakeVisible (c);

    outputLabel.setFont (theme::label (11.0f));
    outputLabel.setColour (juce::Label::textColourId, theme::colours::textMuted);
    outputLabel.setBorderSize ({});

    for (auto* c : std::initializer_list<juce::Component*> { &returnLevel, &returnMute, &outputLabel, &returnOutput })
        returnModule.addAndMakeVisible (c);
}

MixerInspector::~MixerInspector() = default;

void MixerInspector::showChannel (params::ChannelId channel, juce::String name, juce::String description)
{
    eq.setSampleRate (processor.getSampleRate());
    eq.attach (channel);
    comp.attach (channel);
    transient.attach (channel);
    saturation.attach (channel);
    routing.attach (channel);
    title = std::move (name);
    subtitle = std::move (description);
    setMode (Mode::channel);
}

void MixerInspector::showReverb()
{
    title = "Reverb";
    subtitle = "One shared room. Each channel feeds it with its reverb send, after its fader.";
    setMode (Mode::reverb);
}

void MixerInspector::showNothing()
{
    title = {};
    subtitle = {};
    eq.attach ({});
    setMode (Mode::none);
}

void MixerInspector::setMode (Mode m)
{
    mode = m;

    for (auto* c : std::initializer_list<juce::Component*> { &eq, &comp, &transient, &saturation, &routing })
        c->setVisible (mode == Mode::channel);

    room.setVisible (mode == Mode::reverb);
    returnModule.setVisible (mode == Mode::reverb);
    resized();
    repaint();
}

void MixerInspector::paint (juce::Graphics& g)
{
    using namespace theme;

    g.setColour (colours::panel);
    g.fillAll();

    if (mode == Mode::none)
    {
        g.setColour (colours::textMuted);
        g.setFont (body (15.0f));
        g.drawText ("Load a kit to mix it.", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    auto header = juce::Rectangle<float> (24.0f, 10.0f, (float) getWidth() - 48.0f - 230.0f, 34.0f);
    g.setColour (colours::text);
    const auto titleFont = label (24.0f, Weight::bold);
    drawTrackedText (g, title.toUpperCase(), titleFont, 0.08f, header, juce::Justification::centredLeft);
    header.removeFromLeft (getTrackedTextWidth (title.toUpperCase(), titleFont, 0.08f) + 14.0f);
    g.setColour (colours::textMuted);
    g.setFont (body (14.0f));
    g.drawText (subtitle, header.translated (0.0f, 2.0f), juce::Justification::centredLeft, true);

}

void MixerInspector::resized()
{
    // EQ | compressor | transient over saturation | send and output
    auto row = getLocalBounds().withTrimmedTop (52).withTrimmedBottom (12).reduced (16, 0);
    eq.setBounds (row.removeFromLeft (595));
    row.removeFromLeft (12);
    comp.setBounds (row.removeFromLeft (315));
    row.removeFromLeft (12);
    auto column = row.removeFromLeft (150);
    transient.setBounds (column.removeFromTop ((column.getHeight() - 8) / 2));
    column.removeFromTop (8);
    saturation.setBounds (column);
    row.removeFromLeft (12);
    routing.setBounds (row);

    // The reverb: its room, and its return.
    auto reverbRow = getLocalBounds().withTrimmedTop (52).withTrimmedBottom (12).reduced (16, 0);
    room.setBounds (reverbRow.removeFromLeft (652));
    reverbRow.removeFromLeft (12);
    returnModule.setBounds (reverbRow.removeFromLeft (340));

    {
        auto area = room.getLocalBounds().reduced (12, 0).withTrimmedTop (32).withTrimmedBottom (10);
        auto knobs = area.removeFromBottom (62);
        area.removeFromBottom (6);
        decay->setBounds (area);
        const auto w = knobs.getWidth() / 6;

        for (auto* k : { &size, &decayTime, &preDelay, &damping, &lowCut, &width })
            k->setBounds (knobs.removeFromLeft (w));
    }

    {
        auto area = returnModule.getLocalBounds().reduced (14, 0).withTrimmedTop (32);
        returnLevel.setBounds (area.removeFromLeft (110).removeFromTop (110));
        area.removeFromLeft (16);
        returnMute.setBounds (area.removeFromTop (26).removeFromLeft (80));
        area.removeFromTop (16);
        outputLabel.setBounds (area.removeFromTop (18).removeFromLeft (140));
        returnOutput.setBounds (area.removeFromTop (26).removeFromLeft (140));
    }
}

//==============================================================================
MixerPage::MixerPage (LithiumDrumsProcessor& p)
    : processor (p), inspector (p), reverbStrip (p, MixerStrip::Kind::reverb), masterStrip (p, MixerStrip::Kind::master)
{
    addAndMakeVisible (inspector);

    for (auto* b : { &channelsView, &trimsView })
    {
        b->setRadioGroupId (2);
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }

    channelsView.setTooltip ("The mixer's channels: one per drum, then the overhead and room mics");
    trimsView.setTooltip ("Trim the close mics: how loud and where each one sits in the drum channels it feeds");
    channelsView.setToggleState (true, juce::dontSendNotification);
    channelsView.onClick = [this] { showingTrims = false; rebuildStrips(); };
    trimsView.onClick = [this] { showingTrims = true; rebuildStrips(); };

    stripView.setViewedComponent (&stripRow, false);   // not owned: a member
    stripView.setScrollBarsShown (false, true);
    stripView.setScrollBarThickness (8);
    stripView.getHorizontalScrollBar().setColour (juce::ScrollBar::thumbColourId, theme::colours::borderStrong);
    addAndMakeVisible (stripView);

    reverbStrip.onSelect = [this] { selectReverb(); };
    addAndMakeVisible (reverbStrip);
    addAndMakeVisible (masterStrip);

    processor.addChangeListener (this);
    updateKitInfo();
    startTimerHz (30);
}

MixerPage::~MixerPage()
{
    processor.removeChangeListener (this);
}

void MixerPage::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateKitInfo();
}

void MixerPage::updateKitInfo()
{
    const auto info = processor.getKitInfo();

    if (info == shownKit)
        return;

    shownKit = info;
    rebuildStrips();
}

void MixerPage::rebuildStrips()
{
    strips.clear();
    dividers.clear();

    if (shownKit != nullptr)
    {
        auto add = [this] (MixerStrip::Kind kind, int slot)
        {
            auto strip = std::make_unique<MixerStrip> (processor, kind, slot, shownKit.get());

            if (kind == MixerStrip::Kind::piece || kind == MixerStrip::Kind::mic)
                strip->onSelect = [this, kind, slot] { select ({ kind == MixerStrip::Kind::piece, slot }); };

            stripRow.addAndMakeVisible (*strip);
            strips.push_back (std::move (strip));
        };

        if (showingTrims)
        {
            for (size_t m = 0; m < shownKit->mics.size(); ++m)
                if (shownKit->mics[m].direct)
                    add (MixerStrip::Kind::trim, (int) m);
        }
        else
        {
            for (size_t p = 0; p < shownKit->pieces.size(); ++p)
                add (MixerStrip::Kind::piece, (int) p);

            dividers.push_back ((int) strips.size());

            for (size_t m = 0; m < shownKit->mics.size(); ++m)
                if (! shownKit->mics[m].direct)
                    add (MixerStrip::Kind::mic, (int) m);
        }
    }

    updateSelection();
    resized();
    repaint();
}

void MixerPage::select (params::ChannelId channel)
{
    processor.selectedMixerChannel = channel;
    processor.reverbSelected = false;
    updateSelection();
}

void MixerPage::selectReverb()
{
    processor.reverbSelected = true;
    updateSelection();
}

void MixerPage::updateSelection()
{
    if (shownKit == nullptr)
    {
        inspector.showNothing();
        return;
    }

    // Keep the selection if it still exists in this kit; otherwise the first drum.
    auto channel = processor.selectedMixerChannel;
    const auto valid = channel.isPiece ? juce::isPositiveAndBelow (channel.slot, (int) shownKit->pieces.size())
                                       : juce::isPositiveAndBelow (channel.slot, (int) shownKit->mics.size())
                                             && ! shownKit->mics[(size_t) channel.slot].direct;

    if (! valid)
        channel = processor.selectedMixerChannel = { true, 0 };

    const auto reverb = processor.reverbSelected;

    for (auto& s : strips)
        s->setSelected (! reverb && s->kind != MixerStrip::Kind::trim
                        && s->slot == channel.slot && (s->kind == MixerStrip::Kind::piece) == channel.isPiece);

    reverbStrip.setSelected (reverb);

    if (reverb)
    {
        inspector.showReverb();
    }
    else if (channel.isPiece && ! shownKit->pieces.empty())
    {
        const auto& piece = shownKit->pieces[(size_t) channel.slot];
        const auto allDirect = std::all_of (shownKit->mics.begin(), shownKit->mics.end(), [] (const auto& m) { return m.direct; });
        inspector.showChannel (channel, piece.name,
                               ! piece.hasDirectSignal ? "This kit has no close mic on this drum, so its channel is silent."
                               : allDirect             ? "Everything the kit recorded of this drum."
                                                       : "The drum's close mics. Its overheads and room are on their own channels.");
    }
    else if (! channel.isPiece)
    {
        const auto& mic = shownKit->mics[(size_t) channel.slot];
        inspector.showChannel (channel, mic.name, juce::String (mic.numChannels > 1 ? "Stereo " : "Mono ")
                                                      + (mic.role == MicRole::room ? "room mic" : "overhead mic")
                                                      + ", shared by every drum");
    }
    else
    {
        inspector.showNothing();
    }
}

void MixerPage::timerCallback()
{
    // Read and clear the peaks even while hidden, so the meters don't show a
    // stale burst when the page comes back.
    auto& fb = processor.getMixerFeedback();
    std::array<float, maxPieces> piecePeaks {}, pieceReduction {};
    std::array<float, maxMics> micPeaks {}, micReduction {};

    for (size_t i = 0; i < (size_t) maxPieces; ++i)
    {
        piecePeaks[i] = MixerFeedback::take (fb.piecePeak[i]);
        pieceReduction[i] = MixerFeedback::take (fb.pieceReductionDb[i]);
    }

    for (size_t i = 0; i < (size_t) maxMics; ++i)
    {
        micPeaks[i] = MixerFeedback::take (fb.micPeak[i]);
        micReduction[i] = MixerFeedback::take (fb.micReductionDb[i]);
    }

    for (auto& s : strips)
        if (s->kind == MixerStrip::Kind::piece || s->kind == MixerStrip::Kind::mic)
            s->pushLevels ((s->kind == MixerStrip::Kind::piece ? piecePeaks : micPeaks)[(size_t) s->slot], 0.0f);

    const auto selected = processor.selectedMixerChannel;

    if (selected.isValid() && ! processor.reverbSelected)
        inspector.pushReduction ((selected.isPiece ? pieceReduction : micReduction)[(size_t) selected.slot]);

    reverbStrip.pushLevels (MixerFeedback::take (fb.reverbPeak), 0.0f);
    masterStrip.pushLevels (MixerFeedback::take (fb.masterPeak[0]), MixerFeedback::take (fb.masterPeak[1]));
}

void MixerPage::paint (juce::Graphics& g)
{
    using namespace theme;

    g.fillAll (colours::background);
    g.setColour (colours::border);
    g.fillRect (0, inspectorHeight, getWidth(), 1);

    // Dividers between the groups of strips, and before the reverb and master.
    g.setColour (colours::borderStrong);
    g.fillRect (reverbStrip.getX() - 9, bankTop + 8, 1, getHeight() - bankTop - 30);

    if (showingTrims && strips.empty())
    {
        g.setColour (colours::textMuted);
        g.setFont (body (14.0f));
        g.drawText ("This kit has no close mics.", stripView.getBounds(), juce::Justification::centred, false);
    }
}

void MixerPage::resized()
{
    inspector.setBounds (getLocalBounds().removeFromTop (inspectorHeight));

    auto views = juce::Rectangle<int> (getWidth() - 16 - 200, 14, 200, 26);
    channelsView.setBounds (views.removeFromLeft (100));
    trimsView.setBounds (views);

    auto bank = getLocalBounds().withTrimmedTop (bankTop).reduced (16, 0).withTrimmedBottom (14);
    constexpr int busWidth = 76, gap = 6, groupGap = 16, minStripWidth = 60, maxStripWidth = 104;

    masterStrip.setBounds (bank.removeFromRight (busWidth));
    bank.removeFromRight (gap);
    reverbStrip.setBounds (bank.removeFromRight (busWidth));
    bank.removeFromRight (18);
    stripView.setBounds (bank);

    const auto n = (int) strips.size();

    if (n == 0)
    {
        stripRow.setSize (0, 0);
        return;
    }

    const auto numGroupGaps = (int) std::count_if (dividers.begin(), dividers.end(), [n] (int d) { return d > 0 && d < n; });
    const auto fitted = (bank.getWidth() - gap * (n - 1) - numGroupGaps * groupGap) / n;
    const auto scrolls = fitted < minStripWidth;
    const auto width = juce::jlimit (minStripWidth, maxStripWidth, fitted);
    const auto height = bank.getHeight() - (scrolls ? 12 : 0);
    int x = 0;

    for (int i = 0; i < n; ++i)
    {
        if (i > 0 && std::find (dividers.begin(), dividers.end(), i) != dividers.end())
            x += groupGap;

        strips[(size_t) i]->setBounds (x, 0, width, height);
        x += width + gap;
    }

    stripRow.setSize (x - gap, height);
}

} // namespace lithium
