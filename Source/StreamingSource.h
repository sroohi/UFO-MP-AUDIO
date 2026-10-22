#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ufo {
struct StreamLevel { std::string path; uint64_t frames=0; };
struct StreamMetadata {
    double sampleRate=48000;
    std::vector<StreamLevel> levels;
    std::vector<float> overview;
    float gain=1;
};

// Explicit, bounded read-ahead cache. Only the worker touches files; the render
// thread sees preallocated heap pages and lock-free requests. There is one render
// consumer per instance, matching SampleLibrary's one-render-thread ownership.
class StreamingSource {
    static_assert(std::atomic<uint64_t>::is_always_lock_free,"Streaming requires lock-free 64-bit atomics");
    static_assert(std::atomic<uint32_t>::is_always_lock_free,"Streaming requires lock-free 32-bit atomics");
    static constexpr size_t pageFrames=8192,ways=8,queueSize=1024,pendingSize=4096;
    struct Slot {
        std::atomic<uint64_t> key{0},used{0};
        std::atomic<uint32_t> readers{0};
        std::array<float,pageFrames*2> data{};
    };
    static size_t setCount(size_t cacheBytes) {
        if(cacheBytes<sizeof(Slot)*ways*4 || cacheBytes>128*1024*1024)throw std::invalid_argument("Unsupported streaming cache memory budget");
        return cacheBytes/(sizeof(Slot)*ways);
    }
    StreamMetadata metadata;
    size_t sets;
    std::unique_ptr<Slot[]> slots;
    std::vector<std::ifstream> files;
    std::array<uint64_t,queueSize> requests{};
    std::atomic<uint32_t> head{0},tail{0};
    std::array<std::atomic<uint64_t>,pendingSize> pending{};
    std::atomic<uint64_t> clock{1},missCount{0},errorCount{0};
    std::atomic<bool> stopping{false},warming{false};
    std::thread worker;

