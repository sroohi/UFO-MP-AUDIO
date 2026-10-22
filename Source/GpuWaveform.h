#pragma once
#include <juce_opengl/juce_opengl.h>
#include <array>
#include <atomic>
#include <mutex>

// The UI publishes small snapshots. Only this component's GL thread touches GL;
// neither the audio engine nor its render callback is called from the GPU thread.
class GpuWaveform final : public juce::Component,
                          private juce::OpenGLRenderer,
                          private juce::Timer {
public:
    struct Diagnostics {
        bool active=false, shaderLinked=false;
        uint64_t frames=0, readbackHash=0;
        int brightPixels=0, readbackWidth=0, readbackHeight=0;
        juce::String renderer,version,error;
    };
    explicit GpuWaveform(juce::Colour);
    ~GpuWaveform() override;
    void setData(const std::vector<float>& waveform,float color,float playhead,float modulation);
    void setGpuEnabled(bool);
    bool isGpuEnabled() const {return wanted.load();}
    Diagnostics diagnostics() const;
    void requestReadback();
    juce::Image readbackImage() const;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    struct Snapshot {
        std::array<float,256> waveform{};
        float color=.25f,playhead=0,modulation=0;
        bool hasWaveform=false;
    };
    enum State {waiting,active,cpu,failed};
    Snapshot snapshot;
    juce::Colour tint;
    mutable std::mutex dataMutex,diagnosticsMutex;
    juce::String rendererName,glVersion,lastError;
    juce::Image lastReadback;
    juce::OpenGLContext context;
    std::unique_ptr<juce::OpenGLShaderProgram> program;
    std::unique_ptr<juce::OpenGLShaderProgram::Attribute> position;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> waveformUniform,resolutionUniform,tintUniform,cursorsUniform,modulationUniform;
    unsigned int vertexArray=0,vertexBuffer=0;
    std::vector<unsigned char> readback;
    std::atomic<int> state{waiting},bright{0},readbackWidth{0},readbackHeight{0},logicalWidth{1},logicalHeight{1};
    std::atomic<bool> wanted{true},linked{false},readbackRequested{true};
    std::atomic<uint64_t> frameCount{0},pixelHash{0};
    double firstShowing=0;
    Snapshot currentSnapshot() const;
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;
    void timerCallback() override;
    void fail(const juce::String&);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GpuWaveform)
};
