#include "HeaderBar.h"

#include "LithiumLookAndFeel.h"
#include "ModelPicker.h"
#include "PresetPanel.h"
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
    constexpr int readoutGap = 16;
    constexpr int readoutsToButtons = 24;
    constexpr int presetBoxWidth = 210, arrowWidth = 24, browseWidth = 66;
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
/** The preset box: the preset's name (with an asterisk once it's been
    changed) over the kit it plays. Clicking it opens the menu of presets
    and kits. */
class HeaderBar::PresetBox final : public juce::Button
{
public:
    PresetBox() : juce::Button ("Presets and kits") {}

    void setLines (const juce::String& top, const juce::String& bottom, bool isModified, bool topDimmed)
    {
        if (top != title || bottom != subtitle || isModified != modified || topDimmed != dimmed)
        {
            title = top;
            subtitle = bottom;
            modified = isModified;
            dimmed = topDimmed;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool isDown) override
    {
        using namespace theme;
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

        g.setColour (isDown ? colours::raisedHigh.brighter (0.05f) : (isMouseOver ? colours::raisedHigh : colours::raised));
        g.fillRoundedRectangle (bounds, cornerRadius);
        g.setColour (isMouseOver ? colours::borderStrong : colours::border);
        g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);

        if (hasKeyboardFocus (false))
            LithiumLookAndFeel::drawFocusRing (g, bounds, cornerRadius);

        // The chevron, as on the drop-downs.
        const auto cx = bounds.getRight() - 14.0f, cy = bounds.getCentreY();
        juce::Path chevron;
        chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.0f);
        chevron.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (colours::textMuted);
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Two lines; long names end in an ellipsis (the tooltip has them whole).
        auto lines = bounds.reduced (10.0f, 3.0f).withTrimmedRight (18.0f);
        const auto mark = modified ? juce::String (" *") : juce::String();
        const auto titleFont = body (13.5f, Weight::semibold);
        auto top = lines.removeFromTop (lines.getHeight() * 0.55f);

        g.setFont (titleFont);
        const auto markWidth = juce::GlyphArrangement::getStringWidth (titleFont, mark);
        const auto titleWidth = std::min (juce::GlyphArrangement::getStringWidth (titleFont, title) + 1.0f, top.getWidth() - markWidth);
        g.setColour (dimmed ? colours::textMuted : colours::text);
        g.drawText (title, top.removeFromLeft (titleWidth), juce::Justification::bottomLeft, true);
        g.setColour (colours::accent);
        g.drawText (mark, top, juce::Justification::bottomLeft, false);

        g.setColour (colours::textMuted);
        g.setFont (body (11.5f));
        g.drawText (subtitle, lines, juce::Justification::topLeft, true);
    }

private:
    juce::String title, subtitle;
    bool modified = false, dimmed = false;
};

//==============================================================================
HeaderBar::HeaderBar (LithiumDrumsProcessor& p)
    : processor (p),
      presetBox (std::make_unique<PresetBox>()),
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

    presetBox->onClick = [this] { showPresetMenu(); };
    addAndMakeVisible (*presetBox);

    previousPreset.onClick = [this] { stepPreset (-1); };
    nextPreset.onClick = [this] { stepPreset (1); };
    addAndMakeVisible (previousPreset);
    addAndMakeVisible (nextPreset);

    browseButton.setTooltip ("Browse every installed kit and kit preset, and download more");
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
    library = kitlibrary::scan (processor.getKitsFolder());
    updatePresetBox();
}

