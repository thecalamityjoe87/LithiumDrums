#include "FxPage.h"

#include "Theme.h"

namespace lithium
{

namespace
{
    using FxModule = fxpresets::Module;

    constexpr int toolbarHeight = theme::size::button;
    constexpr int cardHeight = 212, chainHeight = 140;
    constexpr int cardHeaderHeight = 30;

    struct ModuleInfo
    {
        const char* name;
        const char* subtitle;
        juce::Colour colour;
    };

    ModuleInfo infoFor (FxModule m)
    {
        using namespace theme::colours;

        switch (m)
        {
            case FxModule::room:      return { "Room", "Convolution with a recorded or synthesized space", fxRoom };
            case FxModule::plate:     return { "Plate", "Algorithmic reverb, for plates and halls", fxPlate };
            case FxModule::delay:     return { "Delay", "Echoes in time with the host, or free", fxDelay };
            case FxModule::eq:        return { "EQ", "Low shelf, two bells and a high shelf, on the whole mix", accent };
            case FxModule::glue:      return { "Glue", "A bus compressor that holds the kit together", groupCymbals };
            case FxModule::crush:     return { "Crush", "The mix slammed hard and blended in underneath", groupKick };
            case FxModule::tape:      return { "Tape", "Saturation at 4x oversampling, with a tone tilt", groupKick };
            case FxModule::transient: return { "Transient", "More or less attack and ring on the whole kit", groupHats };
            case FxModule::limiter:   return { "Limiter", "A brickwall at the ceiling", groupToms };
        }

        return { "", "", accent };
    }

    juce::String formatMs (double ms)
    {
        return ms >= 1000.0 ? juce::String (ms / 1000.0, 2) + " s" : juce::String (juce::roundToInt (ms)) + " ms";
    }

    /** The parameter's current value, in its own units. */
    float valueOf (juce::AudioProcessorValueTreeState& state, const juce::String& id)
    {
        auto* p = state.getParameter (id);
        return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
    }

    void drawCard (juce::Graphics& g, juce::Rectangle<float> bounds, bool selected, juce::Colour colour)
    {
        using namespace theme;
        g.setColour (selected ? colours::raisedHigh : colours::raised);
        g.fillRoundedRectangle (bounds, 8.0f);
        g.setColour (selected ? colour : colours::border);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, selected ? 1.5f : 1.0f);
    }

    /** A card's header: the colour dot, the name and (if there's room) the subtitle. */
    void drawHeader (juce::Graphics& g, juce::Rectangle<int> area, const ModuleInfo& info, bool withSubtitle)
    {
        using namespace theme;
        auto line = area.toFloat();
        g.setColour (info.colour);
        g.fillEllipse (line.removeFromLeft (10.0f).withSizeKeepingCentre (6.0f, 6.0f));
        line.removeFromLeft (4.0f);

        const auto font = label (type::heading, Weight::semibold);
        const auto name = juce::String (info.name).toUpperCase();
        g.setColour (colours::text);
        drawTrackedText (g, name, font, 0.1f, line, juce::Justification::centredLeft);

        if (withSubtitle)
        {
            line.removeFromLeft (getTrackedTextWidth (name, font, 0.1f) + 10.0f);
            g.setColour (colours::textMuted);
            g.setFont (body (type::small));
            g.drawText (info.subtitle, line, juce::Justification::centredLeft, true);
        }
    }

    /** A gain-reduction readout as a bar growing leftwards from the right, 0 to 18 dB. */
    void drawReductionBar (juce::Graphics& g, juce::Rectangle<float> area, float db, juce::Colour colour)
    {
        using namespace theme;
        g.setColour (colours::background);
        g.fillRoundedRectangle (area, 2.0f);
        const auto w = area.getWidth() * juce::jlimit (0.0f, 1.0f, db / 18.0f);
        g.setColour (colour.withAlpha (0.85f));
        g.fillRoundedRectangle (area.withLeft (area.getRight() - w), 2.0f);
    }
}

//==============================================================================
/** The preset menu of one module: shows the factory preset its settings
    match (or "Custom"), and lists them when clicked. */
class FxPage::PresetButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    /** compact: just the chevron (the chain's tiles), with the preset's name in the tooltip. */
    PresetButton (juce::AudioProcessorValueTreeState& s, FxModule m, bool isCompact = false) : state (s), module (m), compact (isCompact)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        refresh();
    }

    void refresh()
    {
        if (const auto name = fxpresets::currentName (state, module); name != shown || getTooltip().isEmpty())
        {
            shown = name;
            setTooltip (juce::String (infoFor (module).name) + " presets" + (shown.isNotEmpty() ? " (now: " + shown + ")" : juce::String (" (now: custom)")));
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (isMouseOver() ? colours::raisedHigh : colours::panel);
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (isMouseOver() ? colours::borderStrong : colours::border);
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

        auto area = getLocalBounds().reduced (compact ? 0 : 8, 0);
        const auto chevron = (compact ? area : area.removeFromRight (8)).toFloat().withSizeKeepingCentre (8.0f, 5.0f);
        juce::Path p;
        p.startNewSubPath (chevron.getX(), chevron.getY());
        p.lineTo (chevron.getCentreX(), chevron.getBottom());
        p.lineTo (chevron.getRight(), chevron.getY());
        g.setColour (colours::textMuted);
        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (compact)
            return;

        area.removeFromRight (4);
        g.setColour (shown.isNotEmpty() ? colours::text : colours::textMuted);
        g.setFont (body (type::small));
        g.drawText (shown.isNotEmpty() ? shown : juce::String ("Custom"), area, juce::Justification::centredLeft, true);
    }

    void mouseEnter (const juce::MouseEvent&) override    { repaint(); }
    void mouseExit (const juce::MouseEvent&) override     { repaint(); }

    void mouseDown (const juce::MouseEvent&) override
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Presets");
        const auto& presets = fxpresets::factory (module);

        for (size_t i = 0; i < presets.size(); ++i)
            menu.addItem ((int) i + 1, presets[i].name, true, presets[i].name == shown);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (std::max (getWidth(), 150)),
                            [this, safe = juce::Component::SafePointer<PresetButton> (this)] (int result)
                            {
                                if (safe == nullptr || result <= 0)
                                    return;

                                fxpresets::apply (state, fxpresets::factory (module)[(size_t) result - 1]);
                                refresh();
                            });
    }

private:
    juce::AudioProcessorValueTreeState& state;
    const FxModule module;
    const bool compact;
    juce::String shown;
};

