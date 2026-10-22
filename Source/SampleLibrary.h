#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include "SampleBank.h"
#include "StreamAssetCache.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace ufo {
// Loader/UI own shared resources. The render callback holds only hazard pointers.
// A bank can be reclaimed only after it is unpublished and no renderer can use it.
class SampleLibrary {
    struct Slot {
        std::shared_ptr<const SampleBank> bank;
        juce::File file;
        juce::String name,pendingName,error;
        uint64_t generation=0;
        bool loading=false;
    };
    mutable std::mutex mutex;
    std::array<Slot,2> slots;
    std::vector<std::shared_ptr<const SampleBank>> retired;
    std::array<std::atomic<const SampleBank*>,2> published{}, hazards{};
    void reclaim() {
        retired.erase(std::remove_if(retired.begin(),retired.end(),[&](const auto& bank) {
            for(size_t l=0;l<2;++l)if(published[l].load()==bank.get() || hazards[l].load()==bank.get())return false;
            return true;
        }),retired.end());
    }
public:
    struct Snapshot {std::array<juce::File,2> files;std::array<juce::String,2> names;std::array<bool,2> loading{},active{},streaming{};};
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);Snapshot copy;
        for(size_t i=0;i<2;++i){copy.files[i]=slots[i].file;copy.names[i]=slots[i].name;copy.loading[i]=slots[i].loading;copy.active[i]=bool(slots[i].bank);copy.streaming[i]=slots[i].bank && slots[i].bank->isStreaming();}
        return copy;
    }
    static juce::File userDirectory() {
        const auto overridePath=juce::SystemStats::getEnvironmentVariable("UFO_USER_DATA_DIR",{});
        return juce::File::isAbsolutePath(overridePath) ? juce::File(overridePath) : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("MP Audio/UFO");
    }
    static juce::File cacheDirectory() { return userDirectory().getChildFile("Samples"); }
    uint64_t begin(size_t layer,const juce::File& file,const juce::String& name={}) {
        std::lock_guard<std::mutex> lock(mutex);auto& slot=slots.at(layer);
        slot.loading=true;slot.error={};slot.pendingName=name.isEmpty()?file.getFileName():name;return ++slot.generation;
    }
    void clear(size_t layer) {
        std::lock_guard<std::mutex> lock(mutex);auto& slot=slots.at(layer);++slot.generation;
        if(slot.bank)retired.push_back(std::move(slot.bank));
        published[layer].store(nullptr);slot.file=juce::File{};slot.name={};slot.error={};slot.loading=false;reclaim();
    }
    void load(size_t layer,const juce::File& file,uint64_t token,float color=.25f) {
        try {
            const auto canceled=[this,layer,token]{std::lock_guard<std::mutex> lock(mutex);return slots.at(layer).generation!=token;};
            juce::AudioFormatManager formats;formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
            if(!reader)throw std::runtime_error("Unsupported or unreadable audio file");
            if(reader->lengthInSamples<64 || !std::isfinite(reader->sampleRate) || reader->sampleRate<8000 || reader->sampleRate>192000 || reader->numChannels<1 || reader->numChannels>2)
                throw std::runtime_error("Use mono/stereo audio at 8-192 kHz, at least64frames");
            if(reader->lengthInSamples>juce::int64(reader->sampleRate*7200))throw std::runtime_error("Audio exceeds the two-hour import limit");
            auto cache=cacheDirectory();if(cache.createDirectory().failed())throw std::runtime_error("Cannot create sample cache directory");
            std::shared_ptr<const SampleBank> bank;juce::File cached;
            if(reader->lengthInSamples>juce::int64(reader->sampleRate*30)) {
                auto asset=StreamAssetCache::build(*reader,cache,color,canceled);bank=std::move(asset.bank);cached=asset.canonical;
            } else {
                const int frames=int(reader->lengthInSamples);juce::AudioBuffer<float> decoded(2,frames);
                if(!reader->read(&decoded,0,frames,0,true,reader->numChannels>1))throw std::runtime_error("Audio decoding failed");
                if(reader->numChannels==1)decoded.copyFrom(1,0,decoded,0,0,frames);
                std::array<std::vector<float>,2> data;
                for(size_t c=0;c<2;++c)data[c].assign(decoded.getReadPointer(int(c)),decoded.getReadPointer(int(c))+frames);
                bank=std::make_shared<SampleBank>(std::move(data),reader->sampleRate);
                const double decodedRate=reader->sampleRate;reader.reset();
                juce::TemporaryFile temporary(cache.getChildFile(juce::Uuid().toString()+".wav"));
                {
                    auto output=temporary.getFile().createOutputStream();if(!output)throw std::runtime_error("Cannot create sample cache file");
                    juce::WavAudioFormat wav;std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(output.release(),decodedRate,2,32,{},0));
                    if(!writer || !writer->writeFromAudioSampleBuffer(decoded,0,frames) || !writer->flush())throw std::runtime_error("Cannot encode cached sample");
                }
                const auto checksum=juce::SHA256(temporary.getFile()).toHexString();cached=cache.getChildFile(checksum+".wav");
                if(!cached.existsAsFile() && !temporary.getFile().moveFileTo(cached))throw std::runtime_error("Cannot cache imported sample");
            }
            std::lock_guard<std::mutex> lock(mutex);auto& slot=slots.at(layer);
            if(slot.generation!=token)return;
            if(slot.bank)retired.push_back(std::move(slot.bank));
            slot.bank=std::move(bank);slot.file=cached;slot.name=slot.pendingName;slot.error={};slot.loading=false;
            published[layer].store(slot.bank.get());reclaim();
        } catch(const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex);auto& slot=slots.at(layer);
            if(slot.generation==token){slot.error=e.what();slot.loading=false;}
        }
    }
    const SampleBank* acquire(size_t layer) {
        const SampleBank* bank;
        do {bank=published[layer].load();hazards[layer].store(bank);}while(bank!=published[layer].load());
        return bank;
    }
    void release(size_t layer) { hazards[layer].store(nullptr); }
    juce::String name(size_t layer) const {std::lock_guard<std::mutex> lock(mutex);const auto& slot=slots.at(layer);return slot.loading ? "Loading "+slot.pendingName : slot.bank ? slot.name : juce::String{};}
    juce::String error(size_t layer) const {std::lock_guard<std::mutex> lock(mutex);return slots.at(layer).error;}
    bool loading(size_t layer) const {std::lock_guard<std::mutex> lock(mutex);return slots.at(layer).loading;}
    std::array<juce::File,2> files() const {std::lock_guard<std::mutex> lock(mutex);return {slots[0].file,slots[1].file};}
    bool streaming(size_t layer) const {std::lock_guard<std::mutex> lock(mutex);return slots.at(layer).bank && slots.at(layer).bank->isStreaming();}
    uint64_t underruns(size_t layer) const {std::lock_guard<std::mutex> lock(mutex);return slots.at(layer).bank ? slots.at(layer).bank->streamUnderruns() : 0;}
    uint64_t ioErrors(size_t layer) const {std::lock_guard<std::mutex> lock(mutex);return slots.at(layer).bank ? slots.at(layer).bank->streamIoErrors() : 0;}
    uint64_t memoryBytes(size_t layer) const {std::lock_guard<std::mutex> lock(mutex);return slots.at(layer).bank ? slots.at(layer).bank->streamMemoryBytes() : 0;}
    std::vector<float> waveform(size_t layer,int points) const {
        std::shared_ptr<const SampleBank> bank;
        {std::lock_guard<std::mutex> lock(mutex);bank=slots.at(layer).bank;}
        return bank ? bank->waveform(points) : std::vector<float>{};
    }
};
}
