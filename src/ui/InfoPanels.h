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

private:
    juce::Rectangle<int> getCardBounds() const;

    const juce::String title;
    const int cardWidth, cardHeight;
    juce::TextButton closeButton { "Close" };
};

/** About and credits: the attribution and licence of every installed kit,
    which CC BY requires to be shown, plus the fonts and JUCE. */
class AboutPanel final : public OverlayPanel
{
public:
    explicit AboutPanel (LithiumDrumsProcessor&);

    /** Re-reads the installed kits. Called each time the panel opens. */
    void refresh();

private:
    class KitCard;

    void layoutContent (juce::Rectangle<int>) override;

    LithiumDrumsProcessor& processor;
    juce::Viewport viewport;
    juce::Component list;
    std::vector<std::unique_ptr<juce::Component>> cards;
};

/** Settings: where the kits are. */
class SettingsPanel final : public OverlayPanel
{
public:
    explicit SettingsPanel (LithiumDrumsProcessor&);

    /** Called after the kits folder changes. */
    std::function<void()> onKitsFolderChanged;

    void refresh();
    void paint (juce::Graphics&) override;

private:
    void layoutContent (juce::Rectangle<int>) override;
    void setFolder (const juce::File&);

    LithiumDrumsProcessor& processor;
    juce::Label folderLabel;
    juce::TextButton changeButton { "Change..." }, openButton { "Open folder" }, defaultButton { "Use default" };
    std::unique_ptr<juce::FileChooser> chooser;   // must stay alive while the async dialog is open
    juce::Rectangle<int> textArea;
};

} // namespace lithium