//==============================================================================
/** One FX return: its effect's controls, its preset menu and a meter of what it returns. */
class FxPage::ReturnCard final : public juce::Component
{
public:
    ReturnCard (LithiumDrumsProcessor& p, params::FxReturn r)
        : processor (p), which (r), module ((FxModule) r), info (infoFor (module)), presets (p.getParameters(), module)
    {
        using namespace params;
        auto& state = p.getParameters();

        // Where the return goes: the main mix or an aux out (its strip in the
        // Mixer has no room for this menu).
        constexpr std::array<const char*, numFxReturns> outputs { roomOutput, reverbOutput, delayOutput };
        output.attach (state, outputs[(size_t) which]);
        output.setTooltip ("Output: the main mix, or an aux out for mixing in the host");
        addAndMakeVisible (output);

        auto add = [&] (const char* name, const char* id)
        {
            auto& k = knobs.emplace_back (std::make_unique<Knob> (name));
            k->attach (state, id);
            k->setArcColour (info.colour);
            addAndMakeVisible (*k);
        };

        switch (which)
        {
            case FxReturn::room:
                irBox.attach (state, roomIr);
                irBox.setTooltip ("The space: a built-in impulse response, or your own file (Load)");
                load.setTooltip ("Load an impulse response from a WAV, FLAC or AIFF file");
                load.onClick = [this] { chooseFile(); };
                addAndMakeVisible (irBox);
                addAndMakeVisible (load);
                add ("Pre-delay", roomPreDelay);
                add ("Decay", roomDecay);
                add ("Low cut", roomLowCut);
                add ("High cut", roomHighCut);
                add ("Width", roomWidth);
                add ("Mix", roomMix);
                break;

            case FxReturn::plate:
                add ("Size", reverbSize);
                add ("Decay", reverbDecay);
                add ("Pre-delay", reverbPreDelay);
                add ("Damping", reverbDamping);
                add ("Low cut", reverbLowCut);
                add ("Width", reverbWidth);
                add ("Mix", reverbMix);
                break;

            case FxReturn::delay:
                sync.attach (state, delaySync);
                note.attach (state, delayNote);
                pingPong.attach (state, delayPingPong);
                note.setTooltip ("The echo's note value, at the host's tempo");

                for (auto* c : std::initializer_list<juce::Component*> { &sync, &note, &pingPong })
                    addAndMakeVisible (c);

                add ("Time", delayTime);
                add ("Feedback", delayFeedback);
                add ("Low cut", delayLowCut);
                add ("High cut", delayHighCut);
                add ("Mix", delayMix);
                break;
        }

        addAndMakeVisible (presets);
    }

    std::function<void()> onSelect;

    void setSelected (bool s)                { if (s != selected) { selected = s; repaint(); } }

    /** What the return sends to the mix now (a linear peak), for its meter. */
    void pushLevel (float peak)
    {
        // A fast rise and a slower fall, in dB, like the mixer's meters.
        const auto db = juce::Decibels::gainToDecibels (peak, -60.0f);
        levelDb = db > levelDb ? db : std::max (-60.0f, levelDb - 1.5f);

        if (std::abs (levelDb - shownDb) > 0.25f)
        {
            shownDb = levelDb;
            repaint (meterArea);
        }
    }

    void refresh()
    {
        presets.refresh();

        if (which == params::FxReturn::delay)
        {
            // Synced, the time knob gives way to the note value.
            const auto synced = sync.getToggleState();
            note.setEnabled (synced);
            knobs.front()->setEnabled (! synced);
        }
    }

    void paint (juce::Graphics& g) override
    {
        drawCard (g, getLocalBounds().toFloat(), selected, info.colour);
        drawHeader (g, getHeaderArea(), info, false);

        using namespace theme;
        g.setColour (colours::textMuted);
        g.setFont (body (type::small));
        g.drawText (info.subtitle, subtitleArea, juce::Justification::centredLeft, true);

        // The return's level, beside its name.
        const auto meter = meterArea.toFloat();
        g.setColour (colours::background);
        g.fillRoundedRectangle (meter, 2.0f);
        g.setColour (info.colour.withAlpha (0.9f));
        g.fillRoundedRectangle (meter.withWidth (meter.getWidth() * juce::jmap (shownDb, -60.0f, 0.0f, 0.0f, 1.0f)), 2.0f);

        auto& state = processor.getParameters();

        if (which == params::FxReturn::delay)
        {
            // The echo time in milliseconds, as it plays now.
            const auto synced = valueOf (state, params::delaySync) >= 0.5f;
            const auto ms = synced ? TempoDelay::syncedMs (juce::roundToInt (valueOf (state, params::delayNote)), processor.getHostBpm())
                                   : valueOf (state, params::delayTime);
            g.setColour (colours::text);
            g.setFont (mono (type::small));
            g.drawText (formatMs (ms), readoutArea, juce::Justification::centredRight, true);
        }
        else if (which == params::FxReturn::plate)
        {
            // A small picture of the decay: silence for the pre-delay, then 60 dB over the decay time.
            const auto area = controlsArea.toFloat().reduced (0.0f, 2.0f);
            g.setColour (colours::background);
            g.fillRoundedRectangle (area, 4.0f);
            const auto decay = valueOf (state, params::reverbDecay), preDelay = valueOf (state, params::reverbPreDelay) / 1000.0f;
            constexpr float span = 8.0f;
            const auto plot = area.reduced (6.0f, 4.0f);
            auto xFor = [&] (float t) { return plot.getX() + plot.getWidth() * std::min (1.0f, t / span); };
            juce::Path env;
            env.startNewSubPath (plot.getX(), plot.getBottom());
            env.lineTo (xFor (preDelay), plot.getBottom());
            env.lineTo (xFor (preDelay), plot.getY());
            env.lineTo (xFor (preDelay + decay), plot.getBottom());
            env.closeSubPath();
            g.setColour (info.colour.withAlpha (0.25f));
            g.fillPath (env);
            g.setColour (info.colour);
            g.drawLine ({ xFor (preDelay), plot.getY(), xFor (preDelay + decay), plot.getBottom() }, 1.5f);
            g.setColour (colours::textMuted);
            g.setFont (mono (10.5f));
            g.drawText ("RT60 " + juce::String (decay, 2) + " s", plot, juce::Justification::centredRight, false);
        }
    }

    void resized() override
    {
        using namespace theme;
        auto header = getHeaderArea();
        presets.setBounds (header.removeFromRight (140).withSizeKeepingCentre (140, size::button - 6));
        header.removeFromRight (space::sm);
        output.setBounds (header.removeFromRight (96).withSizeKeepingCentre (96, size::button - 6));
        header.removeFromRight (space::md);
        const auto meterWidth = juce::jlimit (24, 72, header.getWidth() - 90);   // what's left after the title's room
        meterArea = header.removeFromRight (meterWidth).withSizeKeepingCentre (meterWidth, 4);

        auto area = getLocalBounds().reduced (space::md).withTrimmedTop (cardHeaderHeight + space::xs);
        subtitleArea = area.removeFromTop (18);
        area.removeFromTop (space::xs);
        auto controls = area.removeFromTop (size::button);
        controlsArea = controls;
        area.removeFromTop (space::md);

        switch (which)
        {
            case params::FxReturn::room:
                load.setBounds (controls.removeFromRight (76));
                controls.removeFromRight (space::sm);
                irBox.setBounds (controls);
                break;

            case params::FxReturn::delay:
                sync.setBounds (controls.removeFromLeft (56));
                controls.removeFromLeft (space::sm);
                note.setBounds (controls.removeFromLeft (120));
                controls.removeFromLeft (space::sm);
                pingPong.setBounds (controls.removeFromLeft (92));
                controls.removeFromLeft (space::sm);
                readoutArea = controls;
                break;

            case params::FxReturn::plate:
                break;
        }

        // The knobs fill the rest, in a row.
        auto row = area.withSizeKeepingCentre (area.getWidth(), std::min (area.getHeight(), 70));
        const auto w = row.getWidth() / (int) knobs.size();

        for (auto& k : knobs)
            k->setBounds (row.removeFromLeft (w));
    }

    void mouseDown (const juce::MouseEvent&) override    { if (onSelect) onSelect(); }

private:
    juce::Rectangle<int> getHeaderArea() const
    {
        return getLocalBounds().reduced (theme::space::md, 0).removeFromTop (cardHeaderHeight + theme::space::xs).withTrimmedTop (theme::space::xs);
    }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Load an impulse response", processor.getRoomIrFile(), irs::fileWildcard);

