#include "HeaderBar.h"

#include "Theme.h"

#include <numeric>

namespace lithium
{

namespace
{
    constexpr float wordmarkSize = 21.0f;
    constexpr float wordmarkTracking = 0.14f;
    constexpr float tabTracking = 0.1f;
    // CPU, RAM, DISK and POLY: each as wide as its longest value ("100%",
    // "1023 MB", "123 MB/s", "64" in 14 px IBM Plex Mono), with a clear gap
    // between them and before the buttons.
    constexpr std::array<int, 4> readoutWidths { 44, 64, 80, 40 };
    constexpr int readoutGap = 20;
    constexpr int readoutsToButtons = 28;
    constexpr double dropoutHighlightMs = 4000.0;   // how long the disk readout stays red after a dropout
    constexpr int iconSize = 30;
    constexpr int numIcons = 3;
    constexpr double midiLightMs = 120.0;   // how long the MIDI light stays on after a note

    juce::String formatBytes (std::size_t bytes)
    {
        const auto mb = (double) bytes / (1024.0 * 1024.0);
        return mb >= 1000.0 ? juce::String (mb / 1024.0, 2) + " GB" : juce::String (juce::roundToInt (mb)) + " MB";
    }

    // The logo mark: two drumsticks crossed in an X, each with a bead tip.
    void drawLogoMark (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto c = area.getCentre();
        const auto r = area.getWidth() * 0.5f;

        for (const auto angle : { -0.25f * juce::MathConstants<float>::pi, 0.25f * juce::MathConstants<float>::pi })
        {
            const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
            const auto butt = c - dir * r * 0.95f;
            const auto tip = c + dir * r * 0.78f;

            juce::Path stick;
            stick.startNewSubPath (butt);
            stick.lineTo (tip);
            g.strokePath (stick, juce::PathStrokeType (r * 0.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.fillEllipse (juce::Rectangle<float> (r * 0.36f, r * 0.36f).withCentre (c + dir * r * 0.9f));
        }
    }
}

//==============================================================================
class HeaderBar::TabButton final : public juce::Button
{
public:
    TabButton (const juce::String& name, Tab t) : juce::Button (name), tab (t)
    {
        setClickingTogglesState (true);
        setRadioGroupId (1);
    }

    int getIdealWidth() const
    {
        return juce::roundToInt (theme::getTrackedTextWidth (getName().toUpperCase(), font(), tabTracking)) + 22;
    }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool) override
    {
        // Spelled out in full: inside a Button, plain "text" would be Button::text.
        const auto& textColour = theme::colours::text;
        const auto active = getToggleState();

        g.setColour (active ? textColour : (isMouseOver ? textColour.withAlpha (0.85f) : theme::colours::textMuted));
        theme::drawTrackedText (g, getName().toUpperCase(), font(), tabTracking,
                                getLocalBounds().toFloat(), juce::Justification::centred);

        if (active)
        {
            g.setColour (theme::colours::accent);
            g.fillRect (getLocalBounds().removeFromBottom (2).reduced (10, 0));
        }
    }

    const Tab tab;

private:
    static juce::Font font()      { return theme::label (15.0f, theme::Weight::semibold); }
};

//==============================================================================
/** A small round button with a drawn icon: "i" for About, a gear for Settings. */
class HeaderBar::IconButton final : public juce::Button
{
public:
    enum class Icon { info, gear };

    IconButton (const juce::String& name, Icon i) : juce::Button (name), icon (i)
    {
        setTooltip (name);
    }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool isDown) override
    {
        using namespace theme;
        const auto bounds = getLocalBounds().toFloat().reduced (1.0f);

        g.setColour (isDown ? colours::raisedHigh : (isMouseOver ? colours::raised : colours::panel));
        g.fillEllipse (bounds);
        g.setColour (isMouseOver ? colours::accent : colours::borderStrong);
        g.drawEllipse (bounds, 1.0f);

        const auto c = bounds.getCentre();
        g.setColour (isMouseOver ? colours::text : colours::textMuted);

        if (icon == Icon::info)
        {
            g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ c.x, c.y - 5.5f }));
            g.fillRoundedRectangle (juce::Rectangle<float> (2.6f, 9.0f).withCentre ({ c.x, c.y + 2.5f }), 1.0f);
        }
        else
        {
            // A gear: eight teeth (each added at the top, then the whole path
            // rotated an eighth of a turn) around a solid ring.
            juce::Path gear;

            for (int t = 0; t < 8; ++t)
            {
                gear.addRectangle (-1.6f, -8.0f, 3.2f, 4.0f);
                gear.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::twoPi / 8.0f));
            }

            gear.addEllipse (-5.5f, -5.5f, 11.0f, 11.0f);
            gear.applyTransform (juce::AffineTransform::translation (c));
            g.fillPath (gear);
            g.setColour (isDown ? colours::raisedHigh : (isMouseOver ? colours::raised : colours::panel));
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (c));
        }
    }

