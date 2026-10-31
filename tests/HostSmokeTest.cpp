// Loads the built VST3 bundle through JUCE's VST3 hosting code, the way a DAW
// would, and checks that it plays MIDI, chokes the hi-hat and restores state.
//
//   LithiumHostSmokeTest [path/to/Lithium Drums.vst3] [--editor]
//
// --editor also opens the plugin's editor (needs a display, e.g. xvfb-run).

#include <juce_audio_processors/juce_audio_processors.h>

#include <iostream>
#include <stdlib.h>   // setenv / _putenv_s

namespace
{
    int failures = 0;

    void check (bool condition, const juce::String& what)
    {
        std::cout << (condition ? "  ok    " : "  FAIL  ") << what << std::endl;
        failures += condition ? 0 : 1;
    }

    // Slot parameters are named after the loaded kit ("Room Mute"), but
    // only once the plugin's message thread has caught up; until then they
    // have their slot names ("Mic 10 Mute"). Either is fine here.
    juce::AudioProcessorParameter* findParameter (juce::AudioPluginInstance& p, const juce::StringArray& names)
    {
        for (auto* param : p.getParameters())
            if (names.contains (param->getName (100)))
                return param;

        return nullptr;
    }

    float render (juce::AudioPluginInstance& p, std::initializer_list<int> notes, int blocks = 1)
    {
        juce::AudioBuffer<float> buffer (p.getTotalNumOutputChannels(), 512);
        float peak = 0.0f;

        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer midi;

            if (b == 0)
                for (const auto note : notes)
                    midi.addEvent (juce::MidiMessage::noteOn (10, note, (juce::uint8) 110), 0);

            p.processBlock (buffer, midi);
            peak = std::max (peak, buffer.getMagnitude (0, 0, 512));
        }

        return peak;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    const auto withEditor = args.contains ("--editor");
    args.removeString ("--editor");
    const auto path = args.isEmpty() ? juce::String (LITHIUM_VST3_PATH) : args[0];

    std::cout << "Hosting " << path << std::endl;

    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> types;
    vst3.findAllTypesForFile (types, path);
    check (types.size() == 1, "the bundle contains one plugin");

    if (types.isEmpty())
        return 1;

    const auto& desc = *types[0];
    check (desc.name == "Lithium Drums", "named \"Lithium Drums\" (got \"" + desc.name + "\")");
    check (desc.isInstrument, "registers as an instrument");

    // The plugin runs in this process, so this points it at a kits folder
    // holding only the synthetic test kit, which a new instance then loads.
   #if JUCE_WINDOWS
    _putenv_s ("LITHIUM_KITS_FOLDER", LITHIUM_TEST_KITS_FOLDER);
   #else
    setenv ("LITHIUM_KITS_FOLDER", LITHIUM_TEST_KITS_FOLDER, 1);
   #endif

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());

    juce::String error;
    auto plugin = formats.createPluginInstance (desc, 48000.0, 512, error);
    check (plugin != nullptr, "instantiates" + (error.isNotEmpty() ? " (" + error + ")" : juce::String()));

    if (plugin == nullptr)
        return 1;

    check (plugin->getMainBusNumOutputChannels() == 2, "stereo main output");
    plugin->prepareToPlay (48000.0, 512);

    // The kit loads in the background; keep hitting the snare until it sounds.
    bool sounded = false;

    for (int i = 0; i < 200 && ! sounded; ++i)
    {
        sounded = render (*plugin, { 38 }) > 0.0f;

        if (! sounded)
            juce::Thread::sleep (50);
    }

    check (sounded, "plays the default kit from MIDI");

    for (const auto note : { 36, 38, 40, 37, 42, 44, 46, 41, 43, 45, 47, 48, 50, 49, 51, 52, 53, 57, 59 })
        check (render (*plugin, { note }, 4) > 0.0f, "note " + juce::String (note) + " makes sound");

    // Open hat, then closed hat 0.1 s later. 0.5 s after that, an unchoked
    // open hat is still ringing, while a choked one (and the short closed
    // hat) is down to its quiet room and bleed tails.
    auto openHatTail = [&] (bool closeAfterwards)
    {
        render (*plugin, {}, 400);      // let everything ring out
        render (*plugin, { 46 }, 9);
        render (*plugin, closeAfterwards ? std::initializer_list<int> { 42 } : std::initializer_list<int> {}, 48);
        return render (*plugin, {}, 20);
    };

    const auto ringing = openHatTail (false);
    const auto choked = openHatTail (true);
    check (ringing > 0.001f, "an open hi-hat rings on by itself");
    check (choked < ringing * 0.25f, "a closed hi-hat chokes the open hi-hat (tail " + juce::String (choked, 4)
                                         + " vs " + juce::String (ringing, 4) + ")");

    // State round-trip through the VST3 wrapper.
    const juce::StringArray snareVolumeNames { "Lithium Brass Snare 14x6.5 Volume", "Piece 2 Volume" };
    auto* snareVolume = findParameter (*plugin, snareVolumeNames);
    check (snareVolume != nullptr, "has a snare volume parameter");
    snareVolume->setValueNotifyingHost (0.25f);

    juce::MemoryBlock state;
    plugin->getStateInformation (state);

    auto restored = formats.createPluginInstance (desc, 48000.0, 512, error);
    restored->setStateInformation (state.getData(), (int) state.getSize());
    check (std::abs (findParameter (*restored, snareVolumeNames)->getValue() - 0.25f) < 0.001f, "state restores parameters");

    if (withEditor)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
        check (editor != nullptr, "opens its editor");

        if (editor != nullptr)
            check (editor->getWidth() == 1280 && editor->getHeight() == 800,
                   "editor is 1280x800 (got " + juce::String (editor->getWidth()) + "x" + juce::String (editor->getHeight()) + ")");
    }

    plugin->releaseResources();
    std::cout << (failures == 0 ? "HOST SMOKE TEST PASSED" : "HOST SMOKE TEST FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