        // The chooser must outlive the async dialog: it's a member, and the
        // callback only runs while this card exists (SafePointer).
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, safe = juce::Component::SafePointer<ReturnCard> (this)] (const juce::FileChooser& fc)
                              {
                                  if (safe != nullptr && fc.getResult() != juce::File())
                                  {
                                      processor.setRoomIrFile (fc.getResult());

                                      if (onSelect)
                                          onSelect();
                                  }
                              });
    }

    LithiumDrumsProcessor& processor;
    const params::FxReturn which;
    const FxModule module;
    const ModuleInfo info;
    PresetButton presets;
    ParamCombo output;
    std::vector<std::unique_ptr<Knob>> knobs;
    ParamCombo irBox, note;
    juce::TextButton load { "Load..." };
    ParamToggle sync { "Sync", "Echoes in time with the host's tempo" }, pingPong { "Ping-pong", "Repeats bounce between left and right" };
    juce::Rectangle<int> subtitleArea, readoutArea, controlsArea, meterArea;
    float levelDb = -60.0f, shownDb = -60.0f;
    std::unique_ptr<juce::FileChooser> chooser;
    bool selected = false;
};

//==============================================================================
/** One module of the master chain: its On switch, main controls and preset menu. */
class FxPage::ChainTile final : public juce::Component
{
public:
    ChainTile (juce::AudioProcessorValueTreeState& state, FxModule m)
        : module (m), info (infoFor (m)), presets (state, m, true)
    {
        using namespace params;
        const auto master = ChannelId::master();

        auto add = [&] (const char* name, const juce::String& id, bool bipolar = false)
        {
            auto& k = knobs.emplace_back (std::make_unique<Knob> (name));
            k->attach (state, id);
            k->setArcColour (info.colour);
            k->setBipolar (bipolar);
            addAndMakeVisible (*k);
        };

        juce::String onId;

        switch (module)
        {
            case FxModule::eq:
                onId = master.param (eqOn);
                add ("Low", master.param (eqLowGain), true);
                add ("Mid", master.param (eqMid1Gain), true);
                add ("High", master.param (eqHighGain), true);
                break;

            case FxModule::glue:
                onId = master.param (compOn);
                add ("Thresh", master.param (compThreshold));
                add ("Ratio", master.param (compRatio));
                add ("Makeup", master.param (compMakeup));
                break;

            case FxModule::crush:
                onId = crushOn;
                add ("Drive", crushDrive);
                add ("Blend", crushBlend);
                break;

            case FxModule::tape:
                onId = tapeOn;
                add ("Drive", tapeDrive);
                add ("Tone", tapeTone, true);
                add ("Output", tapeOutput, true);
                break;

            case FxModule::transient:
                onId = master.param (transientOn);
                add ("Attack", master.param (transientAttack), true);
                add ("Sustain", master.param (transientSustain), true);
                break;

            case FxModule::limiter:
                onId = limitOn;
                add ("Ceiling", limitCeiling);
                add ("Release", limitRelease);
                break;

            case FxModule::room:
            case FxModule::plate:
            case FxModule::delay:
                jassertfalse;
                break;
        }

        on.attach (state, onId);
        on.setTooltip (juce::String (info.name) + " on/off");
        addAndMakeVisible (on);
        addAndMakeVisible (presets);
    }

    std::function<void()> onSelect;

    bool showsReduction() const noexcept     { return module == FxModule::glue || module == FxModule::crush || module == FxModule::limiter; }

    void setSelected (bool s)                { if (s != selected) { selected = s; repaint(); } }
    void refresh()                           { presets.refresh(); }

    void pushReduction (float db)
    {
        // A fast attack and a slower fall, like the channels' meters.
        reductionDb = db > reductionDb ? db : reductionDb + 0.25f * (db - reductionDb);

        if (std::abs (reductionDb - shownDb) > 0.05f)
        {
            shownDb = reductionDb;
            repaint (reductionArea);
        }
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto active = on.getToggleState();
        drawCard (g, getLocalBounds().toFloat(), selected, info.colour);

        // The name dims while the stage is bypassed.
        auto header = getLocalBounds().reduced (space::sm + 2, 0).removeFromTop (cardHeaderHeight).translated (0, 2);
        header.removeFromLeft (on.getWidth() + space::xs);
        g.setColour (active ? info.colour : colours::textMuted.withAlpha (0.5f));
        g.fillEllipse (header.removeFromLeft (10).toFloat().withSizeKeepingCentre (6.0f, 6.0f));
        header.removeFromLeft (3);
        g.setColour (active ? colours::text : colours::textMuted);
        drawTrackedText (g, juce::String (info.name).toUpperCase(), label (type::body, Weight::semibold), 0.1f, header.toFloat(),
                         juce::Justification::centredLeft);

        if (showsReduction())
        {
            const auto bar = reductionArea.toFloat();
            drawReductionBar (g, bar.withTrimmedRight (46.0f), shownDb, info.colour);
            g.setColour (colours::textMuted);
            g.setFont (mono (10.0f));
            g.drawText ("GR " + juce::String (shownDb >= 0.05f ? -shownDb : 0.0f, 1), bar.withLeft (bar.getRight() - 44.0f),
                        juce::Justification::centredRight, false);
        }
    }

    void resized() override
    {
        using namespace theme;
        auto header = getLocalBounds().reduced (space::sm, 0).removeFromTop (cardHeaderHeight).translated (0, 2);
        on.setBounds (header.removeFromLeft (30).withSizeKeepingCentre (30, size::buttonCompact));
        presets.setBounds (header.removeFromRight (size::buttonCompact + 6).withSizeKeepingCentre (size::buttonCompact + 6, size::buttonCompact + 2));

        auto area = getLocalBounds().reduced (space::xs).withTrimmedTop (cardHeaderHeight + space::xs);

        if (showsReduction())
        {
            reductionArea = area.removeFromBottom (12).reduced (space::xs, 2);
            area.removeFromBottom (space::xxs);
        }

        auto row = area.withSizeKeepingCentre (area.getWidth(), std::min (area.getHeight(), 68));
        const auto w = row.getWidth() / 3;
        auto x = row.withSizeKeepingCentre (w * (int) knobs.size(), row.getHeight());

        for (auto& k : knobs)
            k->setBounds (x.removeFromLeft (w));
    }

    void mouseDown (const juce::MouseEvent&) override    { if (onSelect) onSelect(); }

    const FxModule module;

private:
    const ModuleInfo info;
    ParamToggle on { "On" };
    PresetButton presets;
    std::vector<std::unique_ptr<Knob>> knobs;
    juce::Rectangle<int> reductionArea;
    float reductionDb = 0.0f, shownDb = 0.0f;
    bool selected = false;
};