private:
    const Icon icon;
};

//==============================================================================
/** Opens the MIDI panel. Its ring lights up as notes come in, so it doubles
    as a MIDI activity light: the first thing to check when an e-kit seems
    to do nothing. */
class HeaderBar::MidiButton final : public juce::Button,
                                    private juce::Timer
{
public:
    explicit MidiButton (const EngineFeedback& f) : juce::Button ("MIDI mapping"), feedback (f)
    {
        setTooltip ("MIDI mapping and e-kit setup. Lights up when notes come in.");
        seen = feedback.lastNote.load();
        startTimerHz (25);
    }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool isDown) override
    {
        using namespace theme;
        const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        const auto lit = juce::Time::getMillisecondCounterHiRes() < litUntil;

        g.setColour (isDown ? colours::raisedHigh : (isMouseOver ? colours::raised : colours::panel));
        g.fillEllipse (bounds);
        g.setColour (lit || isMouseOver ? colours::accent : colours::borderStrong);
        g.drawEllipse (bounds, lit ? 2.0f : 1.0f);

        // A five-pin DIN socket: a ring with a key notch, pins in a half circle.
        const auto c = bounds.getCentre();
        g.setColour (lit ? colours::accent : (isMouseOver ? colours::text : colours::textMuted));
        g.drawEllipse (juce::Rectangle<float> (15.0f, 15.0f).withCentre (c), 1.4f);
        g.fillRect (juce::Rectangle<float> (3.0f, 2.6f).withCentre ({ c.x, c.y - 6.6f }));

        for (const auto degrees : { 90.0f, 135.0f, 180.0f, 225.0f, 270.0f })
        {
            const auto a = juce::degreesToRadians (degrees);
            g.fillEllipse (juce::Rectangle<float> (2.2f, 2.2f).withCentre ({ c.x + 4.0f * std::sin (a), c.y - 4.0f * std::cos (a) }));
        }
    }

private:
    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (const auto bits = feedback.lastNote.load (std::memory_order_relaxed); bits != seen)
        {
            seen = bits;
            litUntil = now + midiLightMs;
            wasLit = true;
            repaint();
        }
        else if (wasLit && now >= litUntil)
        {
            wasLit = false;
            repaint();
        }
    }

    const EngineFeedback& feedback;
    std::uint32_t seen = 0;
    double litUntil = 0.0;
    bool wasLit = false;
};

//==============================================================================
HeaderBar::HeaderBar (LithiumDrumsProcessor& p)
    : processor (p),
      aboutButton (std::make_unique<IconButton> ("About and credits", IconButton::Icon::info)),
      settingsButton (std::make_unique<IconButton> ("Settings", IconButton::Icon::gear)),
      midiButton (std::make_unique<MidiButton> (p.getFeedback()))
{
    const std::pair<const char*, Tab> tabNames[] { { "Kit", Tab::kit }, { "Edit", Tab::edit }, { "Mixer", Tab::mixer },
                                                   { "Grooves", Tab::grooves }, { "FX", Tab::fx } };

    for (const auto& [name, tab] : tabNames)
    {
        auto button = std::make_unique<TabButton> (name, tab);
        button->onClick = [this, b = button.get()]
        {
            if (b->getToggleState() && onTabChanged)
                onTabChanged (b->tab);
        };
        addAndMakeVisible (*button);
        tabs.push_back (std::move (button));
    }

    tabs.front()->setToggleState (true, juce::dontSendNotification);

    kitBox.setTooltip ("Kit");
    kitBox.onChange = [this] { kitBoxChanged(); };
    addAndMakeVisible (kitBox);

    browseButton.setTooltip ("Browse every installed kit and preset");
    browseButton.onClick = [this] { if (onShowBrowser) onShowBrowser(); };
    addAndMakeVisible (browseButton);

    aboutButton->onClick = [this] { if (onShowAbout) onShowAbout(); };
    settingsButton->onClick = [this] { if (onShowSettings) onShowSettings(); };
    midiButton->onClick = [this] { if (onShowMidi) onShowMidi(); };
    addAndMakeVisible (*midiButton);
    addAndMakeVisible (*settingsButton);
    addAndMakeVisible (*aboutButton);

    processor.addChangeListener (this);
    installer->addChangeListener (this);
    refreshKitBox();
    startTimerHz (4);
}

