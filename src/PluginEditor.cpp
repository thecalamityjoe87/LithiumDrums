#include "PluginEditor.h"

#include "ui/Logo.h"

namespace lithium
{

LithiumDrumsEditor::LithiumDrumsEditor (LithiumDrumsProcessor& p)
    : AudioProcessorEditor (p), view (p)
{
    // Set on the editor, the LookAndFeel applies to every child component.
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (view);

    setResizable (true, true);
    setResizeLimits (theme::windowWidth * 3 / 4, theme::windowHeight * 3 / 4,
                     theme::windowWidth * 2, theme::windowHeight * 2);
    getConstrainer()->setFixedAspectRatio ((double) theme::windowWidth / (double) theme::windowHeight);
    setSize (theme::windowWidth, theme::windowHeight);
}

LithiumDrumsEditor::~LithiumDrumsEditor()
{
    // Components keep a pointer to their LookAndFeel; clear it before the
    // LookAndFeel member is destroyed.
    setLookAndFeel (nullptr);
}

void LithiumDrumsEditor::resized()
{
    const auto scale = (float) getWidth() / (float) theme::windowWidth;
    view.setBounds (0, 0, theme::windowWidth, theme::windowHeight);
    view.setTransform (juce::AffineTransform::scale (scale));
}

void LithiumDrumsEditor::parentHierarchyChanged()
{
    // As a standalone app the editor sits in JUCE's own window: give that
    // window the app icon, for the taskbar and the window switcher. (On
    // Windows and macOS the icon also comes from the executable; on Linux
    // this is the only place the window gets it.) A plugin's window belongs
    // to the host, so it's left alone.
    if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        return;

    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>(); window != nullptr && window != iconisedWindow)
    {
        window->setIcon (logo::renderIcon (256));
        iconisedWindow = window;
    }
}

} // namespace lithium
