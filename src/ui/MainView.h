#pragma once

#include "EditPage.h"
#include "FxPage.h"
#include "GrooveImportPanel.h"
#include "GroovesPage.h"
#include "HeaderBar.h"
#include "InfoPanels.h"
#include "KitBrowser.h"
#include "KitPage.h"
#include "MidiPanel.h"
#include "MixerPage.h"

namespace lithium
{

/** The whole UI, laid out at 1280x800. The editor scales it as one piece. */
class MainView final : public juce::Component
{
public:
    explicit MainView (LithiumDrumsProcessor&);

    void showTab (Tab);
    void paint (juce::Graphics&) override;
    void resized() override;

    /** Asks something over the whole UI (see PromptPanel). */
    void showPrompt (PromptPanel::Request);

private:
    void showPanel (OverlayPanel*);
    void openReturn (params::FxReturn);

    HeaderBar header;
    KitPage kitPage;
    EditPage editPage;
    MixerPage mixerPage;
    GroovesPage groovesPage;
    FxPage fxPage;
    AboutPanel aboutPanel;
    SettingsPanel settingsPanel;
    MidiPanel midiPanel;
    KitBrowser browser;
    GrooveImportPanel importPanel;
    PromptPanel prompt;
};

} // namespace lithium
