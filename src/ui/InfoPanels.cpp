#include "InfoPanels.h"

#include "Logo.h"
#include "Theme.h"
#include "engine/StreamCache.h"
#include "grooves/GrooveLibrary.h"

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
PromptPanel::PromptPanel() : OverlayPanel ({}, 460, 236)
{
    using namespace theme;

    field.setFont (body (type::body));
    field.setIndents (10, 7);
    field.onReturnKey = [this] { finish (true); };
    field.onEscapeKey = [this] { finish (false); };
    addChildComponent (field);

    okButton.onClick = [this] { finish (true); };
    addAndMakeVisible (okButton);

    getCloseButton().setButtonText ("Cancel");
    onClose = [this] { finish (false); };
}

void PromptPanel::show (Request r)
{
    request = std::move (r);
    setTitle (request.title);
    okButton.setButtonText (request.okText);
    okButton.setColour (juce::TextButton::buttonColourId, request.destructive ? theme::colours::warning : theme::colours::accent);
    okButton.setColour (juce::TextButton::textColourOffId, theme::colours::background);
    getCloseButton().setVisible (request.cancellable);
    field.setVisible (request.withTextField);
    field.setText (request.text, false);
    field.selectAll();
    resized();
    repaint();
}

void PromptPanel::finish (bool ok)
{
    // Copied first: onOk may show another request, which replaces this one.
    auto callback = ok ? request.onOk : nullptr;
    const auto text = field.getText().trim();

    if (onFinished)
        onFinished();

    if (callback != nullptr && (! request.withTextField || text.isNotEmpty()))
        callback (text);
}

bool PromptPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey)
    {
        finish (true);
        return true;
    }

    return OverlayPanel::keyPressed (key);
}

void PromptPanel::layoutContent (juce::Rectangle<int> area)
{
    using namespace theme;

    const auto buttons = getCardBounds().reduced (28, 18).removeFromBottom (30);
    okButton.setBounds (buttons.withTrimmedRight (request.cancellable ? 96 + space::sm : 0).removeFromRight (96));

    if (request.withTextField)
        field.setBounds (area.removeFromBottom (size::button + 4));

    messageArea = area.withTrimmedBottom (space::sm);
}

void PromptPanel::paint (juce::Graphics& g)
{
    using namespace theme;

    OverlayPanel::paint (g);
    g.setColour (colours::textMuted);
    g.setFont (body (type::body));
    g.drawFittedText (request.message, messageArea, juce::Justification::topLeft, 4, 1.0f);
}

//==============================================================================
namespace
{
    // About's layout, relative to the card.
    constexpr int aboutPadding = 32;
    constexpr int iconSize = 84;
    constexpr int tabsTop = aboutPadding + iconSize + 22;
    constexpr int tabsHeight = 34;

    juce::AttributedString wrapped (const juce::String& text, juce::Font font, juce::Colour colour)
    {
        juce::AttributedString s;
        s.append (text, font, colour);
        s.setWordWrap (juce::AttributedString::byWord);
        s.setLineSpacing (2.0f);
        return s;
    }

    /** How tall `text` wraps to at `width`. */
    int heightOf (const juce::String& text, juce::Font font, int width)
    {
        if (text.isEmpty())
            return 0;

        juce::TextLayout layout;
        layout.createLayout (wrapped (text, font, {}), (float) width);
        return (int) std::ceil (layout.getHeight());
    }

    /** Draws wrapped text at the top of `area`; returns how tall it was. */
    int drawWrapped (juce::Graphics& g, const juce::String& text, juce::Font font, juce::Colour colour, juce::Rectangle<int> area)
    {
        if (text.isEmpty())
            return 0;

        juce::TextLayout layout;
        layout.createLayout (wrapped (text, font, colour), (float) area.getWidth());
        layout.draw (g, area.toFloat().withHeight (layout.getHeight()));
        return (int) std::ceil (layout.getHeight());
    }

