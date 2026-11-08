#include "ChannelModules.h"

#include "Theme.h"

namespace lithium
{

namespace
{
    constexpr int moduleHeaderHeight = 30, padding = 12;
}

//==============================================================================
Module::Module (juce::String t) : title (std::move (t)) {}

void Module::paint (juce::Graphics& g)
{
    using namespace theme;

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (colours::raised);
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (colours::border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);

    g.setColour (colours::textMuted);
    drawTrackedText (g, title.toUpperCase(), label (12.0f), 0.12f,
                     getLocalBounds().reduced (padding, 0).removeFromTop (moduleHeaderHeight).toFloat().translated (0.0f, 2.0f),
                     juce::Justification::centredLeft);
}

juce::Rectangle<int> Module::getContentArea() const
{
    return getLocalBounds().reduced (padding, 0).withTrimmedTop (moduleHeaderHeight + 2).withTrimmedBottom (padding - 2);
}

void Module::layoutHeaderButtons (juce::Component* on, juce::Component* reset)
{
    auto row = getLocalBounds().reduced (padding - 2, 0).removeFromTop (moduleHeaderHeight).withSizeKeepingCentre (getWidth() - 2 * padding + 4, 20).translated (0, 2);

    if (reset != nullptr)
    {
        reset->setBounds (row.removeFromRight (50));
        row.removeFromRight (6);
    }

    if (on != nullptr)
        on->setBounds (row.removeFromRight (38));
}

//==============================================================================
EqModule::EqModule (juce::AudioProcessorValueTreeState& s) : Module ("EQ"), state (s), graph (s)
{
    for (auto* k : { &lowGain, &mid1Gain, &mid2Gain, &highGain })
        k->setBipolar (true);

    reset.setTooltip ("Flatten this channel's EQ");
    reset.onClick = [this] { if (channel.isValid()) params::resetEq (state, channel); };

    for (auto* c : std::initializer_list<juce::Component*> { &graph, &on, &reset, &hpf, &lowFreq, &lowGain, &mid1Freq, &mid1Gain,
                                                             &mid1Q, &mid2Freq, &mid2Gain, &mid2Q, &highFreq, &highGain })
        addAndMakeVisible (c);
}

void EqModule::attach (params::ChannelId c)
{
    channel = c;
    graph.setChannel (c);
    on.attach (state, c.param (params::eqOn));

    const std::pair<Knob*, std::string_view> knobs[] {
        { &hpf, params::eqHighPass },   { &lowFreq, params::eqLowFreq },   { &lowGain, params::eqLowGain },
        { &mid1Freq, params::eqMid1Freq }, { &mid1Gain, params::eqMid1Gain }, { &mid1Q, params::eqMid1Q },
        { &mid2Freq, params::eqMid2Freq }, { &mid2Gain, params::eqMid2Gain }, { &mid2Q, params::eqMid2Q },
        { &highFreq, params::eqHighFreq }, { &highGain, params::eqHighGain } };

    for (const auto& [knob, name] : knobs)
        knob->attach (state, c.param (name));
}

void EqModule::resized()
{
    layoutHeaderButtons (&on, &reset);
    auto area = getContentArea();
    dividers.clear();

    // One row of knobs if they fit side by side; otherwise two (the
    // shelves and high-pass, then the two mids), with a smaller curve.
    constexpr int groupGap = 10, knobHeight = 66;   // a dial of theme::size::knob plus its name and value
    const auto oneRow = (area.getWidth() - 4 * groupGap) / 11 >= 46;
    auto knobs = area.removeFromBottom (oneRow ? knobHeight : knobHeight * 2 + 4);
    area.removeFromBottom (6);
    graph.setBounds (area);

    auto layoutGroups = [this] (juce::Rectangle<int> row, std::initializer_list<std::initializer_list<Knob*>> groups)
    {
        int count = 0;

        for (auto& group : groups)
            count += (int) group.size();

        const auto w = (row.getWidth() - groupGap * ((int) groups.size() - 1)) / count;
        bool first = true;

        for (auto& group : groups)
        {
            if (! first)
            {
                dividers.push_back ({ row.getX() + groupGap / 2, row.getY() + 8, 1, row.getHeight() - 16 });
                row.removeFromLeft (groupGap);
            }

            for (auto* k : group)
                k->setBounds (row.removeFromLeft (w));

            first = false;
        }
    };

    if (oneRow)
    {
        layoutGroups (knobs, { { &hpf }, { &lowFreq, &lowGain }, { &mid1Freq, &mid1Gain, &mid1Q },
                               { &mid2Freq, &mid2Gain, &mid2Q }, { &highFreq, &highGain } });
    }
    else
    {
        layoutGroups (knobs.removeFromTop (knobHeight), { { &hpf }, { &lowFreq, &lowGain }, { &highFreq, &highGain } });
        knobs.removeFromTop (4);
        layoutGroups (knobs, { { &mid1Freq, &mid1Gain, &mid1Q }, { &mid2Freq, &mid2Gain, &mid2Q } });
    }
}

void EqModule::paint (juce::Graphics& g)
{
    Module::paint (g);
    g.setColour (theme::colours::border);

    for (const auto& d : dividers)
        g.fillRect (d);
}

//==============================================================================
/** The compressor's static curve: output level against input level. */
class CompModule::TransferGraph final : public juce::Component, private juce::Timer
{
public:
    explicit TransferGraph (juce::AudioProcessorValueTreeState& s) : state (s)   { startTimerHz (15); }

