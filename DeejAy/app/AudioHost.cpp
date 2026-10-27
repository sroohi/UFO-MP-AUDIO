#include "AudioHost.h"

namespace app
{

AudioHost::AudioHost (dj::Engine& engine) : engine_ (engine) {}

AudioHost::~AudioHost()
{
    deviceManager_.removeAudioCallback (this);
    deviceManager_.closeAudioDevice();
}

juce::String AudioHost::initialise (const juce::File& stateFile)
{
    std::unique_ptr<juce::XmlElement> saved;
    if (stateFile.existsAsFile()) saved = juce::XmlDocument::parse (stateFile);
    // up to 6 outputs: master, headphones, booth
    const auto err = deviceManager_.initialise (0, 6, saved.get(), true);
    if (saved == nullptr)
    {
        // prefer a DDJ-FLX10 when one is connected
        for (auto* type : deviceManager_.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            for (const auto& name : type->getDeviceNames (false))
                if (name.containsIgnoreCase ("DDJ-FLX10"))
                {
                    auto setup = deviceManager_.getAudioDeviceSetup();
                    setup.outputDeviceName = name;
                    setup.useDefaultOutputChannels = true;
                    deviceManager_.setAudioDeviceSetup (setup, true);
                }
        }
    }
    deviceManager_.addAudioCallback (this);
    return err;
}

void AudioHost::saveState (const juce::File& stateFile)
{
    if (auto xml = deviceManager_.createStateXml())
        xml->writeTo (stateFile);
}

void AudioHost::applyRouting()
{
    auto& r = engine_.routing;
    const int n = numOutputs_.load();
    r.masterL.store (0);
    r.masterR.store (n > 1 ? 1 : 0);
    r.phonesL.store (n >= 4 ? 2 : -1);
    r.phonesR.store (n >= 4 ? 3 : -1);
    r.boothL.store (n >= 6 ? 4 : -1);
    r.boothR.store (n >= 6 ? 5 : -1);
}

void AudioHost::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int block = device->getCurrentBufferSizeSamples();
    sampleRate_.store (sr);
    bufferSize_.store (block);
    outputLatency_.store (device->getOutputLatencyInSamples());
    numOutputs_.store (device->getActiveOutputChannels().countNumberOfSetBits());
    // prepare() allocates; it runs before the first callback, with no callback active
    engine_.prepare (sr, juce::jmax (64, juce::jmin (block, 2048)));
    applyRouting();
}

void AudioHost::audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs,
                                                  int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    engine_.process (nullptr, 0, outputs, numOutputs, numSamples);
}

} // namespace app
