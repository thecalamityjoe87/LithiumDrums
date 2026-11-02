#pragma once

#include "Controls.h"
#include "PluginProcessor.h"

#include <deque>

namespace lithium
{

/** The MIDI panel's E-Drums view: how the e-kit's controllers are read.

     - The hi-hat pedal: its controller and MIDI channel (with Learn), invert,
       calibration, response curve, where the openness steps change, and the
       chicks and foot splashes the pedal can play by itself.
     - The positional pads (snare, ride, toms): on or off, controller and
       channel (with Learn), curve, calibration, and how strongly position
       changes the sound on kits without positional recordings.
     - The MIDI monitor: every note and controller as it arrives, and what
       it did.

    Every change edits the MIDI map's ControllerSettings, so it's saved with
    the map and the project, never with a preset: it's the player's
    hardware, not the sound. */
class EdrumsView final : public juce::Component
{
public:
    explicit EdrumsView (LithiumDrumsProcessor&);
    ~EdrumsView() override;

    /** Shows the map's settings again (after the map changes). */
    void refresh();

    /** A MIDI message from the monitor queue: shown, and used by Learn and Calibrate. */
    void handle (const MidiMonitorEvent&);

    /** At the panel's refresh rate: the live readouts. */
    void tick();

    /** Stops learning and calibrating (when the panel closes). */
    void stopListening();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ThresholdBar;
    class PadRow;
    class Monitor;

    void setControllers (const std::function<void (ControllerSettings&)>&);
    void startLearning (int target);
    void startCalibrating (int target);
    void updateButtons();

    LithiumDrumsProcessor& processor;

    // The hi-hat pedal
    juce::ComboBox pedalCc, pedalChannel, chickMode;
    juce::TextButton pedalLearn { "Learn" }, pedalCalibrate { "Calibrate" };
    juce::TextButton pedalInvert { "Invert" }, footSplash { "Foot splash" };
    ControlSlider pedalCurve, chickSensitivity;
    std::unique_ptr<ThresholdBar> thresholds;
    juce::Rectangle<int> hatCard, pedalBar, rangeText, curveLabel, stepsBox, stepsLabel, chickLabel, sensitivityLabel, splashNote;
    int pedalRaw = -1;

    // The positional pads
    std::array<std::unique_ptr<PadRow>, numPositionSources> pads;
    juce::Rectangle<int> padsCard;

    std::unique_ptr<Monitor> monitor;
    juce::TextButton pause { "Pause" }, clear { "Clear" };
    juce::Rectangle<int> monitorCard;

    // Learn and Calibrate: -1 = none, 0 = the pedal, 1 + PositionSource = a pad.
    int learning = -1, calibrating = -1;
    int calibrationLow = 127, calibrationHigh = 0;
};

} // namespace lithium