    void attach (params::ChannelId c)
    {
        threshold = state.getParameter (c.param (params::compThreshold));
        ratio = state.getParameter (c.param (params::compRatio));
        enabled = state.getParameter (c.param (params::compOn));
        timerCallback();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        const auto bounds = getLocalBounds().toFloat();
        g.setColour (colours::background);
        g.fillRoundedRectangle (bounds, 4.0f);

        const auto area = bounds.reduced (6.0f);
        auto xFor = [&] (float db) { return juce::jmap (db, -60.0f, 0.0f, area.getX(), area.getRight()); };
        auto yFor = [&] (float db) { return juce::jmap (db, -60.0f, 0.0f, area.getBottom(), area.getY()); };

        g.setColour (colours::border);

        for (const auto db : { -48.0f, -36.0f, -24.0f, -12.0f })
        {
            g.fillRect (juce::Rectangle<float> (xFor (db), area.getY(), 1.0f, area.getHeight()));
            g.fillRect (juce::Rectangle<float> (area.getX(), yFor (db), area.getWidth(), 1.0f));
        }

        g.drawLine ({ xFor (-60.0f), yFor (-60.0f), xFor (0.0f), yFor (0.0f) }, 1.0f);

        juce::Path curve;

        for (auto db = -60.0f; db <= 0.01f; db += 0.5f)
        {
            const auto p = juce::Point<float> (xFor (db), yFor (juce::jmax (-60.0f, Compressor::transfer (db, shownThreshold, shownRatio))));
            curve.isEmpty() ? curve.startNewSubPath (p) : curve.lineTo (p);
        }

        g.setColour (shownOn ? colours::groupCymbals : colours::textMuted);
        g.strokePath (curve, juce::PathStrokeType (2.0f));
    }

private:
    void timerCallback() override
    {
        auto value = [] (juce::RangedAudioParameter* p, float fallback) { return p != nullptr ? p->convertFrom0to1 (p->getValue()) : fallback; };
        const auto t = value (threshold, 0.0f), r = value (ratio, 1.0f);
        const auto on = enabled != nullptr && enabled->getValue() >= 0.5f;

        if (! juce::exactlyEqual (t, shownThreshold) || ! juce::exactlyEqual (r, shownRatio) || on != shownOn)
        {
            shownThreshold = t;
            shownRatio = r;
            shownOn = on;
            repaint();
        }
    }