//==============================================================================
/** The selected module in full: a card per module, one shown at a time. */
class FxPage::Detail final : public juce::Component, private juce::Timer
{
public:
    explicit Detail (LithiumDrumsProcessor& p)
        : processor (p), state (p.getParameters()),
          eq (state), glue (state), transient (state, TwoKnobModule::Kind::transient)
    {
        const auto master = params::ChannelId::master();
        eq.attach (master);
        eq.setTitle ("Master EQ");
        glue.attach (master);
        glue.setTitle ("Glue compressor");
        transient.attach (master);
        transient.setTitle ("Transient shaper");

        auto addKnobs = [&] (Module& card, std::vector<std::unique_ptr<Knob>>& list,
                             std::initializer_list<std::pair<const char*, const char*>> knobs, juce::Colour colour)
        {
            for (const auto& [name, id] : knobs)
            {
                auto& k = list.emplace_back (std::make_unique<Knob> (name));
                k->attach (state, id);
                k->setArcColour (colour);
                card.addAndMakeVisible (*k);
            }
        };

        addKnobs (crush, crushKnobs, { { "Drive", params::crushDrive }, { "Blend", params::crushBlend },
                                       { "Attack", params::crushAttack }, { "Release", params::crushRelease } }, theme::colours::groupKick);
        addKnobs (tape, tapeKnobs, { { "Drive", params::tapeDrive }, { "Tone", params::tapeTone }, { "Output", params::tapeOutput } },
                  theme::colours::groupKick);
        addKnobs (limiter, limiterKnobs, { { "Ceiling", params::limitCeiling }, { "Release", params::limitRelease } },
                  theme::colours::groupToms);

        crushOn.attach (state, params::crushOn);
        tapeOn.attach (state, params::tapeOn);
        limiterOn.attach (state, params::limitOn);
        lookahead.attach (state, params::limitLookahead);
        crush.addAndMakeVisible (crushOn);
        crush.addAndMakeVisible (crushMeter);
        tape.addAndMakeVisible (tapeOn);
        limiter.addAndMakeVisible (limiterOn);
        limiter.addAndMakeVisible (lookahead);
        limiter.addAndMakeVisible (limiterMeter);

        loadIr.setTooltip ("Load an impulse response from a WAV, FLAC or AIFF file");
        loadIr.onClick = [this] { chooseFile(); };
        room.addAndMakeVisible (loadIr);

        for (auto* c : std::initializer_list<juce::Component*> { &eq, &glue, &glueAbout, &crush, &tape, &transient, &transientAbout, &limiter, &room, &plate, &delay })
            addChildComponent (c);

        startTimerHz (15);
    }

    void show (FxModule m)
    {
        shown = m;
        const std::array<juce::Component*, fxpresets::numModules> cards { &room, &plate, &delay, &eq, &glue, &crush, &tape, &transient, &limiter };

        for (size_t i = 0; i < cards.size(); ++i)
            cards[i]->setVisible ((int) i == (int) m);

        glueAbout.setVisible (m == FxModule::glue);
        transientAbout.setVisible (m == FxModule::transient);
        eq.setSampleRate (processor.getSampleRate());
        repaint();
    }

    void pushReductions (float crushDb, float limiterDb)
    {
        crushMeter.push (crushDb);
        limiterMeter.push (limiterDb);
    }

    void pushGlueReduction (float db)        { glue.pushReduction (db); }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto info = infoFor (shown);
        auto header = getLocalBounds().removeFromTop (cardHeaderHeight).toFloat();
        g.setColour (colours::text);
        const auto titleFont = label (type::title, Weight::bold);
        const auto title = juce::String (info.name).toUpperCase();
        drawTrackedText (g, title, titleFont, 0.08f, header, juce::Justification::centredLeft);
        header.removeFromLeft (getTrackedTextWidth (title, titleFont, 0.08f) + 14.0f);
        g.setColour (colours::textMuted);
        g.setFont (body (type::body));
        g.drawText (info.subtitle, header.translated (0.0f, 1.0f), juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        using namespace theme;
        const auto area = getLocalBounds().withTrimmedTop (cardHeaderHeight + space::xs);

        for (auto* c : std::initializer_list<juce::Component*> { &eq, &crush, &tape, &limiter, &room, &plate, &delay })
            c->setBounds (area);

        // The glue compressor and the transient shaper are cards of their own, with what they do in a card beside them.
        {
            auto glueArea = area;
            glue.setBounds (glueArea.removeFromLeft (660));
            glueArea.removeFromLeft (space::lg);
            glueAbout.setBounds (glueArea);

            auto transientArea = area;
            transient.setBounds (transientArea.removeFromLeft (300));
            transientArea.removeFromLeft (space::lg);
            transientAbout.setBounds (transientArea);
        }

        // The knob cards: a graph (or the meter) on the left, the knobs right.
        auto layoutKnobs = [] (std::vector<std::unique_ptr<Knob>>& knobs, juce::Rectangle<int> row)
        {
            row = row.withSizeKeepingCentre (row.getWidth(), std::min (row.getHeight(), 76));
            const auto w = std::min (90, row.getWidth() / (int) knobs.size());

            for (auto& k : knobs)
                k->setBounds (row.removeFromLeft (w));
        };

        auto content = [] (juce::Component& c) { return c.getLocalBounds().reduced (space::md).withTrimmedTop (cardHeaderHeight); };
        auto onButton = [] (juce::Component& card, juce::Component& button)
        {
            button.setBounds (card.getLocalBounds().reduced (space::md - 2, 0).removeFromTop (cardHeaderHeight).removeFromRight (38)
                                  .withSizeKeepingCentre (38, 20).translated (0, 2));
        };

        {
            auto c = content (crush);
            onButton (crush, crushOn);
            crushMeter.setBounds (c.removeFromRight (10).withTrimmedBottom (14));
            c.removeFromRight (space::lg);
            controlsBox[(size_t) FxModule::crush] = c.removeFromLeft (std::min (420, c.getWidth() / 2));
            layoutKnobs (crushKnobs, controlsBox[(size_t) FxModule::crush].reduced (space::md));
            c.removeFromLeft (space::lg);
            textArea[(size_t) FxModule::crush] = c;
        }

        {
            auto c = content (tape);
            onButton (tape, tapeOn);
            graphArea[(size_t) FxModule::tape] = c.removeFromLeft (std::min (c.getHeight() + 80, c.getWidth() / 3));
            c.removeFromLeft (space::xl);
            controlsBox[(size_t) FxModule::tape] = c.removeFromLeft (3 * 90 + 2 * space::md);
            layoutKnobs (tapeKnobs, controlsBox[(size_t) FxModule::tape].reduced (space::md));
            c.removeFromLeft (space::xl);
            textArea[(size_t) FxModule::tape] = c;
        }

        {
            auto c = content (limiter);
            onButton (limiter, limiterOn);
            limiterMeter.setBounds (c.removeFromRight (10).withTrimmedBottom (14));
            c.removeFromRight (space::lg);
            controlsBox[(size_t) FxModule::limiter] = c.removeFromLeft (std::min (300, c.getWidth() / 2));
            auto inner = controlsBox[(size_t) FxModule::limiter].reduced (space::md);
            lookahead.setBounds (inner.removeFromBottom (size::button).withSizeKeepingCentre (std::min (inner.getWidth(), 130), size::button));
            inner.removeFromBottom (space::md);
            layoutKnobs (limiterKnobs, inner);
            c.removeFromLeft (space::lg);
            textArea[(size_t) FxModule::limiter] = c;
        }

        {
            auto c = content (room);
            loadIr.setBounds (room.getLocalBounds().reduced (space::md - 2, 0).removeFromTop (cardHeaderHeight).removeFromRight (110)
                                  .withSizeKeepingCentre (110, 22).translated (0, 2));
            textArea[(size_t) FxModule::room] = c.removeFromRight (c.getWidth() / 3);
            c.removeFromRight (space::lg);
            graphArea[(size_t) FxModule::room] = c;
        }

        for (auto* m : { &plate, &delay })
        {
            auto c = content (*m);
            const auto index = m == &plate ? FxModule::plate : FxModule::delay;
            textArea[(size_t) index] = c.removeFromRight (c.getWidth() / 3);
            c.removeFromRight (space::lg);
            graphArea[(size_t) index] = c;
        }
    }

private:
    /** A card that draws a graph and some text, through the Detail. */
    class GraphCard final : public Module
    {
    public:
        GraphCard (juce::String t, std::function<void (juce::Graphics&)> draw) : Module (std::move (t)), drawContent (std::move (draw)) {}