    /** A small uppercase heading with a rule under it, as the Mixer and FX pages label their sections. */
    void drawHeading (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area)
    {
        using namespace theme;
        g.setColour (colours::textMuted);
        drawTrackedText (g, text.toUpperCase(), label (13.0f, Weight::semibold), 0.12f,
                         area.toFloat().withTrimmedBottom (6.0f), juce::Justification::bottomLeft);
        g.setColour (colours::border);
        g.fillRect (area.removeFromBottom (1));
    }

    juce::Font linkFont() { return theme::body (13.0f); }

    /** A line of muted text in the Credits list. */
    struct Note final : public juce::Component
    {
        explicit Note (juce::String t) : text (std::move (t)) {}

        int heightFor (int width) const { return heightOf (text, font(), width) + 16; }

        void paint (juce::Graphics& g) override
        {
            drawWrapped (g, text, font(), theme::colours::textMuted, getLocalBounds().withTrimmedTop (4));
        }

        static juce::Font font() { return theme::body (14.0f); }
        const juce::String text;
    };

    int linkWidth (const juce::HyperlinkButton& link)
    {
        return (int) std::ceil (theme::getTrackedTextWidth (link.getButtonText(), linkFont(), 0.0f)) + 2;
    }
}

//==============================================================================
/** About / Credits, drawn like the header's page tabs. */
class AboutPanel::TabButton final : public juce::Button
{
public:
    explicit TabButton (const juce::String& name) : juce::Button (name)
    {
        setClickingTogglesState (true);
        setRadioGroupId (1);
    }

    int getIdealWidth() const
    {
        return juce::roundToInt (theme::getTrackedTextWidth (getName().toUpperCase(), font(), tracking)) + 22;
    }

    void paintButton (juce::Graphics& g, bool isMouseOver, bool) override
    {
        const auto& textColour = theme::colours::text;   // inside a Button, plain "text" would be Button::text
        const auto active = getToggleState();

        g.setColour (active ? textColour : (isMouseOver ? textColour.withAlpha (0.85f) : theme::colours::textMuted));
        theme::drawTrackedText (g, getName().toUpperCase(), font(), tracking, getLocalBounds().toFloat(), juce::Justification::centred);

        if (active)
        {
            g.setColour (theme::colours::accent);
            g.fillRect (getLocalBounds().removeFromBottom (2).reduced (10, 0));
        }
    }

private:
    static constexpr float tracking = 0.1f;
    static juce::Font font() { return theme::label (15.0f, theme::Weight::semibold); }
};

//==============================================================================
/** The About tab: what Lithium Drums is, its licence, and what it's built with. */
class AboutPanel::AboutTab final : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        auto area = getLocalBounds();

        area.removeFromTop (drawWrapped (g, "Lithium Drums is a sampled acoustic drum kit. It plays real multi-mic recordings through "
                                            "a mixer and effects, and plays recorded grooves in time with your host. Use it as a VST3, "
                                            "CLAP or AU plugin, or on its own as a standalone app.",
                                         body (15.0f), colours::text, area));
        area.removeFromTop (18);

        drawHeading (g, "Licence", area.removeFromTop (26));
        area.removeFromTop (10);
        // The GPL family asks interactive programs to show a copyright and
        // no-warranty notice; this is it.
        area.removeFromTop (drawWrapped (g, "Copyright (C) 2026 " LITHIUM_AUTHOR_STRING ". Lithium Drums is free software: you can "
                                            "redistribute and change it under the terms of the GNU Affero General Public License, "
                                            "version 3. It comes with no warranty.",
                                         body (14.0f), colours::text.withAlpha (0.85f), area));
        area.removeFromTop (18);

        drawHeading (g, "Built with", area.removeFromTop (26));
        area.removeFromTop (8);

        static const std::pair<const char*, const char*> parts[] {
            { "JUCE", "GNU AGPLv3" },
            { "VST3 SDK (Steinberg)", "MIT" },
            { "CLAP and clap-juce-extensions", "MIT" },
            { "LZMA SDK (Igor Pavlov)", "Public domain" },
            { "Barlow and Barlow Condensed fonts", "SIL Open Font License" },
            { "IBM Plex Mono font", "SIL Open Font License" },
        };

        for (const auto& [name, licence] : parts)
        {
            auto row = area.removeFromTop (22);
            g.setColour (colours::text);
            g.setFont (body (14.0f));
            g.drawText (name, row, juce::Justification::centredLeft, true);
            g.setColour (colours::textMuted);
            g.setFont (mono (12.5f));
            g.drawText (licence, row.withTrimmedLeft (row.getWidth() / 2), juce::Justification::centredLeft, true);
        }

        area.removeFromTop (6);
        g.setColour (colours::textMuted);
        g.setFont (body (12.5f));
        g.drawText ("Their full notices are in THIRD-PARTY-NOTICES.txt, installed with Lithium Drums.",
                    area.removeFromTop (20), juce::Justification::centredLeft, true);
    }
};