    static uint64_t encode(size_t level,uint64_t page) {return (uint64_t(level)<<56)|(page+1);}
    static uint64_t mix(uint64_t value) {
        value^=value>>30;value*=0xbf58476d1ce4e5b9ULL;value^=value>>27;value*=0x94d049bb133111ebULL;return value^(value>>31);
    }
    size_t firstSlot(uint64_t key) const {return size_t(mix(key)%sets)*ways;}
    bool cached(uint64_t key) const {
        const auto first=firstSlot(key);
        for(size_t way=0;way<ways;++way)if(slots[first+way].key.load()==key)return true;
        return false;
    }
    void request(uint64_t key) {
        auto& marker=pending[size_t(mix(key)%pendingSize)];
        if(marker.exchange(key,std::memory_order_relaxed)==key)return;
        const auto write=head.load(std::memory_order_relaxed),next=(write+1)%uint32_t(queueSize);
        if(next==tail.load(std::memory_order_acquire)) {
            uint64_t expected=key;marker.compare_exchange_strong(expected,0,std::memory_order_relaxed);return;
        }
        requests[write]=key;head.store(next,std::memory_order_release);
    }
    void complete(uint64_t key) {
        uint64_t expected=key;pending[size_t(mix(key)%pendingSize)].compare_exchange_strong(expected,0,std::memory_order_relaxed);
    }
    bool dequeue(uint64_t& key) {
        const auto read=tail.load(std::memory_order_relaxed);
        if(read==head.load(std::memory_order_acquire))return false;
        key=requests[read];tail.store((read+1)%uint32_t(queueSize),std::memory_order_release);return true;
    }
    void loadPage(uint64_t key) {
        if(cached(key))return;
        const auto level=size_t(key>>56);const uint64_t page=(key&0x00ffffffffffffffULL)-1;
        if(level>=metadata.levels.size() || page>=((metadata.levels[level].frames+pageFrames-1)/pageFrames))return;
        const auto first=firstSlot(key);size_t victim=first;
        for(size_t way=0;way<ways;++way) {
            auto& candidate=slots[first+way];
            if(candidate.key.load()==0){victim=first+way;break;}
            if(candidate.used.load(std::memory_order_relaxed)<slots[victim].used.load(std::memory_order_relaxed))victim=first+way;
        }
        auto& slot=slots[victim];slot.key.store(0);
        // Pin protocol: a renderer increments readers, then revalidates key before
        // reading data. A writer invalidates key before waiting for existing pins.
        while(slot.readers.load()!=0) {if(stopping.load())return;std::this_thread::yield();}
        const uint64_t offset=page*pageFrames,count=std::min<uint64_t>(pageFrames,metadata.levels[level].frames-offset);
        auto& file=files[level];file.clear();file.seekg(std::streamoff(offset*2*sizeof(float)),std::ios::beg);
        file.read(reinterpret_cast<char*>(slot.data.data()),std::streamsize(count*2*sizeof(float)));
        if(!file || uint64_t(file.gcount())!=count*2*sizeof(float)) {errorCount.fetch_add(1,std::memory_order_relaxed);return;}
        for(size_t i=0;i<size_t(count)*2;++i)if(!std::isfinite(slot.data[i])) {errorCount.fetch_add(1,std::memory_order_relaxed);return;}
        // Avoid uninitialised tails for interpolation of a short final page.
        for(size_t i=size_t(count);i<pageFrames;++i){slot.data[i*2]=slot.data[(size_t(count)-1)*2];slot.data[i*2+1]=slot.data[(size_t(count)-1)*2+1];}
        slot.used.store(clock.fetch_add(1,std::memory_order_relaxed),std::memory_order_relaxed);slot.key.store(key);
    }
    void run() {
        while(!stopping.load()) {
            // Keep the newest bounded request batch when a rapid Color seek
            // outpaces disk. Obsolete positions must not delay the settled seek.
            std::array<uint64_t,64> newest{};size_t count=0;uint64_t key;
            size_t drained=0;
            while(drained++<queueSize && dequeue(key)) {
                if(count==newest.size()) {complete(newest[0]);std::move(newest.begin()+1,newest.end(),newest.begin());--count;}
                newest[count++]=key;
            }
            if(count==0){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
            while(count>0 && !stopping.load()) {
                key=newest[--count];loadPage(key);complete(key);
                const size_t level=size_t(key>>56);const uint64_t page=(key&0x00ffffffffffffffULL)-1;
                if(!warming.load() && level<metadata.levels.size())for(uint64_t distance=1;distance<=2;++distance) {
                    if((page+distance)*pageFrames<metadata.levels[level].frames)loadPage(encode(level,page+distance));
                }
            }
        }
    }
    void readAhead(size_t level,uint64_t index) {
        if(index%pageFrames<pageFrames/2)return;
        const uint64_t next=index/pageFrames+1;
        if(next*pageFrames>=metadata.levels[level].frames)return;
        const uint64_t key=encode(level,next);
        if(!cached(key))request(key);
    }
    bool frame(size_t level,uint64_t index,std::array<float,2>& out) {
        const uint64_t key=encode(level,index/pageFrames);const auto first=firstSlot(key);
        for(size_t way=0;way<ways;++way) {
            auto& slot=slots[first+way];if(slot.key.load()!=key)continue;
            slot.readers.fetch_add(1);
            if(slot.key.load()==key) {
                const size_t offset=size_t(index%pageFrames)*2;
                out={slot.data[offset],slot.data[offset+1]};
                slot.used.store(clock.fetch_add(1,std::memory_order_relaxed),std::memory_order_relaxed);
                slot.readers.fetch_sub(1);readAhead(level,index);return true;
            }
            slot.readers.fetch_sub(1);
        }
        request(key);return false;
    }
public:
    explicit StreamingSource(StreamMetadata info,size_t cacheBytes=16*1024*1024)
        :metadata(std::move(info)),sets(setCount(cacheBytes)),slots(std::make_unique<Slot[]>(sets*ways)) {
        if(!std::isfinite(metadata.sampleRate)||metadata.sampleRate<8000||metadata.sampleRate>384000 || metadata.levels.empty() || metadata.levels.size()>32 || metadata.levels[0].frames<64 || !std::isfinite(metadata.gain) || metadata.gain<=0 || metadata.gain>1)
            throw std::invalid_argument("Invalid streaming metadata");
        for(size_t level=0;level<metadata.levels.size();++level) {
            const auto& source=metadata.levels[level];
            if(source.frames==0 || source.frames>uint64_t(std::numeric_limits<std::streamoff>::max())/8 || (level>0 && source.frames!=metadata.levels[level-1].frames/2))throw std::invalid_argument("Invalid streaming mip length");
            std::error_code error;const auto bytes=std::filesystem::file_size(source.path,error);
            if(error || bytes!=source.frames*8)throw std::invalid_argument("Streaming cache file size mismatch");
            files.emplace_back(source.path,std::ios::binary);if(!files.back())throw std::runtime_error("Cannot open streaming cache file");
        }
        for(float value:metadata.overview)if(!std::isfinite(value))throw std::invalid_argument("Invalid streaming waveform summary");
        worker=std::thread([this]{run();});
    }
    ~StreamingSource() {stopping=true;if(worker.joinable())worker.join();}
    StreamingSource(const StreamingSource&)=delete;
    StreamingSource& operator=(const StreamingSource&)=delete;
    uint64_t frameCount() const {return metadata.levels[0].frames;}
    double sourceRate() const {return metadata.sampleRate;}
    float gain() const {return metadata.gain;}
    size_t levelCount() const {return metadata.levels.size();}
    const std::vector<float>& overview() const {return metadata.overview;}
    size_t memoryBytes() const {
        size_t bytes=sizeof(*this)+sizeof(Slot)*sets*ways+metadata.overview.capacity()*sizeof(float)+files.capacity()*sizeof(std::ifstream);
        for(const auto& level:metadata.levels)bytes+=level.path.capacity();
        return bytes;
    }
    uint64_t underruns() const {return missCount.load(std::memory_order_relaxed);}
    uint64_t ioErrors() const {return errorCount.load(std::memory_order_relaxed);}
    bool isResident(size_t level,uint64_t frameIndex) const {
        return level<metadata.levels.size() && frameIndex<metadata.levels[level].frames && cached(encode(level,frameIndex/pageFrames));
    }
    void reportUnderrun() {missCount.fetch_add(1,std::memory_order_relaxed);}
private:
    bool trySample(size_t level,double index,std::array<float,2>& out) {
        if(level>=metadata.levels.size() || !std::isfinite(index))return false;
        index=std::clamp(index,0.0,double(metadata.levels[level].frames-1));
        const auto i=uint64_t(index),next=std::min(i+1,metadata.levels[level].frames-1);
        const float fraction=float(index-double(i));
        if(i/pageFrames==next/pageFrames) {
            const uint64_t key=encode(level,i/pageFrames);const auto first=firstSlot(key);
            for(size_t way=0;way<ways;++way) {
                auto& slot=slots[first+way];if(slot.key.load()!=key)continue;
                slot.readers.fetch_add(1);
                if(slot.key.load()==key) {
                    const size_t a=size_t(i%pageFrames)*2,b=size_t(next%pageFrames)*2;
                    for(size_t channel=0;channel<2;++channel)out[channel]=(slot.data[a+channel]+fraction*(slot.data[b+channel]-slot.data[a+channel]))*metadata.gain;
                    slot.used.store(clock.fetch_add(1,std::memory_order_relaxed),std::memory_order_relaxed);slot.readers.fetch_sub(1);readAhead(level,i);return true;
                }
                slot.readers.fetch_sub(1);
            }
            request(key);return false;
        }
        // A page-boundary interpolation needs two independently pinned pages.
        std::array<float,2> a{},b{};const bool first=frame(level,i,a),second=frame(level,next,b);
        if(!first || !second)return false;
        for(size_t channel=0;channel<2;++channel)out[channel]=(a[channel]+fraction*(b[channel]-a[channel]))*metadata.gain;
        return true;
    }
public:
    bool sample(size_t level,double index,std::array<float,2>& out,bool nonRealtime=false) {
        if(level>=metadata.levels.size() || !std::isfinite(index))return false;
        if(trySample(level,index,out))return true;
        if(!nonRealtime)return false;
        // Offline bounces may advance faster than disk read-ahead. Only this
        // explicitly non-realtime path waits; file access remains on the worker.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        do {
            if(stopping.load() || ioErrors()>0)return false;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            if(trySample(level,index,out))return true;
        }while(std::chrono::steady_clock::now()<deadline);
        errorCount.fetch_add(1,std::memory_order_relaxed);return false;
    }
    // Loader-only: queue the first scanning window at every mip and wait until
    // those pages are actually resident. Does not run concurrently with rendering.
    bool prewarm(float color,int timeoutMs=5000) {
        if(!std::isfinite(color))return false;
        warming=true;
        struct Finish {std::atomic<bool>& value;~Finish(){value=false;}} finish{warming};
        color=std::clamp(color,0.f,1.f);std::array<uint64_t,64> needed{};size_t count=0;
        const double window=std::min(4096.0,double(frameCount()));const double base=double(color)*(double(frameCount())-window);
        for(size_t level=0;level<metadata.levels.size();++level) {
            const double divisor=std::ldexp(1.0,int(level));
            const auto first=uint64_t(base/divisor)/pageFrames,last=uint64_t((base+window-1)/divisor)/pageFrames;
            needed[count++]=encode(level,first);request(needed[count-1]);
            if(last!=first){needed[count++]=encode(level,last);request(needed[count-1]);}
        }
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
        do {
            bool ready=true;for(size_t i=0;i<count;++i)if(!cached(needed[i])){ready=false;request(needed[i]);}
            if(ready)return true;
            if(ioErrors()>0)return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }while(std::chrono::steady_clock::now()<deadline);
        return false;
    }
};
}