        void paint (juce::Graphics& g) override
        {
            Module::paint (g);
            drawContent (g);
        }

    private:
        std::function<void (juce::Graphics&)> drawContent;
    };

    void timerCallback() override
    {
        // Repaint what's showing when what it draws changes.
        juce::String key;
        auto& s = state;

        switch (shown)
        {
            case FxModule::room:
            {
                const auto status = processor.getIrLoader().getStatus();
                key << status.serial << (status.loading ? "l" : "") << valueOf (s, params::roomPreDelay);
                break;
            }

            case FxModule::plate:  key << valueOf (s, params::reverbDecay) << valueOf (s, params::reverbPreDelay) << valueOf (s, params::reverbSize); break;
            case FxModule::delay:  key << valueOf (s, params::delaySync) << valueOf (s, params::delayNote) << valueOf (s, params::delayTime)
                                     << valueOf (s, params::delayFeedback) << valueOf (s, params::delayPingPong) << processor.getHostBpm(); break;
            case FxModule::tape:   key << valueOf (s, params::tapeDrive) << valueOf (s, params::tapeOn); break;
            case FxModule::limiter: key << valueOf (s, params::limitLookahead) << processor.getLatencySamples(); break;
            case FxModule::eq:
            case FxModule::glue:
            case FxModule::crush:
            case FxModule::transient: break;
        }

        if (key != shownKey)
        {
            shownKey = key;

            for (auto* c : std::initializer_list<juce::Component*> { &room, &plate, &delay, &tape, &limiter })
                if (c->isVisible())
                    c->repaint();
        }
    }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Load an impulse response", processor.getRoomIrFile(), irs::fileWildcard);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, safe = juce::Component::SafePointer<Detail> (this)] (const juce::FileChooser& fc)
                              {
                                  if (safe != nullptr && fc.getResult() != juce::File())
                                      processor.setRoomIrFile (fc.getResult());
                              });
    }

    static void drawText (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text)
    {
        g.setColour (theme::colours::textMuted);
        g.setFont (theme::body (theme::type::small));
        g.drawFittedText (text, area, juce::Justification::topLeft, 12, 1.0f);
    }

    /** The panel a card's controls sit in, apart from its graph and text. */
    static void drawControlsBox (juce::Graphics& g, juce::Rectangle<int> area)
    {
        g.setColour (theme::colours::background);
        g.fillRoundedRectangle (area.toFloat(), 6.0f);
        g.setColour (theme::colours::border);
        g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    /** A card of words: what a module does and how to use it. */
    void drawAbout (juce::Graphics& g, juce::Component& card, const juce::String& text)
    {
        drawText (g, card.getLocalBounds().reduced (theme::space::md).withTrimmedTop (cardHeaderHeight), text);
    }

    // Each graph's background and grid.
    static juce::Rectangle<float> graphBackground (juce::Graphics& g, juce::Rectangle<int> area)
    {
        g.setColour (theme::colours::background);
        g.fillRoundedRectangle (area.toFloat(), 4.0f);
        return area.toFloat().reduced (10.0f, 8.0f).withTrimmedBottom (12.0f);
    }

    void drawRoom (juce::Graphics& g)
    {
        using namespace theme;
        const auto status = processor.getIrLoader().getStatus();
        const auto area = graphBackground (g, graphArea[(size_t) FxModule::room]);
        const auto preDelay = valueOf (state, params::roomPreDelay) / 1000.0f;
        const auto seconds = std::max (1.0f, std::ceil ((status.seconds + preDelay) * 1.1f * 2.0f) / 2.0f);
        auto xFor = [&] (float t) { return juce::jmap (t, 0.0f, seconds, area.getX(), area.getRight()); };
        auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (-60.0f, 0.0f, db), 0.0f, -60.0f, area.getY(), area.getBottom()); };

        g.setFont (mono (10.0f));

        for (auto t = 0.0f; t <= seconds + 0.01f; t += seconds > 3.0f ? 1.0f : 0.5f)
        {
            g.setColour (colours::border);
            g.fillRect (juce::Rectangle<float> (xFor (t), area.getY(), 1.0f, area.getHeight()));
            g.setColour (colours::textMuted);
            g.drawText (juce::String (t, juce::exactlyEqual (t, std::floor (t)) ? 0 : 1) + " s", juce::Rectangle<float> (xFor (t) - 20.0f, area.getBottom() + 1.0f, 40.0f, 12.0f),
                        juce::Justification::centred);
        }

        // The IR's envelope, in dB, after the pre-delay.
        if (! status.thumbnail.empty())
        {
            juce::Path env;
            env.startNewSubPath (xFor (preDelay), area.getBottom());
            const auto n = (int) status.thumbnail.size();

            for (int i = 0; i < n; ++i)
                env.lineTo (xFor (preDelay + status.seconds * (float) i / (float) n),
                            yFor (juce::Decibels::gainToDecibels (status.thumbnail[(size_t) i], -60.0f)));

            env.lineTo (xFor (preDelay + status.seconds), area.getBottom());
            env.closeSubPath();
            g.setColour (colours::fxRoom.withAlpha (0.25f));
            g.fillPath (env);
            g.setColour (colours::fxRoom);
            g.strokePath (env, juce::PathStrokeType (1.2f));
        }

        g.setColour (colours::text);
        g.setFont (mono (11.0f));
        g.drawText (status.loading ? juce::String ("Loading...")
                                   : "Length " + juce::String (status.seconds, 2) + " s   RT60 " + juce::String (status.rt60, 2) + " s",
                    area.reduced (6.0f), juce::Justification::topRight, false);

        // What's playing, and whose it is.
        auto text = textArea[(size_t) FxModule::room];
        g.setColour (colours::text);
        g.setFont (body (type::body, Weight::semibold));
        g.drawText (status.name, text.removeFromTop (20), juce::Justification::centredLeft, true);
        text.removeFromTop (4);
        juce::String about;
        const auto choice = juce::roundToInt (valueOf (state, params::roomIr));

        if (juce::isPositiveAndBelow (choice, (int) irs::builtIn().size()))
        {
            const auto& ir = irs::builtIn()[(size_t) choice];
            about << ir.description << "\n\n" << ir.author << ". " << ir.licenseName << ".";

            if (ir.changes.isNotEmpty())
                about << " Changes: " << ir.changes;
        }
        else
        {
            about << (processor.getRoomIrFile() == juce::File() ? juce::String ("Choose a file with Load IR file.")
                                                                : processor.getRoomIrFile().getFullPathName());
        }

        if (status.error.isNotEmpty())
        {
            g.setColour (colours::warning);
            g.setFont (body (type::small));
            g.drawFittedText (status.error, text.removeFromTop (34), juce::Justification::topLeft, 2, 1.0f);
        }

        drawText (g, text, about + "\n\nDecay shortens the IR's tail without changing its start, which also makes it cheaper to run.");
    }

    void drawPlate (juce::Graphics& g)
    {
        using namespace theme;
        const auto area = graphBackground (g, graphArea[(size_t) FxModule::plate]);
        const auto decay = valueOf (state, params::reverbDecay);
        const auto preDelay = valueOf (state, params::reverbPreDelay) / 1000.0f;
        const auto seconds = std::max (1.0f, std::ceil ((decay + preDelay) * 1.25f));
        auto xFor = [&] (float s) { return juce::jmap (s, 0.0f, seconds, area.getX(), area.getRight()); };
        auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (-60.0f, 0.0f, db), 0.0f, -60.0f, area.getY(), area.getBottom()); };

        g.setFont (mono (10.0f));

        for (int s = 0; s <= (int) seconds; ++s)
        {
            g.setColour (colours::border);
            g.fillRect (juce::Rectangle<float> (xFor ((float) s), area.getY(), 1.0f, area.getHeight()));
            g.setColour (colours::textMuted);
            g.drawText (juce::String (s) + " s", juce::Rectangle<float> (xFor ((float) s) - 20.0f, area.getBottom() + 1.0f, 40.0f, 12.0f),
                        juce::Justification::centred);
        }

        juce::Path env;
        env.startNewSubPath (xFor (0.0f), area.getBottom());
        env.lineTo (xFor (preDelay), area.getBottom());
        env.lineTo (xFor (preDelay), yFor (0.0f));
        env.lineTo (xFor (preDelay + decay), yFor (-60.0f));
        env.closeSubPath();
        g.setColour (colours::fxPlate.withAlpha (0.18f));
        g.fillPath (env);
        g.setColour (colours::fxPlate);
        g.drawLine ({ xFor (preDelay), yFor (0.0f), xFor (preDelay + decay), yFor (-60.0f) }, 2.0f);

        g.setColour (colours::text);
        g.setFont (mono (11.0f));
        g.drawText ("RT60 " + juce::String (decay, 2) + " s", area.reduced (6.0f), juce::Justification::topRight, false);

        drawText (g, textArea[(size_t) FxModule::plate],
                  "A feedback delay network: the send is diffused, then circulates through eight delay lines of unrelated "
                  "lengths, mixed by a Hadamard matrix on every pass and damped so it falls 60 dB in the decay time. "
                  "Slow modulation keeps cymbal tails from ringing metallically.\n\n"
                  "Small sizes and short decays make rooms; large ones halls. Damping darkens the tail; Low cut keeps kicks out of it.\n\n"
                  "This was the mixer's reverb before the FX page, so older projects' reverb sends come here.");
    }

    void drawDelay (juce::Graphics& g)
    {
        using namespace theme;
        const auto area = graphBackground (g, graphArea[(size_t) FxModule::delay]);
        const auto synced = valueOf (state, params::delaySync) >= 0.5f;
        const auto ms = synced ? TempoDelay::syncedMs (juce::roundToInt (valueOf (state, params::delayNote)), processor.getHostBpm())
                               : valueOf (state, params::delayTime);
        const auto feedback = valueOf (state, params::delayFeedback) / 100.0f;
        const auto pingPong = valueOf (state, params::delayPingPong) >= 0.5f;
        const auto mid = area.getCentreY();

        // Eight echoes' worth of time, the repeats as bars: left above the
        // line, right below; ping-pong alternates them.
        const auto span = ms * 8.0f;
        auto xFor = [&] (float t) { return juce::jmap (t, 0.0f, span, area.getX() + 4.0f, area.getRight() - 4.0f); };
        g.setColour (colours::border);
        g.fillRect (juce::Rectangle<float> (area.getX(), mid, area.getWidth(), 1.0f));

        for (int k = 0; k < 8; ++k)
        {
            const auto level = std::pow (feedback, (float) k);
            const auto db = juce::Decibels::gainToDecibels (level, -60.0f);
            const auto h = (area.getHeight() * 0.5f - 6.0f) * juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f);

            if (h < 0.5f)
                break;

            const auto x = xFor (ms * (float) (k + 1));
            g.setColour (colours::fxDelay.withAlpha (k == 0 ? 1.0f : 0.75f));

            if (! pingPong || k % 2 == 0)
                g.fillRect (juce::Rectangle<float> (x - 2.0f, mid - h, 4.0f, h));

            if (! pingPong || k % 2 == 1)
                g.fillRect (juce::Rectangle<float> (x - 2.0f, mid + 1.0f, 4.0f, h));
        }

        g.setColour (colours::textMuted);
        g.setFont (mono (10.0f));
        g.drawText ("L", area.withHeight (14.0f), juce::Justification::topLeft, false);
        g.drawText ("R", area.withTop (area.getBottom() - 14.0f), juce::Justification::bottomLeft, false);
        g.setColour (colours::text);
        g.setFont (mono (11.0f));
        g.drawText (formatMs (ms) + (synced ? "  (" + juce::String (processor.getHostBpm(), 1) + " bpm)" : juce::String()),
                    area.reduced (6.0f), juce::Justification::topRight, false);

        drawText (g, textArea[(size_t) FxModule::delay],
                  "Synced, the echo follows the host's tempo: straight, dotted or triplet note values. Off, Time sets it in "
                  "milliseconds.\n\nEvery repeat goes through Low cut and High cut again, so they thin out and darken as they "
                  "fade. Feedback near the top saturates gently rather than running away. Ping-pong bounces the repeats "
                  "between left and right.");
    }

    void drawTape (juce::Graphics& g)
    {
        using namespace theme;
        drawControlsBox (g, controlsBox[(size_t) FxModule::tape]);
        const auto area = graphBackground (g, graphArea[(size_t) FxModule::tape]).withTrimmedBottom (-12.0f);
        const auto drive = juce::Decibels::decibelsToGain (valueOf (state, params::tapeDrive));
        const auto on = valueOf (state, params::tapeOn) >= 0.5f;
        auto xFor = [&] (float x) { return juce::jmap (x, -1.5f, 1.5f, area.getX(), area.getRight()); };
        auto yFor = [&] (float y) { return juce::jmap (y, -1.5f, 1.5f, area.getBottom(), area.getY()); };

        g.setColour (colours::border);
        g.fillRect (juce::Rectangle<float> (xFor (0.0f), area.getY(), 1.0f, area.getHeight()));
        g.fillRect (juce::Rectangle<float> (area.getX(), yFor (0.0f), area.getWidth(), 1.0f));
        g.drawLine ({ xFor (-1.5f), yFor (-1.5f), xFor (1.5f), yFor (1.5f) }, 1.0f);

        // The curve as the tape stage applies it: off centre, then back to the input's level.
        juce::Path curve;
        const auto zero = Saturator::shape (0.18f);

        for (auto x = -1.5f; x <= 1.5f; x += 0.02f)
        {
            const auto y = (Saturator::shape (x * drive + 0.18f) - zero) / drive;
            const auto p = juce::Point<float> (xFor (x), yFor (juce::jlimit (-1.5f, 1.5f, y)));
            curve.isEmpty() ? curve.startNewSubPath (p) : curve.lineTo (p);
        }

        g.setColour (on ? colours::groupKick : colours::textMuted);
        g.strokePath (curve, juce::PathStrokeType (2.0f));

        drawText (g, textArea[(size_t) FxModule::tape],
                  "Drive pushes the mix into a soft curve that sits a little off centre, so it rounds one side of the wave more "
                  "than the other, as tape does. Quiet parts pass at their own level; only the peaks are shaped.\n\n"
                  "It runs at four times the sample rate (with polyphase IIR filters), so its harmonics don't fold back down "
                  "as aliasing. Tone tilts the result darker or brighter around 1 kHz.");
    }

    void drawLimiter (juce::Graphics& g)
    {
        drawControlsBox (g, controlsBox[(size_t) FxModule::limiter]);
        const auto latency = processor.getLatencySamples();
        const auto rate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
        drawText (g, textArea[(size_t) FxModule::limiter],
                  juce::String ("Nothing leaves the mix above the ceiling.\n\nWithout lookahead the gain drops the instant a peak "
                                "would cross it, which can be heard on the hardest hits. With lookahead the limiter sees "
                                "2 ms ahead and eases the gain down to meet each peak, without distortion. That delays the "
                                "plugin's output, which it reports to the host so the track stays in time.\n\n")
                      + (latency > 0 ? "Latency reported now: " + juce::String (latency) + " samples ("
                                           + juce::String (1000.0 * latency / rate, 1) + " ms)."
                                     : juce::String ("Latency reported now: none.")));
    }

    void drawCrush (juce::Graphics& g)
    {
        drawControlsBox (g, controlsBox[(size_t) FxModule::crush]);
        drawText (g, textArea[(size_t) FxModule::crush],
                  "A copy of the mix through a fast, 20:1 compressor, pushed down by Drive, with most of its level made up, "
                  "then blended in under the mix. The quiet parts (room, ghost notes, cymbal wash) come up while the hits "
                  "keep their attack: the \"New York\" drum sound.\n\nStart with Blend low and bring it up until the kit "
                  "gets fat, not flat.");
    }

    void drawLimiterAndCrushLabels (juce::Graphics& g, juce::Component& meter)
    {
        g.setColour (theme::colours::textMuted);
        g.setFont (theme::mono (10.0f));
        g.drawText ("GR", meter.getBounds().withY (meter.getBottom() + 2).withHeight (12).expanded (6, 0), juce::Justification::centred, false);
    }

    LithiumDrumsProcessor& processor;
    juce::AudioProcessorValueTreeState& state;
    FxModule shown = FxModule::eq;
    juce::String shownKey;

    EqModule eq;
    CompModule glue;
    TwoKnobModule transient;
    GraphCard glueAbout { "What it does", [this] (juce::Graphics& g)
    {
        drawAbout (g, glueAbout,
                   "A bus compressor across the whole mix. Whenever the kit rises above Threshold it's turned down by Ratio: at 2:1, "
                   "every 2 dB over the threshold comes out as 1 dB. It senses peaks, with a gentle 6 dB soft knee, and turns both "
                   "channels down together so the stereo image doesn't shift.\n\n"
                   "Attack is how long it waits before clamping down, so a slower attack lets the crack of each hit through. "
                   "Release is how quickly it lets go, so the kit swells back between hits. Makeup brings the level back up after "
                   "compression, and Mix blends the compressed sound with the original (parallel compression). The GR meter shows "
                   "how much it's turning the kit down.\n\n"
                   "Gentle settings work best: 2:1, and a dB or three of gain reduction, hold the kit together without flattening it.");
    } };
    GraphCard transientAbout { "What it does", [this] (juce::Graphics& g)
    {
        drawAbout (g, transientAbout,
                   "Makes the hits of the whole kit punchier or softer, and works the same at any volume: there's no threshold to "
                   "set. It compares a fast follower of the signal with a slow one. When the fast one jumps ahead, that's the attack "
                   "of a hit; when the slow one lingers above, that's the ring after it.\n\n"
                   "Attack turns the start of every hit up or down, by up to 12 dB. Sustain does the same for the ring. Positive "
                   "values add, negative values take away.\n\n"
                   "More Attack brings out the stick. Less Sustain tightens a roomy kit; more Sustain lets the drums and the room ring.");
    } };
    GraphCard crush { "Parallel crush", [this] (juce::Graphics& g) { drawCrush (g); drawLimiterAndCrushLabels (g, crushMeter); } };
    GraphCard tape { "Tape", [this] (juce::Graphics& g) { drawTape (g); } };
    GraphCard limiter { "Limiter", [this] (juce::Graphics& g) { drawLimiter (g); drawLimiterAndCrushLabels (g, limiterMeter); } };
    GraphCard room { "Impulse response", [this] (juce::Graphics& g) { drawRoom (g); } };
    GraphCard plate { "Decay", [this] (juce::Graphics& g) { drawPlate (g); } };
    GraphCard delay { "Echoes", [this] (juce::Graphics& g) { drawDelay (g); } };

    std::vector<std::unique_ptr<Knob>> crushKnobs, tapeKnobs, limiterKnobs;
    ParamToggle crushOn { "On", "Crush on/off" }, tapeOn { "On", "Tape on/off" }, limiterOn { "On", "Limiter on/off" };
    ParamToggle lookahead { "Lookahead", "Look 2 ms ahead, for limiting without distortion (adds latency)" };
    ReductionMeter crushMeter, limiterMeter;
    juce::TextButton loadIr { "Load IR file..." };
    std::array<juce::Rectangle<int>, fxpresets::numModules> graphArea, textArea, controlsBox;
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
FxPage::FxPage (LithiumDrumsProcessor& p) : processor (p), detail (std::make_unique<Detail> (p))
{
    auto& state = p.getParameters();
    bypass.attach (state, params::fxBypass);
    bypass.setColour (juce::TextButton::buttonOnColourId, theme::colours::warning.withAlpha (0.8f));
    addAndMakeVisible (bypass);

    for (auto* b : { &slotA, &slotB })
    {
        b->setRadioGroupId (3);
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }

    slotA.setTooltip ("Compare two settings of the whole FX page: A");
    slotB.setTooltip ("Compare two settings of the whole FX page: B");
    slotA.onClick = [this] { processor.selectFxSlot (0); updateAB(); };
    slotB.onClick = [this] { processor.selectFxSlot (1); updateAB(); };
    copySlot.onClick = [this] { processor.copyFxSlot(); };
    addAndMakeVisible (copySlot);

    for (int r = 0; r < params::numFxReturns; ++r)
    {
        auto& card = cards[(size_t) r];
        card = std::make_unique<ReturnCard> (p, (params::FxReturn) r);
        card->onSelect = [this, r] { selectModule (r); };
        addAndMakeVisible (*card);
    }

    constexpr std::array<FxModule, 6> chain { FxModule::eq, FxModule::glue, FxModule::crush, FxModule::tape, FxModule::transient, FxModule::limiter };

    for (size_t i = 0; i < chain.size(); ++i)
    {
        tiles[i] = std::make_unique<ChainTile> (state, chain[i]);
        tiles[i]->onSelect = [this, m = chain[i]] { selectModule ((int) m); };
        addAndMakeVisible (*tiles[i]);
    }

    addAndMakeVisible (inMeter);
    addAndMakeVisible (outMeter);
    addAndMakeVisible (*detail);

    enableKeyboardNavigation (*this);
    selectModule (p.selectedFxModule);
    updateAB();
    startTimerHz (30);
}