HeaderBar::~HeaderBar()
{
    installer->removeChangeListener (this);
    processor.removeChangeListener (this);
}

void HeaderBar::setTab (Tab t)
{
    for (auto& b : tabs)
        if (b->tab == t)
            b->setToggleState (true, juce::sendNotificationSync);
}

//==============================================================================
void HeaderBar::refreshKitBox()
{
    const auto current = processor.getKitFolder();
    library = kitlibrary::scan (processor.getKitsFolder());
    const auto& kits = library.kits;

    kitBox.clear (juce::dontSendNotification);

    // Grouped by content pack, each pack's kits before its presets.
    int lastPack = std::numeric_limits<int>::min();

    for (size_t i = 0; i < kits.size(); ++i)
    {
        if (kits[i].pack != lastPack)
        {
            const auto* pack = library.packOf (kits[i]);
            kitBox.addSectionHeading (pack != nullptr ? pack->name : juce::String ("Other kits"));
            lastPack = kits[i].pack;
        }

        kitBox.addItem (kits[i].credits().name, (int) i + 1);
    }

    // Catalog packs that aren't installed, greyed: Browse downloads them.
    auto missingId = missingItemId;

    for (const auto& c : packs::catalog())
    {
        if (c.type != packs::CatalogPack::Type::kits
            || std::any_of (library.packs.begin(), library.packs.end(), [&] (const auto& p) { return p.id == c.id; }))
            continue;

        if (missingId == missingItemId)
            kitBox.addSectionHeading ("Not installed");

        kitBox.addItem (c.name + "  (" + packs::formatSize (c.downloadSize()) + ")", missingId);
        kitBox.setItemEnabled (missingId++, false);
    }

    if (missingId != missingItemId)
        kitBox.addItem ("Download kits...", browseItemId + 1);

    if (! kits.empty() || missingId != missingItemId)
        kitBox.addSeparator();

    kitBox.addItem ("Browse kits...", browseItemId);
    kitBox.addItem ("Refresh kit list", refreshItemId);
    kitBox.addItem ("Kits folder...", settingsItemId);

    const auto info = processor.getKitInfo();
    auto currentName = current.getFileName();

    for (size_t i = 0; i < kits.size(); ++i)
        if (kits[i].folder == current)
            currentName = kits[i].credits().name;

    if (processor.isKitLoading())
    {
        // Decoding a big kit into the stream cache takes a while the first
        // time, so say so.
        const auto caching = processor.getKitLoadPhase() == LoadPhase::caching;
        kitBox.setText ((caching ? "Caching " : "Loading ") + currentName + "...", juce::dontSendNotification);
    }
    else if (processor.getKitError().isNotEmpty() && (info == nullptr || info->folder != current))
    {
        kitBox.setText ("Kit failed to load", juce::dontSendNotification);
    }
    else
    {
        for (size_t i = 0; i < kits.size(); ++i)
            if (kits[i].folder == current)
                kitBox.setSelectedId ((int) i + 1, juce::dontSendNotification);

        if (current == juce::File())
            kitBox.setText (kits.empty() ? "No kits installed" : "Choose a kit", juce::dontSendNotification);
    }

    // The tooltip carries the details a kit author needs: load errors, or the
    // folder plus any warnings from kit.json.
    auto tooltip = processor.getKitError().isNotEmpty() ? processor.getKitError() : current.getFullPathName();

    if (info != nullptr && ! info->warnings.isEmpty())
        tooltip << "\n" << info->warnings.joinIntoString ("\n");

    kitBox.setTooltip (tooltip);
}

void HeaderBar::kitBoxChanged()
{
    const auto id = kitBox.getSelectedId();

    if (id == refreshItemId || id == settingsItemId || id == browseItemId || id == browseItemId + 1)
    {
        refreshKitBox();   // put the current kit's name back

        if (id == settingsItemId && onShowSettings)
            onShowSettings();

        if ((id == browseItemId || id == browseItemId + 1) && onShowBrowser)
            onShowBrowser();
    }
    else if (id >= 1 && id <= (int) library.kits.size())
    {
        // Choosing a kit from the menu starts it with neutral settings.
        processor.loadKit (library.kits[(size_t) id - 1].folder, true);
    }
}