//==============================================================================
/** A heading in the Credits list. */
class AboutPanel::SectionHeading final : public juce::Component
{
public:
    explicit SectionHeading (juce::String t) : text (std::move (t)) {}

    void paint (juce::Graphics& g) override  { drawHeading (g, text, getLocalBounds()); }

private:
    const juce::String text;
};

//==============================================================================
/** One credit: a kit pack, groove pack or reverb IR. Its name with the
    licence and source links on the same line, who made it, what's in it,
    and the authors' credits text. Rows are as tall as their text. */
class AboutPanel::CreditRow final : public juce::Component
{
public:
    struct Content
    {
        juce::String title, author, contents, credits, licenseName, licenseUrl, sourceUrl;
        juce::File licenseFile;
    };

    explicit CreditRow (Content c) : content (std::move (c))
    {
        auto setUp = [this] (juce::HyperlinkButton& link, const juce::String& text, const juce::String& url, bool visible)
        {
            link.setButtonText (text);
            link.setURL (juce::URL (url));
            link.setFont (linkFont(), false, juce::Justification::centredRight);
            link.setColour (juce::HyperlinkButton::textColourId, theme::colours::accent);
            link.setTooltip (url);
            link.setVisible (visible);
            addChildComponent (link);
        };

        setUp (licenseLink, content.licenseName.isNotEmpty() ? content.licenseName : juce::String ("Licence"),
               content.licenseUrl, content.licenseUrl.isNotEmpty());
        setUp (sourceLink, "Source", content.sourceUrl, content.sourceUrl.isNotEmpty());

        // The licence text that came with the download, opened in the
        // system's text viewer.
        setUp (licenseFileLink, "Licence file", {}, content.licenseFile.existsAsFile());
        licenseFileLink.setTooltip (content.licenseFile.getFullPathName());
        licenseFileLink.onClick = [file = content.licenseFile] { file.startAsProcess(); };
    }

