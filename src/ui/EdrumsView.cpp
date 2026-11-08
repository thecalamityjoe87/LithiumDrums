#include "EdrumsView.h"

#include "Theme.h"

namespace lithium
{

namespace
{
    constexpr int rowHeight = theme::size::button;
    constexpr int labelWidth = 92;
    constexpr int cardHeader = 34;

    // A positional pad: its name, the controller row, three slider rows and
    // the position bar with its labels underneath.
    constexpr int sliderRowHeight = 18;
    constexpr int padHeight = 22 + 4 + rowHeight + 4 + 3 * sliderRowHeight + 4 + 6 + 14;
    constexpr int maxMonitorLines = 300;
    const std::array<const char*, numPositionSources> padNames { "Snare", "Ride", "Toms", "Hi-hat" };   // by PositionSource

    void fillControllers (juce::ComboBox& box)
    {
        box.addItem ("Off", 1);

        for (int cc = 0; cc < 128; ++cc)
            box.addItem ("CC " + juce::String (cc), cc + 2);
    }

    void fillChannels (juce::ComboBox& box)
    {
        box.addItem ("Any channel", 1);

        for (int ch = 1; ch <= 16; ++ch)
            box.addItem ("Channel " + juce::String (ch), ch + 1);
    }

    /** A switch, drawn like the other toggles in the UI (lit when on). */
    void setUpToggle (juce::TextButton& b)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonOnColourId, theme::colours::accent.withAlpha (0.85f));
    }

    void setUpSlider (ControlSlider& s, double low, double high, double defaultValue)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        s.setRange (low, high, 0.01);
        s.setDoubleClickReturnValue (true, defaultValue);
        s.setColour (juce::Slider::trackColourId, theme::colours::accent);
        s.setColour (juce::Slider::backgroundColourId, theme::colours::background);
    }

    void drawCard (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
    {
        using namespace theme;
        g.setColour (colours::raised);
        g.fillRoundedRectangle (area.toFloat(), 8.0f);
        g.setColour (colours::border);
        g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 8.0f, 1.0f);
        g.setColour (colours::textMuted);
        drawTrackedText (g, title, label (12.5f, Weight::semibold), 0.12f,
                         area.reduced (space::lg, 0).removeFromTop (cardHeader).toFloat().translated (0.0f, 2.0f),
                         juce::Justification::centredLeft);
    }

    void drawLabel (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text)
    {
        g.setColour (theme::colours::textMuted);
        g.setFont (theme::body (theme::type::small));
        g.drawText (text, area, juce::Justification::centredLeft, true);
    }

    /** A live value as a bar, with optional calibration marks (raw 0-127). */
    void drawValueBar (juce::Graphics& g, juce::Rectangle<float> bar, float value, juce::Colour colour, int low = -1, int high = -1)
    {
        using namespace theme;
        g.setColour (colours::background);
        g.fillRoundedRectangle (bar, 3.0f);

        if (value >= 0.0f)
        {
            g.setColour (colour);
            g.fillRoundedRectangle (bar.withWidth (std::max (6.0f, bar.getWidth() * value)), 3.0f);
        }

        if (low >= 0)
        {
            g.setColour (colours::text.withAlpha (0.7f));

            for (const auto raw : { low, high })
                g.fillRect (juce::Rectangle<float> (bar.getX() + bar.getWidth() * (float) raw / 127.0f - 0.5f, bar.getY() - 3.0f, 1.0f,
                                                    bar.getHeight() + 6.0f));
        }
    }
}

//==============================================================================
/** Where the pedal's travel moves the hats from one openness step to the
    next, upright like the pedal itself: closed at the bottom, fully open at
    the top. Four handles to drag, between closed, a quarter, half, three
    quarters and open, and an arrow for where the pedal is now. */
class EdrumsView::ThresholdBar final : public juce::Component, public juce::SettableTooltipClient
{
public:
    ThresholdBar()
    {
        setTooltip ("The pedal's travel, closed at the bottom and open at the top, and which hi-hat sound plays in each part of it. "
                    "Drag a handle up or down to move where the sound changes. Double-click for the defaults.");
    }

    std::function<void (std::array<float, ControllerSettings::numThresholds>)> onChange;

    void setThresholds (const std::array<float, ControllerSettings::numThresholds>& t)    { values = t; repaint(); }

