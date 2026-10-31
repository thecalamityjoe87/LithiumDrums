#include "PluginEditor.h"

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

} // namespace lithium