    int heightFor (int width) const
    {
        return padding + 22 + 18 + gapBefore (content.contents) + heightOf (content.contents, contentsFont(), width)
               + gapBefore (content.credits) + heightOf (content.credits, creditsFont(), width) + padding;
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        auto area = getLocalBounds().withTrimmedTop (padding);

        g.setColour (colours::text);
        g.setFont (body (15.5f, Weight::semibold));
        g.drawText (content.title, area.removeFromTop (22).withTrimmedRight (linksWidth + 16), juce::Justification::centredLeft, true);

        g.setColour (colours::textMuted);
        g.setFont (body (13.5f));
        g.drawText ("by " + (content.author.isNotEmpty() ? content.author : juce::String ("unknown author")),
                    area.removeFromTop (18), juce::Justification::centredLeft, true);

        area.removeFromTop (gapBefore (content.contents));
        area.removeFromTop (drawWrapped (g, content.contents, contentsFont(), colours::textMuted, area));
        area.removeFromTop (gapBefore (content.credits));
        drawWrapped (g, content.credits, creditsFont(), colours::text.withAlpha (0.85f), area);

        g.setColour (colours::border);
        g.fillRect (getLocalBounds().removeFromBottom (1));

        for (const auto& dot : dots)
        {
            g.setColour (colours::textMuted);
            g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre (dot));
        }
    }

    void resized() override
    {
        // The links right-aligned on the title's line, a dot between each.
        auto row = getLocalBounds().withTrimmedTop (padding).removeFromTop (22);
        dots.clear();
        linksWidth = 0;
        bool first = true;

        for (auto* link : { &licenseFileLink, &sourceLink, &licenseLink })
        {
            if (! link->isVisible())
                continue;

            if (! first)
            {
                dots.push_back ({ (float) row.getRight() - 7.0f, (float) row.getCentreY() + 1.0f });
                row.removeFromRight (14);
            }

            link->setBounds (row.removeFromRight (linkWidth (*link)));
            first = false;
        }

        linksWidth = getWidth() - row.getRight();
    }

private:
    static constexpr int padding = 14;
    static juce::Font contentsFont() { return theme::body (13.0f); }
    static juce::Font creditsFont()  { return theme::body (13.5f); }
    static int gapBefore (const juce::String& text) { return text.isNotEmpty() ? 6 : 0; }

    const Content content;
    juce::HyperlinkButton licenseLink, sourceLink, licenseFileLink;
    std::vector<juce::Point<float>> dots;
    int linksWidth = 0;
};

//==============================================================================
AboutPanel::AboutPanel (LithiumDrumsProcessor& p)
    : OverlayPanel ({}, 760, 640),   // no title: the brand block is the heading
      processor (p),
      icon (logo::renderIcon (iconSize * 2)),
      aboutTabButton (std::make_unique<TabButton> ("About")),
      creditsTabButton (std::make_unique<TabButton> ("Credits")),
      aboutTab (std::make_unique<AboutTab>())
{
    aboutTabButton->onClick = [this] { showCredits (false); };
    creditsTabButton->onClick = [this] { showCredits (true); };
    addAndMakeVisible (*aboutTabButton);
    addAndMakeVisible (*creditsTabButton);

    websiteButton.setTooltip (LITHIUM_WEBSITE_STRING);
    websiteButton.onClick = [] { juce::URL (LITHIUM_WEBSITE_STRING).launchInDefaultBrowser(); };
    licenseButton.setTooltip ("The GNU Affero General Public License, version 3");
    licenseButton.onClick = [] { juce::URL ("https://www.gnu.org/licenses/agpl-3.0.html").launchInDefaultBrowser(); };
    addAndMakeVisible (websiteButton);
    addAndMakeVisible (licenseButton);

    addChildComponent (*aboutTab);
    creditsView.setViewedComponent (&creditsList, false);
    creditsView.setScrollBarsShown (true, false);
    creditsView.setScrollBarThickness (8);
    creditsView.getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, theme::colours::borderStrong);
    addChildComponent (creditsView);

    showCredits (false);
}

AboutPanel::~AboutPanel() = default;

void AboutPanel::showCredits (bool credits)
{
    aboutTabButton->setToggleState (! credits, juce::dontSendNotification);
    creditsTabButton->setToggleState (credits, juce::dontSendNotification);
    aboutTab->setVisible (! credits);
    creditsView.setVisible (credits);
}

