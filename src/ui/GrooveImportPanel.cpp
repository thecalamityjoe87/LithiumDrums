#include "GrooveImportPanel.h"

#include "Controls.h"
#include "Settings.h"
#include "Theme.h"
#include "engine/MidiMap.h"
#include "grooves/GrooveLibrary.h"

namespace lithium
{

namespace
{
    // Card sizes for each step.
    constexpr int formWidth = 520, formHeight = 380;
    constexpr int importingWidth = 520, importingHeight = 240;
    constexpr int doneWidth = 560, doneHeight = 470, doneHeightNothingLeftOut = 380;
    constexpr int failedWidth = 480, failedHeight = 260;

    constexpr int captionHeight = 18;
    constexpr int chipHeight = 30;
    constexpr int chipRows = 2;
    constexpr float chipGap = 6.0f;
    constexpr int leftOutRowHeight = 22;

    // "18576" -> "18,576".
    juce::String withThousands (int n)
    {
        auto digits = juce::String (std::abs (n));

        for (int i = digits.length() - 3; i > 0; i -= 3)
            digits = digits.substring (0, i) + "," + digits.substring (i);

        return (n < 0 ? "-" : "") + digits;
    }

    void drawCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area)
    {
        using namespace theme;
        g.setColour (colours::textMuted);
        drawTrackedText (g, text.toUpperCase(), label (12.0f), 0.1f, area.toFloat(), juce::Justification::centredLeft);
    }
}

//==============================================================================
/** The files left out of an import, one row each: the file, then why. */
class GrooveImportPanel::LeftOutList final : public juce::Component
{
public:
    void setLines (const juce::StringArray& warnings)
    {
        rows.clear();

        for (const auto& w : warnings)
        {
            // The importer says "Skipped <path>: <reason>"; anything else is shown as it is.
            if (w.startsWith ("Skipped ") && w.contains (": "))
                rows.push_back ({ w.fromFirstOccurrenceOf ("Skipped ", false, false).upToLastOccurrenceOf (": ", false, false),
                                  w.fromLastOccurrenceOf (": ", false, false) });
            else
                rows.push_back ({ w, {} });
        }

        setSize (getWidth(), (int) rows.size() * leftOutRowHeight);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        auto y = 0;

        for (const auto& [path, reason] : rows)
        {
            auto row = juce::Rectangle<int> (0, y, getWidth(), leftOutRowHeight).reduced (space::sm, 0);
            y += leftOutRowHeight;

            if (reason.isNotEmpty())
            {
                const auto reasonWidth = std::min (row.getWidth() / 2, juce::roundToInt (getTrackedTextWidth (reason, body (type::small), 0.0f)) + 2);
                g.setColour (colours::textMuted);
                g.setFont (body (type::small));
                g.drawText (reason, row.removeFromRight (reasonWidth), juce::Justification::centredRight, true);
                row.removeFromRight (space::md);
            }

            g.setColour (colours::text.withAlpha (0.85f));
            g.setFont (mono (12.0f));
            g.drawText (path, row, juce::Justification::centredLeft, true);
        }
    }

private:
    std::vector<std::pair<juce::String, juce::String>> rows;   // file, reason
};

//==============================================================================
GrooveImportPanel::GrooveImportPanel()
    : OverlayPanel ("Import MIDI grooves", formWidth, formHeight),
      leftOut (std::make_unique<LeftOutList>())
{
    using namespace theme;

    for (const auto& m : MidiMap::factoryMaps())
        if (m.id != MidiMap::kitDefaultId)
            maps.emplace_back (m.id, m.name);

    nameField.setFont (body (type::body));
    nameField.setIndents (10, 7);
    nameField.onTextChange = [this] { updateReplaceWarning(); };
    nameField.onReturnKey = [this] { primaryClicked(); };
    nameField.onEscapeKey = [this] { if (onClose) onClose(); };
    addChildComponent (nameField);

    for (size_t i = 0; i < maps.size(); ++i)
        mapBox.addItem (maps[i].second, (int) i + 1);

    addChildComponent (mapBox);

    primaryButton.onClick = [this] { primaryClicked(); };
    addChildComponent (primaryButton);

    leftOutView.setViewedComponent (leftOut.get(), false);
    leftOutView.setScrollBarsShown (true, false);
    leftOutView.setScrollBarThickness (8);
    addChildComponent (leftOutView);

    setStep (Step::form);
}

