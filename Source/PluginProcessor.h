#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"
#include "SampleLibrary.h"
class UFOProcessor final : public juce::AudioProcessor {
public:
    UFOProcessor();
    ~UFOProcessor() override;
    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    using juce::AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "UFO"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Current patch"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    juce::AudioProcessorValueTreeState state;
    juce::MidiKeyboardState keyboard;
    std::atomic<float> meter {0};
    std::atomic<bool> panicRequested {false};
    void loadFactory(int);
    void saveUserPreset(const juce::File&,juce::String& error) const;
    bool loadUserPreset(const juce::File&,juce::String& error);
    using PresetCallback=std::function<void(const juce::String& error)>;
    bool saveUserPresetAsync(const juce::File&,PresetCallback);
    bool loadUserPresetAsync(const juce::File&,PresetCallback);
    bool presetJobBusy() const;
    juce::File presetDirectory() const;
    void importSample(int layer,const juce::File&);
    void clearSample(int layer);
    juce::String sampleName(int layer) const;
    juce::String sampleError(int layer) const;
    bool sampleLoading(int layer) const;
    std::vector<float> sampleWaveform(int layer,int points) const;
    std::array<std::atomic<float>,4> modMeters{};
    std::array<std::atomic<float>,2> samplePositions{},colorMeters{};
    bool sampleStreaming(int layer) const;
    uint64_t sampleUnderruns(int layer) const;
    uint64_t sampleIoErrors(int layer) const;
    uint64_t sampleCacheBytes(int layer) const;
private:
    ufo::Engine engine;
    std::shared_ptr<ufo::SampleLibrary> samples=std::make_shared<ufo::SampleLibrary>();
    juce::ThreadPool sampleLoader{1};
    struct PresetAsyncState;
    std::shared_ptr<PresetAsyncState> presetAsync;
    juce::ThreadPool presetLoader{1};
    std::atomic<uint64_t> presetRevision{0};
    std::recursive_mutex presetMutationMutex;
    juce::ValueTree stateWithSamples() const;
    bool restoreState(const juce::ValueTree&,juce::String& error);
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    std::vector<std::atomic<float>*> parameterValues;
    void queueSample(int,const juce::File&,const juce::String& name);
    void updateParameters();
    void midi(const juce::MidiMessage&);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UFOProcessor)
};
