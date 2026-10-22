#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include "SampleBank.h"
#include "WidePresetArchive.h"
#include <functional>
#include <fstream>
#include <chrono>
#include <filesystem>
#include <limits>

namespace ufo {
// All methods in this helper run on an import worker, never the render callback.
class StreamAssetCache {
    static constexpr int blockFrames=32768;
    static void demand(bool ok,const char* error) {if(!ok)throw std::runtime_error(error);}
    static void canceled(const std::function<bool()>& check) {if(check && check())throw std::runtime_error("Import canceled");}
    static void write(juce::FileOutputStream& stream,const void* data,size_t bytes) {demand(stream.write(data,bytes),"Cannot write streaming cache (disk full or unavailable)");}
    static bool numeric(const juce::var& value) {return value.isInt() || value.isInt64() || value.isDouble();}
    static void warm(StreamingSource& stream,float color,const std::function<bool()>& check) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        do {
            canceled(check);
            if(stream.prewarm(color,20)){canceled(check);return;}
            demand(stream.ioErrors()==0,"Cannot read streaming cache during prewarm");
        }while(std::chrono::steady_clock::now()<deadline);
        throw std::runtime_error("Cannot prewarm streamed audio");
    }
    static StreamMetadata readManifest(const juce::File& directory,uint64_t frames,double sampleRate) {
        demand(directory.isDirectory() && !directory.isSymbolicLink(),"Invalid streaming cache directory");
        const auto file=directory.getChildFile("manifest.json");
        demand(!file.isSymbolicLink() && file.existsAsFile() && file.getSize()>0 && file.getSize()<1024*1024,"Invalid streaming cache manifest");
        const auto json=juce::JSON::parse(file.loadFileAsString());
        demand(json.isObject(),"Invalid streaming cache manifest object");
        const auto version=json["version"],storedFrames=json["frames"],storedRate=json["sampleRate"],gain=json["gain"],levels=json["levels"];
        demand(version.isInt() && int(version)==1 && (storedFrames.isInt() || storedFrames.isInt64()) && juce::int64(storedFrames)>0 && uint64_t(juce::int64(storedFrames))==frames && numeric(storedRate) && double(storedRate)==sampleRate,"Streaming cache version or dimensions changed");
        StreamMetadata meta;meta.sampleRate=sampleRate;
        demand(numeric(gain),"Invalid streaming cache gain type");meta.gain=float(gain);
        demand(std::isfinite(meta.gain) && meta.gain>0 && meta.gain<=1,"Invalid streaming cache gain");
        int expected=1;for(auto n=frames;n>=2 && expected<32;n/=2)++expected;
        demand(levels.isInt() && int(levels)==expected,"Streaming cache mip pyramid is incomplete");
        const int count=int(levels);auto n=frames;
        for(int i=0;i<count;++i) {
            demand(n>0 && n<=uint64_t(std::numeric_limits<juce::int64>::max())/8,"Invalid streaming mip dimensions");
            auto path=directory.getChildFile("level"+juce::String(i)+".f32");
            demand(!path.isSymbolicLink() && path.existsAsFile() && path.getSize()==juce::int64(n*8),"Streaming cache level is missing or truncated");
            meta.levels.push_back({path.getFullPathName().toStdString(),n});n/=2;
        }
        const auto* overview=json["overview"].getArray();demand(overview && overview->size()>0 && overview->size()<=4096,"Invalid streaming waveform overview");
        for(const auto& entry:*overview){demand(numeric(entry),"Invalid streaming waveform value type");const float v=float(entry);demand(std::isfinite(v) && std::abs(v)<=1.001f,"Invalid streaming waveform value");meta.overview.push_back(v);}
        return meta;
    }
    static void downsample(const juce::File& source,const juce::File& destination,uint64_t frames,const std::function<bool()>& check) {
        std::array<double,47> kernel{};double total=0;
        for(int i=0;i<47;++i){const int n=i-23;const double sinc=n==0?.44:std::sin(.44*3.141592653589793*n)/(3.141592653589793*n);const double w=.42-.5*std::cos(2*3.141592653589793*i/46)+.08*std::cos(4*3.141592653589793*i/46);kernel[size_t(i)]=sinc*w;total+=sinc*w;}
        for(auto& k:kernel)k/=total;
        juce::FileInputStream input(source);juce::FileOutputStream output(destination);
        demand(input.openedOk() && output.openedOk(),"Cannot open streaming mip file");
        std::vector<float> incoming(size_t((blockFrames*2+48)*2)),outgoing(size_t(blockFrames*2));
        const uint64_t count=frames/2;
        for(uint64_t first=0;first<count;first+=blockFrames) {
            canceled(check);const int size=int(std::min(uint64_t(blockFrames),count-first));
            const juce::int64 begin=std::max(juce::int64(0),juce::int64(first*2)-23);
            const juce::int64 end=std::min(juce::int64(frames),juce::int64((first+uint64_t(size)-1)*2)+24);
            demand(input.setPosition(begin*8),"Cannot seek streaming mip source");
            const int bytes=int((end-begin)*8);demand(input.read(incoming.data(),bytes)==bytes,"Truncated streaming mip source");
            for(int frame=0;frame<size;++frame)for(size_t channel=0;channel<2;++channel) {
                double value=0;
                for(int tap=0;tap<47;++tap){const auto index=std::clamp(juce::int64((first+uint64_t(frame))*2)+tap-23,juce::int64(0),juce::int64(frames-1));value+=incoming[size_t(index-begin)*2+channel]*kernel[size_t(tap)];}
                outgoing[size_t(frame)*2+channel]=float(value);
            }
            write(output,outgoing.data(),size_t(size)*8);
        }
        output.flush();demand(output.getStatus().wasOk(),"Cannot flush streaming mip file");
    }
public:
    struct Result {std::shared_ptr<const SampleBank> bank;juce::File canonical;};
    static Result build(juce::AudioFormatReader& reader,const juce::File& cache,float color,const std::function<bool()>& check={}) {
        canceled(check);
        demand(reader.lengthInSamples>=64 && std::isfinite(reader.sampleRate) && reader.sampleRate>=8000 && reader.sampleRate<=192000 && reader.numChannels>=1 && reader.numChannels<=2 && reader.lengthInSamples<=juce::int64(reader.sampleRate*7200),"Unsupported streaming audio dimensions");
        const auto frames=uint64_t(reader.lengthInSamples);
        // Canonical WAV + raw level zero + all smaller levels total <24 bytes/frame.
        const auto required=juce::int64(frames*24+4*1024*1024);
        demand(cache.getBytesFreeOnVolume()>required,"Not enough free disk space for the streamed audio and mip cache");
        const auto staging=cache.getChildFile("import-"+juce::Uuid().toString());demand(staging.createDirectory().wasOk(),"Cannot create streaming import directory");
        struct Cleanup {juce::File directory;~Cleanup(){directory.deleteRecursively();}} cleanup{staging};
        const auto wave=staging.getChildFile("source.wav");
        float peak=1;std::vector<float> overview(size_t(std::min(uint64_t(4096),frames)),0);
        {
            auto file=wave.createOutputStream();demand(bool(file),"Cannot create canonical streaming WAV");
            juce::WavAudioFormat wav;std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(file.release(),reader.sampleRate,2,32,{},0));demand(bool(writer),"Cannot create streaming WAV writer");
            juce::FileOutputStream raw(staging.getChildFile("level0.f32"));demand(raw.openedOk(),"Cannot create streaming raw audio");
            juce::AudioBuffer<float> decoded(2,blockFrames);std::vector<float> interleaved(size_t(blockFrames*2));
            for(uint64_t first=0;first<frames;first+=blockFrames) {
                canceled(check);const int size=int(std::min(uint64_t(blockFrames),frames-first));
                demand(reader.read(&decoded,0,size,juce::int64(first),true,reader.numChannels>1),"Cannot decode long audio file");
                if(reader.numChannels==1)decoded.copyFrom(1,0,decoded,0,0,size);
                for(int i=0;i<size;++i)for(int c=0;c<2;++c) {
                    const float v=decoded.getSample(c,i);demand(std::isfinite(v),"Imported audio contains non-finite samples");
                    peak=std::max(peak,std::abs(v));interleaved[size_t(i)*2+size_t(c)]=v;
                    auto& bin=overview[size_t((first+uint64_t(i))*overview.size()/frames)];if(std::abs(v)>std::abs(bin))bin=v;
                }
                demand(writer->writeFromAudioSampleBuffer(decoded,0,size),"Cannot write canonical long WAV");write(raw,interleaved.data(),size_t(size)*8);
            }
            demand(writer->flush(),"Cannot flush canonical long WAV");raw.flush();demand(raw.getStatus().wasOk(),"Cannot flush raw streaming audio");
        }
        canceled(check);const auto hash=WidePresetArchive::hashFile(wave,check);canceled(check);
        auto canonical=cache.getChildFile(hash+".wav");
        demand(!canonical.isSymbolicLink(),"Canonical cache path is a symbolic link");
        if(!canonical.existsAsFile() || canonical.getSize()!=wave.getSize() || WidePresetArchive::hashFile(canonical,check)!=hash) {
            // Both paths are on the cache volume. POSIX rename atomically replaces
            // a stale file without the delete-before-move gap in File::moveFileTo.
            canceled(check);std::error_code error;
            std::filesystem::rename(wave.getFullPathName().toStdString(),canonical.getFullPathName().toStdString(),error);
            demand(!error,"Cannot publish canonical streaming WAV");
        } else demand(wave.deleteFile(),"Cannot remove staged duplicate audio");
        auto finalDirectory=cache.getChildFile(hash+".stream-v1");
        if(finalDirectory.isDirectory() && !finalDirectory.isSymbolicLink()) {
            try {
                auto stream=std::make_shared<StreamingSource>(readManifest(finalDirectory,frames,reader.sampleRate));
                warm(*stream,color,check);
                return {std::make_shared<SampleBank>(stream),canonical};
            } catch(const std::exception&) {
                canceled(check);
                // Keep old directories intact while existing instances hold
                // open files, but reuse a previously repaired pyramid on reload.
                for(const auto& repaired:cache.findChildFiles(juce::File::findDirectories,false,hash+".stream-v1-*")) {
                    canceled(check);
                    try {
                        auto stream=std::make_shared<StreamingSource>(readManifest(repaired,frames,reader.sampleRate));
                        warm(*stream,color,check);
                        return {std::make_shared<SampleBank>(stream),canonical};
                    }catch(const std::exception&){canceled(check);}
                }
                finalDirectory=cache.getChildFile(hash+".stream-v1-"+juce::Uuid().toString());
            }
        } else demand(!finalDirectory.exists() && !finalDirectory.isSymbolicLink(),"Streaming cache path is unavailable");
        uint64_t count=frames;int level=1;
        while(count>=2 && level<32) {
            downsample(staging.getChildFile("level"+juce::String(level-1)+".f32"),staging.getChildFile("level"+juce::String(level)+".f32"),count,check);count/=2;++level;
        }
        const float gain=1/peak;juce::Array<juce::var> bins;for(float v:overview)bins.add(v*gain);
        auto* object=new juce::DynamicObject;juce::var manifest(object);
        object->setProperty("version",1);object->setProperty("frames",juce::int64(frames));object->setProperty("sampleRate",reader.sampleRate);object->setProperty("gain",gain);object->setProperty("levels",level);object->setProperty("overview",bins);
        demand(staging.getChildFile("manifest.json").replaceWithText(juce::JSON::toString(manifest)),"Cannot save streaming cache manifest");
        // This move publishes a complete cache. A failed/canceled build leaves no partial manifest.
        canceled(check);std::error_code publishError;
        std::filesystem::rename(staging.getFullPathName().toStdString(),finalDirectory.getFullPathName().toStdString(),publishError);
        // Another plugin instance may have finished the same content first.
        // Its complete, validated directory is reusable; our staging is cleaned.
        demand(!publishError || (finalDirectory.isDirectory() && !finalDirectory.isSymbolicLink()),"Cannot publish streaming cache directory");
        auto stream=std::make_shared<StreamingSource>(readManifest(finalDirectory,frames,reader.sampleRate));
        warm(*stream,color,check);
        return {std::make_shared<SampleBank>(stream),canonical};
    }
};
}