FxPage::~FxPage() = default;

void FxPage::selectModule (int module)
{
    selected = juce::jlimit (0, fxpresets::numModules - 1, module);
    processor.selectedFxModule = selected;

    for (size_t r = 0; r < cards.size(); ++r)
        cards[r]->setSelected ((int) r == selected);

    for (auto& t : tiles)
        t->setSelected ((int) t->module == selected);

    detail->show ((FxModule) selected);
}

void FxPage::visibilityChanged()
{
    // The Mixer page may have asked for another module meanwhile.
    if (isVisible() && processor.selectedFxModule != selected)
        selectModule (processor.selectedFxModule);
}

void FxPage::updateAB()
{
    const auto slot = processor.getFxSlot();
    slotA.setToggleState (slot == 0, juce::dontSendNotification);
    slotB.setToggleState (slot == 1, juce::dontSendNotification);
    copySlot.setButtonText (slot == 0 ? "Copy A to B" : "Copy B to A");
    copySlot.setTooltip ("Copy the settings you hear now into " + juce::String (slot == 0 ? "B" : "A"));
}

void FxPage::timerCallback()
{
    // The meters are read (and cleared) even while hidden, so they don't
    // show a stale burst when the page comes back.
    auto& fb = processor.getMixerFeedback();

    for (size_t r = 0; r < cards.size(); ++r)
        cards[r]->pushLevel (MixerFeedback::take (fb.fxReturnPeak[r]));

    inMeter.push (MixerFeedback::take (fb.chainInPeak[0]), MixerFeedback::take (fb.chainInPeak[1]));
    outMeter.push (MixerFeedback::take (fb.chainOutPeak[0]), MixerFeedback::take (fb.chainOutPeak[1]));

    const auto glue = MixerFeedback::take (fb.glueReductionDb);
    const auto crush = MixerFeedback::take (fb.crushReductionDb);
    const auto limiter = MixerFeedback::take (fb.limiterReductionDb);

    for (auto& t : tiles)
        t->pushReduction (t->module == FxModule::glue ? glue : t->module == FxModule::crush ? crush : t->module == FxModule::limiter ? limiter : 0.0f);

    detail->pushGlueReduction (glue);
    detail->pushReductions (crush, limiter);

    if (! isVisible())
        return;

    // The preset menus follow the knobs (a few times a second is plenty).
    if (++refreshCount % 6 == 0)
    {
        for (auto& c : cards)
            c->refresh();

        for (auto& t : tiles)
            t->refresh();

        updateAB();
    }
}