void AboutPanel::paint (juce::Graphics& g)
{
    using namespace theme;
    OverlayPanel::paint (g);

    const auto card = getCardBounds();
    const auto iconArea = juce::Rectangle<int> (iconSize, iconSize).withPosition (card.getPosition() + juce::Point<int> (aboutPadding, aboutPadding));
    g.drawImage (icon, iconArea.toFloat());

    // The wordmark, as in the header, then the version and who made it.
    auto text = card.withTrimmedLeft (iconArea.getRight() - card.getX() + 22).withTrimmedTop (aboutPadding + 2).toFloat();
    const auto bold = label (30.0f, Weight::bold);
    const auto lithiumWidth = getTrackedTextWidth ("LITHIUM", bold, 0.14f);
    const auto wordmarkRow = text.removeFromTop (34.0f);
    g.setColour (colours::text);
    drawTrackedText (g, "LITHIUM", bold, 0.14f, wordmarkRow, juce::Justification::centredLeft);
    g.setColour (colours::textMuted);
    drawTrackedText (g, "DRUMS", label (30.0f, Weight::semibold), 0.14f, wordmarkRow.withTrimmedLeft (lithiumWidth + 8.0f),
                     juce::Justification::centredLeft);

    g.setFont (mono (13.0f));
    g.drawText ("Version " LITHIUM_VERSION_STRING, text.removeFromTop (22.0f), juce::Justification::centredLeft, false);

    // "by Isaac Joseph · Calamity Audio": the name in full colour, the brand muted.
    const auto byLine = text.removeFromTop (22.0f);
    const juce::String by = "by " LITHIUM_AUTHOR_STRING;
    const auto byWidth = getTrackedTextWidth (by, body (15.0f), 0.0f);
    g.setColour (colours::text);
    g.setFont (body (15.0f));
    g.drawText (by, byLine, juce::Justification::centredLeft, false);
    g.setColour (colours::textMuted);
    g.drawText (juce::String (juce::CharPointer_UTF8 ("\xc2\xb7 ")) + LITHIUM_COMPANY_STRING,
                byLine.withTrimmedLeft (byWidth + 4.5f), juce::Justification::centredLeft, false);

    // The rule under the tabs.
    g.setColour (colours::border);
    g.fillRect (card.getX() + aboutPadding, card.getY() + tabsTop + tabsHeight, card.getWidth() - 2 * aboutPadding, 1);
}