    void setPedal (float openness)
    {
        if (std::abs (openness - pedal) > 0.002f)
        {
            pedal = openness;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto track = getTrack();
        constexpr std::array<const char*, 5> names { "CLOSED", "1/4", "1/2", "3/4", "OPEN" };
        auto start = track.getBottom();

        // The regions, from the bottom up, each named beside the track.
        for (size_t i = 0; i < names.size(); ++i)
        {
            const auto end = i < values.size() ? yFor (values[i]) : track.getY();
            const auto region = juce::Rectangle<float> (track.getX(), end, track.getWidth(), start - end);
            g.setColour (colours::groupHats.withAlpha (0.08f + 0.1f * (float) i));
            g.fillRect (region);
            g.setColour (colours::textMuted);
            drawTrackedText (g, names[i], label (10.0f), 0.06f, region.withX (track.getRight() + (float) space::sm).withWidth (namesWidth),
                             juce::Justification::centredLeft);
            start = end;
        }

        g.setColour (colours::border);
        g.drawRect (track, 1.0f);

        for (size_t i = 0; i < values.size(); ++i)
        {
            const auto y = yFor (values[i]);
            g.setColour ((int) i == dragging || (int) i == hovering ? colours::accent : colours::text);
            g.fillRoundedRectangle (juce::Rectangle<float> (track.getWidth() + 6.0f, 5.0f).withCentre ({ track.getCentreX(), y }), 2.0f);
        }

        if (pedal >= 0.0f)
        {
            const auto y = yFor (pedal);
            juce::Path marker;
            marker.addTriangle (track.getX() - 9.0f, y - 5.0f, track.getX() - 9.0f, y + 5.0f, track.getX() - 2.0f, y);
            g.setColour (colours::accent);
            g.fillPath (marker);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (const auto h = handleAt (e.position.y); h != hovering)
        {
            hovering = h;
            setMouseCursor (h >= 0 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override     { hovering = -1; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override   { dragging = handleAt (e.position.y); }
    void mouseUp (const juce::MouseEvent&) override       { dragging = -1; repaint(); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;

        // Each handle stays between its neighbours, a little apart.
        constexpr float gap = 0.02f;
        const auto i = (size_t) dragging;
        const auto low = i == 0 ? gap : values[i - 1] + gap;
        const auto high = i + 1 == values.size() ? 1.0f - gap : values[i + 1] - gap;
        values[i] = juce::jlimit (low, high, (getTrack().getBottom() - e.position.y) / getTrack().getHeight());
        repaint();

        if (onChange)
            onChange (values);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        values = ControllerSettings().hatThresholds;
        repaint();

        if (onChange)
            onChange (values);
    }

private:
    // The track in the middle; the pedal's arrow on its left, the zone names on its right.
    static constexpr float trackWidth = 18.0f, namesWidth = 46.0f;

    juce::Rectangle<float> getTrack() const
    {
        return getLocalBounds().toFloat().withSizeKeepingCentre (trackWidth, (float) getHeight()).reduced (0.0f, 6.0f);
    }
    float yFor (float v) const                    { return getTrack().getBottom() - v * getTrack().getHeight(); }

    int handleAt (float y) const
    {
        for (size_t i = 0; i < values.size(); ++i)
            if (std::abs (y - yFor (values[i])) < 6.0f)
                return (int) i;

        return -1;
    }

    std::array<float, ControllerSettings::numThresholds> values {};
    float pedal = -1.0f;
    int dragging = -1, hovering = -1;
};

//==============================================================================
/** One positional pad: on/off, its controller and channel, Learn, Calibrate,
    the curve, the emulation amount, the edge velocity compensation and
    where the last hit was. */
class EdrumsView::PadRow final : public juce::Component
{
public:
    explicit PadRow (PositionSource s) : source (s)
    {
        fillControllers (cc);
        fillChannels (channel);
        setUpSlider (curve, -1.0, 1.0, 0.0);
        setUpSlider (emulation, 0.0, 1.0, 0.5);
        setUpSlider (edgeVelocity, -1.0, 1.0, 0.0);

        const auto ride = source == PositionSource::ride;
        const auto hat = source == PositionSource::hihat;
        setUpToggle (on);
        on.setTooltip (juce::String ("Follow the ") + padNames[(size_t) s] + " pad's positional sensing");
        cc.setTooltip ("The controller the module sends the strike position on, just before each hit");
        channel.setTooltip ("The MIDI channel it comes on");
        learn.setTooltip ("Click, then hit the pad: its position controller and channel are set");
        calibrate.setTooltip ("Click, then hit the pad from the centre right out to the edge, then click Done");
        curve.setTooltip ("Below the middle, the sound reaches the edge later in the pad's range; above, sooner");
        emulation.setTooltip (ride ? "On kits without positional ride recordings: how much of the ride's edge hit blends in at the far edge of the bow"
                              : hat ? "How strongly the hats' tone changes from the bell to the edge of the bow (brighter, more wash). "
                                      "No kit has hats recorded at several positions, so this always applies; the pedal still sets how open they are."
                                    : "On kits without positional recordings: how strongly the tone changes towards the edge (less body, more ring)");
        edgeVelocity.setTooltip ("Velocity compensation, for pads that read edge hits softer (or louder) than centre hits. "
                                 "Right of the middle, hits get louder towards the edge, up to half as loud again at the far edge; "
                                 "left, softer. Only hits that follow the position.");

        for (auto* c : std::initializer_list<juce::Component*> { &on, &cc, &channel, &learn, &calibrate, &curve, &emulation, &edgeVelocity })
            addAndMakeVisible (c);
    }

    const PositionSource source;
    juce::TextButton on { "On" };
    juce::ComboBox cc, channel;
    juce::TextButton learn { "Learn" }, calibrate { "Calibrate" };
    ControlSlider curve, emulation, edgeVelocity;
    float position = -1.0f;
    int low = 0, high = 127;

    void setPosition (float p)
    {
        if (std::abs (p - position) > 0.002f)
        {
            position = p;
            repaint (bar);
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        g.setColour (colours::text);
        drawTrackedText (g, juce::String (padNames[(size_t) source]).toUpperCase(), label (type::heading, Weight::semibold), 0.1f,
                         title.toFloat(), juce::Justification::centredLeft);
        drawLabel (g, curveText, "Curve");
        drawLabel (g, emulationText, source == PositionSource::ride ? "Edge blend" : "Edge tone");
        drawLabel (g, edgeVelocityText, "Edge velocity");
        drawValueBar (g, bar.toFloat(), position, colours::groupSnare, low, high);
        g.setColour (colours::textMuted);
        g.setFont (body (11.0f));
        g.drawText (source == PositionSource::ride || source == PositionSource::hihat ? "bell side" : "centre", bar.translated (0, 8).withHeight (12),
                    juce::Justification::centredLeft, false);
        g.drawText ("edge", bar.translated (0, 8).withHeight (12), juce::Justification::centredRight, false);
    }

    void resized() override
    {
        using namespace theme;
        auto area = getLocalBounds();
        auto top = area.removeFromTop (22);
        on.setBounds (top.removeFromRight (48).withSizeKeepingCentre (48, theme::size::buttonCompact + 4));
        title = top;
        area.removeFromTop (space::xs);

        auto row = area.removeFromTop (rowHeight);
        cc.setBounds (row.removeFromLeft (84));
        row.removeFromLeft (space::xs);
        channel.setBounds (row.removeFromLeft (112));
        row.removeFromLeft (space::xs);
        calibrate.setBounds (row.removeFromRight (82));
        row.removeFromRight (space::xs);
        learn.setBounds (row);
        area.removeFromTop (space::xs);

        for (auto [slider, text] : { std::pair { &curve, &curveText }, std::pair { &emulation, &emulationText },
                                     std::pair { &edgeVelocity, &edgeVelocityText } })
        {
            auto r = area.removeFromTop (sliderRowHeight);
            *text = r.removeFromLeft (labelWidth);
            slider->setBounds (r);
        }

        area.removeFromTop (space::xs);
        bar = area.removeFromTop (6).reduced (2, 0);
    }

private:
    juce::Rectangle<int> title, curveText, emulationText, edgeVelocityText, bar;
};

//==============================================================================
/** The MIDI monitor: a line per message, newest at the top. */
class EdrumsView::Monitor final : public juce::Component
{
public:
    struct Line
    {
        juce::String what, result;
        juce::Colour colour;
    };

    bool paused = false;

    void add (Line line)
    {
        if (paused)
            return;

        lines.push_front (std::move (line));

        while (lines.size() > (size_t) maxMonitorLines)
            lines.pop_back();

        repaint();
    }

    void clear()
    {
        lines.clear();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        constexpr int lineHeight = 22;
        auto area = getLocalBounds();

        if (lines.empty())
        {
            g.setColour (colours::textMuted);
            g.setFont (body (type::small));
            g.drawFittedText ("Play the kit: every note and controller shows up here, with what it played.", area.reduced (space::sm),
                              juce::Justification::topLeft, 3, 1.0f);
            return;
        }

        for (size_t i = 0; i < lines.size() && area.getHeight() >= lineHeight; ++i)
        {
            auto row = area.removeFromTop (lineHeight);

            if (i % 2 == 0)
            {
                g.setColour (colours::panel);
                g.fillRect (row);
            }

            row.reduce (space::sm, 0);
            g.setColour (lines[i].colour);
            g.fillEllipse (row.removeFromLeft (8).toFloat().withSizeKeepingCentre (5.0f, 5.0f));
            row.removeFromLeft (space::xs);
            g.setColour (colours::text);
            g.setFont (mono (11.5f));
            g.drawText (lines[i].what, row.removeFromLeft (132), juce::Justification::centredLeft, true);
            g.setColour (colours::textMuted);
            g.setFont (body (type::small));
            g.drawText (lines[i].result, row, juce::Justification::centredLeft, true);
        }
    }

private:
    std::deque<Line> lines;
};

//==============================================================================
EdrumsView::EdrumsView (LithiumDrumsProcessor& p) : processor (p), thresholds (std::make_unique<ThresholdBar>()),
                                                     monitor (std::make_unique<Monitor>())
{
    // The pedal
    fillControllers (pedalCc);
    fillChannels (pedalChannel);
    pedalCc.setTooltip ("The controller the hi-hat pedal sends: CC 4 on most modules");
    pedalChannel.setTooltip ("The MIDI channel it comes on");
    pedalLearn.setTooltip ("Click, then move the pedal: its controller and channel are set");
    pedalCalibrate.setTooltip ("Click, press the pedal fully down and let it fully up, then click Done: the range it really sends is kept");
    setUpToggle (pedalInvert);
    setUpToggle (footSplash);
    pedalInvert.setTooltip ("For pedals that send 0 closed and 127 open (most send 127 closed)");
    pedalCc.onChange = [this] { setControllers ([this] (auto& c) { c.hihatPedal = pedalCc.getSelectedId() - 2; }); };
    pedalChannel.onChange = [this] { setControllers ([this] (auto& c) { c.hihatChannel = pedalChannel.getSelectedId() - 1; }); };
    pedalInvert.onClick = [this] { setControllers ([this] (auto& c) { c.pedalInverted = pedalInvert.getToggleState(); }); };
    pedalLearn.onClick = [this] { startLearning (learning == 0 ? -1 : 0); };
    pedalCalibrate.onClick = [this] { startCalibrating (calibrating == 0 ? -1 : 0); };

    setUpSlider (pedalCurve, -1.0, 1.0, 0.0);
    pedalCurve.setTooltip ("Below the middle, the hats open later in the pedal's travel; above, sooner");
    pedalCurve.onValueChange = [this] { setControllers ([this] (auto& c) { c.pedalCurve = (float) pedalCurve.getValue(); }); };

    thresholds->onChange = [this] (auto t) { setControllers ([t] (auto& c) { c.hatThresholds = t; }); };

    chickMode.addItem ("Off", 1);
    chickMode.addItem ("Until the module sends pedal notes", 2);
    chickMode.addItem ("Always", 3);
    chickMode.setTooltip ("Play a pedal chick when the pedal closes quickly. Most modules send a note for it (44), "
                          "which plays anyway; the default plays one from the pedal only until such a note arrives.");
    chickMode.onChange = [this] { setControllers ([this] (auto& c) { c.chickFromPedal = (ChickMode) (chickMode.getSelectedId() - 1); }); };

    setUpSlider (chickSensitivity, 0.0, 1.0, 0.5);
    chickSensitivity.setTooltip ("How slow a close still plays a chick (and a reopening a foot splash)");
    chickSensitivity.onValueChange = [this] { setControllers ([this] (auto& c) { c.chickSensitivity = (float) chickSensitivity.getValue(); }); };

    footSplash.setTooltip ("Closing the pedal and quickly opening it again plays the kit's foot splash");
    footSplash.onClick = [this] { setControllers ([this] (auto& c) { c.footSplash = footSplash.getToggleState(); }); };

    for (auto* c : std::initializer_list<juce::Component*> { &pedalCc, &pedalChannel, &pedalLearn, &pedalCalibrate, &pedalInvert,
                                                             &pedalCurve, thresholds.get(), &chickMode, &chickSensitivity, &footSplash })
        addAndMakeVisible (c);

    // The pads
    for (size_t i = 0; i < pads.size(); ++i)
    {
        auto& pad = pads[i];
        pad = std::make_unique<PadRow> ((PositionSource) i);
        pad->on.onClick = [this, i] { setControllers ([this, i] (auto& c) { c.positionEnabled[i] = pads[i]->on.getToggleState(); }); };
        pad->cc.onChange = [this, i] { setControllers ([this, i] (auto& c) { c.position[i] = pads[i]->cc.getSelectedId() - 2; }); };
        pad->channel.onChange = [this, i] { setControllers ([this, i] (auto& c) { c.positionChannel[i] = pads[i]->channel.getSelectedId() - 1; }); };
        pad->curve.onValueChange = [this, i] { setControllers ([this, i] (auto& c) { c.positionCurve[i] = (float) pads[i]->curve.getValue(); }); };
        pad->emulation.onValueChange = [this, i] { setControllers ([this, i] (auto& c) { c.positionEmulation[i] = (float) pads[i]->emulation.getValue(); }); };
        pad->edgeVelocity.onValueChange = [this, i] { setControllers ([this, i] (auto& c) { c.positionVelocity[i] = (float) pads[i]->edgeVelocity.getValue(); }); };
        pad->learn.onClick = [this, i] { startLearning (learning == (int) i + 1 ? -1 : (int) i + 1); };
        pad->calibrate.onClick = [this, i] { startCalibrating (calibrating == (int) i + 1 ? -1 : (int) i + 1); };
        addAndMakeVisible (*pad);
    }

    // The monitor
    pause.setClickingTogglesState (true);
    pause.setTooltip ("Hold the list still, to read it");
    pause.onClick = [this] { monitor->paused = pause.getToggleState(); };
    clear.onClick = [this] { monitor->clear(); };

    for (auto* c : std::initializer_list<juce::Component*> { monitor.get(), &pause, &clear })
        addAndMakeVisible (c);

    refresh();
}

EdrumsView::~EdrumsView() = default;

void EdrumsView::setControllers (const std::function<void (ControllerSettings&)>& change)
{
    auto c = processor.getMidiMap().controllers;
    const auto before = c;
    change (c);

    if (c != before)
        processor.setControllers (c);
}

void EdrumsView::refresh()
{
    const auto c = processor.getMidiMap().controllers;
    pedalCc.setSelectedId (c.hihatPedal + 2, juce::dontSendNotification);
    pedalChannel.setSelectedId (c.hihatChannel + 1, juce::dontSendNotification);
    pedalInvert.setToggleState (c.pedalInverted, juce::dontSendNotification);
    pedalCurve.setValue (c.pedalCurve, juce::dontSendNotification);
    thresholds->setThresholds (c.hatThresholds);
    chickMode.setSelectedId ((int) c.chickFromPedal + 1, juce::dontSendNotification);
    chickSensitivity.setValue (c.chickSensitivity, juce::dontSendNotification);
    footSplash.setToggleState (c.footSplash, juce::dontSendNotification);

    for (size_t i = 0; i < pads.size(); ++i)
    {
        auto& pad = *pads[i];
        pad.on.setToggleState (c.positionEnabled[i], juce::dontSendNotification);
        pad.cc.setSelectedId (c.position[i] + 2, juce::dontSendNotification);
        pad.channel.setSelectedId (c.positionChannel[i] + 1, juce::dontSendNotification);
        pad.curve.setValue (c.positionCurve[i], juce::dontSendNotification);
        pad.emulation.setValue (c.positionEmulation[i], juce::dontSendNotification);
        pad.edgeVelocity.setValue (c.positionVelocity[i], juce::dontSendNotification);
        pad.low = c.positionMin[i];
        pad.high = c.positionMax[i];
        pad.repaint();
    }

    repaint();
}

void EdrumsView::startLearning (int target)
{
    learning = target;
    calibrating = -1;
    updateButtons();
}

void EdrumsView::startCalibrating (int target)
{
    // Done: keep the range seen, if the controller really moved.
    if (target < 0 && calibrating >= 0 && calibrationHigh - calibrationLow >= 8)
    {
        const auto which = calibrating;
        const auto low = calibrationLow, high = calibrationHigh;

        setControllers ([which, low, high] (auto& c)
        {
            if (which == 0)
            {
                c.pedalMin = low;
                c.pedalMax = high;
            }
            else
            {
                c.positionMin[(size_t) which - 1] = low;
                c.positionMax[(size_t) which - 1] = high;
            }
        });
    }

    calibrating = target;
    learning = -1;
    calibrationLow = 127;
    calibrationHigh = 0;
    updateButtons();
    refresh();
}

void EdrumsView::stopListening()
{
    learning = calibrating = -1;
    updateButtons();
}

void EdrumsView::updateButtons()
{
    pedalLearn.setButtonText (learning == 0 ? "Move it" : "Learn");
    pedalLearn.setToggleState (learning == 0, juce::dontSendNotification);
    pedalCalibrate.setButtonText (calibrating == 0 ? "Done" : "Calibrate");
    pedalCalibrate.setToggleState (calibrating == 0, juce::dontSendNotification);

    for (size_t i = 0; i < pads.size(); ++i)
    {
        pads[i]->learn.setButtonText (learning == (int) i + 1 ? "Hit the pad" : "Learn");
        pads[i]->learn.setToggleState (learning == (int) i + 1, juce::dontSendNotification);
        pads[i]->calibrate.setButtonText (calibrating == (int) i + 1 ? "Done" : "Calibrate");
        pads[i]->calibrate.setToggleState (calibrating == (int) i + 1, juce::dontSendNotification);
    }

    repaint (rangeText);
}

void EdrumsView::handle (const MidiMonitorEvent& e)
{
    using Kind = MidiMonitorEvent::Kind;
    using Detail = MidiMonitorEvent::Detail;
    const auto c = processor.getMidiMap().controllers;
    const auto info = processor.getKitInfo();

    auto articulationName = [&info] (int a) -> juce::String
    {
        if (info == nullptr || a < 0)
            return {};

        const auto [p, local] = pieceAndArticulation (*info, a);
        return p >= 0 ? info->pieces[(size_t) p].name + ", " + info->pieces[(size_t) p].articulations[(size_t) local].name : juce::String();
    };

    auto percent = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
    const auto channel = e.channel == 0 ? juce::String ("key ") : "ch" + juce::String (e.channel).paddedLeft (' ', 2) + " ";
    Monitor::Line line;
    line.colour = theme::colours::textMuted;

    switch (e.kind)
    {
        case Kind::note:
        {
            line.what = channel + "NOTE " + juce::String (e.number).paddedLeft (' ', 3) + " v" + juce::String (e.value);

            if (e.detail == Detail::choke)
            {
                line.result = "choke";
                line.colour = theme::colours::groupCymbals;
                break;
            }

            if (e.articulation < 0)
            {
                line.result = "not mapped";
                break;
            }

            line.colour = theme::colours::accent;
            line.result = articulationName (e.articulation);

            if (e.blendedWith >= 0)
                line.result << " + " << articulationName (e.blendedWith).fromFirstOccurrenceOf (", ", false, false) << " " << percent (e.blend);

            switch (e.detail)
            {
                case Detail::pedal:     line.result << "  (pedal " << percent (e.amount) << " open"
                                                    << (e.position >= 0.0f ? ", position " + percent (e.position) : juce::String()) << ")"; break;
                case Detail::position:  line.result << "  (position " << percent (e.amount) << ")"; break;
                case Detail::emulated:  line.result << "  (position " << percent (e.amount) << ", tone)"; break;
                case Detail::rideBlend: line.result << "  (position " << percent (e.amount) << ")"; break;
                case Detail::none:
                case Detail::choke:
                case Detail::ignored:   break;
            }

            break;
        }

        case Kind::controller:
        {
            line.what = channel + "CC " + juce::String (e.number).paddedLeft (' ', 3) + " = " + juce::String (e.value);
            const auto name = e.source < 0 ? juce::String ("hi-hat pedal") : juce::String (padNames[(size_t) e.source]).toLowerCase() + " position";

            if (e.detail == Detail::ignored)
            {
                line.result = name + ", but on another channel: ignored";
                line.colour = theme::colours::warning;
            }
            else if (e.detail == Detail::pedal)
            {
                line.result = "hi-hat pedal, " + (e.amount < 0.05f ? juce::String ("closed") : percent (e.amount) + " open");
                line.colour = theme::colours::groupHats;
            }
            else if (e.detail == Detail::position)
            {
                line.result = name + " " + percent (e.amount);
                line.colour = theme::colours::groupSnare;
            }
            else
            {
                line.result = "not used";
            }

            // Learn takes the first controller that comes in; Calibrate the range of the one being set up.
            if (learning >= 0)
            {
                const auto target = learning;
                const int number = e.number, ch = e.channel;

                setControllers ([target, number, ch] (auto& s)
                {
                    if (target == 0)
                    {
                        s.hihatPedal = number;
                        s.hihatChannel = ch;
                    }
                    else
                    {
                        s.position[(size_t) target - 1] = number;
                        s.positionChannel[(size_t) target - 1] = ch;
                    }
                });

                line.result = "learned for the " + (target == 0 ? juce::String ("hi-hat pedal") : juce::String (padNames[(size_t) target - 1]));
                startLearning (-1);
                refresh();
            }

            const auto calibratingCc = calibrating == 0 ? c.hihatPedal : calibrating > 0 ? c.position[(size_t) calibrating - 1] : -1;

            if (calibrating >= 0 && e.number == calibratingCc)
            {
                calibrationLow = std::min (calibrationLow, (int) e.value);
                calibrationHigh = std::max (calibrationHigh, (int) e.value);
                repaint (rangeText);
            }

            if (e.number == c.hihatPedal)
            {
                pedalRaw = e.value;
                repaint (pedalBar);
            }

            break;
        }

        case Kind::aftertouch:
            line.what = channel + "AT " + juce::String (e.number).paddedLeft (' ', 3) + " = " + juce::String (e.value);
            line.result = e.detail == Detail::choke ? "grab: choke " + articulationName (e.articulation).upToFirstOccurrenceOf (",", false, false)
                                                    : juce::String ("not used");
            line.colour = e.detail == Detail::choke ? theme::colours::groupCymbals : theme::colours::textMuted;
            break;

        case Kind::pedalChick:
        case Kind::footSplash:
            line.what = channel + (e.kind == Kind::pedalChick ? "PEDAL CHICK" : "FOOT SPLASH");
            line.result = articulationName (e.articulation) + ", v" + juce::String (e.value) + "  (from the pedal's movement)";
            line.colour = theme::colours::groupHats;
            break;
    }

    monitor->add (std::move (line));
}

void EdrumsView::tick()
{
    const auto& fb = processor.getFeedback();
    thresholds->setPedal (fb.hihatOpenness.load (std::memory_order_relaxed));

    for (size_t i = 0; i < pads.size(); ++i)
        pads[i]->setPosition (fb.position[i].load (std::memory_order_relaxed));
}

void EdrumsView::paint (juce::Graphics& g)
{
    using namespace theme;
    drawCard (g, hatCard, "HI-HAT PEDAL");
    drawCard (g, padsCard, "POSITIONAL SENSING");
    drawCard (g, monitorCard, "MIDI MONITOR");

    const auto c = processor.getMidiMap().controllers;

    // The pedal's raw value against its calibrated range.
    drawValueBar (g, pedalBar.toFloat(), pedalRaw >= 0 ? (float) pedalRaw / 127.0f : -1.0f, colours::groupHats, c.pedalMin, c.pedalMax);
    g.setColour (colours::textMuted);
    g.setFont (body (11.0f));
    g.drawText (pedalRaw >= 0 ? "sends " + juce::String (pedalRaw) : juce::String ("not received yet"),
                pedalBar.translated (0, 8).withHeight (12), juce::Justification::centredLeft, false);

    const auto calibratingNow = calibrating >= 0;
    g.setColour (calibratingNow ? colours::accent : colours::textMuted);
    g.setFont (body (type::small));
    g.drawFittedText (calibratingNow ? (calibrationHigh >= calibrationLow ? "Seen " + juce::String (calibrationLow) + " to " + juce::String (calibrationHigh) + ". Click Done when you've covered the whole range."
                                                                         : juce::String ("Move it through its whole range, then click Done."))
                                     : "Range " + juce::String (c.pedalMin) + " to " + juce::String (c.pedalMax),
                      rangeText, juce::Justification::centredLeft, 2, 1.0f);

    drawLabel (g, curveLabel, "Curve");

    // The openness steps in an inset panel of their own, captioned like a card.
    g.setColour (colours::panel);
    g.fillRoundedRectangle (stepsBox.toFloat(), cornerRadius);
    g.setColour (colours::border);
    g.drawRoundedRectangle (stepsBox.toFloat().reduced (0.5f), cornerRadius, 1.0f);
    g.setColour (colours::textMuted);
    drawTrackedText (g, "PEDAL ZONES", label (type::caption, Weight::semibold), 0.12f, stepsLabel.toFloat(), juce::Justification::centred);
    drawLabel (g, chickLabel, "Pedal chick");
    drawLabel (g, sensitivityLabel, "Sensitivity");

    // Whether the loaded kit can play what the pedal settings ask for.
    if (const auto info = processor.getKitInfo())
    {
        const auto has = [&info] (const char* word)
        {
            for (const auto& p : info->pieces)
                if (p.role == PieceRole::hihat)
                    for (const auto& a : p.articulations)
                        if (a.id.containsIgnoreCase (word))
                            return true;

            return false;
        };

        g.setColour (colours::textMuted);
        g.setFont (body (11.5f));
        g.drawFittedText ((has ("splash") || has ("swish") ? juce::String() : info->credits.name + " has no foot splash recording. ")
                              + (has ("pedal") || has ("foot") ? juce::String() : info->credits.name + " has no pedal chick recording."),
                          splashNote, juce::Justification::topLeft, 2, 1.0f);
    }

}

void EdrumsView::resized()
{
    using namespace theme;
    auto area = getLocalBounds();
    hatCard = area.removeFromLeft (415);
    area.removeFromLeft (space::lg);
    padsCard = area.removeFromLeft (395);
    area.removeFromLeft (space::lg);
    monitorCard = area;

    // The pedal card: the settings in a column on the left, the openness
    // steps upright down the right, like the pedal's travel.
    {
        auto a = hatCard.reduced (space::lg, 0).withTrimmedTop (cardHeader + space::xs).withTrimmedBottom (space::lg);
        stepsBox = a.removeFromRight (112);
        a.removeFromRight (space::md);
        auto steps = stepsBox.reduced (space::xs, space::sm);
        stepsLabel = steps.removeFromTop (16);
        steps.removeFromTop (space::xs);
        thresholds->setBounds (steps);

        auto row = a.removeFromTop (rowHeight);
        pedalCc.setBounds (row.removeFromLeft (78));
        row.removeFromLeft (space::xs);
        pedalChannel.setBounds (row.removeFromLeft (108));
        row.removeFromLeft (space::xs);
        pedalLearn.setBounds (row);
        a.removeFromTop (space::sm);

        row = a.removeFromTop (rowHeight);
        pedalInvert.setBounds (row.removeFromLeft (78));
        row.removeFromLeft (space::xs);
        pedalCalibrate.setBounds (row.removeFromLeft (108));
        a.removeFromTop (space::xs);
        rangeText = a.removeFromTop (34);
        a.removeFromTop (space::xs);

        pedalBar = a.removeFromTop (6).reduced (2, 0);
        a.removeFromTop (space::xl);

        row = a.removeFromTop (22);
        curveLabel = row.removeFromLeft (labelWidth - 16);
        pedalCurve.setBounds (row);
        a.removeFromTop (space::lg);

        chickLabel = a.removeFromTop (18);
        a.removeFromTop (space::xs);
        chickMode.setBounds (a.removeFromTop (rowHeight));
        a.removeFromTop (space::sm);
        row = a.removeFromTop (22);
        sensitivityLabel = row.removeFromLeft (labelWidth - 16);
        chickSensitivity.setBounds (row);
        a.removeFromTop (space::md);
        footSplash.setBounds (a.removeFromTop (rowHeight).withWidth (120));
        a.removeFromTop (space::xs);
        splashNote = a.removeFromTop (34);
    }

    // The pads card.
    {
        auto a = padsCard.reduced (space::lg, 0).withTrimmedTop (cardHeader);

        for (const auto source : positionDisplayOrder)
        {
            pads[(size_t) source]->setBounds (a.removeFromTop (padHeight));
            a.removeFromTop (space::xxs);
        }
    }

    // The monitor card.
    {
        auto header = monitorCard.reduced (space::md - 2, 0).removeFromTop (cardHeader).withSizeKeepingCentre (monitorCard.getWidth() - 2 * space::md + 4, 22).translated (0, 2);
        clear.setBounds (header.removeFromRight (60));
        header.removeFromRight (space::xs);
        pause.setBounds (header.removeFromRight (60));
        monitor->setBounds (monitorCard.reduced (space::xs).withTrimmedTop (cardHeader + space::xs));
    }
}

} // namespace lithium
