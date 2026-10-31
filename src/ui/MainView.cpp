#include "MainView.h"

#include "Theme.h"

namespace lithium
{

MainView::MainView (LithiumDrumsProcessor& p)
    : header (p),
      kitPage (p),
      editPage ("Edit", "Articulations, velocity layers, tuning, decay and mic sends for each piece."),
      mixerPage ("Mixer", "Channel EQ and compression, reverb sends and multi-output routing."),
      groovesPage ("Grooves", "Browse MIDI grooves, preview them in tempo and drag them into your host."),
      fxPage ("FX", "Master bus effects."),
      aboutPanel (p),
      settingsPanel (p)
{
    header.onTabChanged = [this] (Tab t) { showTab (t); };
    header.onShowAbout = [this] { showPanel (&aboutPanel); };
    header.onShowSettings = [this] { showPanel (&settingsPanel); };
    kitPage.onEditPiece = [this] { header.setTab (Tab::edit); };

    aboutPanel.onClose = [this] { showPanel (nullptr); };
    settingsPanel.onClose = [this] { showPanel (nullptr); };
    settingsPanel.onKitsFolderChanged = [this] { header.refreshKitBox(); };

    addAndMakeVisible (header);
    addChildComponent (kitPage);
    addChildComponent (editPage);
    addChildComponent (mixerPage);
    addChildComponent (groovesPage);
    addChildComponent (fxPage);
    addChildComponent (aboutPanel);      // added last, so they sit on top
    addChildComponent (settingsPanel);

    showTab (Tab::kit);
}

void MainView::showPanel (OverlayPanel* panel)
{
    if (panel == &aboutPanel)
        aboutPanel.refresh();
    else if (panel == &settingsPanel)
        settingsPanel.refresh();

    aboutPanel.setVisible (panel == &aboutPanel);
    settingsPanel.setVisible (panel == &settingsPanel);

    if (panel != nullptr)
        panel->grabKeyboardFocus();   // for Escape
}

void MainView::showTab (Tab t)
{
    kitPage.setVisible (t == Tab::kit);
    editPage.setVisible (t == Tab::edit);
    mixerPage.setVisible (t == Tab::mixer);
    groovesPage.setVisible (t == Tab::grooves);
    fxPage.setVisible (t == Tab::fx);
}

void MainView::paint (juce::Graphics& g)
{
    g.fillAll (theme::colours::background);
}

void MainView::resized()
{
    auto area = getLocalBounds();
    header.setBounds (area.removeFromTop (theme::headerHeight));

    for (auto* page : std::initializer_list<juce::Component*> { &kitPage, &editPage, &mixerPage, &groovesPage, &fxPage })
        page->setBounds (area);

    aboutPanel.setBounds (getLocalBounds());
    settingsPanel.setBounds (getLocalBounds());
}

} // namespace lithium
