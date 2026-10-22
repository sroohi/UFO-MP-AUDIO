#include <juce_data_structures/juce_data_structures.h>
#include "StreamAssetCache.h"
#include "WidePresetArchive.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
constexpr double pi=3.14159265358979323846;
constexpr int sampleRate=44100,seconds=31;
constexpr juce::int64 frames=juce::int64(sampleRate)*seconds;
int checks=0;
void require(bool passed,const char* what) {++checks;if(!passed)throw std::runtime_error(what);}
float signal(juce::int64 frame,int channel) {return float((channel==0?.6:.3)*std::sin(2*pi*(channel==0?200:311)*double(frame)/sampleRate));}
void writeWave(const juce::File& file) {
    auto output=file.createOutputStream();require(bool(output),"create source WAV");
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(output.release(),sampleRate,2,32,{},0));
    require(bool(writer),"create source WAV writer");juce::AudioBuffer<float> data(2,8192);
    for(juce::int64 first=0;first<frames;first+=8192) {
        const int count=int(std::min(juce::int64(8192),frames-first));
        for(int channel=0;channel<2;++channel)for(int i=0;i<count;++i)data.setSample(channel,i,signal(first+i,channel));
        require(writer->writeFromAudioSampleBuffer(data,0,count),"write real long-source WAV frames");
    }
    require(writer->flush(),"flush source WAV");
}
std::unique_ptr<juce::AudioFormatReader> readerFor(const juce::File& file) {
    juce::AudioFormatManager formats;formats.registerBasicFormats();
    auto reader=std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(file));require(bool(reader),"decode source with JUCE format reader");return reader;
}
ufo::StreamAssetCache::Result build(const juce::File& source,const juce::File& cache,float color) {
    auto reader=readerFor(source);return ufo::StreamAssetCache::build(*reader,cache,color);
}
float expectedAt(double position,int channel) {
    const auto first=juce::int64(position);const float fraction=float(position-double(first));
    return signal(first,channel)+fraction*(signal(first+1,channel)-signal(first,channel));
}
void corruptByte(const juce::File& file,juce::int64 position) {
    int byte;
    {juce::FileInputStream input(file);require(input.openedOk()&&input.setPosition(position),"seek fixture byte");byte=input.readByte();}
    juce::FileOutputStream output(file);require(output.openedOk()&&output.setPosition(position),"open fixture byte for mutation");
    require(output.writeByte(char(byte^0x40)),"mutate fixture byte");output.flush();require(output.getStatus().wasOk(),"flush fixture mutation");
}
void archiveRejected(const juce::File& archive,const juce::File& cache,const char* message) {
    juce::String error;const auto loaded=ufo::WidePresetArchive::read(archive,cache,error);
    require(!loaded.isValid()&&error.isNotEmpty(),message);
}
void floatPeakRoundTrip(const juce::File& directory) {
    constexpr int hotFrames=8192;
    const auto source=directory.getChildFile("Float-over-unity.wav"),cache=directory.getChildFile("float-cache");
    require(cache.createDirectory().wasOk(),"create float asset cache");
    juce::AudioBuffer<float> data(2,hotFrames);
    for(int frame=0;frame<hotFrames;++frame) {
        data.setSample(0,frame,1.5f*float(std::sin(2*pi*200*frame/sampleRate)));
        data.setSample(1,frame,.4f*float(std::sin(2*pi*311*frame/sampleRate)));
    }
    const float originalPeak=data.getMagnitude(0,hotFrames);
    {
        auto output=source.createOutputStream();require(bool(output),"create over-unity float WAV fixture");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(output.release(),sampleRate,2,32,{},0));
        require(bool(writer)&&writer->writeFromAudioSampleBuffer(data,0,hotFrames)&&writer->flush(),"write float WAV without pre-normalizing audio");
    }
    auto sourceReader=readerFor(source);
    require(sourceReader->usesFloatingPointData&&sourceReader->bitsPerSample==32,"over-unity fixture uses actual IEEE float32 WAV encoding");
    auto asset=ufo::StreamAssetCache::build(*sourceReader,cache,0);sourceReader.reset();
    auto canonicalReader=readerFor(asset.canonical);juce::AudioBuffer<float> canonicalData(2,hotFrames);
    require(canonicalReader->usesFloatingPointData&&canonicalReader->bitsPerSample==32,"canonical cache retains IEEE float32 WAV encoding");
    require(canonicalReader->read(&canonicalData,0,hotFrames,0,true,true),"decode canonical over-unity sample");canonicalReader.reset();
    const float canonicalPeak=canonicalData.getMagnitude(0,hotFrames);
    require(canonicalPeak>1.49f&&std::abs(canonicalPeak-originalPeak)<1e-6,"canonical audio preserves over-unity peaks rather than clipping to one");
    double largestDifference=0;
    for(int channel=0;channel<2;++channel)for(int frame=0;frame<hotFrames;++frame)
        largestDifference=std::max(largestDifference,double(std::abs(canonicalData.getSample(channel,frame)-data.getSample(channel,frame))));
    require(largestDifference<1e-7,"canonical float WAV round-trips every decoded source sample exactly");
    const auto hash=juce::SHA256(asset.canonical).toHexString();
    const auto manifest=juce::JSON::parse(cache.getChildFile(hash+".stream-v1/manifest.json").loadFileAsString());
    const float gain=float(manifest["gain"]);
    require(gain<1&&std::abs(gain-1/originalPeak)<1e-6,"manifest stores normalization gain separately from unmodified canonical audio");
    const auto normalized=asset.bank->read(200,0,true,.5);
    require(std::abs(normalized[0]-data.getSample(0,200)/originalPeak)<1e-6&&std::abs(normalized[0])<=1,"streamed render applies the separate normalization gain");
    juce::ValueTree state("UFO");state.setProperty("upperSample","samples/0.wav",nullptr);state.setProperty("lowerSample","",nullptr);
    state.setProperty("upperSampleSha256",hash,nullptr);const auto archive=directory.getChildFile("Float-over-unity.ufo");juce::String error;
    require(ufo::WidePresetArchive::write(archive,state,{asset.canonical,juce::File{}},error)&&error.isEmpty(),"bundle over-unity canonical WAV in portable preset");
    const auto canonical=asset.canonical;asset.bank.reset();require(source.deleteFile()&&canonical.deleteFile(),"remove float source and canonical before portable restore");
    const auto restoredCache=directory.getChildFile("float-restored-cache");
    const auto restored=ufo::WidePresetArchive::read(archive,restoredCache,error);
    require(restored.isValid()&&error.isEmpty(),"restore portable over-unity float preset into empty cache");
    const juce::File restoredWave(restored.getProperty("upperSample").toString());
    require(juce::SHA256(restoredWave).toHexString()==hash,"portable restore preserves exact over-unity WAV bytes");
    auto reimported=build(restoredWave,restoredCache,0);
    const auto replay=reimported.bank->read(200,0,true,.5);
    require(std::abs(replay[0]-normalized[0])<1e-7&&std::abs(replay[1]-normalized[1])<1e-7,"portable preset reimport reproduces normalized stereo samples");
    auto restoredReader=readerFor(reimported.canonical);juce::AudioBuffer<float> restoredData(2,hotFrames);
    require(restoredReader->read(&restoredData,0,hotFrames,0,true,true),"decode reimported float canonical sample");
    require(std::abs(restoredData.getMagnitude(0,hotFrames)-originalPeak)<1e-6,"reimport does not bake normalization into canonical media");
    std::cout<<"Float WAV evidence: source/canonical peak="<<originalPeak<<"/"<<canonicalPeak<<", gain="<<gain<<", IEEE float32=1, largest sample difference="<<largestDifference<<'\n';
}
}
int main() {
    try {
        const auto directory=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ufo-stream-assets-"+juce::Uuid().toString());
        require(directory.createDirectory().wasOk(),"create isolated fixture directory");
        struct Cleanup {juce::File path;~Cleanup(){path.deleteRecursively();}} cleanup{directory};
        const auto source=directory.getChildFile("31-second-stereo.wav"),cache=directory.getChildFile("cache");
        require(cache.createDirectory().wasOk(),"create fixture asset cache");writeWave(source);
        auto originalReader=readerFor(source);
        require(originalReader->lengthInSamples==frames&&originalReader->numChannels==2&&originalReader->sampleRate==sampleRate,"fixture really decodes as31-second stereo44.1kHz audio");
        originalReader.reset();
        constexpr float color=.37f;
        auto asset=build(source,cache,color);
        require(asset.bank&&asset.bank->isStreaming(),"actual long audio import selects streaming bank");
        require(asset.bank->frames()==uint64_t(frames)&&asset.bank->sourceRate()==sampleRate,"streaming dimensions retain decoded metadata");
        require(asset.bank->streamMemoryBytes()<17*1024*1024,"imported long-source resident cache is bounded below17MiB");
        require(asset.canonical.existsAsFile(),"canonical decoded audio is published");
        auto canonicalReader=readerFor(asset.canonical);
        require(canonicalReader->lengthInSamples==frames&&canonicalReader->sampleRate==sampleRate&&canonicalReader->numChannels==2,"canonical WAV preserves complete source dimensions");
        juce::AudioBuffer<float> lastFrames(2,32);
        require(canonicalReader->read(&lastFrames,0,32,frames-32,true,true),"canonical WAV seeks to actual final frames");
        require(std::abs(lastFrames.getSample(1,31)-signal(frames-1,1))<1e-6,"canonical WAV retains final stereo sample");canonicalReader.reset();

        const auto digest=juce::SHA256(asset.canonical).toHexString();
        const auto pyramid=cache.getChildFile(digest+".stream-v1"),manifest=pyramid.getChildFile("manifest.json");
        require(manifest.existsAsFile(),"complete mip manifest is published");
        auto metadata=juce::JSON::parse(manifest.loadFileAsString());
        require(int(metadata["version"])==1&&juce::int64(metadata["frames"])==frames&&double(metadata["sampleRate"])==sampleRate,"manifest stores verified source metadata");
        const int levels=int(metadata["levels"]);require(levels>16&&levels<=32,"long source receives a complete downsample pyramid");
        uint64_t count=uint64_t(frames);
        for(int level=0;level<levels;++level) {
            const auto file=pyramid.getChildFile("level"+juce::String(level)+".f32");
            require(file.existsAsFile()&&file.getSize()==juce::int64(count*8),"every published stereo mip has exact byte length");count/=2;
        }
        require(count==0,"pyramid reaches the final one-frame level");
        require(asset.bank->prewarm(color),"source scanning window prewarms at every mip");
        const double position=double(color)*double(frames-4096)+200;
        const auto sample=asset.bank->read(200,color,false,.5);
        require(std::abs(sample[0]-expectedAt(position,0))<1e-5&&std::abs(sample[1]-expectedAt(position,1))<1e-5,"actual decoded stereo content flows through the streamed bank");
        require(asset.bank->streamIoErrors()==0,"published cache reads without I/O corruption");
        const auto overview=asset.bank->waveform(257);
        require(overview.size()==257&&std::all_of(overview.begin(),overview.end(),[](float value){return std::isfinite(value)&&std::abs(value)<=1;}),"real long-source waveform overview is bounded and resizes exactly");
        require(std::any_of(overview.begin(),overview.end(),[](float value){return std::abs(value)>.59f;}),"streamed overview contains actual decoded peaks");
        const auto manifestTime=manifest.getLastModificationTime();
        auto reused=build(source,cache,color);
        require(reused.canonical==asset.canonical&&manifest.getLastModificationTime()==manifestTime,"unchanged canonical audio reuses the published mip manifest");
        require(cache.findChildFiles(juce::File::findDirectories,false,"*.stream-v1*").size()==1,"cache reuse creates no duplicate pyramid");
        require(reused.bank->waveform(257)==overview,"manifest reuse restores the same actual waveform overview");
        asset.bank.reset();reused.bank.reset();

        // A truncated cached level must be replaced by a complete newly published
        // pyramid; cached byte lengths are validation evidence, not readiness alone.
        {
            juce::FileOutputStream output(pyramid.getChildFile("level0.f32"));
            require(output.openedOk()&&output.setPosition(16)&&output.truncate().wasOk(),"truncate an existing cache level");
        }
        auto recovered=build(source,cache,color);
        require(recovered.bank->isStreaming()&&recovered.bank->prewarm(color),"truncated existing cache is rebuilt before publication");
        require(cache.findChildFiles(juce::File::findDirectories,false,"*.stream-v1*").size()==2,"invalid pyramid is preserved while complete replacement is published");
        const auto restored=recovered.bank->read(200,color,false,.5);
        require(std::abs(restored[0]-expectedAt(position,0))<1e-5,"rebuilt pyramid restores actual source samples");
        juce::File repairDirectory;
        for(const auto& candidate:cache.findChildFiles(juce::File::findDirectories,false,"*.stream-v1*"))if(candidate!=pyramid)repairDirectory=candidate;
        require(repairDirectory.isDirectory(),"find published repair pyramid");
        const auto repairManifest=repairDirectory.getChildFile("manifest.json");const auto repairTime=repairManifest.getLastModificationTime();
        auto reusedRepair=build(source,cache,color);
        require(cache.findChildFiles(juce::File::findDirectories,false,"*.stream-v1*").size()==2&&repairManifest.getLastModificationTime()==repairTime,"subsequent import reuses valid repair rather than multiplying UUID pyramids");
        require(reusedRepair.bank->waveform(257)==overview,"reused repair preserves real waveform overview");
        reusedRepair.bank.reset();recovered.bank.reset();
        {
            auto reader=readerFor(source);bool canceled=false;
            try{ufo::StreamAssetCache::build(*reader,cache,0,[]{return true;});}catch(const std::runtime_error&){canceled=true;}
            require(canceled,"canceled import stops before publishing a bank");
            require(cache.findChildFiles(juce::File::findDirectories,false,"import-*").isEmpty(),"canceled import removes incomplete staging files");
        }

        // The64-bit preset container owns its audio; neither original nor source
        // cache files are needed to extract and verify it on another installation.
        juce::ValueTree state("UFO");state.setProperty("upperSample","samples/0.wav",nullptr);state.setProperty("lowerSample","",nullptr);
        state.setProperty("upperSampleSha256",digest,nullptr);juce::ValueTree parameter("PARAM");parameter.setProperty("id","gain",nullptr);parameter.setProperty("value",.42,nullptr);state.addChild(parameter,-1,nullptr);
        const auto archive=directory.getChildFile("LongTexture.ufo");juce::String error;
        require(ufo::WidePresetArchive::write(archive,state,{asset.canonical,juce::File{}},error)&&error.isEmpty(),"write self-contained wide preset with actual long media");
        require(ufo::WidePresetArchive::matches(archive),"wide container identifies its own magic");
        const auto archiveSize=archive.getSize();require(archiveSize>asset.canonical.getSize(),"wide preset physically contains the canonical audio bytes");
        const auto canonical=asset.canonical;require(source.deleteFile()&&canonical.deleteFile(),"remove original and source cache before restore");
        const auto destinationCache=directory.getChildFile("restored-cache");
        auto loaded=ufo::WidePresetArchive::read(archive,destinationCache,error);
        require(loaded.isValid()&&error.isEmpty()&&int(loaded.getProperty("version"))==3,"wide preset round-trips state into an empty cache");
        const juce::File extracted(loaded.getProperty("upperSample").toString());
        require(extracted.isAChildOf(destinationCache)&&extracted.existsAsFile()&&juce::SHA256(extracted).toHexString()==digest,"restored media has the exact declared SHA256");
        auto extractedReader=readerFor(extracted);require(extractedReader->lengthInSamples==frames,"self-contained archive restores all31seconds");extractedReader.reset();
        require(std::abs(double(loaded.getChild(0).getProperty("value"))-.42)<1e-6,"wide preset preserves serialized parameter state");
        error={};const auto second=ufo::WidePresetArchive::read(archive,destinationCache,error);
        require(second.isValid()&&error.isEmpty()&&second.getProperty("upperSample")==loaded.getProperty("upperSample"),"already valid cached media is verified and reused on restore");

        const auto tampered=directory.getChildFile("Tampered.ufo");require(archive.copyFileTo(tampered),"copy tamper fixture");
        juce::int64 mediaOffset;
        {juce::FileInputStream input(tampered);require(input.setPosition(8),"seek wide header");mediaOffset=12+input.readInt()+8;}
        corruptByte(tampered,mediaOffset+2048);
        archiveRejected(tampered,directory.getChildFile("tamper-empty-cache"),"media checksum rejects tampering before extraction publication");
        archiveRejected(tampered,destinationCache,"media checksum also rejects tampering when a valid cached copy exists");
        const auto truncated=directory.getChildFile("Truncated.ufo");require(archive.copyFileTo(truncated),"copy truncated fixture");
        {juce::FileOutputStream output(truncated);require(output.openedOk()&&output.setPosition(archiveSize-1000)&&output.truncate().wasOk(),"truncate actual preset media");}
        archiveRejected(truncated,destinationCache,"truncated wide media is rejected");
        const auto trailing=directory.getChildFile("Trailing.ufo");require(archive.copyFileTo(trailing),"copy trailing fixture");
        {juce::FileOutputStream output(trailing);require(output.openedOk()&&output.writeByte(1),"append unexpected archive data");}
        archiveRejected(trailing,destinationCache,"unexpected data following media is rejected");
        const auto invalid=directory.getChildFile("Invalid.ufo");require(invalid.replaceWithText("not a UFO preset"),"create invalid header fixture");
        require(!ufo::WidePresetArchive::matches(invalid),"unrelated files do not match wide preset format");
        archiveRejected(invalid,destinationCache,"invalid wide header is rejected");
        floatPeakRoundTrip(directory);
        std::cout<<"PASS: "<<checks<<" stream asset integration checks; real31-second decode, complete mip publication/reuse/recovery, bounded resident memory, waveform, cancellation and self-contained wide preset integrity\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
