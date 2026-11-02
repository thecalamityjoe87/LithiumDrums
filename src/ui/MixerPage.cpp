#include "MixerPage.h"

#include "Theme.h"

namespace lithium
{

namespace
{

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
namespace
{
    // The strip's stack (see MixerStrip), top to bottom, in strip pixels.
    // Everything but the fader is kept as short as it can be, so the bank
    // takes little of the page; the fader keeps its full travel.
    constexpr int stripPadding = theme::space::sm - 2;
    constexpr int stripHeaderHeight = 16;                          // the colour dot and name, on one line
    constexpr int insertRow = 14, insertGap = theme::space::xxs;
    constexpr int knobRow = theme::size::knobCompact + 10;    // the dial and its caption
    constexpr int faderHeight = theme::size::faderTravel + theme::size::faderCap;
    constexpr int readoutHeight = 12;
    constexpr int scaleWidth = 18, faderWidth = 26;

    constexpr int insertsTop = stripPadding + stripHeaderHeight + theme::space::xs + 2;
    constexpr int knobsTop = insertsTop + 2 * insertRow + insertGap + theme::space::xs + 2;
    constexpr int faderTop = knobsTop + knobRow + theme::space::xxs;
    constexpr int readoutTop = faderTop + faderHeight + theme::space::xxs;
    constexpr int buttonsTop = readoutTop + readoutHeight + theme::space::xs;
    constexpr int outputTop = buttonsTop + theme::size::buttonCompact + theme::space::xs;
    constexpr int stripHeight = outputTop + theme::size::buttonCompact + stripPadding;
}

int MixerStrip::getContentHeight()
{
    return stripHeight;
}

MixerStrip::MixerStrip (LithiumDrumsProcessor& p, Kind k, int s, const KitInfo* kit)
    : kind (k), slot (s), meter (k == Kind::master ? 2 : 1)
{
    using namespace theme::colours;
    auto& state = p.getParameters();

    fader.setSliderStyle (juce::Slider::LinearVertical);
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
        send.attach (state, channel.param (params::reverbSend));
        pan.attach (state, channel.param ("pan"));
        pan.setBipolar (true);
        eq.attach (state, channel.param (params::eqOn));
        comp.attach (state, channel.param (params::compOn));
        transient.attach (state, channel.param (params::transientOn));
        saturation.attach (state, channel.param (params::satOn));
        output.attach (state, channel.param (params::output));

        for (auto* c : std::initializer_list<juce::Component*> { &send, &pan, &eq, &comp, &transient, &saturation, &mute, &solo, &output })
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
            dot = accent;
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
            dot = groupToms;
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

    // The meter reads on the fader's own scale, so its marks line up.
    meter.setTopDb ((float) fader.getMaximum());

    fader.setTooltip (kind == Kind::trim ? name + ": how loud this close mic is in the drum channels"
                                         : name + " level (double-click for 0 dB, Shift-drag for fine steps)");
    send.setTooltip (name + ": reverb send");
    pan.setTooltip (name + ": pan");
    mute.setTooltip ("Mute " + name);
    solo.setTooltip ("Solo " + name);
    mute.setColour (juce::TextButton::buttonOnColourId, textMuted);
    output.setTooltip ("Output: the main mix, or an aux out for mixing in the host");

    for (auto* b : { &eq, &comp, &transient, &saturation })
        b->setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.85f));

    setTooltip (silent ? "In this kit, nothing of this drum was recorded on a close mic: it's all in the overheads and room, "
                         "so its channel stays silent."
                       : name + " (" + tag + ")");
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
    return { stripPadding, readoutTop, getWidth() - 2 * stripPadding, readoutHeight };
}

