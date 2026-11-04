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
    constexpr int sendsTop = insertsTop + 2 * insertRow + insertGap + theme::space::xs + 2;
    constexpr int knobsTop = sendsTop + knobRow + theme::space::xxs;   // Pre and pan
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
        const std::array<std::string_view, params::numFxReturns> sendIds { params::roomSend, params::reverbSend, params::delaySend };

        for (size_t i = 0; i < sends.size(); ++i)
        {
            sends[i].attach (state, channel.param (sendIds[i]));
            sends[i].setArcColour (theme::fxColour ((int) i));
            addAndMakeVisible (sends[i]);
        }

        pre.attach (state, channel.param (params::sendPre));
        pan.attach (state, channel.param ("pan"));
        pan.setBipolar (true);
        eq.attach (state, channel.param (params::eqOn));
        comp.attach (state, channel.param (params::compOn));
        transient.attach (state, channel.param (params::transientOn));
        saturation.attach (state, channel.param (params::satOn));
        output.attach (state, channel.param (params::output));

        for (auto* c : std::initializer_list<juce::Component*> { &pre, &pan, &eq, &comp, &transient, &saturation, &mute, &solo, &output })
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

        case Kind::fxReturn:
        {
            using namespace params;
            struct Ids { const char *level, *mute, *pan, *output; };
            constexpr std::array<Ids, numFxReturns> ids {{ { roomReturn, roomMute, roomPan, roomOutput },
                                                          { reverbReturn, reverbMute, reverbPan, reverbOutput },
                                                          { delayReturn, delayMute, delayPan, delayOutput } }};
            const auto& r = ids[(size_t) slot];
            name = theme::fxName (slot);
            tag = "Return";
            dot = theme::fxColour (slot);
            attachFader (r.level);
            mute.attach (state, r.mute);
            pan.attach (state, r.pan);
            pan.setBipolar (true);
            output.attach (state, r.output);
            fader.setColour (juce::Slider::trackColourId, dot);

            for (auto* c : std::initializer_list<juce::Component*> { &pan, &mute, &output })
                addAndMakeVisible (c);

            break;
        }

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
    for (size_t i = 0; i < sends.size(); ++i)
        sends[i].setTooltip (name + ": " + theme::fxName ((int) i) + " send. Click its name below to open the "
                             + theme::fxName ((int) i) + " on the FX page.");

    pre.setTooltip (name + ": sends before the fader, so they don't follow it (off: after the fader)");
    pre.setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.85f));
    pan.setTooltip (name + ": pan");
    mute.setTooltip ("Mute " + name);
    solo.setTooltip ("Solo " + name);
    mute.setColour (juce::TextButton::buttonOnColourId, textMuted);
    output.setTooltip ("Output: the main mix, or an aux out for mixing in the host");

    for (auto* b : { &eq, &comp, &transient, &saturation })
        b->setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.85f));

    setTooltip (silent              ? "In this kit, nothing of this drum was recorded on a close mic: it's all in the overheads and room, "
                                      "so its channel stays silent."
                : kind == Kind::fxReturn ? name + " return: every channel's " + name + " send comes back here. Click to open it on the FX page."
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

juce::Rectangle<int> MixerStrip::getSendCaption (int index) const
{
    // The knob's share of the row, which is a little wider than the knob.
    const auto cell = (getWidth() - 2 * stripPadding) / params::numFxReturns;
    return { stripPadding + index * cell, sends[(size_t) index].getBottom(), cell, 10 };
}

void MixerStrip::mouseDown (const juce::MouseEvent& e)
{
    // A send's name opens its effect on the FX page.
    for (int i = 0; i < params::numFxReturns; ++i)
    {
        if (sends[(size_t) i].isVisible() && getSendCaption (i).contains (e.getPosition()))
        {
            if (onOpenReturn)
                onOpenReturn ((params::FxReturn) i);

            return;
        }
    }

    if (onSelect)
        onSelect();
}

void MixerStrip::mouseMove (const juce::MouseEvent& e)
{
    auto hovered = -1;

    for (int i = 0; i < params::numFxReturns; ++i)
        if (sends[(size_t) i].isVisible() && getSendCaption (i).contains (e.getPosition()))
            hovered = i;

    if (hovered != hoveredCaption)
    {
        hoveredCaption = hovered;
        setMouseCursor (hovered >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void MixerStrip::mouseExit (const juce::MouseEvent&)
{
    if (hoveredCaption >= 0)
    {
        hoveredCaption = -1;
        repaint();
    }
}

juce::Rectangle<int> MixerStrip::getReadoutArea() const
{
    return { stripPadding, readoutTop, getWidth() - 2 * stripPadding, readoutHeight };
}

void MixerStrip::paint (juce::Graphics& g)
{
    using namespace theme;

    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (selected ? colours::raisedHigh : (kind == Kind::trim ? colours::background : colours::panel));
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

    // Captions under the knobs. The sends' are links to their effects,
    // underlined while the mouse is over them.
    for (int i = 0; i < params::numFxReturns; ++i)
    {
        if (! sends[(size_t) i].isVisible())
            continue;

        const auto caption = getSendCaption (i).toFloat();
        const auto text = theme::fxName (i).toUpperCase();
        const auto font = label (9.0f, Weight::medium);
        g.setColour (hoveredCaption == i ? fxColour (i) : colours::textMuted);
        drawTrackedText (g, text, font, 0.0f, caption, juce::Justification::centred);

        if (hoveredCaption == i)
        {
            const auto w = getTrackedTextWidth (text, font, 0.0f);
            g.fillRect (juce::Rectangle<float> (caption.getCentreX() - w * 0.5f, caption.getBottom() - 0.5f, w, 1.0f));
        }
    }

    if (pan.isVisible())
    {
        g.setColour (colours::textMuted);
        drawTrackedText (g, "PAN", label (10.0f), 0.06f,
                         pan.getBounds().toFloat().withY ((float) pan.getBottom()).withHeight (10.0f).expanded (6.0f, 0.0f),
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

    // The three sends in a row, then Pre and pan (a trim's or a return's pan
    // sits where the pan is on the others).
    {
        auto row = inner.withY (sendsTop).withHeight (size::knobCompact);
        const auto w = row.getWidth() / params::numFxReturns;

        for (auto& k : sends)
            k.setBounds (row.removeFromLeft (w).withSizeKeepingCentre (size::knobCompact, size::knobCompact));

        auto lower = inner.withY (knobsTop).withHeight (size::knobCompact);
        const auto half = inner.getWidth() / 2;
        pre.setBounds (lower.removeFromLeft (half).withSizeKeepingCentre (half - insertGap, insertRow + 2));
        pan.setBounds (lower.withSizeKeepingCentre (size::knobCompact, size::knobCompact));
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

    if (kind == Kind::fxReturn)
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
MixerInspector::MixerInspector (LithiumDrumsProcessor& p)
    : processor (p),
      eq (p.getParameters()),
      comp (p.getParameters()),
      transient (p.getParameters(), TwoKnobModule::Kind::transient),
      saturation (p.getParameters(), TwoKnobModule::Kind::saturation),
      routing (p.getParameters(), false)
{
    routing.onOpenReturn = [this] (params::FxReturn r) { if (onOpenReturn) onOpenReturn (r); };

    // The EQ here is the whole kit's, whichever channel is selected.
    eq.setSampleRate (processor.getSampleRate());
    eq.attach (params::ChannelId::master());
    eq.setTitle ("Master EQ");

    for (auto* c : std::initializer_list<juce::Component*> { &eq, &comp, &transient, &saturation, &routing })
        addChildComponent (c);
}

MixerInspector::~MixerInspector() = default;

void MixerInspector::showChannel (params::ChannelId channel)
{
    eq.setSampleRate (processor.getSampleRate());
    comp.attach (channel);
    transient.attach (channel);
    saturation.attach (channel);
    routing.attach (channel);
    setMode (Mode::channel);
}

void MixerInspector::showNothing()
{
    setMode (Mode::none);
}

void MixerInspector::setMode (Mode m)
{
    mode = m;

    for (auto* c : std::initializer_list<juce::Component*> { &eq, &comp, &transient, &saturation, &routing })
        c->setVisible (mode == Mode::channel);

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
}

void MixerInspector::resized()
{
    // Master EQ | compressor | transient over saturation | send and output
    auto row = getLocalBounds().withTrimmedTop (12).withTrimmedBottom (12).reduced (16, 0);
    eq.setBounds (row.removeFromLeft (595));
    row.removeFromLeft (12);
    comp.setBounds (row.removeFromLeft (290));
    row.removeFromLeft (12);
    auto column = row.removeFromLeft (150);
    transient.setBounds (column.removeFromTop ((column.getHeight() - 8) / 2));
    column.removeFromTop (8);
    saturation.setBounds (column);
    row.removeFromLeft (12);
    routing.setBounds (row);

}

//==============================================================================
MixerPage::MixerPage (LithiumDrumsProcessor& p)
    : processor (p), inspector (p), masterStrip (p, MixerStrip::Kind::master)
{
    addAndMakeVisible (inspector);
    inspector.onOpenReturn = [this] (params::FxReturn r) { openReturn (r); };

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

    for (int r = 0; r < params::numFxReturns; ++r)
    {
        auto& strip = returnStrips[(size_t) r];
        strip = std::make_unique<MixerStrip> (p, MixerStrip::Kind::fxReturn, r);
        strip->onSelect = [this, r] { openReturn ((params::FxReturn) r); };
        addAndMakeVisible (*strip);
    }

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

            strip->onOpenReturn = [this] (params::FxReturn r) { openReturn (r); };

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

    for (auto& s : strips)
        s->setSelected (s->kind != MixerStrip::Kind::trim && s->slot == channel.slot && (s->kind == MixerStrip::Kind::piece) == channel.isPiece);

    if (channel.isPiece && shownKit->pieces.empty())
        inspector.showNothing();
    else
        inspector.showChannel (channel);
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

    if (selected.isValid())
        inspector.pushReduction ((selected.isPiece ? pieceReduction : micReduction)[(size_t) selected.slot]);

    for (size_t r = 0; r < returnStrips.size(); ++r)
        returnStrips[r]->pushLevels (MixerFeedback::take (fb.returnPeak[r]), 0.0f);
    masterStrip.pushLevels (MixerFeedback::take (fb.masterPeak[0]), MixerFeedback::take (fb.masterPeak[1]));
}

void MixerPage::paint (juce::Graphics& g)
{
    using namespace theme;

    g.fillAll (colours::background);
    // The bank's card: the view switch and every strip sit in it, like the
    // inspector's modules on their panel.
    g.setColour (colours::panel);
    g.fillRect (0, inspector.getBottom(), getWidth(), getHeight() - inspector.getBottom());
    g.setColour (colours::raised);
    g.fillRoundedRectangle (bankCard.toFloat(), 8.0f);
    g.setColour (colours::border);
    g.drawRoundedRectangle (bankCard.toFloat().reduced (0.5f), 8.0f, 1.0f);

    g.setColour (colours::textMuted);
    drawTrackedText (g, showingTrims ? "CLOSE MIC TRIMS" : "CHANNEL STRIPS", label (12.0f), 0.12f, bankHeader.toFloat().translated (0.0f, 1.0f), juce::Justification::centredLeft);

    // A divider before the returns and master.
    const auto& first = *returnStrips.front();
    g.setColour (colours::borderStrong);
    g.fillRect (first.getX() - space::sm - 1, first.getY() + space::sm, 1, first.getHeight() - 2 * space::sm);

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
    constexpr int scrollBarSpace = 10, gap = 6, groupGap = space::lg, cardPadding = space::md;
    const auto stripHeight = MixerStrip::getContentHeight();
    const auto cardHeight = 2 * cardPadding + size::buttonCompact + space::sm + stripHeight + scrollBarSpace;
    bankCard = { space::page, getHeight() - space::sm - cardHeight, getWidth() - 2 * space::page, cardHeight };
    inspector.setBounds (getLocalBounds().withBottom (bankCard.getY()));

    // The card's title on the left, and the Channels / Close mics switch on the right, above the strips it changes.
    auto inner = bankCard.reduced (cardPadding);
    bankHeader = inner.removeFromTop (size::buttonCompact);
    auto views = bankHeader.removeFromRight (160);
    channelsView.setBounds (views.removeFromLeft (80));
    trimsView.setBounds (views);
    inner.removeFromTop (space::sm);
    auto bank = inner;

    // The returns and master stay pinned on the right; the rest scrolls.
    masterStrip.setBounds (bank.removeFromRight (size::mixerStrip).withHeight (stripHeight));

    for (auto it = returnStrips.rbegin(); it != returnStrips.rend(); ++it)
    {
        bank.removeFromRight (gap);
        (*it)->setBounds (bank.removeFromRight (size::mixerStrip).withHeight (stripHeight));
    }

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
