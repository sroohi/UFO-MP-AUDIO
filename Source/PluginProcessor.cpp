#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "WidePresetArchive.h"
using namespace juce;
struct UFOProcessor::PresetAsyncState {
    std::mutex mutex;
    UFOProcessor* owner=nullptr;
    std::atomic<bool> canceled{false},busy{false};
};
AudioProcessorValueTreeState::ParameterLayout UFOProcessor::layout() {
    AudioProcessorValueTreeState::ParameterLayout result;
    auto number=[&](String id,String name,float lo,float hi,float initial,float skew=1.f) {
        result.add(std::make_unique<AudioParameterFloat>(ParameterID{id,1},name,NormalisableRange<float>(lo,hi,0,skew),initial));
    };
    auto choice=[&](String id,String name,StringArray options,int initial=0) {
        result.add(std::make_unique<AudioParameterChoice>(ParameterID{id,1},name,options,initial));
    };
    for (auto prefix : {String("upper"),String("lower")}) {
        auto id=[&](const char* suffix){ return prefix+suffix; };
        choice(id("Bank"),prefix+" source",{"Solar","Hollow","Prism","Chaos"});
        number(id("Color"),prefix+" color",0,1,prefix=="upper"?.25f:.65f);
        number(id("Movement"),prefix+" movement",0,1,.15f);
        number(id("Attack"),prefix+" color attack",.001f,16,.1f,.3f);
        number(id("Decay"),prefix+" color decay",.001f,16,1,.3f);
        choice(id("Octave"),prefix+" octave",{"-1","0","+1"},1);
        result.add(std::make_unique<AudioParameterInt>(ParameterID{id("Pitch"),1},prefix+" pitch",-12,12,0));
        choice(id("Shaper"),prefix+" shaper",{"SVF","LPG","Resonator"});
        choice(id("Filter"),prefix+" filter",{"Low pass","Band pass","High pass"});
        number(id("Cutoff"),prefix+" cutoff",16,17600,6000,.25f);
        number(id("Resonance"),prefix+" resonance",0,1,.15f);
        number(id("Slew"),prefix+" vactrol lag",.02f,2,.15f,.4f);
        number(id("Tilt"),prefix+" resonator tilt",0,1,.5f);
        number(id("Spread"),prefix+" resonator spread",2,8,3);
        result.add(std::make_unique<AudioParameterBool>(ParameterID{id("Oneshot"),1},prefix+" one shot",false));
    }
    for(int i=0;i<4;++i) {
        String p="mod"+String(i);
        choice(p+"Type","Mod "+String(i+1)+" generator",{"LFO","Random","Euclidean","Envelope","Sequence"});
        choice(p+"Target","Mod "+String(i+1)+" destination",{"Upper color","Lower color","Upper cutoff","Lower cutoff","Upper pitch","Lower pitch","Layer mix","Output"});
        number(p+"Rate","Mod "+String(i+1)+" rate",.1f,50,1,.35f);
        number(p+"Depth","Mod "+String(i+1)+" depth",-1,1,0);
        number(p+"Shape","Mod "+String(i+1)+" shape / hits",0,1,.5f);
        result.add(std::make_unique<AudioParameterBool>(ParameterID{p+"Sync",1},"Mod "+String(i+1)+" tempo sync",false));
    }
    number("mix","Layer balance",0,1,.5f);
    number("attack","Amplitude attack",.001f,16,.015f,.3f);
    number("decay","Amplitude decay",.001f,16,.4f,.3f);
    number("sustain","Amplitude sustain",0,1,.7f);
    number("release","Amplitude release",.005f,16,.5f,.3f);
    number("drive","Drive",0,1,0); number("chorus","Chorus",0,1,0); number("flanger","Flanger",0,1,0);
    number("delay","Delay mix",0,1,.15f); number("delayTime","Delay seconds",.001f,2,.3f,.4f);
    number("feedback","Delay feedback",0,.9f,.3f); number("reverb","Space",0,1,.15f); number("gain","Output",0,1,.5f);
    // Append new IDs after v0.1 parameters to preserve host automation indices.
    for(int i=0;i<4;++i) {
        String p="mod"+String(i);
        choice(p+"Length","Mod "+String(i+1)+" steps",{"8","16"});
        number(p+"Shift","Mod "+String(i+1)+" rotation",0,1,0);
        number(p+"EnvAttack","Mod "+String(i+1)+" envelope attack",.001f,16,.02f,.3f);
        number(p+"EnvDecay","Mod "+String(i+1)+" envelope decay",.001f,16,.4f,.3f);
        number(p+"EnvSustain","Mod "+String(i+1)+" envelope sustain",0,1,.5f);
        number(p+"EnvRelease","Mod "+String(i+1)+" envelope release",.005f,16,.3f,.3f);
        constexpr float pattern[]={0,.5f,-.25f,1,.25f,-.5f,.75f,-1};
        for(int step=0;step<16;++step)number(p+"Step"+String(step),"Mod "+String(i+1)+" step "+String(step+1),-1,1,pattern[step%8]);
    }
    return result;
}
UFOProcessor::UFOProcessor() : AudioProcessor(BusesProperties().withOutput("Output",AudioChannelSet::stereo(),true)),state(*this,nullptr,"UFO",layout()) {
    presetAsync=std::make_shared<PresetAsyncState>();presetAsync->owner=this;
    for (auto* parameter : getParameters()) {
        auto* ranged = dynamic_cast<RangedAudioParameter*>(parameter);
        jassert(ranged != nullptr);
        parameterValues.push_back(state.getRawParameterValue(ranged->paramID));
    }
}
UFOProcessor::~UFOProcessor() {
    {std::lock_guard<std::mutex> lock(presetAsync->mutex);presetAsync->owner=nullptr;presetAsync->canceled=true;}
    // Cancel generation-aware imports before waiting for their worker to exit.
    samples->clear(0);samples->clear(1);
    presetLoader.removeAllJobs(true,-1);sampleLoader.removeAllJobs(true,-1);
}
double UFOProcessor::getTailLengthSeconds() const {
    const double release=state.getRawParameterValue("release")->load();
    double echo=0;
    if(state.getRawParameterValue("delay")->load()>.00001f) {
        const double feedback=std::clamp(double(state.getRawParameterValue("feedback")->load()),.000001,.9);
        echo=state.getRawParameterValue("delayTime")->load()*(1+std::log(.001)/std::log(feedback));
    }
    // Include release plus the maximum filter/diffusion decay after the echo tail.
    return release+echo+8;
}
void UFOProcessor::prepareToPlay(double sr,int) { updateParameters();engine.prepare(sr);setLatencySamples(engine.latencySamples());keyboard.reset(); }
bool UFOProcessor::isBusesLayoutSupported(const BusesLayout& b) const { return b.getMainOutputChannelSet()==AudioChannelSet::stereo() && b.getMainInputChannelSet().isDisabled(); }
void UFOProcessor::updateParameters() {
    // Cache parameter addresses once; avoid strings and lookups in the audio callback.
    size_t index=0;
    auto next=[&]{ return parameterValues[index++]->load(std::memory_order_relaxed); };
    auto& p=engine.params;
    for(auto& l:p.layers) {
        l.bank=int(next()); l.color=next(); l.movement=next(); l.attack=next(); l.decay=next();
        l.octave=int(next())-1; l.pitch=int(next()); l.shaper=int(next()); l.filter=int(next());
        l.cutoff=next(); l.resonance=next(); l.slew=next(); l.tilt=next(); l.spread=next(); l.oneshot=next()>.5f;
    }
    for(auto& m:p.mods) { m.type=int(next()); m.destination=int(next()); m.rate=next(); m.depth=next(); m.shape=next(); m.sync=next()>.5f; }
    p.mix=next(); p.attack=next(); p.decay=next(); p.sustain=next(); p.release=next();
    p.drive=next(); p.chorus=next(); p.flanger=next(); p.delay=next(); p.delayTime=next(); p.feedback=next(); p.reverb=next(); p.gain=next();
    for(auto& m:p.mods) {
        m.stepCount=int(next())==0 ? 8 : 16; m.shift=next();
        m.attack=next();m.decay=next();m.sustain=next();m.release=next();
        for(auto& step:m.steps)step=next();
    }
    jassert(index==parameterValues.size());
}
void UFOProcessor::midi(const MidiMessage& m) {
    if(m.isNoteOn()) engine.noteOn(m.getNoteNumber(),m.getFloatVelocity(),m.getChannel());
    else if(m.isNoteOff()) engine.noteOff(m.getNoteNumber(),m.getChannel());
    else if(m.isPitchWheel()) engine.pitchBend(m.getChannel(),float(m.getPitchWheelValue()-8192)/8192.f);
    else if(m.isController() && m.getControllerNumber()==64) engine.sustain(m.getChannel(),m.getControllerValue()>=64);
    else if(m.isAllNotesOff() || m.isAllSoundOff()) engine.reset();
}
void UFOProcessor::processBlock(AudioBuffer<float>& buffer,MidiBuffer& messages) {
    ScopedNoDenormals guard; buffer.clear(); updateParameters();
    engine.setNonRealtime(isNonRealtime());
    if (panicRequested.exchange(false)) { engine.reset(); keyboard.reset(); }
    for(size_t layer=0;layer<2;++layer)engine.setSampleBank(layer,samples->acquire(layer));
    bool transportValid=false;
    if(auto* playhead=getPlayHead()) if(auto pos=playhead->getPosition()) {
        if(auto bpm=pos->getBpm())engine.setTempo(float(*bpm));
        if(auto ppq=pos->getPpqPosition()) { engine.setTransport(*ppq,pos->getIsPlaying(),true);transportValid=true; }
    }
    if(!transportValid)engine.setTransport(0,false,false);
    keyboard.processNextMidiBuffer(messages,0,buffer.getNumSamples(),true);
    int rendered=0; float peak=0;
    auto render=[&](int end){ for(;rendered<end;++rendered) { auto sample=engine.next(); for(int c=0;c<buffer.getNumChannels();++c) buffer.setSample(c,rendered,sample[size_t(c)%2]); peak=std::max(peak,std::max(std::abs(sample[0]),std::abs(sample[1]))); } };
    for(const auto metadata:messages) { render(std::clamp(metadata.samplePosition,rendered,buffer.getNumSamples())); midi(metadata.getMessage()); }
    render(buffer.getNumSamples()); meter.store(peak); messages.clear();
    const auto modulation=engine.modulationValues();
    for(size_t slot=0;slot<4;++slot)modMeters[slot].store(modulation[slot],std::memory_order_relaxed);
    const auto positions=engine.playbackPositions(),colors=engine.playbackColors();
    for(size_t layer=0;layer<2;++layer) {
        samplePositions[layer].store(positions[layer],std::memory_order_relaxed);
        colorMeters[layer].store(colors[layer],std::memory_order_relaxed);
        samples->release(layer);
    }
}
void UFOProcessor::getStateInformation(MemoryBlock& dest) { auto xml=stateWithSamples().createXml();copyXmlToBinary(*xml,dest); }
void UFOProcessor::setStateInformation(const void* data,int size) {
    if(size<=0 || size>1024*1024)return;
    if(auto xml=getXmlFromBinary(data,size)){String error;restoreState(ValueTree::fromXml(*xml),error);}
}
void UFOProcessor::loadFactory(int index) {
    std::lock_guard<std::recursive_mutex> mutation(presetMutationMutex);
    ++presetRevision;
    clearSample(0);clearSample(1);
    for(auto* parameter:getParameters()) {
        auto* p=dynamic_cast<RangedAudioParameter*>(parameter);
        if(p) { p->beginChangeGesture(); p->setValueNotifyingHost(p->getDefaultValue()); p->endChangeGesture(); }
    }
    auto set=[&](const char* id,float v){auto* p=state.getParameter(id); p->beginChangeGesture(); p->setValueNotifyingHost(p->convertTo0to1(v)); p->endChangeGesture();};
    if(index==1) { set("upperBank",1); set("lowerBank",2); set("attack",1.4f); set("release",3); set("chorus",.55f); set("reverb",.6f); set("mod0Depth",.2f); set("mod0Rate",.2f); }
    if(index==2) { set("upperShaper",1); set("lowerShaper",1); set("upperCutoff",2400); set("lowerCutoff",1200); set("upperOctave",0); set("lowerOctave",0); set("decay",.2f); set("sustain",0); set("release",.12f); set("drive",.25f); }
    if(index==3) { set("upperBank",3); set("lowerBank",2); set("upperShaper",2); set("mod0Type",2); set("mod0Target",2); set("mod0Depth",.65f); set("mod0Rate",2); set("mod0Sync",1); set("delay",.4f); set("flanger",.3f); }
}
AudioProcessorEditor* UFOProcessor::createEditor() { return new UFOEditor(*this); }
AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new UFOProcessor(); }