void AboutPanel::refresh()
{
    creditRows.clear();

    auto add = [this] (std::unique_ptr<juce::Component> c) { creditRows.push_back (std::move (c)); };
    auto heading = [&] (const juce::String& text) { add (std::make_unique<SectionHeading> (text)); };

    add (std::make_unique<Note> ("These recordings are their authors' work, released under open licences. "
                                 "Credit them as shown when you publish music made with them."));

    // Kits: one row per content pack, naming its kits and presets; kits
    // outside a pack get a row each.
    heading ("Kits");
    const auto library = kitlibrary::scan (processor.getKitsFolder());
    const auto& kits = library.kits;

    for (size_t p = 0; p < library.packs.size(); ++p)
    {
        const auto& pack = library.packs[p];
        juce::StringArray kitNames, presetNames;

        for (const auto& k : kits)
            if (k.pack == (int) p)
                (k.summary.isPreset ? presetNames : kitNames).add (k.credits().name);

        if (kitNames.isEmpty() && presetNames.isEmpty())
            continue;

        juce::String contents = (kitNames.size() == 1 ? "Kit: " : "Kits: ") + kitNames.joinIntoString (", ");

        if (! presetNames.isEmpty())
            contents << ".  " << (presetNames.size() == 1 ? "Preset: " : "Presets: ") << presetNames.joinIntoString (", ");

        add (std::make_unique<CreditRow> (CreditRow::Content {
            pack.name, pack.author, contents, pack.credits.isNotEmpty() ? pack.credits : pack.description,
            pack.licenseName, pack.licenseUrl, pack.sourceUrl, pack.licenseFile }));
    }

    for (const auto& k : kits)
    {
        if (k.pack >= 0)
            continue;

        const auto& c = k.credits();
        add (std::make_unique<CreditRow> (CreditRow::Content {
            c.name, c.author, c.basedOn.isNotEmpty() ? "Preset of " + c.basedOn : juce::String(),
            c.credits.isNotEmpty() ? c.credits : c.description, c.licenseName, c.licenseUrl, c.sourceUrl, c.licenseFile }));
    }

    if (kits.empty())
        add (std::make_unique<Note> ("No kits are installed yet. Open Browse, in the header, to download one."));

    // Groove packs need crediting too: the grooves dragged into a project are their drummers' playing.
    const auto groovePacks = grooves::scan (Settings::getGroovesFolder()).packs;

    if (! groovePacks.empty())
        heading ("Grooves");

    for (const auto& pack : groovePacks)
    {
        int fills = 0;

        for (const auto& gr : pack.grooves)
            fills += (int) gr.fills.size();

        add (std::make_unique<CreditRow> (CreditRow::Content {
            pack.name, pack.author, juce::String ((int) pack.grooves.size()) + " grooves and " + juce::String (fills) + " fills",
            pack.credits.isNotEmpty() ? pack.credits : pack.description, pack.licenseName, pack.licenseUrl,
            pack.sourceUrl, pack.licenseFile }));
    }

    // The Room reverb's impulse responses: the measured ones each with their
    // attribution and what was changed (as CC BY asks), the synthesized ones together.
    heading ("Room reverb impulse responses");
    {
        juce::StringArray synthesized;
        const IrInfo* cc0 = nullptr;

        for (const auto& ir : irs::builtIn())
        {
            if (ir.sourceUrl.isEmpty())
            {
                synthesized.add (ir.name);
                cc0 = &ir;
                continue;
            }

            add (std::make_unique<CreditRow> (CreditRow::Content {
                ir.name, ir.author, ir.source, ir.description + " Changes: " + ir.changes,
                ir.licenseName, ir.licenseUrl, ir.sourceUrl, {} }));
        }

        if (cc0 != nullptr)
            add (std::make_unique<CreditRow> (CreditRow::Content {
                synthesized.joinIntoString (", "), cc0->author, "Synthesized for Lithium Drums by tools/irgen",
                "Image-source early reflections and a band-split noise tail. Free to use for anything.",
                cc0->licenseName, cc0->licenseUrl, {}, {} }));
    }

    for (auto& c : creditRows)
        creditsList.addAndMakeVisible (*c);

    resized();
}

void AboutPanel::layoutContent (juce::Rectangle<int>)
{
    const auto card = getCardBounds();

    // The links at the right of the brand block, level with the version line.
    auto links = juce::Rectangle<int> (card.getRight() - aboutPadding - 236, card.getY() + aboutPadding + 26, 236, 30);
    licenseButton.setBounds (links.removeFromRight (112));
    links.removeFromRight (12);
    websiteButton.setBounds (links.removeFromRight (112));

    auto tabs = juce::Rectangle<int> (card.getX() + aboutPadding - 11, card.getY() + tabsTop, 300, tabsHeight);
    aboutTabButton->setBounds (tabs.removeFromLeft (aboutTabButton->getIdealWidth()));
    tabs.removeFromLeft (6);
    creditsTabButton->setBounds (tabs.removeFromLeft (creditsTabButton->getIdealWidth()));

    // The tabs' content: under the tabs, above the Close button.
    const auto content = card.reduced (aboutPadding, 0).withTop (card.getY() + tabsTop + tabsHeight + 20).withTrimmedBottom (64);
    aboutTab->setBounds (content);
    creditsView.setBounds (content.withTrimmedRight (-12));   // the scroll bar sits in the padding

    const auto width = content.getWidth();
    int y = 0;

    for (size_t i = 0; i < creditRows.size(); ++i)
    {
        auto* c = creditRows[i].get();
        int h = 34;   // a section heading

        if (auto* row = dynamic_cast<CreditRow*> (c))
            h = row->heightFor (width);
        else if (auto* note = dynamic_cast<Note*> (c))
            h = note->heightFor (width);
        else if (i > 0)
            y += 14;   // space above each heading

        c->setBounds (0, y, width, h);
        y += h;
    }

    creditsList.setSize (width, y);
}