void MixerStrip::paint (juce::Graphics& g)
{
    using namespace theme;

    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (selected ? colours::raisedHigh : (kind == Kind::trim ? colours::panel : colours::raised));
    g.fillRoundedRectangle (bounds, cornerRadius);
    g.setColour (selected ? colours::accent : colours::border);
    g.drawRoundedRectangle (bounds, cornerRadius, selected ? 1.5f : 1.0f);

    // The colour dot and name on one line, set like the other section labels
    // (condensed capitals). Long names end in an ellipsis; the strip's
    // tooltip has them whole, with what kind of channel it is.
    auto header = juce::Rectangle<int> (stripPadding - 2, stripPadding, getWidth() - 2 * stripPadding + 4, stripHeaderHeight).toFloat();
    const auto nameFont = label (14.0f, Weight::semibold);
    const auto nameText = name.toUpperCase();
    const auto dotSpace = dot.isTransparent() ? 0.0f : 9.0f;
    auto line = header.withSizeKeepingCentre (std::min (header.getWidth(), juce::GlyphArrangement::getStringWidth (nameFont, nameText) + dotSpace + 1.0f),
                                              header.getHeight());

    if (! dot.isTransparent())
    {
        g.setColour (dot);
        g.fillEllipse (line.removeFromLeft (dotSpace).withSizeKeepingCentre (5.0f, 5.0f).translated (-2.0f, 0.0f));
    }

    g.setColour (silent ? colours::textMuted : colours::text);
    g.setFont (nameFont);
    g.drawText (nameText, line, juce::Justification::centredLeft, true);

    // Captions under the two knobs.
    if (send.isVisible() || pan.isVisible())
    {
        g.setColour (colours::textMuted);

        for (const auto* k : { &send, &pan })
            if (k->isVisible())
                drawTrackedText (g, k == &send ? "SEND" : "PAN", label (10.0f), 0.06f,
                                 k->getBounds().toFloat().withY ((float) k->getBottom()).withHeight (10.0f).expanded (6.0f, 0.0f),
                                 juce::Justification::centred);
    }

    // The dB scale, left of the fader, at the fader's own positions.
    g.setFont (mono (9.0f));

    // Every mark gets a tick; the ±6 dB marks have no number, which at
    // this fader length would crowd the 0 dB one.
    constexpr float labelHeight = 12.0f;

    for (const auto db : { 12.0, 6.0, 0.0, -6.0, -12.0, -24.0, -40.0, -60.0 })
    {
        if (db > fader.getMaximum())
            continue;

        const auto y = (float) fader.getY() + (float) fader.getPositionOfValue (db);
        g.setColour (juce::exactlyEqual (db, 0.0) ? colours::textMuted : colours::textMuted.withAlpha (0.75f));
        g.fillRect (juce::Rectangle<float> ((float) fader.getX() - 3.0f, y - 0.5f, 3.0f, 1.0f));

        if (juce::exactlyEqual (std::abs (db), 6.0))
            continue;

        g.drawText (db > 0.0 ? "+" + juce::String ((int) db) : juce::String ((int) db),
                    juce::Rectangle<float> ((float) fader.getX() - scaleWidth - 3.0f, y - labelHeight * 0.5f, (float) scaleWidth, labelHeight),
                    juce::Justification::centredRight, false);
    }

    g.setColour (colours::text);
    g.setFont (mono (type::caption));
    g.drawText (formatFaderDb (fader.getValue()) + " dB", getReadoutArea(), juce::Justification::centred, false);
}