void UFOProcessor::importSample(int layer,const File& file) { std::lock_guard<std::recursive_mutex> mutation(presetMutationMutex);++presetRevision;queueSample(layer,file,file.getFileName()); }
void UFOProcessor::queueSample(int layer,const File& file,const String& name) {
    if(layer<0 || layer>1)return;
    const auto token=samples->begin(size_t(layer),file,name);auto library=samples;
    const float color=state.getRawParameterValue(layer==0?"upperColor":"lowerColor")->load();
    sampleLoader.addJob([library,file,layer,token,color]{ library->load(size_t(layer),file,token,color); });
}
void UFOProcessor::clearSample(int layer) {std::lock_guard<std::recursive_mutex> mutation(presetMutationMutex);if(layer>=0 && layer<2){++presetRevision;samples->clear(size_t(layer));}}
String UFOProcessor::sampleName(int layer) const {return samples->name(size_t(std::clamp(layer,0,1)));}
String UFOProcessor::sampleError(int layer) const {return samples->error(size_t(std::clamp(layer,0,1)));}
bool UFOProcessor::sampleLoading(int layer) const {return samples->loading(size_t(std::clamp(layer,0,1)));}
std::vector<float> UFOProcessor::sampleWaveform(int layer,int points) const {
    const int selected=std::clamp(layer,0,1),count=std::clamp(points,1,4096);
    auto data=samples->waveform(size_t(selected),count);
    if(!data.empty())return data;
    const String prefix=selected==0?"upper":"lower";
    return engine.waveformPreview(int(state.getRawParameterValue(prefix+"Bank")->load()),state.getRawParameterValue(prefix+"Color")->load(),count);
}
bool UFOProcessor::sampleStreaming(int layer) const {return samples->streaming(size_t(std::clamp(layer,0,1)));}
uint64_t UFOProcessor::sampleUnderruns(int layer) const {return samples->underruns(size_t(std::clamp(layer,0,1)));}
uint64_t UFOProcessor::sampleIoErrors(int layer) const {return samples->ioErrors(size_t(std::clamp(layer,0,1)));}
uint64_t UFOProcessor::sampleCacheBytes(int layer) const {return samples->memoryBytes(size_t(std::clamp(layer,0,1)));}
File UFOProcessor::presetDirectory() const {return ufo::SampleLibrary::userDirectory().getChildFile("Presets");}
ValueTree UFOProcessor::stateWithSamples() const {
    auto tree=const_cast<AudioProcessorValueTreeState&>(state).copyState();const auto snapshot=samples->snapshot();
    for(int layer=0;layer<2;++layer) {
        tree.setProperty(layer==0?"upperSample":"lowerSample",snapshot.files[size_t(layer)].getFullPathName(),nullptr);
        tree.setProperty(layer==0?"upperSampleName":"lowerSampleName",snapshot.names[size_t(layer)],nullptr);
    }
    tree.setProperty("version",3,nullptr);return tree;
}
bool UFOProcessor::restoreState(const ValueTree& incoming,String& error) {
    std::lock_guard<std::recursive_mutex> mutation(presetMutationMutex);
    error={};
    if(!incoming.isValid() || incoming.getType()!=state.state.getType() || incoming.getNumChildren()>512){error="Invalid UFO preset state";return false;}
    ValueTree clean(state.state.getType());
    // Migrate partial/older presets from defaults, not from the previous patch.
    for(auto* base:getParameters()) {
        auto* parameter=dynamic_cast<RangedAudioParameter*>(base);ValueTree value("PARAM");
        value.setProperty("id",parameter->paramID,nullptr);
        value.setProperty("value",parameter->convertFrom0to1(parameter->getDefaultValue()),nullptr);clean.addChild(value,-1,nullptr);
    }
    for(const auto& child:incoming) {
        if(!child.hasType("PARAM"))continue;
        const String id=child.getProperty("id").toString();
        auto* parameter=state.getParameter(id);if(!parameter)continue;
        const auto raw=child.getProperty("value");
        const auto text=raw.toString().trim().toStdString();char* end=nullptr;
        const double numeric=std::strtod(text.c_str(),&end);
        if(text.empty() || end!=text.c_str()+text.size() || !std::isfinite(numeric)){error="Invalid value for "+id;return false;}
        for(auto entry:clean)if(entry.getProperty("id").toString()==id) {
            const float bounded=parameter->convertFrom0to1(std::clamp(parameter->convertTo0to1(float(numeric)),0.f,1.f));
            entry.setProperty("value",bounded,nullptr);break;
        }
    }
    ++presetRevision;state.replaceState(clean);
    for(int layer=0;layer<2;++layer) {
        const auto path=incoming.getProperty(layer==0?"upperSample":"lowerSample").toString();
        clearSample(layer);if(path.isNotEmpty())queueSample(layer,File(path),incoming.getProperty(layer==0?"upperSampleName":"lowerSampleName").toString());
    }
    return true;
}
namespace {
void checkPresetCanceled(const std::function<bool()>& check) {if(check && check())throw std::runtime_error("Preset canceled");}
void writePresetFile(const File& file,ValueTree tree,const ufo::SampleLibrary::Snapshot& snapshot,String& error,const std::function<bool()>& check={}) {
    error={};
    try {
    checkPresetCanceled(check);const auto& files=snapshot.files;
    for(size_t layer=0;layer<2;++layer) {
        if(snapshot.loading[layer]){error="Wait for sample loading before saving";return;}
        if(snapshot.active[layer] && !files[layer].existsAsFile()){error="An imported sample is missing from the cache; reimport it before saving";return;}
    }
    tree.setProperty("version",3,nullptr);ZipFile::Builder archive;
    for(int layer=0;layer<2;++layer) {
        const auto& source=files[size_t(layer)];String entry;
        if(source.existsAsFile()) {
            entry="samples/"+String(layer)+source.getFileExtension();archive.addFile(source,0,entry);
            tree.setProperty(layer==0?"upperSampleSha256":"lowerSampleSha256",ufo::WidePresetArchive::hashFile(source,check),nullptr);
        }
        tree.setProperty(layer==0?"upperSample":"lowerSample",entry,nullptr);
        tree.setProperty(layer==0?"upperSampleName":"lowerSampleName",snapshot.names[size_t(layer)],nullptr);
    }
    if(snapshot.streaming[0] || snapshot.streaming[1]) {ufo::WidePresetArchive::write(file,tree,files,error,check);return;}
    const auto xml=tree.createXml()->toString();
    archive.addEntry(new MemoryInputStream(xml.toRawUTF8(),size_t(xml.getNumBytesAsUTF8()),true),6,"state.xml",Time::getCurrentTime());
    if(file.getParentDirectory().createDirectory().failed()){error="Cannot create preset directory";return;}
    TemporaryFile temporary(file);
    {
        FileOutputStream stream(temporary.getFile());
        if(!stream.openedOk() || !archive.writeToStream(stream,nullptr)){error="Cannot write preset archive";return;}
        stream.flush();if(stream.getStatus().failed()){error=stream.getStatus().getErrorMessage();return;}
    }
    checkPresetCanceled(check);if(!temporary.overwriteTargetFileWithTemporary())error="Cannot replace preset file";
    } catch(const std::exception& e){error=e.what();}
}
ValueTree readPresetFile(const File& file,const File& cache,String& error,const std::function<bool()>& check={}) {
    error={};
    try {
    checkPresetCanceled(check);
    if(ufo::WidePresetArchive::matches(file)) {
        return ufo::WidePresetArchive::read(file,cache,error,check);
    }
    ZipFile archive(file);
    if(archive.getNumEntries()<1 || archive.getNumEntries()>3){error="Invalid UFO preset archive";return {};}
    const int stateIndex=archive.getIndexOfFileName("state.xml");const auto* stateEntry=archive.getEntry(stateIndex);
    if(!stateEntry || stateEntry->uncompressedSize>1024*1024 || stateEntry->uncompressedSize<1){error="Missing or oversized preset state";return {};}
    std::unique_ptr<InputStream> stream(archive.createStreamForEntry(stateIndex));
    if(!stream){error="Cannot read preset state";return {};}
    MemoryOutputStream stateBytes;
    const auto stateSize=stateBytes.writeFromInputStream(*stream,1024*1024+1);
    if(stateSize!=stateEntry->uncompressedSize || stateSize>1024*1024){error="Preset state size mismatch";return {};}
    const auto xml=XmlDocument::parse(stateBytes.toUTF8());
    if(!xml){error="Invalid preset XML";return {};}
    auto tree=ValueTree::fromXml(*xml);
    if(!tree.isValid() || !tree.hasType("UFO")){error="Not a UFO preset";return {};}
    if(cache.createDirectory().failed()){error="Cannot create sample cache";return {};}
    for(int layer=0;layer<2;++layer) {
        checkPresetCanceled(check);
        const Identifier key=layer==0?"upperSample":"lowerSample";const auto name=tree.getProperty(key).toString();
        if(name.isEmpty())continue;
        const auto extension=File(name).getFileExtension().toLowerCase();
        if(name!="samples/"+String(layer)+extension || !StringArray{".wav",".aiff",".aif",".flac",".ogg",".mp3"}.contains(extension)) {error="Invalid sample entry path";return {};}
        const int index=archive.getIndexOfFileName(name);const auto* entry=archive.getEntry(index);
        if(!entry || entry->isSymbolicLink || entry->uncompressedSize<1 || entry->uncompressedSize>128*1024*1024){error="Missing or oversized preset sample";return {};}
        std::unique_ptr<InputStream> audio(archive.createStreamForEntry(index));
        if(!audio){error="Cannot read preset sample";return {};}
        const auto expected=tree.getProperty(layer==0?"upperSampleSha256":"lowerSampleSha256").toString();
        if(expected.length()!=64 || !expected.containsOnly("0123456789abcdef")){error="Invalid preset sample checksum";return {};}
        const auto destination=cache.getChildFile(expected+extension);
        if(destination.isSymbolicLink()){error="Sample cache path is a symbolic link";return {};}
        TemporaryFile temporary(destination);
        {
            FileOutputStream output(temporary.getFile());
            if(!output.openedOk()){error="Cannot extract preset sample";return {};}
            std::array<char,65536> buffer{};auto remaining=entry->uncompressedSize;
            while(remaining>0){checkPresetCanceled(check);const auto count=int(std::min(juce::int64(buffer.size()),remaining));if(audio->read(buffer.data(),count)!=count || !output.write(buffer.data(),size_t(count))){error="Cannot extract preset sample";return {};}remaining-=count;}
            output.flush();if(output.getStatus().failed()){error="Cannot finish extracting preset sample";return {};}
        }
        if(ufo::WidePresetArchive::hashFile(temporary.getFile(),check)!=expected){error="Preset sample checksum mismatch";return {};}
        AudioFormatManager formats;formats.registerBasicFormats();
        std::unique_ptr<AudioFormatReader> reader(formats.createReaderFor(temporary.getFile().createInputStream()));
        if(!reader){error="Invalid audio in preset archive";return {};}
        reader.reset();
        if((!destination.existsAsFile() || ufo::WidePresetArchive::hashFile(destination,check)!=expected) && !temporary.overwriteTargetFileWithTemporary()) {
            error="Cannot cache preset sample";return {};
        }
        tree.setProperty(key,destination.getFullPathName(),nullptr);
    }
    checkPresetCanceled(check);return tree;
    } catch(const std::exception& e){error=e.what();return {};}
}
}

