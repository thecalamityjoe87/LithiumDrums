#pragma once

#include "HeaderBar.h"
#include "InfoPanels.h"
#include "KitBrowser.h"
#include "KitPage.h"

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

private:
    void showPanel (OverlayPanel*);

    HeaderBar header;
    KitPage kitPage;
    ComingSoonPage editPage, mixerPage, groovesPage, fxPage;
    AboutPanel aboutPanel;
    SettingsPanel settingsPanel;
    KitBrowser browser;
};

} // namespace lithium
