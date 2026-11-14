#include "MainView.h"

#include "Theme.h"

namespace lithium
{

MainView::MainView (LithiumDrumsProcessor& p)
    : header (p),
      kitPage (p),
      editPage (p),
      mixerPage (p),
      groovesPage (p),
      fxPage (p),
      aboutPanel (p),
      settingsPanel (p),
      midiPanel (p),
      browser (p)
{
    header.onTabChanged = [this] (Tab t) { showTab (t); };
    header.onShowAbout = [this] { showPanel (&aboutPanel); };
    header.onShowSettings = [this] { showPanel (&settingsPanel); };
    header.onShowBrowser = [this] { showPanel (&browser); };
    header.onShowKit = [this] (const juce::String& kitName)
    {
        browser.showKit (kitName);
        showPanel (&browser);
    };
    header.onShowMidi = [this] { showPanel (&midiPanel); };
    header.onPrompt = [this] (PromptPanel::Request r) { showPrompt (std::move (r)); };
    prompt.onFinished = [this] { prompt.setVisible (false); };
    kitPage.onEditPiece = [this] { header.setTab (Tab::edit); };
    kitPage.onOpenGrooves = [this] { header.setTab (Tab::grooves); };
    kitPage.setGroovesPage (groovesPage);
    mixerPage.onOpenReturn = [this] (params::FxReturn r) { openReturn (r); };

    aboutPanel.onClose = [this] { showPanel (nullptr); };
    settingsPanel.onClose = [this] { showPanel (nullptr); };
    browser.onClose = [this] { showPanel (nullptr); };
    browser.onPrompt = [this] (PromptPanel::Request r) { showPrompt (std::move (r)); };
    midiPanel.onClose = [this] { showPanel (nullptr); };

    // Importing MIDI grooves: the page picks the folder, the card does the rest.
    groovesPage.onImportFolder = [this] (const juce::File& folder)
    {
        importPanel.showForm (folder);
        showPanel (&importPanel);
    };
    importPanel.onClose = [this] { showPanel (nullptr); };
    importPanel.onImported = [this] (const juce::File& packFolder) { groovesPage.showImportedPack (packFolder); };
    importPanel.onShowPack = [this] (const juce::String& name) { groovesPage.showPack (name); };
    importPanel.onShowStyle = [this] (const juce::String& key) { groovesPage.showStyle (key); };
    importPanel.onChooseFolder = [this] { groovesPage.chooseImportFolder(); };
    settingsPanel.onKitsFolderChanged = [this] { header.refreshKitBox(); };

    addAndMakeVisible (header);
    addChildComponent (kitPage);
    addChildComponent (editPage);
    addChildComponent (mixerPage);
    addChildComponent (groovesPage);
    addChildComponent (fxPage);
    addChildComponent (aboutPanel);      // added last, so they sit on top
    addChildComponent (settingsPanel);
    addChildComponent (midiPanel);
    addChildComponent (browser);
    addChildComponent (importPanel);
    addChildComponent (prompt);          // over everything, even the other panels

    // Tab moves through the controls, with a visible outline (a mouse click
    // doesn't take the focus, so the outline only shows from the keyboard).
    enableKeyboardNavigation (*this);

    showTab (Tab::kit);
}

void MainView::openReturn (params::FxReturn r)
{
    fxPage.selectModule ((int) r);
    header.setTab (Tab::fx);
}

void MainView::showPrompt (PromptPanel::Request request)
{
    prompt.show (std::move (request));
    prompt.setVisible (true);
    prompt.toFront (false);
    prompt.grabKeyboardFocus();
}

void MainView::showPanel (OverlayPanel* panel)
{
    if (panel == &aboutPanel)
        aboutPanel.refresh();
    else if (panel == &settingsPanel)
        settingsPanel.refresh();
    else if (panel == &midiPanel)
        midiPanel.refresh();

    aboutPanel.setVisible (panel == &aboutPanel);
    settingsPanel.setVisible (panel == &settingsPanel);
    midiPanel.setVisible (panel == &midiPanel);
    browser.setVisible (panel == &browser);
    importPanel.setVisible (panel == &importPanel);

    // The browser puts the focus in its search box (once visible); the
    // others take it themselves, for Escape.
    if (panel == &browser)
        browser.refresh();
    else if (panel != nullptr)
        panel->grabKeyboardFocus();
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
    midiPanel.setBounds (getLocalBounds());
    browser.setBounds (getLocalBounds());
    importPanel.setBounds (getLocalBounds());
    prompt.setBounds (getLocalBounds());
}

} // namespace lithium
