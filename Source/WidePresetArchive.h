#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <functional>

namespace ufo {
// The legacy JUCE ZIP writer has 32-bit sizes. Streamed presets use a bounded
// 64-bit container instead of silently truncating multi-gigabyte sample media.
class WidePresetArchive {
    static constexpr const char* magic="UFOSET03";
    static constexpr juce::int64 maximumMedia=12LL*1024*1024*1024;
    static void demand(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
    static void canceled(const std::function<bool()>& check){if(check && check())throw std::runtime_error("Preset canceled");}
    static void validateAudio(const juce::File& file) {
        auto input=file.createInputStream();demand(bool(input),"Cannot open streamed preset audio");
        juce::WavAudioFormat wav;std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(input.release(),true));
        demand(reader && std::isfinite(reader->sampleRate) && reader->sampleRate>=8000 && reader->sampleRate<=192000
               && reader->numChannels>=1 && reader->numChannels<=2 && reader->lengthInSamples>=64
               && reader->lengthInSamples<=juce::int64(reader->sampleRate*7200),"Streamed preset needs mono/stereo WAV audio at 8-192 kHz, between 64 frames and two hours");
    }
    static void transfer(juce::InputStream& input,juce::OutputStream& output,juce::int64 bytes,const std::function<bool()>& check) {
        std::array<char,65536> buffer{};
        while(bytes>0) {
            canceled(check);
            const int count=int(std::min(bytes,juce::int64(buffer.size())));
            demand(input.read(buffer.data(),count)==count,"Preset media is truncated");
            demand(output.write(buffer.data(),size_t(count)),"Cannot write preset media (disk full or unavailable)");bytes-=count;
        }
    }
public:
    // JUCE's hash implementation reads bounded chunks through this wrapper, so
    // closing a processor need not wait for an entire multi-gigabyte checksum.
    static juce::String hashStream(juce::InputStream& input,juce::int64 bytes,const std::function<bool()>& check={}) {
        struct CheckedInput final : juce::InputStream {
            juce::InputStream& source;const std::function<bool()>& check;const juce::int64 begin,end;
            CheckedInput(juce::InputStream& in,juce::int64 bytes,const std::function<bool()>& test):source(in),check(test),begin(in.getPosition()),end(begin+bytes){}
            juce::int64 getTotalLength() override{return end;}
            juce::int64 getPosition() override{return source.getPosition();}
            bool setPosition(juce::int64 position) override{return position>=begin && position<=end && source.setPosition(position);}
            bool isExhausted() override{return source.getPosition()>=end || source.isExhausted();}
            int read(void* data,int count) override{canceled(check);const auto remaining=end-source.getPosition();return count<=0 || remaining<=0 ? 0 : source.read(data,int(std::min(juce::int64(count),remaining)));}
        };
        const auto before=input.getPosition();demand(bytes>=0 && before>=0 && bytes<=std::numeric_limits<juce::int64>::max()-before,"Invalid preset hash byte range");
        CheckedInput checked(input,bytes,check);canceled(check);
        // SHA256 requests 64-byte blocks. Buffer disk reads without allowing
        // read-ahead to consume the following archive record.
        juce::BufferedInputStream buffered(checked,65536);
        const auto hash=juce::SHA256(buffered,bytes).toHexString();
        canceled(check);demand(input.getPosition()-before==bytes,"Preset media is truncated");return hash;
    }
    static juce::String hashFile(const juce::File& file,const std::function<bool()>& check={}) {
        juce::FileInputStream input(file);demand(input.openedOk(),"Cannot open preset source audio");
        return hashStream(input,file.getSize(),check);
    }
    static bool matches(const juce::File& file) {
        juce::FileInputStream stream(file);char header[8]{};
        return stream.openedOk() && stream.read(header,8)==8 && std::memcmp(header,magic,8)==0;
    }
    static bool write(const juce::File& target,const juce::ValueTree& state,const std::array<juce::File,2>& files,juce::String& error,const std::function<bool()>& check={}) {
        error={};
        try {
            canceled(check);
            auto tree=state.createCopy();tree.setProperty("version",3,nullptr);const auto xml=tree.createXml()->toString();
            const auto xmlBytes=xml.getNumBytesAsUTF8();demand(xmlBytes>0 && xmlBytes<=1024*1024,"Preset state is too large");
            juce::int64 total=juce::int64(xmlBytes)+64;
            for(const auto& file:files)if(file.existsAsFile()){demand(file.getSize()<=maximumMedia,"Preset media exceeds the supported two-hour limit");total+=file.getSize();}
            demand(target.getParentDirectory().createDirectory().wasOk(),"Cannot create preset directory");
            demand(target.getParentDirectory().getBytesFreeOnVolume()>total,"Not enough disk space for the streamed preset archive");
            juce::TemporaryFile temporary(target);
            {
                juce::FileOutputStream output(temporary.getFile());demand(output.openedOk(),"Cannot create streamed preset archive");
                demand(output.write(magic,8) && output.writeInt(int(xmlBytes)) && output.write(xml.toRawUTF8(),size_t(xmlBytes)),"Cannot write streamed preset header");
                for(const auto& file:files) {
                    const auto bytes=file.existsAsFile()?file.getSize():0;
                    demand(output.writeInt64(bytes),"Cannot write streamed preset media size");
                    if(bytes>0){juce::FileInputStream input(file);demand(input.openedOk(),"Cannot open preset source audio");transfer(input,output,bytes,check);}
                }
                output.flush();demand(output.getStatus().wasOk(),"Cannot finish streamed preset archive");
            }
            canceled(check);demand(temporary.overwriteTargetFileWithTemporary(),"Cannot replace streamed preset archive");return true;
        }catch(const std::exception& e){error=e.what();return false;}
    }
    static juce::ValueTree read(const juce::File& file,const juce::File& cache,juce::String& error,const std::function<bool()>& check={}) {
        error={};
        try {
            canceled(check);
            juce::FileInputStream input(file);char header[8]{};
            demand(input.openedOk() && input.read(header,8)==8 && std::memcmp(header,magic,8)==0,"Invalid streamed preset header");
            const int size=input.readInt();demand(size>0 && size<=1024*1024 && input.getNumBytesRemaining()>=size,"Invalid streamed preset state size");
            juce::MemoryBlock data(size_t(size)+1,true);demand(input.read(data.getData(),size)==size,"Truncated streamed preset state");
            auto xml=juce::XmlDocument::parse(juce::String::fromUTF8(static_cast<const char*>(data.getData()),size));
            demand(bool(xml),"Invalid streamed preset XML");auto tree=juce::ValueTree::fromXml(*xml);
            demand(tree.isValid() && tree.hasType("UFO") && int(tree.getProperty("version"))==3,"Not a UFO version 3 preset");demand(cache.createDirectory().wasOk(),"Cannot create sample cache");
            for(int layer=0;layer<2;++layer) {
                canceled(check);
                demand(input.getNumBytesRemaining()>=8,"Missing streamed preset media size");
                const auto bytes=input.readInt64();demand(bytes>=0 && bytes<=maximumMedia && input.getNumBytesRemaining()>=bytes,"Invalid streamed preset media length");
                const juce::Identifier key=layer==0?"upperSample":"lowerSample";
                if(bytes==0){demand(tree.getProperty(key).toString().isEmpty(),"Preset media is missing");continue;}
                demand(tree.getProperty(key).toString()=="samples/"+juce::String(layer)+".wav","Invalid streamed sample entry");
                const auto sha=tree.getProperty(layer==0?"upperSampleSha256":"lowerSampleSha256").toString();
                demand(sha.length()==64 && sha.containsOnly("0123456789abcdef"),"Invalid streamed sample checksum");
                const auto destination=cache.getChildFile(sha+".wav");demand(!destination.isSymbolicLink(),"Sample cache path is a symbolic link");
                if(destination.existsAsFile() && destination.getSize()==bytes && hashFile(destination,check)==sha) {
                    const auto before=input.getPosition();
                    demand(hashStream(input,bytes,check)==sha && input.getPosition()-before==bytes,"Preset sample checksum mismatch");
                    validateAudio(destination);
                    tree.setProperty(key,destination.getFullPathName(),nullptr);continue;
                }
                demand(cache.getBytesFreeOnVolume()>bytes+1024*1024,"Not enough free space to extract streamed preset media");
                juce::TemporaryFile temporary(destination);
                {juce::FileOutputStream output(temporary.getFile());demand(output.openedOk(),"Cannot extract streamed preset media");transfer(input,output,bytes,check);output.flush();demand(output.getStatus().wasOk(),"Cannot finish extracted media");}
                demand(hashFile(temporary.getFile(),check)==sha,"Preset sample checksum mismatch");
                validateAudio(temporary.getFile());
                canceled(check);
                if(!destination.existsAsFile() || destination.getSize()!=bytes || hashFile(destination,check)!=sha)
                    demand(temporary.overwriteTargetFileWithTemporary(),"Cannot cache streamed preset sample");
                tree.setProperty(key,destination.getFullPathName(),nullptr);
            }
            demand(input.isExhausted(),"Unexpected data after streamed preset media");return tree;
        }catch(const std::exception& e){error=e.what();return {};}
    }
};
}
