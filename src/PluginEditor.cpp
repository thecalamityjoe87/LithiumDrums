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
    // LookAndFeel members are destroyed. The standalone window outlives the
    // editor (the editor goes when the window closes, or when the plugin's
    // settings recreate it), so it gets its default look back.
    if (standaloneWindow != nullptr)
        standaloneWindow->setLookAndFeel (nullptr);

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
    // As a standalone app the editor sits in JUCE's own window. A plugin's
    // window belongs to the host, so it's left alone.
    if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        return;

    auto* window = findParentComponentOfClass<juce::DocumentWindow>();

    if (window == nullptr || window == standaloneWindow)
        return;

    standaloneWindow = window;

    // Both wait until the window is built: this runs while JUCE is still
    // putting the window together, before it has its size. Changing the
    // window's LookAndFeel makes it lay its content out again at its
    // current size, which at this point would squeeze the editor to its
    // minimum. And the app icon for the taskbar and window switcher goes to
    // the window system directly (DocumentWindow::setIcon would only draw it
    // in the title bar), which needs the native window to exist.
    juce::MessageManager::callAsync ([editor = juce::Component::SafePointer<LithiumDrumsEditor> (this),
                                      safe = juce::Component::SafePointer<juce::DocumentWindow> (window)]
    {
        if (editor == nullptr || safe == nullptr)
            return;

        const auto bounds = safe->getBounds();
        safe->setLookAndFeel (&editor->titleBarLookAndFeel);
        safe->setBounds (bounds);   // a no-op unless the relayout moved it

        if (auto* peer = safe->getPeer())
            peer->setIcon (logo::renderIcon (256));
    });
}

} // namespace lithium
