#pragma once

#include "Controls.h"
#include "InfoPanels.h"
#include "KitLibrary.h"
#include "PluginProcessor.h"
#include "packs/PackInstaller.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lithium
{

enum class Tab { kit, edit, mixer, grooves, fx };

/** The 56 px bar across the top: logo, tabs, the preset box (with the kit
    under the preset's name, and its menu of presets and kits), live
    readouts and the MIDI, settings and info buttons. */
class HeaderBar final : public juce::Component,
                        public juce::TooltipClient,
                        private juce::ChangeListener,
                        private juce::Timer
{
public:
    explicit HeaderBar (LithiumDrumsProcessor&);
    ~HeaderBar() override;

    std::function<void (Tab)> onTabChanged;
    std::function<void()> onShowAbout, onShowSettings, onShowBrowser, onShowMidi;
    std::function<void (const juce::String& kitName)> onShowKit;   // the kit browser, on a kit that isn't installed
    std::function<void (PromptPanel::Request)> onPrompt;   // asks something (a preset's name), or reports a problem
    void setTab (Tab);

    /** Re-reads the kits folder (after it changes). */
    void refreshKitBox();

    /** Loads the next (or previous) preset in the menu's order. */
    void stepPreset (int delta);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Explains the readout under the mouse. */
    juce::String getTooltip() override;

private:
    class TabButton;
    class IconButton;
    class MidiButton;
    class PresetBox;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updatePresetBox();
    void showPresetMenu();
    void loadPreset (const juce::File&);
    void savePresetAs();
    void renamePreset();
    void deletePreset();
    void report (const juce::String& title, const juce::String& message);
    juce::Rectangle<int> getReadoutArea() const;
    juce::Rectangle<int> getReadoutBounds (int index) const;
    juce::Rectangle<int> getProgressArea() const;
    float getWordmarkRight() const;

    LithiumDrumsProcessor& processor;
    std::vector<std::unique_ptr<TabButton>> tabs;
    std::unique_ptr<PresetBox> presetBox;
    ChevronButton previousPreset { "Previous preset", ChevronButton::Direction::left };
    ChevronButton nextPreset { "Next preset", ChevronButton::Direction::right };
    juce::TextButton browseButton { "Browse" };
    std::unique_ptr<IconButton> aboutButton, settingsButton;
    std::unique_ptr<MidiButton> midiButton;
    kitlibrary::Library library;
    juce::SharedResourcePointer<packs::Installer> installer;

    int cpuPercent = 0, voices = 0;
    std::size_t sampleBytes = 0;
    float loadProgress = -1.0f;   // -1 while no kit is loading
    LoadPhase loadPhase = LoadPhase::reading;

    // Disk readout: bytes per second, worked out from the stream counters.
    std::uint64_t lastBytesRead = 0, lastDropouts = 0;
    double lastReadTime = 0.0, lastDropoutTime = -1.0e9;
    double diskBytesPerSecond = 0.0;
    bool streaming = false, recentDropouts = false;

};

} // namespace lithium