void FxPage::paint (juce::Graphics& g)
{
    using namespace theme;
    g.fillAll (colours::background);

    // The signal flow: the returns down onto the master bus, which runs
    // into the chain's input; the chain's modules joined by arrows to OUT.
    g.setColour (colours::borderStrong);
    const auto busY = chainArea.getY() - space::md;

    for (auto& c : cards)
        g.fillRect (juce::Rectangle<int> (c->getBounds().getCentreX(), c->getBottom(), 1, busY - c->getBottom()));

    g.fillRect (juce::Rectangle<int> (inMeter.getBounds().getCentreX(), busY, cards.back()->getBounds().getCentreX() - inMeter.getBounds().getCentreX() + 1, 1));
    g.fillRect (juce::Rectangle<int> (inMeter.getBounds().getCentreX(), busY, 1, inMeter.getY() - space::xs - busY));

    // The meters on panels of their own, so their tracks show on the page.
    for (const auto* m : { &inMeter, &outMeter })
    {
        g.setColour (colours::raised);
        g.fillRoundedRectangle (m->getBounds().toFloat().expanded (4.0f), 4.0f);
        g.setColour (colours::border);
        g.drawRoundedRectangle (m->getBounds().toFloat().expanded (4.0f).reduced (0.5f), 4.0f, 1.0f);
    }

    g.setColour (colours::borderStrong);

    auto arrow = [&g] (int x0, int x1, int y)
    {
        g.fillRect (juce::Rectangle<int> (x0, y, x1 - x0 - 4, 1));
        juce::Path head;
        head.addTriangle ((float) x1 - 6.0f, (float) y - 3.5f, (float) x1, (float) y + 0.5f, (float) x1 - 6.0f, (float) y + 4.5f);
        g.fillPath (head);
    };

    const auto y = tiles.front()->getBounds().getCentreY();
    arrow (inMeter.getRight() + space::xs, tiles.front()->getX() - space::xxs, y);

    for (size_t i = 0; i + 1 < tiles.size(); ++i)
        arrow (tiles[i]->getRight() + space::xxs, tiles[i + 1]->getX() - space::xxs, y);

    arrow (tiles.back()->getRight() + space::xxs, outMeter.getX() - space::xs, y);

    g.setColour (colours::textMuted);

    for (const auto& [m, text] : { std::pair { &inMeter, "IN" }, std::pair { &outMeter, "OUT" } })
        drawTrackedText (g, text, label (type::caption), 0.1f,
                         m->getBounds().toFloat().withY ((float) m->getBottom() + 2.0f).withHeight (12.0f).expanded (8.0f, 0.0f),
                         juce::Justification::centred);

    drawTrackedText (g, "MASTER BUS", label (type::caption), 0.12f,
                     juce::Rectangle<int> (inMeter.getRight() + space::md, busY - 14, 200, 12).toFloat(), juce::Justification::centredLeft);

    if (bypass.getToggleState())
    {
        g.setColour (colours::warning);
        g.setFont (body (type::small));
        g.drawText ("Every effect on this page is bypassed.", bypass.getBounds().translated (bypass.getWidth() + space::md, 0).withWidth (300),
                    juce::Justification::centredLeft, false);
    }
}