void HeaderBar::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    // A new instance with nothing installed loads a kit as soon as there is one.
    if (source == &installer.get() && processor.getKitInfo() == nullptr && ! processor.isKitLoading())
        if (const auto folder = kitlibrary::findDefaultKit (processor.getKitsFolder()); folder != juce::File())
            processor.loadKit (folder, true);

    refreshKitBox();
}

void HeaderBar::timerCallback()
{
    const auto newCpu = juce::roundToInt (processor.getCpuLoadPercent());
    const auto newVoices = processor.getFeedback().activeVoices.load (std::memory_order_relaxed);
    const auto newBytes = processor.getSampleMemoryUsage();

    // Disk throughput over the last tick, and whether a voice has had to
    // play silence lately because the disk didn't keep up.
    const auto& stats = processor.getStreamStats();
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto bytesRead = stats.bytesRead.load (std::memory_order_relaxed);
    const auto dropouts = stats.dropouts.load (std::memory_order_relaxed);

    if (lastReadTime > 0.0 && now > lastReadTime)
        diskBytesPerSecond = (double) (bytesRead - lastBytesRead) * 1000.0 / (now - lastReadTime);

    if (dropouts != lastDropouts)
        lastDropoutTime = now;

    lastBytesRead = bytesRead;
    lastDropouts = dropouts;
    lastReadTime = now;

    const auto info = processor.getKitInfo();
    streaming = info != nullptr && info->streaming;
    recentDropouts = now - lastDropoutTime < dropoutHighlightMs;

    // Repainting four numbers is cheap; just do it every tick.
    cpuPercent = newCpu;
    voices = newVoices;
    sampleBytes = newBytes;
    repaint (getReadoutArea());

    const auto loading = processor.isKitLoading();
    const auto newProgress = loading ? processor.getKitLoadProgress() : -1.0f;
    const auto newPhase = processor.getKitLoadPhase();

    if (std::abs (newProgress - loadProgress) > 0.001f)
    {
        loadProgress = newProgress;
        repaint (getProgressArea());
    }

    if (loading && newPhase != loadPhase)
    {
        loadPhase = newPhase;
        refreshKitBox();     // "Caching..." rather than "Loading..."
    }
}

//==============================================================================
float HeaderBar::getWordmarkRight() const
{
    const auto font = theme::label (wordmarkSize, theme::Weight::bold);
    return 58.0f + theme::getTrackedTextWidth ("LITHIUM", font, wordmarkTracking) + 6.0f
           + theme::getTrackedTextWidth ("DRUMS", theme::label (wordmarkSize, theme::Weight::semibold), wordmarkTracking);
}

juce::Rectangle<int> HeaderBar::getReadoutArea() const
{
    const auto width = std::accumulate (readoutWidths.begin(), readoutWidths.end(), 0) + readoutGap * ((int) readoutWidths.size() - 1);
    return getLocalBounds().withTrimmedRight (20 + numIcons * iconSize + (numIcons - 1) * 8 + readoutsToButtons).removeFromRight (width);
}

juce::Rectangle<int> HeaderBar::getReadoutBounds (int index) const
{
    auto area = getReadoutArea();

    for (int i = 0; i < index; ++i)
        area.removeFromLeft (readoutWidths[(size_t) i] + readoutGap);

    return area.withWidth (readoutWidths[(size_t) index]);
}

juce::String HeaderBar::getTooltip()
{
    const auto mouse = getMouseXYRelative();
    int readout = 0;

    while (readout < (int) readoutWidths.size() && ! getReadoutBounds (readout).expanded (readoutGap / 2, 0).contains (mouse))
        ++readout;

    const auto info = processor.getKitInfo();
    const auto& stats = processor.getStreamStats();

    switch (readout)
    {
        case 0:
            return "Audio processing load";

        case 1:
            if (info != nullptr && info->streaming)
                return "Sample data in RAM: " + formatBytes (sampleBytes) + ", of which "
                       + formatBytes (info->streamBufferBytes) + " is stream buffers.\nThe rest of the kit streams from disk; "
                       "fully in RAM it would take " + formatBytes (info->fullMemoryBytes) + ".\nMuted mics are unloaded after a couple of seconds.";

            return "Sample data in RAM. Muted mics are unloaded after a couple of seconds.";

        case 2:
        {
            auto text = streaming ? juce::String ("Reading from the stream cache.")
                                  : juce::String ("This kit is held in RAM, so nothing streams.");
            const auto dropouts = stats.dropouts.load (std::memory_order_relaxed);

            if (dropouts > 0)
                text << "\n" << (juce::int64) dropouts << " dropouts since this instance started ("
                     << juce::String ((double) stats.dropoutFrames.load (std::memory_order_relaxed) / (info != nullptr ? info->sampleRate : 44100.0), 2)
                     << " s of audio played as silence). A larger preload in Settings helps.";

            return text;
        }

        case 3:
            return "Polyphony: how many sounds are playing right now, out of " + juce::String (DrumEngine::numVoices)
                   + ".\nEvery hit is one, and a ringing cymbal keeps its own until it fades.";

        default:
            return {};
    }
}

