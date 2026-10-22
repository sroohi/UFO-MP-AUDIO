#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <iostream>
#include <stdexcept>
#include <thread>
static void check(bool passed,const char* what) { if(!passed) throw std::runtime_error(what); }
static juce::Component* findComponent(juce::Component& parent,const juce::String& id) {
    if(parent.getComponentID()==id)return &parent;
    for(auto* child:parent.getChildren())if(auto* found=findComponent(*child,id))return found;
    return nullptr;
}
static void waitForImport(UFOProcessor& synth,int layer) {
    const auto deadline=juce::Time::getMillisecondCounterHiRes()+10000;
    while(synth.sampleLoading(layer) && juce::Time::getMillisecondCounterHiRes()<deadline)juce::Thread::sleep(2);
    check(!synth.sampleLoading(layer),"sample loader timeout");
    check(synth.sampleError(layer).isEmpty(),"sample loader failed");
}
struct PresetResult {bool done=false,onMessageThread=false;juce::String error;};
static UFOProcessor::PresetCallback completion(const std::shared_ptr<PresetResult>& result) {
    return [result](const juce::String& error){result->done=true;result->error=error;result->onMessageThread=juce::MessageManager::getInstance()->isThisTheMessageThread();};
}
static void waitForPreset(const std::shared_ptr<PresetResult>& result) {
    const auto deadline=juce::Time::getMillisecondCounterHiRes()+15000;
    while(!result->done && juce::Time::getMillisecondCounterHiRes()<deadline)juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    check(result->done && result->onMessageThread,"preset job completes on the message thread");
}
static void asyncPresetTests(UFOProcessor& synth,const juce::File& folder) {
    const auto file=folder.getChildFile("Async stream.ufo");
    auto* step=synth.state.getParameter("mod0Step15");step->setValueNotifyingHost(step->convertTo0to1(-.33f));
    auto result=std::make_shared<PresetResult>();
    check(synth.saveUserPresetAsync(file,completion(result)) && synth.presetJobBusy(),"streamed preset save starts in background");
    check(!synth.saveUserPresetAsync(folder.getChildFile("Overlap.ufo"),{}),"overlapping preset jobs are refused");
    step->setValueNotifyingHost(step->convertTo0to1(.77f));
    bool messageDelivered=false;juce::MessageManager::callAsync([&]{messageDelivered=true;});
    waitForPreset(result);check(result->error.isEmpty() && file.existsAsFile() && messageDelivered && !synth.presetJobBusy(),"background save permits message dispatch and finishes its archive");
    synth.loadFactory(2);result=std::make_shared<PresetResult>();
    check(synth.loadUserPresetAsync(file,completion(result)),"background streamed preset load starts");
    waitForPreset(result);check(result->error.isEmpty(),"background streamed preset load succeeds");waitForImport(synth,0);
    check(synth.sampleStreaming(0) && synth.sampleName(0)=="Long stereo.wav","asynchronous preset load restores streamed source metadata");
    check(std::abs(synth.state.getRawParameterValue("mod0Step15")->load()+.33f)<.0001f,"background save captures parameter snapshot before later edits");
    result=std::make_shared<PresetResult>();check(synth.loadUserPresetAsync(file,completion(result)),"superseded load starts");
    synth.loadFactory(2);waitForPreset(result);
    check(result->error.contains("superseded") && synth.sampleName(0).isEmpty(),"a newer patch supersedes pending preset load");
    check(std::abs(synth.state.getRawParameterValue("drive")->load()-.25f)<.0001f,"superseded job preserves newer patch parameters");
    const auto invalid=folder.getChildFile("Invalid async.ufo");check(invalid.replaceWithText("bad archive"),"async malformed fixture");
    result=std::make_shared<PresetResult>();check(synth.loadUserPresetAsync(invalid,completion(result)),"invalid preset is checked by background job");waitForPreset(result);
    check(result->error.isNotEmpty() && !synth.presetJobBusy(),"background errors return and release preset controls");
    auto destroyedResult=std::make_shared<PresetResult>();
    {auto doomed=std::make_unique<UFOProcessor>();check(doomed->loadUserPresetAsync(file,completion(destroyedResult)),"pending destruction fixture starts");}
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    check(!destroyedResult->done,"destroyed processor cancels queued preset completion safely");
}
static void sampleAndPresetTests(UFOProcessor& synth) {
    const auto folder=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ufo-integration-"+juce::Uuid().toString());
    check(folder.createDirectory().wasOk(),"fixture directory");
    struct Cleanup {juce::File folder;~Cleanup(){folder.deleteRecursively();}} cleanup{folder};
    auto audio=folder.getChildFile("Stereo.wav");
    {
        auto stream=audio.createOutputStream();check(bool(stream),"fixture output");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream.release(),48000,2,24,{},0));
        check(bool(writer),"fixture writer");
        juce::AudioBuffer<float> data(2,96000);
        for(int i=0;i<data.getNumSamples();++i){data.setSample(0,i,.6f*std::sin(float(2*ufo::pi*200*i/48000)));data.setSample(1,i,.6f*std::sin(float(2*ufo::pi*300*i/48000)));}
        check(writer->writeFromAudioSampleBuffer(data,0,data.getNumSamples()),"fixture audio write");
    }
    synth.loadFactory(0);synth.importSample(0,audio);waitForImport(synth,0);
    check(synth.sampleName(0)=="Stereo.wav","sample display metadata");
    const auto waveform=synth.sampleWaveform(0,128);check(waveform.size()==128,"actual waveform bins");
    check(std::any_of(waveform.begin(),waveform.end(),[](float v){return std::abs(v)>.4f;}),"waveform reflects sample data");
    auto* steps=synth.state.getParameter("mod0Step15");steps->setValueNotifyingHost(steps->convertTo0to1(-.73f));
    auto preset=folder.getChildFile("Stereo.ufo");juce::String error;synth.saveUserPreset(preset,error);
    check(error.isEmpty() && preset.existsAsFile(),"save user preset");
    juce::ZipFile archive(preset);check(archive.getNumEntries()==2,"preset bundles its audio and state");
    check(audio.deleteFile(),"remove original sample before portable restore");
    synth.loadFactory(2);check(synth.sampleName(0).isEmpty(),"factory clears sample overrides");
    check(synth.loadUserPreset(preset,error) && error.isEmpty(),"load self-contained preset");waitForImport(synth,0);
    check(std::abs(synth.state.getRawParameterValue("mod0Step15")->load()+.73f)<.0001f,"editable sequence survives preset archive");
    check(synth.sampleWaveform(0,128)==waveform,"portable preset restores identical sample data");
    check(synth.sampleName(0)=="Stereo.wav","portable preset retains human sample name");
    synth.panicRequested=true;
    juce::AudioBuffer<float> output(2,256);juce::MidiBuffer events;synth.processBlock(output,events);
    synth.state.getParameter("mix")->setValueNotifyingHost(0);events.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
    double difference=0,energy=0;
    for(int block=0;block<60;++block){synth.processBlock(output,events);for(int i=0;i<256;++i){difference+=std::abs(output.getSample(0,i)-output.getSample(1,i));energy+=output.getSample(0,i)*output.getSample(0,i);}}
    check(energy>.1 && difference>.1,"sample import flows through stereo plugin rendering");
    juce::MemoryBlock hostState;synth.getStateInformation(hostState);synth.loadFactory(0);synth.setStateInformation(hostState.getData(),int(hostState.getSize()));waitForImport(synth,0);
    check(!synth.sampleWaveform(0,64).empty(),"host state reloads cached sample asynchronously");
    auto malformed=folder.getChildFile("Bad.ufo");check(malformed.replaceWithText("invalid archive"),"malformed fixture");
    const float before=synth.state.getRawParameterValue("mod0Step15")->load();
    check(!synth.loadUserPreset(malformed,error) && error.isNotEmpty(),"invalid preset is rejected");
    check(synth.state.getRawParameterValue("mod0Step15")->load()==before,"invalid preset preserves current patch");
    // Corrupt media in an otherwise valid archive: the saved SHA must reject it.
    {
        const int mediaIndex=archive.getIndexOfFileName("samples/0.wav");
        std::unique_ptr<juce::InputStream> media(archive.createStreamForEntry(mediaIndex));juce::MemoryBlock bytes;media->readIntoMemoryBlock(bytes);
        check(bytes.getSize()>64,"portable preset media fixture");static_cast<unsigned char*>(bytes.getData())[bytes.getSize()-1]^=1;
        std::unique_ptr<juce::InputStream> originalState(archive.createStreamForEntry(archive.getIndexOfFileName("state.xml")));
        const auto xml=originalState->readEntireStreamAsString();juce::ZipFile::Builder damaged;
        damaged.addEntry(new juce::MemoryInputStream(bytes,true),0,"samples/0.wav",juce::Time::getCurrentTime());
        damaged.addEntry(new juce::MemoryInputStream(xml.toRawUTF8(),size_t(xml.getNumBytesAsUTF8()),true),0,"state.xml",juce::Time::getCurrentTime());
        auto corrupt=folder.getChildFile("Corrupt.ufo");{juce::FileOutputStream output(corrupt);check(damaged.writeToStream(output,nullptr),"corrupt archive fixture");}
        check(!synth.loadUserPreset(corrupt,error) && error.containsIgnoreCase("checksum"),"corrupt preset media is rejected by SHA256");
    }
    {
        juce::ValueTree invalid("UFO"),parameter("PARAM");parameter.setProperty("id","gain",nullptr);parameter.setProperty("value","nan",nullptr);invalid.addChild(parameter,-1,nullptr);
        const auto xml=invalid.createXml()->toString();juce::ZipFile::Builder damaged;
        damaged.addEntry(new juce::MemoryInputStream(xml.toRawUTF8(),size_t(xml.getNumBytesAsUTF8()),true),0,"state.xml",juce::Time::getCurrentTime());
        auto corrupt=folder.getChildFile("InvalidValue.ufo");{juce::FileOutputStream output(corrupt);check(damaged.writeToStream(output,nullptr),"invalid numeric fixture");}
        check(!synth.loadUserPreset(corrupt,error) && error.contains("gain"),"nonfinite preset parameter is rejected");
    }
    synth.clearSample(0);synth.importSample(0,folder.getChildFile("missing.wav"));
    const auto deadline=juce::Time::getMillisecondCounterHiRes()+5000;
    while(synth.sampleLoading(0) && juce::Time::getMillisecondCounterHiRes()<deadline)juce::Thread::sleep(2);
    check(!synth.sampleLoading(0) && synth.sampleError(0).isNotEmpty(),"missing audio reports a loader error");
    synth.loadFactory(0);
    // Swap and clear immutable banks while the real processor is rendering.
    {
        std::unique_ptr<juce::InputStream> media(archive.createStreamForEntry(archive.getIndexOfFileName("samples/0.wav")));
        {juce::FileOutputStream restored(audio);check(restored.writeFromInputStream(*media,-1)>0,"concurrent loader fixture");}
        std::atomic<bool> stop{false},finite{true};std::atomic<int> blocks{0};
        std::thread renderer([&]{
            juce::AudioBuffer<float> buffer(2,128);juce::MidiBuffer midi;midi.addEvent(juce::MidiMessage::noteOn(1,60,.7f),0);
            while(!stop.load()) {
                synth.processBlock(buffer,midi);++blocks;
                for(int c=0;c<2;++c)for(int i=0;i<128;++i)if(!std::isfinite(buffer.getSample(c,i)))finite=false;
            }
        });
        try {
            for(int round=0;round<6;++round){synth.importSample(0,audio);waitForImport(synth,0);synth.clearSample(0);}
        } catch(...) {stop=true;renderer.join();throw;}
        stop=true;renderer.join();check(finite && blocks>0,"background import/clear is safe during actual render blocks");
    }
    synth.loadFactory(0);
}
static void longSampleTests(UFOProcessor& synth) {
    const auto folder=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ufo-long-plugin-"+juce::Uuid().toString());
    check(folder.createDirectory().wasOk(),"long fixture directory");
    struct Cleanup {juce::File folder;~Cleanup(){folder.deleteRecursively();}} cleanup{folder};
    const auto audio=folder.getChildFile("Long stereo.wav");
    constexpr int rate=44100,frames=31*rate;
    {
        auto stream=audio.createOutputStream();check(bool(stream),"long fixture output");
        juce::WavAudioFormat wav;std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream.release(),rate,2,24,{},0));
        check(bool(writer),"long fixture writer");juce::AudioBuffer<float> chunk(2,8192);
        for(int first=0;first<frames;first+=chunk.getNumSamples()) {
            const int count=std::min(chunk.getNumSamples(),frames-first);
            for(int i=0;i<count;++i){chunk.setSample(0,i,.6f*std::sin(float(2*ufo::pi*200*(first+i)/rate)));chunk.setSample(1,i,.5f*std::cos(float(2*ufo::pi*313*(first+i)/rate)));}
            check(writer->writeFromAudioSampleBuffer(chunk,0,count),"long fixture chunk write");
        }
    }
    synth.loadFactory(0);synth.importSample(0,audio);waitForImport(synth,0);
    check(synth.sampleStreaming(0),"audio longer than thirty seconds uses disk streaming");
    check(synth.sampleCacheBytes(0)>0 && synth.sampleCacheBytes(0)<17*1024*1024,"long plugin sample cache has a bounded memory budget");
    auto set=[&](const char* id,float value){auto* p=synth.state.getParameter(id);p->setValueNotifyingHost(p->convertTo0to1(value));};
    set("mix",0);set("upperMovement",0);set("upperOneshot",1);set("delay",0);set("reverb",0);
    synth.setNonRealtime(true);synth.panicRequested=true;
    juce::AudioBuffer<float> output(2,256);juce::MidiBuffer events;synth.processBlock(output,events);
    const auto misses=synth.sampleUnderruns(0);double energy=0,stereo=0;
    // These seeks include the tail beyond the old thirty-second import ceiling.
    for(float color:{.995f,.02f,.72f}) {
        set("upperColor",color);events.addEvent(juce::MidiMessage::noteOn(1,60,.8f),0);
        for(int block=0;block<40;++block) {
            synth.processBlock(output,events);
            for(int i=0;i<256;++i) {
                const auto left=output.getSample(0,i),right=output.getSample(1,i);
                check(std::isfinite(left) && std::isfinite(right) && std::abs(left)<=1 && std::abs(right)<=1,"streamed plugin audio is finite and bounded");
                energy+=left*left;stereo+=std::abs(left-right);
            }
        }
        events.addEvent(juce::MidiMessage::allSoundOff(1),0);synth.processBlock(output,events);
    }
    synth.setNonRealtime(false);
    check(energy>1 && stereo>1,"streamed long stereo audio reaches the real processor output");
    check(synth.sampleUnderruns(0)==misses,"non-realtime plugin rendering waits for pages instead of dropping audio");
    const auto preset=folder.getChildFile("Long.ufo");juce::String error;synth.saveUserPreset(preset,error);
    check(error.isEmpty() && preset.existsAsFile(),"save streamed user preset");
    {juce::FileInputStream input(preset);char magic[8]{};check(input.read(magic,8)==8 && std::memcmp(magic,"UFOSET03",8)==0,"streamed preset uses 64-bit media container");}
    check(audio.deleteFile(),"remove long source before portable restore");synth.loadFactory(0);
    check(synth.loadUserPreset(preset,error) && error.isEmpty(),"load streamed self-contained preset");waitForImport(synth,0);
    check(synth.sampleStreaming(0) && synth.sampleName(0)=="Long stereo.wav","streaming and sample metadata survive user preset restore");
    asyncPresetTests(synth,folder);
    check(synth.loadUserPreset(preset,error) && error.isEmpty(),"restore stream after asynchronous preset tests");waitForImport(synth,0);
    juce::MemoryBlock hostState;synth.getStateInformation(hostState);synth.loadFactory(0);synth.setStateInformation(hostState.getData(),int(hostState.getSize()));waitForImport(synth,0);
    check(synth.sampleStreaming(0),"serialized host state restores long-file streaming");synth.loadFactory(0);
}
int main() {
    juce::ScopedJuceInitialiser_GUI initialise;
    UFOProcessor synth;
    synth.setPlayConfigDetails(0,2,48000,256);
    synth.prepareToPlay(48000,256);
    check(synth.getLatencySamples()==32,"oversampling latency is reported to the host");
    synth.state.getParameter("delay")->setValueNotifyingHost(1);
    synth.state.getParameter("delayTime")->setValueNotifyingHost(1);
    synth.state.getParameter("feedback")->setValueNotifyingHost(1);
    synth.state.getParameter("release")->setValueNotifyingHost(1);
    check(synth.getTailLengthSeconds()>145,"long feedback delay tail is reported to the host");
    synth.loadFactory(0);
    juce::AudioBuffer<float> buffer(2,256);juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1,60,.8f),128);
    synth.processBlock(buffer,midi);
    check(buffer.getMagnitude(0,0,128)==0,"MIDI sample offset ignored");
    check(buffer.getMagnitude(0,128,128)>0,"MIDI produced no audio");
    synth.loadFactory(1);juce::MemoryBlock saved;synth.getStateInformation(saved);
    synth.loadFactory(2);synth.setStateInformation(saved.getData(),int(saved.getSize()));
    check(std::abs(synth.state.getRawParameterValue("attack")->load()-1.4f)<.001f,"state recall failed");
    for(int i=0;i<100;++i){synth.processBlock(buffer,midi);check(std::isfinite(buffer.getMagnitude(0,256)),"nonfinite plugin audio");}
    synth.panicRequested=true;synth.processBlock(buffer,midi);check(buffer.getMagnitude(0,256)==0,"panic failed");
    sampleAndPresetTests(synth);
    longSampleTests(synth);
    std::unique_ptr<juce::AudioProcessorEditor> editor(synth.createEditor());
    check(editor && editor->getWidth()==1080 && editor->getHeight()==846,"editor layout failed");
    auto* handle= findComponent(*editor,"modHandle2");
    auto* cutoff=dynamic_cast<UFODropSlider*>(findComponent(*editor,"upperCutoff"));
    check(handle && cutoff,"modulation drag components exist");
    juce::DragAndDropTarget::SourceDetails drag(juce::var("UFO:mod:2"),handle,{});
    check(cutoff->isInterestedInDragSource(drag),"cutoff accepts modulation source");
    cutoff->itemDropped(drag);
    check(int(synth.state.getRawParameterValue("mod2Target")->load())==2,"drop assigns real modulation parameter");
    auto* edit=dynamic_cast<juce::TextButton*>(findComponent(*editor,"modEdit0"));check(edit!=nullptr,"mod editor button");
    edit->onClick();
    auto* length=dynamic_cast<juce::ComboBox*>(findComponent(*editor,"mod0Length"));check(length!=nullptr,"sequence length selector");length->setSelectedId(2,juce::sendNotificationSync);
    auto* step=dynamic_cast<juce::Slider*>(findComponent(*editor,"mod0Step15"));check(step!=nullptr,"16-step editor opens");
    step->setValue(.62,juce::sendNotificationSync);
    check(std::abs(synth.state.getRawParameterValue("mod0Step15")->load()-.62f)<.001f,"step editor updates host parameter");
    juce::Image image(juce::Image::ARGB,editor->getWidth(),editor->getHeight(),true);juce::Graphics graphics(image);editor->paintEntireComponent(graphics,true);
    if(const char* path=std::getenv("UFO_SCREENSHOT")){juce::FileOutputStream stream{juce::File{path}};check(stream.openedOk(),"screenshot open failed");stream.setPosition(0);check(stream.truncate().wasOk(),"screenshot truncate failed");juce::PNGImageFormat png;check(png.writeImageToStream(image,stream),"screenshot failed");stream.flush();auto captured=juce::ImageFileFormat::loadFrom(juce::File{path});check(captured.getWidth()==editor->getWidth(),"stale screenshot dimensions");}
    editor.reset();synth.releaseResources();std::cout<<"PASS: plugin MIDI offsets, audio, factory presets, state recall, panic, async stereo import, long-file streaming/offline rendering, portable user presets, modulation drag/drop, step editing and native editor\n";
}
