// CoreAudio (or ALSA on the Linux compile check) device -> Engine::process. Nothing in the callback allocates.
#pragma once
#include <JuceHeader.h>
#include "dj/Engine.h"

namespace app
{

class AudioHost : private juce::AudioIODeviceCallback
{
public:
    explicit AudioHost (dj::Engine& engine);
    ~AudioHost() override;

    juce::AudioDeviceManager& deviceManager() { return deviceManager_; }
    juce::String initialise (const juce::File& stateFile);
    void saveState (const juce::File& stateFile);

    // Master on outputs 1/2; headphones on 3/4 when the device has them (DDJ-FLX10: Audio Out 1 = master,
    // Audio Out 2 = headphone cue). Booth is optional.
    void applyRouting();
    int numOutputs() const { return numOutputs_.load(); }
    double sampleRate() const { return sampleRate_.load(); }
    int bufferSize() const { return bufferSize_.load(); }
    double cpuLoad() { return deviceManager_.getCpuUsage(); }
    int outputLatencySamples() const { return outputLatency_.load(); }

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels, int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override {}

    dj::Engine& engine_;
    juce::AudioDeviceManager deviceManager_;
    std::atomic<int> numOutputs_ { 2 }, bufferSize_ { 0 }, outputLatency_ { 0 };
    std::atomic<double> sampleRate_ { 44100.0 };
};

} // namespace app
