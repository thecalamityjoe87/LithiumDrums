#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

// Factory presets for each module of the FX page: a few starting points per
// effect, in the units its parameters show. Applying one sets that module's
// parameters (and switches it on); the returns' faders, mutes and outputs
// are left alone, since those are part of the mix, not the effect.
//
// These are separate from the plugin's presets (Presets.h), which save
// everything, FX settings included.

namespace lithium::fxpresets
{

enum class Module { room, plate, delay, eq, glue, crush, tape, transient, limiter };
inline constexpr int numModules = 9;

struct Preset
{
    juce::String name;
    std::vector<std::pair<juce::String, float>> values;   // parameter id, value in the parameter's units
};

/** A module's factory presets. Message thread. */
const std::vector<Preset>& factory (Module);

/** Sets the preset's parameters, each as one gesture the host can record. */
void apply (juce::AudioProcessorValueTreeState&, const Preset&);

/** The name of the factory preset the module's settings match, or empty. */
juce::String currentName (juce::AudioProcessorValueTreeState&, Module);

} // namespace lithium::fxpresets