void FxPage::resized()
{
    using namespace theme;
    auto area = getLocalBounds().reduced (space::page, space::md);

    auto bar = area.removeFromTop (toolbarHeight);
    bypass.setBounds (bar.removeFromLeft (110));
    copySlot.setBounds (bar.removeFromRight (120));
    bar.removeFromRight (space::sm);
    slotB.setBounds (bar.removeFromRight (36));
    slotA.setBounds (bar.removeFromRight (36));
    area.removeFromTop (space::md);

    // The returns.
    auto top = area.removeFromTop (cardHeight);
    const auto cardWidth = (top.getWidth() - 2 * space::panelGap) / 3;

    for (auto& c : cards)
    {
        c->setBounds (top.removeFromLeft (cardWidth));
        top.removeFromLeft (space::panelGap);
    }

    area.removeFromTop (space::xl + space::sm);

    // The chain: IN, six modules with arrows between, OUT.
    chainArea = area.removeFromTop (chainHeight);
    auto chain = chainArea;
    constexpr int meterSpace = 28, arrowGap = 18;
    auto in = chain.removeFromLeft (meterSpace);
    auto out = chain.removeFromRight (meterSpace);
    inMeter.setBounds (in.withSizeKeepingCentre (2 * size::meterWidth + 2, chain.getHeight() - 30).translated (0, -6));
    outMeter.setBounds (out.withSizeKeepingCentre (2 * size::meterWidth + 2, chain.getHeight() - 30).translated (0, -6));
    chain.removeFromLeft (arrowGap);
    chain.removeFromRight (arrowGap);
    const auto tileWidth = (chain.getWidth() - 5 * arrowGap) / 6;

    for (auto& t : tiles)
    {
        t->setBounds (chain.removeFromLeft (tileWidth));
        chain.removeFromLeft (arrowGap);
    }

    area.removeFromTop (space::md);
    detail->setBounds (area);
}

} // namespace lithium