//==============================================================================
namespace
{
    juce::String describeBytes (std::int64_t bytes)
    {
        const auto mb = (double) bytes / (1024.0 * 1024.0);
        return mb >= 1000.0 ? juce::String (mb / 1024.0, 2) + " GB" : juce::String (juce::roundToInt (mb)) + " MB";
    }

    void styleLabel (juce::Label& l)
    {
        l.setFont (theme::mono (13.0f));
        l.setColour (juce::Label::textColourId, theme::colours::text);
        l.setColour (juce::Label::backgroundColourId, theme::colours::raised);
        l.setColour (juce::Label::outlineColourId, theme::colours::border);
        l.setMinimumHorizontalScale (0.6f);
    }

    constexpr std::pair<StreamingMode, const char*> modeItems[] {
        { StreamingMode::automatic, "Large kits (over 512 MB)" },
        { StreamingMode::always, "Every kit" },
        { StreamingMode::never, "Never: load kits fully into RAM" },
    };
}

SettingsPanel::SettingsPanel (LithiumDrumsProcessor& p) : OverlayPanel ("Settings", 640, 610), processor (p)
{
    styleLabel (folderLabel);
    styleLabel (cacheLabel);
    addAndMakeVisible (folderLabel);
    addAndMakeVisible (cacheLabel);

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

    for (const auto& [mode, name] : modeItems)
        modeBox.addItem (name, (int) mode + 1);

    for (const auto frames : Settings::preloadChoices)
        preloadBox.addItem (juce::String (frames) + " frames (" + juce::String (juce::roundToInt (frames * 1000.0 / 44100.0))
                              + " ms at 44.1 kHz)", frames);

    modeBox.onChange = [this]
    {
        const auto mode = (StreamingMode) (modeBox.getSelectedId() - 1);

        if (modeBox.getSelectedId() > 0 && mode != processor.getStreamingSettings().mode)
            processor.setStreamingMode (mode);

        preloadBox.setEnabled (mode != StreamingMode::never);
    };

    preloadBox.onChange = [this]
    {
        if (const auto frames = preloadBox.getSelectedId(); frames > 0 && frames != processor.getStreamingSettings().preloadFrames)
            processor.setPreloadFrames (frames);
    };

    clearCacheButton.onClick = [this]
    {
        processor.clearStreamCache();
        cacheSummary = {};
        updateStatus();
    };

    openCacheButton.onClick = [this]
    {
        const auto folder = processor.getStreamingSettings().cacheFolder;
        folder.createDirectory();
        folder.revealToUser();
    };

    for (auto* c : std::initializer_list<juce::Component*> { &changeButton, &openButton, &defaultButton, &modeBox,
                                                             &preloadBox, &clearCacheButton, &openCacheButton })
        addAndMakeVisible (c);
}

void SettingsPanel::refresh()
{
    folderLabel.setText (" " + processor.getKitsFolder().getFullPathName(), juce::dontSendNotification);

    const auto streaming = processor.getStreamingSettings();
    modeBox.setSelectedId ((int) streaming.mode + 1, juce::dontSendNotification);
    preloadBox.setSelectedId (streaming.preloadFrames, juce::dontSendNotification);
    preloadBox.setEnabled (streaming.mode != StreamingMode::never);
    cacheLabel.setText (" " + streaming.cacheFolder.getFullPathName(), juce::dontSendNotification);

    statusFor = nullptr;
    cacheSummary = {};
    updateStatus();
}

void SettingsPanel::visibilityChanged()
{
    // Only poll while the panel is showing.
    if (isVisible())
        startTimerHz (2);
    else
        stopTimer();
}

void SettingsPanel::timerCallback()
{
    updateStatus();
}

