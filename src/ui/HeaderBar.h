#pragma once

#include "KitLibrary.h"
#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lithium
{

enum class Tab { kit, edit, mixer, grooves, fx };

/** The 56 px bar across the top: logo, tabs, kit picker, live readouts and
    the info and settings buttons. */
class HeaderBar final : public juce::Component,
                        public juce::TooltipClient,
                        private juce::ChangeListener,
                        private juce::Timer
{
public:
    explicit HeaderBar (LithiumDrumsProcessor&);
    ~HeaderBar() override;

    std::function<void (Tab)> onTabChanged;
    std::function<void()> onShowAbout, onShowSettings, onShowBrowser;
    void setTab (Tab);

    /** Re-reads the kits folder (after it changes). */
    void refreshKitBox();

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Explains the readout under the mouse. */
    juce::String getTooltip() override;

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
    juce::TextButton browseButton { "Browse" };
    std::unique_ptr<IconButton> aboutButton, settingsButton;
    kitlibrary::Library library;

    int cpuPercent = 0, voices = 0;
    std::size_t sampleBytes = 0;
    float loadProgress = -1.0f;   // -1 while no kit is loading
    LoadPhase loadPhase = LoadPhase::reading;

    // Disk readout: bytes per second, worked out from the stream counters.
    std::uint64_t lastBytesRead = 0, lastDropouts = 0;
    double lastReadTime = 0.0, lastDropoutTime = -1.0e9;
    double diskBytesPerSecond = 0.0;
    bool streaming = false, recentDropouts = false;

    static constexpr int refreshItemId = 10000, settingsItemId = 10001, browseItemId = 10002;
};

} // namespace lithium
