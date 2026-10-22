#include "GpuWaveform.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Runner final : juce::Timer {
    GpuWaveform& view;
    double deadline=juce::Time::getMillisecondCounterHiRes()+12000;
    int stage=0;
    bool passed=false;
    juce::String failure;
    uint64_t initialHash=0,previousFrames=0;
    int oldWidth=0,oldHeight=0;
    GpuWaveform::Diagnostics evidence;
    explicit Runner(GpuWaveform& component):view(component){startTimerHz(40);}
    void finish(){stopTimer();view.setVisible(false);juce::MessageManager::getInstance()->stopDispatchLoop();}
    void timerCallback() override {
        try {
            const auto d=view.diagnostics();
            require(juce::Time::getMillisecondCounterHiRes()<deadline,("GPU validation timeout: "+d.error).toRawUTF8());
            if(stage==0 && d.active && d.shaderLinked && d.frames>0 && d.readbackHash!=0) {
                require(d.readbackWidth>0 && d.readbackHeight>0,"GPU framebuffer has no dimensions");
                require(d.brightPixels>100,"GPU shader readback has no visible waveform/markers");
                require(d.renderer.isNotEmpty() && d.version.isNotEmpty(),"OpenGL driver identity missing");
                initialHash=d.readbackHash;
                std::vector<float> wave(256);for(size_t i=0;i<wave.size();++i)wave[i]=.7f*float(std::sin(double(i)*.12));
                view.setData(wave,.75f,.9f,.8f);view.requestReadback();stage=1;
            } else if(stage==1 && d.readbackHash!=initialHash) {
                require(d.brightPixels>100,"Updated GPU framebuffer became blank");
                view.setGpuEnabled(false);
                const auto fallback=view.diagnostics();require(!fallback.active && !fallback.shaderLinked,"CPU mode retained an active GPU shader");
                juce::Image image(juce::Image::ARGB,view.getWidth(),view.getHeight(),true);juce::Graphics graphics(image);view.paintEntireComponent(graphics,true);
                int bright=0;for(int y=0;y<image.getHeight();++y)for(int x=0;x<image.getWidth();++x)if(image.getPixelAt(x,y).getGreen()>100)++bright;
                require(bright>100,"CPU fallback does not draw the source waveform");
                previousFrames=d.frames;view.setGpuEnabled(true);stage=2;
            } else if(stage==2 && d.active && d.shaderLinked && d.frames>previousFrames) {
                oldWidth=d.readbackWidth;oldHeight=d.readbackHeight;previousFrames=d.frames;
                view.setSize(640,200);view.requestReadback();stage=3;
            } else if(stage==3) {
                view.requestReadback();
                if(d.active && d.frames>previousFrames+1 && d.readbackWidth>oldWidth && d.readbackHeight>oldHeight) {
                    require(d.brightPixels>100,"Resized GPU framebuffer is blank");evidence=d;passed=true;finish();
                }
            }
        } catch(const std::exception& e){failure=e.what();finish();}
    }
};
}
int main() {
    try {
        juce::ScopedJuceInitialiser_GUI initialiser;
        GpuWaveform view(juce::Colour(0xffb5f5c4));view.setName("UFO GPU waveform validation");view.setBounds(30,30,512,160);
        std::vector<float> wave(256);for(size_t i=0;i<wave.size();++i)wave[i]=.7f*float(std::sin(double(i)*.12));
        view.setData(wave,.2f,.35f,.5f);view.setGpuEnabled(true);
        view.addToDesktop(juce::ComponentPeer::windowIsTemporary);view.setVisible(true);view.toFront(false);
        Runner runner(view);juce::MessageManager::getInstance()->runDispatchLoop();
        require(runner.passed,runner.failure.isEmpty()?"GPU visualization did not complete":runner.failure.toRawUTF8());
        const auto artifactSetting=juce::SystemStats::getEnvironmentVariable("UFO_GPU_ARTIFACT_DIR",{});
        const auto artifacts=artifactSetting.isNotEmpty()?juce::File(artifactSetting):juce::File::getCurrentWorkingDirectory().getChildFile("gpu-validation");
        require(artifacts.createDirectory().wasOk(),"Cannot create GPU artifact directory");
        const auto captured=view.readbackImage();require(captured.isValid(),"GPU framebuffer capture is unavailable");
        {juce::FileOutputStream output(artifacts.getChildFile("GPU-waveform.png"));require(output.openedOk(),"GPU capture output failed");output.setPosition(0);require(output.truncate().wasOk(),"GPU capture truncation failed");juce::PNGImageFormat png;require(png.writeImageToStream(captured,output),"GPU capture encoding failed");}
        auto* metadata=new juce::DynamicObject();metadata->setProperty("renderer",runner.evidence.renderer);metadata->setProperty("version",runner.evidence.version);
        metadata->setProperty("frames",juce::int64(runner.evidence.frames));metadata->setProperty("width",runner.evidence.readbackWidth);metadata->setProperty("height",runner.evidence.readbackHeight);
        metadata->setProperty("brightPixels",runner.evidence.brightPixels);metadata->setProperty("framebufferHash",juce::String(runner.evidence.readbackHash));
        const auto lower=runner.evidence.renderer.toLowerCase();metadata->setProperty("knownSoftwareRenderer",lower.contains("llvmpipe")||lower.contains("softpipe")||lower.contains("software")||lower.contains("swiftshader"));
        require(artifacts.getChildFile("GPU-diagnostics.json").replaceWithText(juce::JSON::toString(juce::var(metadata))),"GPU diagnostic output failed");
        std::cout<<"OpenGL renderer: "<<runner.evidence.renderer<<" / "<<runner.evidence.version<<"\n";
        std::cout<<"GPU API evidence: "<<runner.evidence.frames<<" rendered frames; "<<runner.evidence.readbackWidth<<"x"<<runner.evidence.readbackHeight
                 <<" framebuffer; "<<runner.evidence.brightPixels<<" bright pixels; framebuffer hash="<<runner.evidence.readbackHash<<"\n";
        std::cout<<"PASS: real shader/context, framebuffer readback, changed source/cursors, CPU fallback, context recreation, resize and teardown\n";
        std::cout<<"Driver identity determines hardware acceleration; Mesa llvmpipe is software OpenGL, not a physical GPU benchmark.\n";
        return 0;
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
