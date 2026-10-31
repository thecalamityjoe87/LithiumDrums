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

private:
    // The LookAndFeel must outlive every component using it, so it is
    // declared first (constructed first, destroyed last).
    LithiumLookAndFeel lookAndFeel;
    MainView view;
    juce::TooltipWindow tooltips { this, 600 };
};

} // namespace lithium
