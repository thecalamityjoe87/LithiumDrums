#include "StandaloneAudioFix.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace lithium
{

#if JUCE_LINUX

namespace
{
    // PipeWire keeps a card open for a few seconds after an app stops using
    // it (the default ALSA device is routed through PipeWire). Until it lets
    // go, JUCE finds the card has no channels and refuses to open it.
    constexpr int maxWaitMs = 8000;
    constexpr int retryIntervalMs = 250;

    class PatientAlsaType final : public juce::AudioIODeviceType,
                                  private juce::AudioIODeviceType::Listener
    {
    public:
        explicit PatientAlsaType (std::unique_ptr<juce::AudioIODeviceType> typeToWrap)
            : AudioIODeviceType (typeToWrap->getTypeName()),
              inner (std::move (typeToWrap))
        {
            inner->addListener (this);
        }

        ~PatientAlsaType() override                                  { inner->removeListener (this); }

        void scanForDevices() override                               { inner->scanForDevices(); }
        juce::StringArray getDeviceNames (bool wantInputNames) const override   { return inner->getDeviceNames (wantInputNames); }
        int getDefaultDeviceIndex (bool forInput) const override     { return inner->getDefaultDeviceIndex (forInput); }
        int getIndexOfDevice (juce::AudioIODevice* d, bool asInput) const override   { return inner->getIndexOfDevice (d, asInput); }
        bool hasSeparateInputsAndOutputs() const override            { return inner->hasSeparateInputsAndOutputs(); }

        juce::AudioIODevice* createDevice (const juce::String& outputName, const juce::String& inputName) override
        {
            const auto start = juce::Time::getMillisecondCounter();

            for (;;)
            {
                std::unique_ptr<juce::AudioIODevice> device (inner->createDevice (outputName, inputName));

                if (device == nullptr || hasRequestedChannels (*device, outputName, inputName)
                     || juce::Time::getMillisecondCounter() - start >= (juce::uint32) maxWaitMs)
                    return device.release();

                juce::Thread::sleep (retryIntervalMs);
            }
        }

    private:
        static bool hasRequestedChannels (juce::AudioIODevice& device, const juce::String& outputName, const juce::String& inputName)
        {
            return (outputName.isEmpty() || ! device.getOutputChannelNames().isEmpty())
                && (inputName.isEmpty()  || ! device.getInputChannelNames().isEmpty());
        }

        void audioDeviceListChanged() override                       { callDeviceChangeListeners(); }

        std::unique_ptr<juce::AudioIODeviceType> inner;
    };
}

void installStandaloneAudioFix()
{
    auto* holder = juce::StandalonePluginHolder::getInstance();

    if (holder == nullptr)
        return;

    // The holder creates the plugin before it sets up its audio devices, so
    // these are the types it will use. Adding any stops JUCE adding its own.
    auto& manager = holder->deviceManager;

    if (auto alsa = std::unique_ptr<juce::AudioIODeviceType> (juce::AudioIODeviceType::createAudioIODeviceType_ALSA()))
        manager.addAudioDeviceType (std::make_unique<PatientAlsaType> (std::move (alsa)));

    if (auto jack = std::unique_ptr<juce::AudioIODeviceType> (juce::AudioIODeviceType::createAudioIODeviceType_JACK()))
        manager.addAudioDeviceType (std::move (jack));
}

#else

void installStandaloneAudioFix() {}

#endif

}