void MixerStrip::resized()
{
    using namespace theme;
    const auto inner = getLocalBounds().reduced (stripPadding, 0);

    // Inserts: EQ and Cmp over Trn and Sat.
    {
        auto rows = inner.withY (insertsTop).withHeight (2 * insertRow + insertGap);
        auto top = rows.removeFromTop (insertRow);
        rows.removeFromTop (insertGap);
        auto bottom = rows;
        const auto half = (inner.getWidth() - insertGap) / 2;
        eq.setBounds (top.removeFromLeft (half));
        comp.setBounds (top.withTrimmedLeft (insertGap));
        transient.setBounds (bottom.removeFromLeft (half));
        saturation.setBounds (bottom.withTrimmedLeft (insertGap));
    }

    // Send and pan, side by side (a trim's pan sits where the pan is on the others).
    {
        auto row = inner.withY (knobsTop).withHeight (size::knobCompact);
        const auto half = inner.getWidth() / 2;
        send.setBounds (row.removeFromLeft (half).withSizeKeepingCentre (size::knobCompact, size::knobCompact));
        pan.setBounds (row.withSizeKeepingCentre (size::knobCompact, size::knobCompact));
    }

    // The fader and its meter, centred as a group with the scale to the fader's left.
    {
        const auto meterWidth = kind == Kind::master ? 2 * size::meterWidth + 2 : size::meterWidth;
        const auto groupWidth = scaleWidth + 3 + faderWidth + space::xs + meterWidth;
        const auto x = (getWidth() - groupWidth) / 2 + scaleWidth + 3;
        fader.setBounds (x, faderTop, faderWidth, faderHeight);
        meter.setBounds (fader.getRight() + space::xs, faderTop + size::faderCap / 2, meterWidth, size::faderTravel);
    }

    auto buttons = inner.withY (buttonsTop).withHeight (size::buttonCompact);

    if (kind == Kind::reverb)
    {
        mute.setBounds (buttons);
    }
    else
    {
        mute.setBounds (buttons.removeFromLeft ((buttons.getWidth() - insertGap) / 2));
        solo.setBounds (buttons.withTrimmedLeft (insertGap));
    }

    output.setBounds (inner.withY (outputTop).withHeight (size::buttonCompact));
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

    enableKeyboardNavigation (stripRow);
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
    g.fillRect (0, inspector.getBottom(), getWidth(), 1);

    // A divider before the reverb and master.
    g.setColour (colours::borderStrong);
    g.fillRect (reverbStrip.getX() - space::sm - 1, reverbStrip.getY() + space::sm, 1, reverbStrip.getHeight() - 2 * space::sm);

    if (showingTrims && strips.empty())
    {
        g.setColour (colours::textMuted);
        g.setFont (body (type::body));
        g.drawText ("This kit has no close mics: every mic is on a channel of its own, under Channels.",
                    stripView.getBounds(), juce::Justification::centred, false);
    }
}

void MixerPage::resized()
{
    using namespace theme;

    // The bank is as tall as a strip's stack (plus room for its scroll bar,
    // kept whether or not it scrolls, so nothing moves); the inspector takes
    // what's above it.
    constexpr int scrollBarSpace = 10, gap = 6, groupGap = space::lg;
    const auto stripHeight = MixerStrip::getContentHeight();
    const auto bankTop = getHeight() - space::sm - scrollBarSpace - stripHeight;
    inspector.setBounds (getLocalBounds().withBottom (bankTop - space::sm - 1));

    auto views = juce::Rectangle<int> (getWidth() - space::page - 200, space::md, 200, size::button);
    channelsView.setBounds (views.removeFromLeft (100));
    trimsView.setBounds (views);

    auto bank = juce::Rectangle<int> (space::page, bankTop, getWidth() - 2 * space::page, stripHeight + scrollBarSpace);

    // The reverb and master stay pinned on the right; the rest scrolls.
    masterStrip.setBounds (bank.removeFromRight (size::mixerStrip).withHeight (stripHeight));
    bank.removeFromRight (gap);
    reverbStrip.setBounds (bank.removeFromRight (size::mixerStrip).withHeight (stripHeight));
    bank.removeFromRight (2 * space::sm + 1);
    stripView.setBounds (bank);

    // Every strip the same width; a group gap between the drums and the room mics.
    int x = 0;

    for (size_t i = 0; i < strips.size(); ++i)
    {
        if (i > 0 && std::find (dividers.begin(), dividers.end(), (int) i) != dividers.end())
            x += groupGap;

        strips[i]->setBounds (x, 0, size::mixerStrip, stripHeight);
        x += size::mixerStrip + gap;
    }

    stripRow.setSize (std::max (0, x - gap), stripHeight);
}

} // namespace lithium
