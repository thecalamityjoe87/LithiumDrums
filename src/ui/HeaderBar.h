#pragma once

#include "KitLibrary.h"
#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lithium
{

enum class Tab { kit, edit, mixer, grooves, fx };

/** The 56 px bar across the top: logo, tabs, kit picker, live readouts and
    the info and settings buttons. */
class HeaderBar final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit HeaderBar (LithiumDrumsProcessor&);
    ~HeaderBar() override;

    std::function<void (Tab)> onTabChanged;
    std::function<void()> onShowAbout, onShowSettings;
    void setTab (Tab);

    /** Re-reads the kits folder (after it changes). */
    void refreshKitBox();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class TabButton;
    class IconButton;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void kitBoxChanged();
    juce::Rectangle<int> getReadoutArea() const;
    juce::Rectangle<int> getProgressArea() const;
    float getWordmarkRight() const;

    LithiumDrumsProcessor& processor;
    std::vector<std::unique_ptr<TabButton>> tabs;
    juce::ComboBox kitBox;
    std::unique_ptr<IconButton> aboutButton, settingsButton;
    std::vector<kitlibrary::Entry> kits;

    int cpuPercent = 0, voices = 0;
    std::size_t sampleBytes = 0;
    float loadProgress = -1.0f;   // -1 while no kit is loading

    static constexpr int refreshItemId = 10000, settingsItemId = 10001;
};

} // namespace lithium