void HeaderBar::updatePresetBox()
{
    const auto current = processor.getKitFolder();
    const auto info = processor.getKitInfo();
    auto kitName = current.getFileName();

    if (const auto* entry = library.find (current))
        kitName = entry->credits().name;

    // Top line: the preset, or the kit when there's no preset. Bottom line:
    // the kit, or what's happening to it.
    const auto presetFile = processor.getPresetFile();
    auto top = presetFile != juce::File() ? presets::nameOf (presetFile) : kitName;
    juce::String bottom = presetFile != juce::File() ? kitName : juce::String ("No preset");
    auto dimmed = false;

    if (processor.isKitLoading())
    {
        // Decoding a big kit into the stream cache takes a while the first
        // time, so say so.
        const auto caching = processor.getKitLoadPhase() == LoadPhase::caching;
        bottom = (caching ? "Caching " : "Loading ") + kitName + "...";
    }
    else if (processor.getKitError().isNotEmpty() && (info == nullptr || info->folder != current))
    {
        bottom = "Kit failed to load: hover for why";
    }
    else if (current == juce::File())
    {
        top = library.kits.empty() ? "No kits installed" : "Choose a kit";
        bottom = library.kits.empty() ? "Press Browse to download one" : juce::String();
        dimmed = true;
    }

    presetBox->setLines (top, bottom, processor.isPresetModified(), dimmed);

    // The tooltip carries the details a kit author needs: load errors, or the
    // folder plus any warnings from kit.json.
    auto tooltip = juce::String ("Presets and kits (click for the menu)\n");
    tooltip << (processor.getKitError().isNotEmpty() ? processor.getKitError() : current.getFullPathName());

    if (info != nullptr && ! info->warnings.isEmpty())
        tooltip << "\n" << info->warnings.joinIntoString ("\n");

    presetBox->setTooltip (tooltip);
}

void HeaderBar::showPresetMenu()
{
    library = kitlibrary::scan (processor.getKitsFolder());

    PresetPanel::Callbacks callbacks;
    auto safe = juce::Component::SafePointer (this);

    callbacks.loadPreset = [safe] (const juce::File& f) { if (safe != nullptr) safe->loadPreset (f); };
    callbacks.loadKit = [safe] (const juce::File& folder)
    {
        // Choosing a kit starts it with neutral settings and no preset.
        if (safe != nullptr)
        {
            safe->processor.loadKit (folder, true);
            safe->updatePresetBox();
        }
    };
    callbacks.save = [safe]
    {
        if (safe == nullptr)
            return;

        if (const auto r = safe->processor.savePreset (safe->processor.getPresetFile()); r.failed())
            safe->report ("Couldn't save the preset", r.getErrorMessage());

        safe->updatePresetBox();
    };
    callbacks.saveAs = [safe] { if (safe != nullptr) safe->savePresetAs(); };
    callbacks.rename = [safe] { if (safe != nullptr) safe->renamePreset(); };
    callbacks.remove = [safe] { if (safe != nullptr) safe->deletePreset(); };
    callbacks.browse = [safe] { if (safe != nullptr && safe->onShowBrowser) safe->onShowBrowser(); };

    // In a call-out pointing at the box; the call-out owns the panel and
    // deletes it when it closes (a click outside, Escape, or a choice).
    if (auto* parent = findOverlayParent (*presetBox))
        juce::CallOutBox::launchAsynchronously (std::make_unique<PresetPanel> (processor, library, std::move (callbacks)),
                                                parent->getLocalArea (presetBox.get(), presetBox->getLocalBounds()), parent);
}

void HeaderBar::loadPreset (const juce::File& file)
{
    if (const auto r = processor.loadPreset (file); r.failed())
        report ("Couldn't load \"" + presets::nameOf (file) + "\"", r.getErrorMessage());

    updatePresetBox();
}

void HeaderBar::stepPreset (int delta)
{
    const auto all = presets::scan();

    if (all.empty())
        return report ("No presets yet", "Save the current sound with Save as... in the preset menu, and it will be here to step through.");

    const auto current = processor.getPresetFile();
    int index = -1;

    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].file == current)
            index = (int) i;

    const auto n = (int) all.size();
    const auto next = index < 0 ? (delta > 0 ? 0 : n - 1) : (index + delta + n) % n;
    loadPreset (all[(size_t) next].file);
}

void HeaderBar::savePresetAs()
{
    if (! onPrompt)
        return;

    const auto current = processor.getPresetFile();
    PromptPanel::Request request;
    request.title = "Save preset";
    request.message = "A preset keeps the kit, its swapped pieces and every setting of the mixer and the pieces.";
    request.okText = "Save";
    request.withTextField = true;
    request.text = current != juce::File() ? presets::nameOf (current) : (processor.getKitInfo() != nullptr ? processor.getKitInfo()->credits.name : juce::String());
    request.onOk = [safe = juce::Component::SafePointer (this)] (const juce::String& name)
    {
        if (safe == nullptr)
            return;

        const auto file = presets::userFileFor (name);

        if (file == juce::File())
            return safe->report ("Couldn't save the preset", "That name can't be used for a file. Try letters and numbers.");

        auto save = [safe, file]
        {
            if (safe == nullptr)
                return;

            if (const auto r = safe->processor.savePreset (file); r.failed())
                safe->report ("Couldn't save the preset", r.getErrorMessage());

            safe->updatePresetBox();
        };

        if (! file.existsAsFile() || file == safe->processor.getPresetFile())
            return save();

        // Asked from inside the first prompt's callback, after it has closed.
        PromptPanel::Request confirm;
        confirm.title = "Replace preset";
        confirm.message = "There's already a preset called \"" + presets::nameOf (file) + "\". Replace it with the current sound?";
        confirm.okText = "Replace";
        confirm.destructive = true;
        confirm.onOk = [save] (const juce::String&) { save(); };
        safe->onPrompt (confirm);
    };

    onPrompt (request);
}