void UFOProcessor::saveUserPreset(const File& file,String& error) const {
    writePresetFile(file,const_cast<AudioProcessorValueTreeState&>(state).copyState(),samples->snapshot(),error);
}
bool UFOProcessor::loadUserPreset(const File& file,String& error) {
    const auto tree=readPresetFile(file,ufo::SampleLibrary::cacheDirectory(),error);
    return tree.isValid() && restoreState(tree,error);
}
bool UFOProcessor::presetJobBusy() const {return presetAsync->busy.load();}
bool UFOProcessor::saveUserPresetAsync(const File& file,PresetCallback callback) {
    bool expected=false;if(!presetAsync->busy.compare_exchange_strong(expected,true))return false;
    std::lock_guard<std::recursive_mutex> mutation(presetMutationMutex);
    auto task=presetAsync;auto tree=state.copyState();const auto snapshot=samples->snapshot();
    presetLoader.addJob([task,file,tree,snapshot,callback=std::move(callback)] {
        String error;writePresetFile(file,tree,snapshot,error,[task]{return task->canceled.load();});
        MessageManager::callAsync([task,error,callback]{
            {std::lock_guard<std::mutex> lock(task->mutex);if(!task->owner || task->canceled.load())return;task->busy=false;}
            if(callback)callback(error);
        });
    });return true;
}
bool UFOProcessor::loadUserPresetAsync(const File& file,PresetCallback callback) {
    bool expected=false;if(!presetAsync->busy.compare_exchange_strong(expected,true))return false;
    std::lock_guard<std::recursive_mutex> mutation(presetMutationMutex);
    auto task=presetAsync;const auto revision=presetRevision.load();const auto cache=ufo::SampleLibrary::cacheDirectory();
    presetLoader.addJob([task,file,cache,revision,callback=std::move(callback)] {
        String error;const auto tree=readPresetFile(file,cache,error,[task]{return task->canceled.load();});
        MessageManager::callAsync([task,tree,error,revision,callback]() mutable {
            {
                std::lock_guard<std::mutex> lock(task->mutex);auto* owner=task->owner;if(!owner || task->canceled.load())return;
                std::lock_guard<std::recursive_mutex> completionMutation(owner->presetMutationMutex);
                if(tree.isValid() && error.isEmpty()) {
                    if(owner->presetRevision.load()!=revision)error="Preset load was superseded by a newer patch";
                    else owner->restoreState(tree,error);
                }
                task->busy=false;
            }
            if(callback)callback(error);
        });
    });return true;
}