GrooveImportPanel::~GrooveImportPanel()
{
    // Waits for an import still running (see `pool`).
    pool.removeAllJobs (false, 30'000);
}

void GrooveImportPanel::showForm (const juce::File& f)
{
    folder = f;
    nameField.setText (folder.getFileName(), false);

    // General MIDI suits most packs; it's where the menu starts.
    const auto gm = std::find_if (maps.begin(), maps.end(), [] (const auto& m) { return m.first == "general-midi"; });
    mapBox.setSelectedItemIndex (gm != maps.end() ? (int) (gm - maps.begin()) : 0, juce::dontSendNotification);

    setStep (Step::form);
    updateReplaceWarning();
}

juce::String GrooveImportPanel::mapName (const juce::String& id) const
{
    for (const auto& [mapId, name] : maps)
        if (mapId == id)
            return name;

    return id;
}

//==============================================================================
void GrooveImportPanel::setStep (Step s)
{
    using namespace theme;
    step = s;

    switch (step)
    {
        case Step::form:
            setTitle ("Import MIDI grooves");
            setCardSize (formWidth, formHeight);
            getCloseButton().setButtonText ("Cancel");
            break;

        case Step::importing:
            setTitle ("Importing");
            setCardSize (importingWidth, importingHeight);
            break;

        case Step::done:
            setTitle ("Imported " + juce::String (juce::CharPointer_UTF8 ("\xc2\xb7 ")) + imported.name);
            setCardSize (doneWidth, imported.warnings.isEmpty() ? doneHeightNothingLeftOut : doneHeight);
            getCloseButton().setButtonText ("Close");
            primaryButton.setButtonText ("Show grooves");
            break;

        case Step::failed:
            setTitle ("Import failed");
            setCardSize (failedWidth, failedHeight);
            getCloseButton().setButtonText ("Close");
            primaryButton.setButtonText ("Choose folder...");
            break;
    }

    nameField.setVisible (step == Step::form);
    mapBox.setVisible (step == Step::form);
    primaryButton.setVisible (step != Step::importing);
    getCloseButton().setVisible (step != Step::importing);
    leftOutView.setVisible (step == Step::done && ! imported.warnings.isEmpty());

    if (step != Step::form)
    {
        primaryButton.setName (primaryButton.getButtonText());
        primaryButton.setEnabled (true);
        primaryButton.setColour (juce::TextButton::buttonColourId, colours::accent);
        primaryButton.setColour (juce::TextButton::textColourOffId, colours::background);
    }

    if (step == Step::importing)
        startTimerHz (60);
    else
        stopTimer();

    hoveredStyle = -1;
    resized();
    repaint();

    if (step == Step::form && isShowing())
        nameField.grabKeyboardFocus();
}

void GrooveImportPanel::updateReplaceWarning()
{
    using namespace theme;

    const auto name = nameField.getText().trim();
    replacing = name.isNotEmpty()
             && grooves::packFolderFor (Settings::getGroovesFolder(), name).getChildFile ("grooves.json").existsAsFile();

    // Replacing a pack is shown like other destructive buttons: in the warning colour.
    primaryButton.setButtonText (replacing ? "Replace" : "Import");
    primaryButton.setName (primaryButton.getButtonText());   // so tests can find it by its label
    primaryButton.setEnabled (name.isNotEmpty());
    primaryButton.setColour (juce::TextButton::buttonColourId, replacing ? colours::warning : colours::accent);
    primaryButton.setColour (juce::TextButton::textColourOffId, colours::background);
    repaint (warningArea);
}

void GrooveImportPanel::primaryClicked()
{
    switch (step)
    {
        case Step::form:
            if (nameField.getText().trim().isNotEmpty())
                startImport();
            break;

        case Step::done:
        {
            const auto name = imported.name;

            if (onClose) onClose();
            if (onShowPack) onShowPack (name);
            break;
        }

        case Step::failed:
            if (onClose) onClose();
            if (onChooseFolder) onChooseFolder();
            break;

        case Step::importing:
            break;
    }
}

//==============================================================================
void GrooveImportPanel::startImport()
{
    const auto mapIndex = mapBox.getSelectedItemIndex();

    grooves::MidiFolderOptions options;
    options.source = folder;
    options.groovesFolder = Settings::getGroovesFolder();
    options.name = nameField.getText().trim();
    options.noteMap = juce::isPositiveAndBelow (mapIndex, (int) maps.size()) ? maps[(size_t) mapIndex].first : juce::String ("general-midi");
    options.overwrite = true;   // the form said so, with Replace

    importedMap = options.noteMap;
    setStep (Step::importing);

    // On the pool's thread: the import, then the new pack's styles counted
    // from its manifest. The result comes back to the message thread.
    pool.addJob ([safe = juce::Component::SafePointer (this), options]
    {
        grooves::ImportedPack pack;
        const auto result = grooves::importMidiFolder (options, pack);
        std::vector<StyleCount> counts;

        if (grooves::Pack read; result.wasOk() && grooves::readPack (pack.folder, read).wasOk())
        {
            for (const auto& g : read.grooves)
            {
                auto it = std::find_if (counts.begin(), counts.end(), [&g] (const StyleCount& c) { return c.key == g.style; });

                if (it == counts.end())
                    it = counts.insert (counts.end(), { g.style, g.styleName, 0, {} });

                ++it->count;
            }

            std::stable_sort (counts.begin(), counts.end(), [] (const StyleCount& a, const StyleCount& b) { return a.count > b.count; });
        }

        juce::MessageManager::callAsync ([safe, result, pack, counts]
        {
            if (safe != nullptr)
                safe->finished (result, pack, counts);
        });
    });
}

void GrooveImportPanel::finished (juce::Result result, const grooves::ImportedPack& pack, std::vector<StyleCount> counts)
{
    imported = pack;
    styles = std::move (counts);

    if (result.failed())
    {
        error = result.getErrorMessage();
        setStep (Step::failed);
        return;
    }

    leftOut->setLines (pack.warnings);

    if (onImported)
        onImported (pack.folder);

    setStep (Step::done);
}

//==============================================================================
void GrooveImportPanel::layoutContent (juce::Rectangle<int> area)
{
    using namespace theme;

    const auto buttons = getCardBounds().reduced (28, 18).removeFromBottom (30);
    const auto primaryWidth = step == Step::form ? 96 : 132;
    primaryButton.setBounds (buttons.withTrimmedRight (96 + space::sm).removeFromRight (primaryWidth));

    if (step == Step::form)
    {
        messageArea = area.removeFromTop (54);
        area.removeFromTop (space::md);
        nameLabelArea = area.removeFromTop (captionHeight);
        nameField.setBounds (area.removeFromTop (size::button + 4));
        area.removeFromTop (space::lg);
        mapLabelArea = area.removeFromTop (captionHeight);
        mapBox.setBounds (area.removeFromTop (size::button + 4));
        area.removeFromTop (space::xs);
        hintArea = area.removeFromTop (20);
        area.removeFromTop (space::sm);
        warningArea = area.removeFromTop (22);
    }
    else if (step == Step::importing)
    {
        messageArea = area.removeFromTop (60);
    }
    else if (step == Step::done)
    {
        countsArea = area.removeFromTop (66);
        hintArea = area.removeFromTop (20);
        area.removeFromTop (space::md);
        stylesArea = area.removeFromTop (chipHeight * chipRows + space::sm * (chipRows - 1));
        area.removeFromTop (space::lg);
        leftOutLabelArea = area.removeFromTop (captionHeight + space::xs);

        leftOutView.setBounds (area);
        leftOut->setSize (area.getWidth() - leftOutView.getScrollBarThickness(), leftOut->getHeight());

        // The style chips, in rows; what doesn't fit becomes "+N more".
        hiddenStyles = 0;
        auto x = 0.0f;
        auto row = 0;
        const auto width = (float) stylesArea.getWidth();

        for (size_t i = 0; i < styles.size(); ++i)
        {
            const auto w = styleChipWidth (styles[i].name, styles[i].count);

            if (x > 0.0f && x + w > width)
            {
                x = 0.0f;
                ++row;
            }

            if (row >= chipRows)
            {
                hiddenStyles = (int) (styles.size() - i);

                for (auto j = i; j < styles.size(); ++j)
                    styles[j].bounds = {};

                break;
            }

            styles[i].bounds = { (float) stylesArea.getX() + x, (float) (stylesArea.getY() + row * (chipHeight + space::sm)), w, (float) chipHeight };
            x += w + chipGap;
        }

        // "+N more" after the last chip shown, giving up chips from the end
        // until it fits (sized for one more than it says, so it still does).
        moreBounds = {};

        if (hiddenStyles > 0)
        {
            const auto moreWidth = styleChipWidth ("+" + juce::String (styles.size()) + " more", -1) - 20.0f;
            auto shown = styles.size() - (size_t) hiddenStyles;

            while (shown > 0 && styles[shown - 1].bounds.getRight() + chipGap + moreWidth > (float) stylesArea.getRight())
            {
                styles[--shown].bounds = {};
                ++hiddenStyles;
            }

            moreBounds = shown > 0 ? styles[shown - 1].bounds.withX (styles[shown - 1].bounds.getRight() + chipGap).withWidth (moreWidth)
                                   : juce::Rectangle<float> ((float) stylesArea.getX(), (float) stylesArea.getY(), moreWidth, (float) chipHeight);
        }
    }
    else
    {
        messageArea = area;
    }
}

void GrooveImportPanel::paint (juce::Graphics& g)
{
    using namespace theme;

    OverlayPanel::paint (g);

    if (step == Step::form)
    {
        g.setColour (colours::textMuted);
        g.setFont (body (type::body));
        g.drawFittedText ("Every MIDI file in " + folder.getFileName() + " and the folders inside it becomes a groove or a fill, "
                          "sorted by the genre in its folder or file name. The files are copied into your grooves folder.",
                          messageArea, juce::Justification::topLeft, 3, 1.0f);

        drawCaption (g, "Pack name", nameLabelArea);
        drawCaption (g, "Drum notes", mapLabelArea);

        g.setColour (colours::textMuted);
        g.setFont (body (type::small));
        g.drawText ("The MIDI map the files were written for. General MIDI suits most packs.", hintArea, juce::Justification::centredLeft, true);

        if (replacing)
        {
            g.setColour (colours::warning);
            g.setFont (body (type::small, Weight::medium));
            g.drawText ("A groove pack called " + nameField.getText().trim() + " is already installed. Importing replaces it.",
                        warningArea, juce::Justification::centredLeft, true);
        }
    }
    else if (step == Step::importing)
    {
        g.setColour (colours::textMuted);
        g.setFont (body (type::body));
        g.drawFittedText ("Reading the MIDI files in " + folder.getFileName() + " and copying them into your grooves folder. "
                          "A big folder takes a little while.",
                          messageArea, juce::Justification::topLeft, 3, 1.0f);

        // A bar with a segment sliding along it: how long it takes isn't known.
        const auto track = getContentArea().removeFromBottom (6).toFloat();
        g.setColour (colours::raisedHigh);
        g.fillRoundedRectangle (track, 3.0f);

        const auto segment = track.getWidth() * 0.3f;
        const auto x = (float) barPhase * (track.getWidth() + segment) - segment;
        const auto lit = track.withX (track.getX() + x).withWidth (segment).getIntersection (track);
        g.setColour (colours::accent);
        g.fillRoundedRectangle (lit, 3.0f);
    }
    else if (step == Step::done)
    {
        // Grooves, fills, styles: big numbers over their captions.
        const std::pair<int, const char*> counts[] { { imported.numGrooves, "Grooves" }, { imported.numFills, "Fills" },
                                                     { (int) styles.size(), "Styles" } };
        auto columns = countsArea;
        const auto columnWidth = columns.getWidth() / 3;

        for (const auto& [n, caption] : counts)
        {
            auto column = columns.removeFromLeft (columnWidth);
            g.setColour (colours::text);
            g.setFont (mono (30.0f, Weight::medium));
            g.drawText (withThousands (n), column.removeFromTop (40), juce::Justification::bottomLeft, false);
            column.removeFromTop (space::xs);
            drawCaption (g, caption, column.removeFromTop (captionHeight));
        }

        g.setColour (colours::textMuted);
        g.setFont (body (type::small));
        g.drawText ("Read through " + mapName (importedMap) + ", saved in your grooves folder as " + imported.folder.getFileName() + ".",
                    hintArea, juce::Justification::centredLeft, true);

        for (size_t i = 0; i < styles.size(); ++i)
            if (! styles[i].bounds.isEmpty())
                drawStyleChip (g, styles[i].bounds, styles[i].name, styles[i].count, false, (int) i == hoveredStyle);

        if (hiddenStyles > 0)
        {
            g.setColour (colours::textMuted);
            g.setFont (body (13.5f, Weight::medium));
            g.drawText ("+" + juce::String (hiddenStyles) + " more", moreBounds, juce::Justification::centredLeft, false);
        }

        if (imported.warnings.isEmpty())
        {
            g.setColour (colours::textMuted);
            g.setFont (body (type::body));
            g.drawText ("Every MIDI file came in.", leftOutLabelArea.withHeight (24), juce::Justification::centredLeft, true);
        }
        else
        {
            drawCaption (g, "Left out " + juce::String (juce::CharPointer_UTF8 ("\xc2\xb7 ")) + juce::String (imported.warnings.size()),
                         leftOutLabelArea.withTrimmedBottom (space::xs));

            const auto box = leftOutView.getBounds().toFloat().expanded (0.5f);
            g.setColour (colours::background);
            g.fillRoundedRectangle (box, cornerRadius);
            g.setColour (colours::border);
            g.drawRoundedRectangle (box, cornerRadius, 1.0f);
        }
    }
    else
    {
        auto area = messageArea;
        g.setColour (colours::textMuted);
        g.setFont (body (type::body));
        g.drawText ("Nothing was imported from " + folder.getFileName() + ".", area.removeFromTop (22), juce::Justification::centredLeft, true);
        area.removeFromTop (space::sm);
        g.setColour (colours::warning);
        g.drawFittedText (error, area, juce::Justification::topLeft, 5, 1.0f);
    }
}

//==============================================================================
void GrooveImportPanel::timerCallback()
{
    barPhase = std::fmod (barPhase + 1.0 / 90.0, 1.0);   // across in 1.5 s
    repaint (getContentArea().removeFromBottom (6));
}

void GrooveImportPanel::mouseDown (const juce::MouseEvent& e)
{
    // While importing, the card stays: the files are being copied.
    if (step == Step::importing)
        return;

    if (step == Step::done)
    {
        for (const auto& s : styles)
        {
            if (s.bounds.contains (e.position))
            {
                const auto key = s.key;

                if (onClose) onClose();
                if (onShowStyle) onShowStyle (key);
                return;
            }
        }
    }

    OverlayPanel::mouseDown (e);
}

void GrooveImportPanel::mouseMove (const juce::MouseEvent& e)
{
    auto hovered = -1;

    if (step == Step::done)
        for (size_t i = 0; i < styles.size(); ++i)
            if (styles[i].bounds.contains (e.position))
                hovered = (int) i;

    if (hovered != hoveredStyle)
    {
        hoveredStyle = hovered;
        setMouseCursor (hovered >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint (stylesArea.expanded (2));
    }
}

void GrooveImportPanel::mouseExit (const juce::MouseEvent&)
{
    if (hoveredStyle >= 0)
    {
        hoveredStyle = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint (stylesArea.expanded (2));
    }
}

void GrooveImportPanel::focusGained (FocusChangeType)
{
    // The main view gives the card the focus when it opens; the form passes it to the name.
    if (step == Step::form && nameField.isVisible())
        nameField.grabKeyboardFocus();
}

bool GrooveImportPanel::keyPressed (const juce::KeyPress& key)
{
    if (step == Step::importing)
        return true;   // Escape doesn't close it either

    if (key == juce::KeyPress::returnKey && step != Step::failed)
    {
        primaryClicked();
        return true;
    }

    return OverlayPanel::keyPressed (key);
}

} // namespace lithium
