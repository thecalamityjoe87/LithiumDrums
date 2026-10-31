#include "InfoPanels.h"

#include "Theme.h"

namespace lithium
{

//==============================================================================
OverlayPanel::OverlayPanel (juce::String t, int width, int height)
    : title (std::move (t)), cardWidth (width), cardHeight (height)
{
    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);
    setWantsKeyboardFocus (true);
}

juce::Rectangle<int> OverlayPanel::getCardBounds() const
{
    return getLocalBounds().withSizeKeepingCentre (cardWidth, cardHeight);
}

juce::Rectangle<int> OverlayPanel::getContentArea() const
{
    return getCardBounds().reduced (28, 0).withTrimmedTop (70).withTrimmedBottom (64);
}

void OverlayPanel::paint (juce::Graphics& g)
{
    using namespace theme;

    g.fillAll (colours::background.withAlpha (0.78f));

    const auto card = getCardBounds().toFloat();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (card, 10.0f);
    g.setColour (colours::borderStrong);
    g.drawRoundedRectangle (card.reduced (0.5f), 10.0f, 1.0f);

    g.setColour (colours::text);
    drawTrackedText (g, title.toUpperCase(), label (20.0f, Weight::bold), 0.12f,
                     card.reduced (28.0f, 0.0f).withTrimmedTop (24.0f).withHeight (28.0f), juce::Justification::centredLeft);
}

void OverlayPanel::resized()
{
    closeButton.setBounds (getCardBounds().reduced (28, 18).removeFromBottom (30).removeFromRight (96));
    layoutContent (getContentArea());
}

void OverlayPanel::mouseDown (const juce::MouseEvent& e)
{
    if (! getCardBounds().contains (e.getPosition()) && onClose)
        onClose();
}

bool OverlayPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && onClose)
    {
        onClose();
        return true;
    }

    return false;
}

//==============================================================================
/** One kit's credits: name, author, credits text, licence and links. */
class AboutPanel::KitCard final : public juce::Component
{
public:
    static constexpr int height = 156;

    explicit KitCard (const kitlibrary::Entry& entry) : credits (entry.credits)
    {
        auto setUpLink = [this] (juce::HyperlinkButton& link, const juce::String& text, const juce::String& url)
        {
            link.setButtonText (text);
            link.setURL (juce::URL (url));
            link.setFont (theme::body (13.0f), false, juce::Justification::centredLeft);
            link.setColour (juce::HyperlinkButton::textColourId, theme::colours::accent);
            link.setTooltip (url);
            link.setVisible (url.isNotEmpty());
            addChildComponent (link);
        };

        setUpLink (sourceLink, "Source", credits.sourceUrl);
        setUpLink (licenseLink, credits.licenseName.isNotEmpty() ? credits.licenseName : juce::String ("Licence"),
                   credits.licenseUrl);

        licenseFileButton.setButtonText ("Licence file");
        licenseFileButton.setTooltip (credits.licenseFile.getFullPathName());
        licenseFileButton.onClick = [file = credits.licenseFile] { file.startAsProcess(); };
        licenseFileButton.setVisible (credits.licenseFile.existsAsFile());
        addChildComponent (licenseFileButton);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;

        g.setColour (colours::raised);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
        g.setColour (colours::border);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 8.0f, 1.0f);

        auto area = getLocalBounds().reduced (16, 12);

        g.setColour (colours::text);
        g.setFont (body (17.0f, Weight::semibold));
        g.drawText (credits.name, area.removeFromTop (24), juce::Justification::centredLeft, true);

        g.setColour (colours::textMuted);
        g.setFont (body (13.5f));
        g.drawText ("by " + (credits.author.isNotEmpty() ? credits.author : juce::String ("unknown author")),
                    area.removeFromTop (20), juce::Justification::centredLeft, true);

        area.removeFromTop (6);
        g.setColour (colours::text.withAlpha (0.85f));
        g.setFont (body (13.0f));
        g.drawFittedText (credits.credits.isNotEmpty() ? credits.credits : credits.description,
                          area.removeFromTop (54), juce::Justification::topLeft, 3, 1.0f);
    }

    void resized() override
    {
        auto row = getLocalBounds().reduced (16, 12).removeFromBottom (24);
        licenseLink.setBounds (row.removeFromLeft (110));
        row.removeFromLeft (8);
        sourceLink.setBounds (row.removeFromLeft (80));
        licenseFileButton.setBounds (row.removeFromRight (110));
    }

private:
    const KitCredits credits;
    juce::HyperlinkButton sourceLink, licenseLink;
    juce::TextButton licenseFileButton;
};

AboutPanel::AboutPanel (LithiumDrumsProcessor& p) : OverlayPanel ("About Lithium Drums", 760, 640), processor (p)
{
    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (8);
    viewport.getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, theme::colours::borderStrong);
    addAndMakeVisible (viewport);
}