void HeaderBar::renamePreset()
{
    const auto file = processor.getPresetFile();

    if (! onPrompt || ! file.existsAsFile())
        return;

    PromptPanel::Request request;
    request.title = "Rename preset";
    request.message = "The preset's new name:";
    request.okText = "Rename";
    request.withTextField = true;
    request.text = presets::nameOf (file);
    request.onOk = [safe = juce::Component::SafePointer (this), file] (const juce::String& name)
    {
        if (safe == nullptr)
            return;

        const auto target = presets::userFileFor (name);

        if (target == juce::File() || target == file)
            return;

        if (target.exists())
            return safe->report ("Couldn't rename the preset", "There's already a preset called \"" + presets::nameOf (target) + "\".");

        if (! file.moveFileTo (target))
            return safe->report ("Couldn't rename the preset", "The file couldn't be renamed: " + file.getFullPathName());

        safe->processor.setPresetFile (target);
        safe->updatePresetBox();
    };

    onPrompt (request);
}

void HeaderBar::deletePreset()
{
    const auto file = processor.getPresetFile();

    if (! onPrompt || ! file.existsAsFile())
        return;

    PromptPanel::Request request;
    request.title = "Delete preset";
    request.message = "Delete \"" + presets::nameOf (file) + "\"? It goes to the trash, so you can still get it back from there. "
                      "What's playing now stays as it is.";
    request.okText = "Delete";
    request.destructive = true;
    request.onOk = [safe = juce::Component::SafePointer (this), file] (const juce::String&)
    {
        if (safe == nullptr)
            return;

        if (! file.moveToTrash() && ! file.deleteFile())
            return safe->report ("Couldn't delete the preset", "The file couldn't be removed: " + file.getFullPathName());

        safe->processor.setPresetFile ({});
        safe->updatePresetBox();
    };

    onPrompt (request);
}

void HeaderBar::report (const juce::String& title, const juce::String& message)
{
    if (! onPrompt)
        return;

    PromptPanel::Request request;
    request.title = title;
    request.message = message;
    request.cancellable = false;
    onPrompt (request);
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

    // The asterisk comes and goes as settings change.
    updatePresetBox();

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

    loadPhase = newPhase;
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
    // A thin bar just under the preset box.
    return presetBox->getBounds().withY (presetBox->getBottom() + 3).withHeight (3).reduced (6, 0);
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

    // ‹ [preset box] ›  Browse, between the tabs and the readouts, all the
    // same height.
    using namespace theme;
    const auto boxHeight = 36;
    auto middle = juce::Rectangle<int> (x + space::lg, (getHeight() - boxHeight) / 2,
                                        getReadoutArea().getX() - space::lg - (x + space::lg), boxHeight);
    const auto groupWidth = arrowWidth * 2 + presetBoxWidth + space::xs * 2 + space::sm + browseWidth;
    middle = middle.withSizeKeepingCentre (std::min (groupWidth, middle.getWidth()), boxHeight);
    previousPreset.setBounds (middle.removeFromLeft (arrowWidth));
    middle.removeFromLeft (space::xs);
    browseButton.setBounds (middle.removeFromRight (browseWidth));
    middle.removeFromRight (space::sm);
    nextPreset.setBounds (middle.removeFromRight (arrowWidth));
    middle.removeFromRight (space::xs);
    presetBox->setBounds (middle);

    // MIDI | Settings | Info, left to right.
    auto icons = getLocalBounds().withTrimmedRight (20).removeFromRight (numIcons * iconSize + (numIcons - 1) * 8);
    midiButton->setBounds (icons.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize));
    icons.removeFromLeft (8);
    settingsButton->setBounds (icons.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize));
    icons.removeFromLeft (8);
    aboutButton->setBounds (icons.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize));
}

} // namespace lithium