void SettingsPanel::updateStatus()
{
    auto newStatus = juce::String();
    const auto info = processor.getKitInfo();

    if (processor.isKitLoading())
    {
        newStatus = processor.getKitLoadPhase() == LoadPhase::caching
                      ? "Decoding the kit into the stream cache (only the first time it streams)..."
                      : "Loading the kit...";
    }
    else if (info != nullptr && info->streaming)
    {
        newStatus = info->credits.name + " streams from disk: " + describeBytes ((std::int64_t) info->memoryBytes)
                    + " in RAM (preloads and stream buffers) instead of " + describeBytes ((std::int64_t) info->fullMemoryBytes) + ".";
    }
    else if (info != nullptr)
    {
        newStatus = info->credits.name + " is fully in RAM: " + describeBytes ((std::int64_t) info->memoryBytes) + ".";
    }

    // The cache size means walking the cache folder, so only redo it when
    // something could have changed it.
    if (info != statusFor || cacheSummary.isEmpty())
    {
        statusFor = info;
        const auto folder = processor.getStreamingSettings().cacheFolder;
        const auto bytes = streamcache::getCacheSize (folder);
        cacheSummary = bytes > 0 ? "The cache takes " + describeBytes (bytes) + " on disk. Clearing it is safe: "
                                   "kits cache their audio again the next time they stream. Kits playing now keep theirs."
                                 : "The cache is empty.";
    }

    if (newStatus != status)
        status = newStatus;

    repaint();
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

    auto heading = [&] (const juce::String& text, juce::Rectangle<int> r)
    {
        g.setColour (colours::textMuted);
        drawTrackedText (g, text, label (12.0f), 0.1f, r.toFloat(), juce::Justification::centredLeft);
    };

    auto paragraph = [&] (const juce::String& text, juce::Rectangle<int> r, juce::Colour colour)
    {
        g.setColour (colour);
        g.setFont (body (13.0f));
        g.drawFittedText (text, r, juce::Justification::topLeft, 3, 1.0f);
    };

    heading ("KITS FOLDER", kitsHeading);
    paragraph ("Lithium Drums lists the kits in this folder, and downloads new ones into it (Browse, in the header). "
               "Libraries you've downloaded yourself go in with the LithiumImport tool (see the README). "
               "Every instance uses the same folder.", kitsText, colours::textMuted);

    heading ("STREAMING", streamingHeading);
    g.setColour (colours::text);
    g.setFont (body (14.0f));
    g.drawText ("Stream from disk", modeLabel, juce::Justification::centredLeft);
    g.drawText ("Preload per sample", preloadLabel, juce::Justification::centredLeft);
    paragraph (status, statusText, colours::text);

    heading ("STREAM CACHE", cacheHeading);
    paragraph (cacheSummary, cacheText, colours::textMuted);
}

void SettingsPanel::layoutContent (juce::Rectangle<int> area)
{
    auto take = [&area] (int h, int gapAfter = 0)
    {
        auto r = area.removeFromTop (h);
        area.removeFromTop (gapAfter);
        return r;
    };

    auto buttonRow = [] (juce::Rectangle<int> row, std::initializer_list<std::pair<juce::Button*, int>> buttons)
    {
        for (const auto& [b, w] : buttons)
        {
            b->setBounds (row.removeFromLeft (w));
            row.removeFromLeft (8);
        }
    };

    kitsHeading = take (18, 6);
    folderLabel.setBounds (take (30, 10));
    buttonRow (take (28, 10), { { &changeButton, 100 }, { &openButton, 110 }, { &defaultButton, 110 } });
    kitsText = take (40, 22);

    streamingHeading = take (18, 6);

    auto row = take (28, 8);
    modeLabel = row.removeFromLeft (150);
    modeBox.setBounds (row.removeFromLeft (280));

    row = take (28, 10);
    preloadLabel = row.removeFromLeft (150);
    preloadBox.setBounds (row.removeFromLeft (280));
    statusText = take (36, 18);

    cacheHeading = take (18, 6);
    cacheLabel.setBounds (take (30, 10));
    buttonRow (take (28, 10), { { &clearCacheButton, 110 }, { &openCacheButton, 110 } });
    cacheText = area.removeFromTop (40);
}

} // namespace lithium