    juce::AudioProcessorValueTreeState& state;
    juce::RangedAudioParameter *threshold = nullptr, *ratio = nullptr, *enabled = nullptr;
    float shownThreshold = 0.0f, shownRatio = 1.0f;
    bool shownOn = false;
};

CompModule::CompModule (juce::AudioProcessorValueTreeState& s)
    : Module ("Compressor"), state (s), graph (std::make_unique<TransferGraph> (s))
{
    for (auto* k : { &threshold, &ratio, &attack, &release, &makeup, &mix })
    {
        k->setArcColour (theme::colours::groupCymbals);
        addAndMakeVisible (k);
    }

    reset.setTooltip ("Put this channel's compressor back to its defaults");
    reset.onClick = [this] { if (channel.isValid()) params::resetCompressor (state, channel); };

    for (auto* c : std::initializer_list<juce::Component*> { graph.get(), &on, &reset, &meter })
        addAndMakeVisible (c);
}

CompModule::~CompModule() = default;

void CompModule::attach (params::ChannelId c)
{
    channel = c;
    graph->attach (c);
    on.attach (state, c.param (params::compOn));

    const std::pair<Knob*, std::string_view> knobs[] { { &threshold, params::compThreshold }, { &ratio, params::compRatio },
                                                       { &attack, params::compAttack }, { &release, params::compRelease },
                                                       { &makeup, params::compMakeup }, { &mix, params::compMix } };

    for (const auto& [knob, name] : knobs)
        knob->attach (state, c.param (name));
}

void CompModule::resized()
{
    layoutHeaderButtons (&on, &reset);
    auto area = getContentArea();

    meter.setBounds (area.removeFromRight (10).withTrimmedBottom (14));
    area.removeFromRight (8);

    // The curve fills the top, the six knobs in a row under it.
    const auto knobHeight = std::min (70, area.getHeight() / 3);
    auto knobs = area.removeFromBottom (knobHeight);
    area.removeFromBottom (8);
    graph->setBounds (area);

    const auto w = knobs.getWidth() / 6;

    for (auto* k : { &threshold, &ratio, &attack, &release, &makeup, &mix })
        k->setBounds (knobs.removeFromLeft (w));
}

void CompModule::paint (juce::Graphics& g)
{
    Module::paint (g);
    g.setColour (theme::colours::textMuted);
    g.setFont (theme::mono (10.0f));
    g.drawText ("GR", meter.getBounds().withY (meter.getBottom() + 2).withHeight (12).expanded (6, 0), juce::Justification::centred, false);
}

//==============================================================================
TwoKnobModule::TwoKnobModule (juce::AudioProcessorValueTreeState& s, Kind k)
    : Module (k == Kind::transient ? "Transient" : "Saturation"),
      state (s),
      kind (k),
      on ("On", k == Kind::transient ? "Transient shaper on/off" : "Saturation on/off"),
      first (k == Kind::transient ? "Attack" : "Drive"),
      second (k == Kind::transient ? "Sustain" : "Mix")
{
    const auto colour = kind == Kind::transient ? theme::colours::groupHats : theme::colours::groupKick;
    first.setArcColour (colour);
    second.setArcColour (colour);
    first.setBipolar (kind == Kind::transient);
    second.setBipolar (kind == Kind::transient);

    for (auto* c : std::initializer_list<juce::Component*> { &on, &first, &second })
        addAndMakeVisible (c);
}

void TwoKnobModule::attach (params::ChannelId c)
{
    on.attach (state, c.param (kind == Kind::transient ? params::transientOn : params::satOn));
    first.attach (state, c.param (kind == Kind::transient ? params::transientAttack : params::satDrive));
    second.attach (state, c.param (kind == Kind::transient ? params::transientSustain : params::satMix));
}

void TwoKnobModule::resized()
{
    layoutHeaderButtons (&on, nullptr);
    auto area = getContentArea();
    const auto h = std::min (area.getHeight(), 76);
    auto row = area.withSizeKeepingCentre (area.getWidth(), h);
    first.setBounds (row.removeFromLeft (row.getWidth() / 2));
    second.setBounds (row);
}

//==============================================================================
RoutingModule::RoutingModule (juce::AudioProcessorValueTreeState& s)
    : Module ("Channel"), state (s)
{
    pan.setBipolar (true);
    output.setTooltip ("The main mix, or an aux out for mixing in the host");

    for (size_t i = 0; i < sends.size(); ++i)
    {
        sends[i].setArcColour (theme::fxColour ((int) i));
        sends[i].onNameClick = [this, i] { if (onOpenReturn) onOpenReturn ((params::FxReturn) i); };
        addAndMakeVisible (sends[i]);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &pan, &width, &pre, &output })
        addAndMakeVisible (c);
}

void RoutingModule::attach (params::ChannelId c)
{
    const std::array<std::string_view, params::numFxReturns> ids { params::roomSend, params::reverbSend, params::delaySend };

    for (size_t i = 0; i < sends.size(); ++i)
    {
        sends[i].attach (state, c.param (ids[i]));
        sends[i].setTooltip (theme::fxName ((int) i) + " send. Click its name to open the " + theme::fxName ((int) i) + " on the FX page.");
    }

    pre.attach (state, c.param (params::sendPre));
    output.attach (state, c.param (params::output));
    pan.attach (state, c.param ("pan"));
    width.attach (state, c.param (params::channelWidth));
    width.setTooltip ("Stereo width: 0% is mono, 100% leaves the image as it is, up to 200% for twice as wide");
}

void RoutingModule::resized()
{
    using namespace theme;

    auto area = getContentArea();
    output.setBounds (area.removeFromBottom (size::button - 2));
    area.removeFromBottom (20);   // the "OUTPUT" label

    auto top = area.removeFromTop (area.getHeight() / 2);
    pan.setBounds (top.removeFromLeft (top.getWidth() / 2));
    width.setBounds (top);

    // The sends side by side when there's room, else one above the other;
    // the Pre switch under them.
    auto preRow = area.removeFromBottom (size::buttonCompact + 2);
    area.removeFromBottom (space::xs);
    pre.setBounds (preRow.withSizeKeepingCentre (std::min (preRow.getWidth(), 64), preRow.getHeight()));

    constexpr int knobWidth = 46, knobHeight = 66;

    if (area.getWidth() >= 3 * knobWidth)
    {
        auto row = area.withSizeKeepingCentre (area.getWidth(), std::min (area.getHeight(), knobHeight));
        const auto w = row.getWidth() / 3;

        for (auto& k : sends)
            k.setBounds (row.removeFromLeft (w));
    }
    else
    {
        const auto h = std::min (knobHeight, area.getHeight() / 3);
        auto column = area.withSizeKeepingCentre (area.getWidth(), 3 * h);

        for (auto& k : sends)
            k.setBounds (column.removeFromTop (h));
    }
}

void RoutingModule::paint (juce::Graphics& g)
{
    Module::paint (g);
    g.setColour (theme::colours::textMuted);
    theme::drawTrackedText (g, "OUTPUT", theme::label (11.0f), 0.1f, output.getBounds().translated (0, -18).withHeight (16).toFloat(),
                            juce::Justification::centredLeft);
}

} // namespace lithium
