#pragma once

#include "KitLibrary.h"
#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lithium
{

/** A card shown over the whole UI, dimming what's behind it. Close it with
    its button, Escape, or a click outside the card.

    These are drawn inside the editor rather than as separate windows:
    plugin windows owned by the plugin itself behave differently in every
    host, while a component inside the editor behaves the same everywhere. */
class OverlayPanel : public juce::Component
{
public:
    OverlayPanel (juce::String title, int width, int height);

    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

protected:
    /** The area inside the card, under the title, for the panel's own content. */
    juce::Rectangle<int> getContentArea() const;
    virtual void layoutContent (juce::Rectangle<int>) {}

    juce::Rectangle<int> getCardBounds() const;
    void setCardSize (int width, int height)        { cardWidth = width; cardHeight = height; resized(); repaint(); }
    void setTitle (juce::String t)                  { title = std::move (t); repaint(); }
    juce::TextButton& getCloseButton() noexcept     { return closeButton; }

private:
    juce::String title;
    int cardWidth, cardHeight;
    juce::TextButton closeButton { "Close" };
};

/** A small question over the UI: a message, an optional text field, and
    OK and Cancel (or just OK, for a message). Used for naming, renaming and
    deleting presets, and for saying what went wrong. */
class PromptPanel final : public OverlayPanel
{
public:
    struct Request
    {
        juce::String title, message;
        juce::String okText = "OK";
        bool withTextField = false;
        juce::String text;                   // the text field's starting text
        bool cancellable = true;
        bool destructive = false;            // OK in the warning colour (Delete)
        std::function<void (const juce::String& text)> onOk;
    };

    PromptPanel();

    /** Shows a request. MainView makes it visible and gives it the focus. */
    void show (Request);

    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Called when it should be hidden (OK or Cancel). */
    std::function<void()> onFinished;

private:
    void layoutContent (juce::Rectangle<int>) override;
    void finish (bool ok);

    Request request;
    juce::TextEditor field;
    juce::TextButton okButton;
    juce::Rectangle<int> messageArea;
};

/** About Lithium Drums: a brand block on top (the app icon, name, version,
    who made it, links to the project and the licence), then two tabs.
    About says what it is, its licence (the copyright and no-warranty notice
    the AGPL asks interactive programs to show) and what it's built with.
    Credits lists the attribution and licence of every installed kit pack,
    groove pack and built-in reverb IR, which their CC BY licences require. */
class AboutPanel final : public OverlayPanel
{
public:
    explicit AboutPanel (LithiumDrumsProcessor&);
    ~AboutPanel() override;

    /** Re-reads the installed kits. Called each time the panel opens. */
    void refresh();

    void paint (juce::Graphics&) override;

private:
    class TabButton;
    class AboutTab;
    class CreditRow;
    class SectionHeading;

    void layoutContent (juce::Rectangle<int>) override;
    void showCredits (bool);

    LithiumDrumsProcessor& processor;
    juce::Image icon;   // the app icon, rendered once at twice its drawn size

    std::unique_ptr<TabButton> aboutTabButton, creditsTabButton;
    juce::TextButton websiteButton { "GitHub" }, licenseButton { "Licence" };

    std::unique_ptr<AboutTab> aboutTab;
    juce::Viewport creditsView;          // the Credits tab: a scrolling list
    juce::Component creditsList;
    std::vector<std::unique_ptr<juce::Component>> creditRows;
};

/** Settings: where the kits are, and how they stream from disk. */
class SettingsPanel final : public OverlayPanel, private juce::Timer
{
public:
    explicit SettingsPanel (LithiumDrumsProcessor&);

    /** Called after the kits folder changes. */
    std::function<void()> onKitsFolderChanged;

    void refresh();
    void paint (juce::Graphics&) override;

private:
    void layoutContent (juce::Rectangle<int>) override;
    void visibilityChanged() override;
    void timerCallback() override;
    void setFolder (const juce::File&);
    void updateStatus();

    LithiumDrumsProcessor& processor;
    juce::Label folderLabel, cacheLabel;
    juce::TextButton changeButton { "Change..." }, openButton { "Open folder" }, defaultButton { "Use default" };
    juce::TextButton clearCacheButton { "Clear cache" }, openCacheButton { "Open folder" };
    juce::ComboBox modeBox, preloadBox;
    std::unique_ptr<juce::FileChooser> chooser;   // must stay alive while the async dialog is open

    juce::Rectangle<int> kitsHeading, kitsText, streamingHeading, modeLabel, preloadLabel, statusText, cacheHeading, cacheText;
    juce::String status, cacheSummary;
    std::shared_ptr<const KitInfo> statusFor;     // the kit the status line describes
};

} // namespace lithium
