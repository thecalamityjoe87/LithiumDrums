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
     - The positional pads (snare, hi-hat, ride, toms): on or off,
       controller and channel (with Learn), curve, calibration, how strongly
       position changes the sound on kits without positional recordings, and
       velocity compensation for pads that read edge hits softer.
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

    // Help for new users, in each card's top-right corner.
    HelpButton hatHelp { "Hi-hat pedal", {
        "How your hi-hat pedal is read. Its controller (CC) and channel: Learn sets them from the next controller "
        "that arrives. Invert is for pedals that send 0 when closed. Calibrate: click it, press the pedal fully down "
        "and let it fully up, then click Done.",
        "Curve changes when the hats start to open as the pedal comes up. Pedal zones set where on the pedal's travel "
        "the sound moves from closed to open, one handle per step.",
        "Pedal chick plays the kit's chick when you close the pedal quickly; Sensitivity sets how quick. Foot splash "
        "plays the splash when you close and reopen it quickly." } };
    HelpButton padsHelp { "Positional sensing", {
        "Pads that send where they were struck, from the centre to the edge, as a controller just before each hit. "
        "Hits then move between the kit's centre and edge recordings, or change tone where it has none.",
        "For each pad: On, its controller and channel (Learn sets them), Calibrate, Curve, Edge tone or Edge blend "
        "(how much the edge changes the sound), and Edge velocity, for pads that read edge hits softer." } };
    HelpButton monitorHelp { "MIDI monitor", {
        "Every note and controller as it arrives from your e-kit, with what it played: the sound, how open the hats "
        "were, or where the pad was struck. Use it to check what your module sends.",
        "Pause holds the list still to read it; Clear empties it." } };

    // Learn and Calibrate: -1 = none, 0 = the pedal, 1 + PositionSource = a pad.
    int learning = -1, calibrating = -1;
    int calibrationLow = 127, calibrationHigh = 0;
};

} // namespace lithium