void AboutPanel::refresh()
{
    cards.clear();

    // About the plugin, then one card per installed kit.
    struct Intro final : public juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            using namespace theme;
            auto area = getLocalBounds();

            g.setColour (colours::text);
            g.setFont (body (14.0f));
            g.drawFittedText ("Lithium Drums " LITHIUM_VERSION_STRING ". The kits below are recordings released under "
                              "open licences by their authors. Credit them as shown when you publish music made with them.",
                              area.removeFromTop (40), juce::Justification::topLeft, 2, 1.0f);

            g.setColour (colours::textMuted);
            g.setFont (body (12.5f));
            g.drawFittedText ("Fonts: Barlow, Barlow Condensed and IBM Plex Mono, under the SIL Open Font License. "
                              "Built with JUCE.",
                              area.removeFromTop (34), juce::Justification::topLeft, 2, 1.0f);
        }
    };

    cards.push_back (std::make_unique<Intro>());

    const auto kits = kitlibrary::findKits (processor.getKitsFolder());

    for (const auto& k : kits)
        cards.push_back (std::make_unique<KitCard> (k));

    if (kits.empty())
    {
        struct Empty final : public juce::Component
        {
            void paint (juce::Graphics& g) override
            {
                g.setColour (theme::colours::textMuted);
                g.setFont (theme::body (14.0f));
                g.drawFittedText ("No kits are installed. The README explains where to download kits and how to import them.",
                                  getLocalBounds(), juce::Justification::centredLeft, 2, 1.0f);
            }
        };

        cards.push_back (std::make_unique<Empty>());
    }

    for (auto& c : cards)
        list.addAndMakeVisible (*c);

    resized();
}

void AboutPanel::layoutContent (juce::Rectangle<int> area)
{
    viewport.setBounds (area);

    constexpr int gap = 10, introHeight = 84;
    const auto width = area.getWidth() - 12;   // room for the scroll bar
    int y = 0;

    for (size_t i = 0; i < cards.size(); ++i)
    {
        const auto h = i == 0 ? introHeight : (dynamic_cast<KitCard*> (cards[i].get()) != nullptr ? KitCard::height : 40);
        cards[i]->setBounds (0, y, width, h);
        y += h + gap;
    }

    list.setSize (width, std::max (0, y - gap));
}

//==============================================================================
SettingsPanel::SettingsPanel (LithiumDrumsProcessor& p) : OverlayPanel ("Settings", 640, 300), processor (p)
{
    folderLabel.setFont (theme::mono (13.0f));
    folderLabel.setColour (juce::Label::textColourId, theme::colours::text);
    folderLabel.setColour (juce::Label::backgroundColourId, theme::colours::raised);
    folderLabel.setColour (juce::Label::outlineColourId, theme::colours::border);
    folderLabel.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (folderLabel);

    changeButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose the folder your kits are in", processor.getKitsFolder());

        // Plugins can't run modal loops, so the dialog is asynchronous. The
        // SafePointer guards against the panel being deleted before it returns.
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safeThis = juce::Component::SafePointer<SettingsPanel> (this)] (const juce::FileChooser& fc)
                              {
                                  if (safeThis != nullptr && fc.getResult().isDirectory())
                                      safeThis->setFolder (fc.getResult());
                              });
    };

    openButton.onClick = [this]
    {
        const auto folder = processor.getKitsFolder();
        folder.createDirectory();
        folder.revealToUser();
    };

    defaultButton.onClick = [this] { setFolder (Settings::getDefaultKitsFolder()); };

    addAndMakeVisible (changeButton);
    addAndMakeVisible (openButton);
    addAndMakeVisible (defaultButton);
}

void SettingsPanel::refresh()
{
    folderLabel.setText (" " + processor.getKitsFolder().getFullPathName(), juce::dontSendNotification);
}

void SettingsPanel::setFolder (const juce::File& folder)
{
    processor.setKitsFolder (folder);
    refresh();

    if (onKitsFolderChanged)
        onKitsFolderChanged();
}

void SettingsPanel::paint (juce::Graphics& g)
{
    OverlayPanel::paint (g);

    using namespace theme;
    auto area = textArea;

    g.setColour (colours::textMuted);
    drawTrackedText (g, "KITS FOLDER", label (12.0f), 0.1f, area.removeFromTop (18).toFloat(), juce::Justification::centredLeft);

    g.setFont (body (13.0f));
    g.drawFittedText ("Lithium Drums lists the kits in this folder, one subfolder per kit. Import DrumGizmo kits "
                      "into it with the LithiumImport tool (see the README). Every instance uses the same folder.",
                      area.withTrimmedTop (92).withHeight (40), juce::Justification::topLeft, 2, 1.0f);
}

void SettingsPanel::layoutContent (juce::Rectangle<int> area)
{
    textArea = area;
    area.removeFromTop (24);
    folderLabel.setBounds (area.removeFromTop (30));
    area.removeFromTop (10);

    auto buttons = area.removeFromTop (28);
    changeButton.setBounds (buttons.removeFromLeft (100));
    buttons.removeFromLeft (8);
    openButton.setBounds (buttons.removeFromLeft (110));
    buttons.removeFromLeft (8);
    defaultButton.setBounds (buttons.removeFromLeft (110));
}

} // namespace lithium
