#pragma once

#include "PluginProcessor.h"
#include "ui/LithiumLookAndFeel.h"
#include "ui/MainView.h"

namespace lithium
{

/** The plugin window. It lays the UI out once at 1280x800 and scales it with
    an AffineTransform, so every size (and every display scale) looks the
    same; the window keeps a fixed aspect ratio while resizing. */
class LithiumDrumsEditor final : public juce::AudioProcessorEditor
{
public:
    explicit LithiumDrumsEditor (LithiumDrumsProcessor&);
    ~LithiumDrumsEditor() override;

    void resized() override;
    void parentHierarchyChanged() override;

private:
    /** For the standalone app's window: JUCE draws that window's title bar
        itself, with the app name (and an icon, if given one) between the
        buttons. The header already says what this is, so the title bar keeps
        only its background and the window buttons. */
    struct BareTitleBarLookAndFeel final : public juce::LookAndFeel_V4
    {
        void drawDocumentWindowTitleBar (juce::DocumentWindow&, juce::Graphics& g, int, int, int, int,
                                         const juce::Image*, bool) override
        {
            g.fillAll (getCurrentColourScheme().getUIColour (ColourScheme::widgetBackground));
        }
    };

    // The LookAndFeels must outlive every component using them, so they are
    // declared first (constructed first, destroyed last).
    LithiumLookAndFeel lookAndFeel;
    BareTitleBarLookAndFeel titleBarLookAndFeel;
    MainView view;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Component::SafePointer<juce::DocumentWindow> standaloneWindow;   // once it has the icon and the bare title bar
};

} // namespace lithium