juce::Rectangle<int> HeaderBar::getProgressArea() const
{
    // A thin bar just under the kit menu.
    return kitBox.getBounds().withY (kitBox.getBottom() + 3).withHeight (3).reduced (6, 0);
}

void HeaderBar::paint (juce::Graphics& g)
{
    using namespace theme;

    g.fillAll (colours::panel);
    g.setColour (colours::border);
    g.fillRect (getLocalBounds().removeFromBottom (1));

    g.setColour (colours::accent);
    drawLogoMark (g, { 20.0f, 15.0f, 26.0f, 26.0f });

    const auto boldFont = label (wordmarkSize, Weight::bold);
    const auto lithiumWidth = getTrackedTextWidth ("LITHIUM", boldFont, wordmarkTracking);
    const auto row = juce::Rectangle<float> (58.0f, 0.0f, 400.0f, (float) getHeight());

    g.setColour (colours::text);
    drawTrackedText (g, "LITHIUM", boldFont, wordmarkTracking, row, juce::Justification::centredLeft);
    g.setColour (colours::textMuted);
    drawTrackedText (g, "DRUMS", label (wordmarkSize, Weight::semibold), wordmarkTracking,
                     row.withTrimmedLeft (lithiumWidth + 6.0f), juce::Justification::centredLeft);

    // Readouts: a small label above a monospaced value.
    const auto diskValue = streaming ? juce::String (diskBytesPerSecond / (1024.0 * 1024.0), diskBytesPerSecond < 10.0 * 1024 * 1024 ? 1 : 0) + " MB/s"
                                     : juce::String ("-");
    const std::tuple<juce::String, juce::String, juce::Colour> readouts[] {
        { "CPU", juce::String (cpuPercent) + "%", colours::text },
        { "RAM", formatBytes (sampleBytes), colours::text },     // sample data; muted mics are unloaded
        { "DISK", diskValue, recentDropouts ? colours::warning : colours::text },
        { "POLY", juce::String (voices), colours::text },
    };

    for (const auto& [i, readout] : juce::enumerate (readouts))
    {
        const auto& [name, value, colour] = readout;
        auto r = getReadoutBounds ((int) i).toFloat().withSizeKeepingCentre ((float) readoutWidths[(size_t) i], 34.0f);
        g.setColour (colours::textMuted);
        drawTrackedText (g, name, label (11.0f, Weight::semibold), 0.12f, r.removeFromTop (14.0f),
                         juce::Justification::centredRight);
        g.setColour (colour);
        g.setFont (mono (14.0f, Weight::medium));
        g.drawText (value, r, juce::Justification::centredRight, false);
    }

    if (loadProgress >= 0.0f)
    {
        const auto bar = getProgressArea().toFloat();
        g.setColour (colours::border);
        g.fillRoundedRectangle (bar, 1.5f);
        g.setColour (colours::accent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.02f, 1.0f, loadProgress)), 1.5f);
    }
}

void HeaderBar::resized()
{
    auto x = juce::roundToInt (getWordmarkRight()) + 18;

    for (auto& t : tabs)
    {
        const auto w = t->getIdealWidth();
        t->setBounds (x, 0, w, getHeight());
        x += w;
    }

    const auto boxWidth = 220;
    kitBox.setBounds (juce::Rectangle<int> (boxWidth, 32).withCentre (getLocalBounds().getCentre()));
    browseButton.setBounds (kitBox.getBounds().withX (kitBox.getRight() + 8).withWidth (72));

    // MIDI | Settings | Info, left to right.
    auto icons = getLocalBounds().withTrimmedRight (20).removeFromRight (numIcons * iconSize + (numIcons - 1) * 8);
    midiButton->setBounds (icons.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize));
    icons.removeFromLeft (8);
    settingsButton->setBounds (icons.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize));
    icons.removeFromLeft (8);
    aboutButton->setBounds (icons.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize));
}

} // namespace lithium
