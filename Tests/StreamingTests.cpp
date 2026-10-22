#include "SampleBank.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <thread>

namespace {
thread_local bool watchAllocations=false;
std::atomic<size_t> renderAllocations{0};
}
[[gnu::noinline]] void* operator new(std::size_t bytes) {
    if(watchAllocations)renderAllocations.fetch_add(1,std::memory_order_relaxed);
    if(void* memory=std::malloc(bytes ? bytes : 1))return memory;
    throw std::bad_alloc();
}
[[gnu::noinline]] void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
[[gnu::noinline]] void operator delete(void* memory) noexcept {std::free(memory);}
[[gnu::noinline]] void operator delete[](void* memory) noexcept {std::free(memory);}
[[gnu::noinline]] void operator delete(void* memory,std::size_t) noexcept {std::free(memory);}
[[gnu::noinline]] void operator delete[](void* memory,std::size_t) noexcept {std::free(memory);}
namespace {
int checks=0;
void require(bool passed,const char* description) {
    ++checks;if(!passed){std::cerr<<"FAIL: "<<description<<'\n';std::exit(1);}
}
constexpr double pi=3.14159265358979323846;
float signal(uint64_t frame,size_t channel) {return float((channel==0?.6:.3)*std::sin(2*pi*(channel==0?200:300)*double(frame)/48000));}
void writeFixture(const std::filesystem::path& path,uint64_t frames) {
    std::ofstream output(path,std::ios::binary);std::array<float,32768*2> block{};
    for(uint64_t first=0;first<frames;first+=32768) {
        const auto count=std::min<uint64_t>(32768,frames-first);
        for(uint64_t i=0;i<count;++i)for(size_t c=0;c<2;++c)block[size_t(i)*2+c]=signal(first+i,c);
        output.write(reinterpret_cast<const char*>(block.data()),std::streamsize(count*8));
    }
    require(bool(output),"write longer-than-30-second streaming fixture");
}
}
int main() {
    const auto directory=std::filesystem::temp_directory_path()/("ufo-stream-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{directory};
    const auto path=directory/"sixty-four-seconds.raw";constexpr uint64_t frames=48000*64;
    writeFixture(path,frames);
    ufo::StreamMetadata metadata;metadata.sampleRate=48000;metadata.levels.push_back({path.string(),frames});metadata.overview.assign(4096,.6f);
    auto stream=std::make_shared<ufo::StreamingSource>(metadata);
    ufo::SampleBank bank(stream);
    require(bank.isStreaming() && bank.frames()==frames,"long source uses streaming metadata");
    require(bank.streamMemoryBytes()<17*1024*1024,"streaming memory stays below 17 MiB independent of file length");
    require(bank.prewarm(.75f),"requested scan region is prewarmed off the render thread");
    ufo::SampleBank::PlaybackState state;
    for(int i=0;i<64;++i)bank.read(300,.75f,false,.5,state);
    const auto sample=bank.read(300,.75f,false,.5,state);
    const uint64_t position=uint64_t(.75*double(frames-4096))+300;
    require(std::abs(sample[0]-signal(position,0))<1e-5 && std::abs(sample[1]-signal(position,1))<1e-5,"streamed source preserves actual stereo samples");
    require(bank.waveform(128).size()==128,"streamed overview returns requested UI bins");
    {
        auto sequential=std::make_shared<ufo::StreamingSource>(metadata);ufo::SampleBank sequentialBank(sequential);
        require(sequentialBank.prewarm(0),"sequential source prewarms its first page");
        require(!sequential->isResident(0,8192),"future page starts uncached in a fresh stream");
        sequentialBank.read(4096,0,true,.5);
        const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!sequential->isResident(0,8192) && std::chrono::steady_clock::now()<limit)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(sequential->isResident(0,8192),"a page hit queues read-ahead before playback reaches its boundary");
        const auto misses=sequentialBank.streamUnderruns();const auto next=sequentialBank.read(8192,0,true,.5);
        require(sequentialBank.streamUnderruns()==misses && std::abs(next[0]-signal(8192,0))<1e-5,"sequential boundary reads without a cache-miss dropout");
    }
    {
        auto offline=std::make_shared<ufo::StreamingSource>(metadata);ufo::SampleBank offlineBank(offline);
        ufo::SampleBank::PlaybackState offlineVoice;offlineVoice.gain=1;
        const uint64_t seek=frames-12345;
        require(!offline->isResident(0,seek),"offline seek begins with an uncached page");
        const auto sought=offlineBank.read(double(seek),0,true,.5,offlineVoice,true);
        require(std::abs(sought[0]-signal(seek,0))<1e-5 && std::abs(sought[1]-signal(seek,1))<1e-5,"explicit offline mode waits for uncached stereo data without dropping the first sample");
        bool exact=true;const auto baseline=renderAllocations.load();watchAllocations=true;
        for(uint64_t frame=0;frame<frames;++frame) {
            const auto out=offlineBank.read(double(frame),0,true,.5,offlineVoice,true);
            if(std::abs(out[0]-signal(frame,0))>1e-5 || std::abs(out[1]-signal(frame,1))>1e-5){exact=false;break;}
        }
        watchAllocations=false;
        require(exact,"faster-than-realtime sequential offline render matches every source sample beyond cache capacity");
        require(offlineBank.streamUnderruns()==0 && offlineBank.streamIoErrors()==0,"offline worker waits avoid cache misses becoming audible underruns");
        require(renderAllocations.load()==baseline,"offline waits reuse fixed cache memory without render allocations");
    }
    const auto previous=sample;const uint64_t misses=bank.streamUnderruns();
    auto fading=bank.read(300,.1f,false,.5,state);
    require(bank.streamUnderruns()>misses,"uncached random seek reports a stream underrun");
    require(std::abs(fading[0]-previous[0]*63.f/64)<1e-6,"missing page fades the previous voice sample instead of hard-cutting");
    float largestJump=0,previousOutput=fading[0];
    for(int i=0;i<64;++i){auto next=bank.read(300,.1f,false,.5,state);largestJump=std::max(largestJump,std::abs(next[0]-previousOutput));previousOutput=next[0];}
    require(largestJump<.03f,"fast cache recovery cannot interrupt the underrun fade with an amplitude jump");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while((state.gain<1 || state.starving) && std::chrono::steady_clock::now()<deadline) {
        for(int i=0;i<128;++i)bank.read(300,.1f,false,.5,state);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(state.gain==1 && !state.starving,"background worker recovers the newly requested scan region");
    require(bank.streamIoErrors()==0,"valid stream has no worker I/O errors");
    require(bank.prewarm(.1f),"resident scanning region stays ready");
    const auto allocationBaseline=renderAllocations.load();watchAllocations=true;
    for(int i=0;i<100000;++i) {
        const auto out=bank.read(double(i%3968),.1f,false,.5,state);
        if(!std::isfinite(out[0]) || !std::isfinite(out[1]))std::abort();
    }
    watchAllocations=false;
    require(renderAllocations.load()==allocationBaseline,"render reads/requests allocate no memory");

    // Many voice positions use the same bounded cache while the worker reads and
    // evicts pages. Only this render thread writes the SPSC request queue.
    std::atomic<bool> finite{true};
    std::thread renderer([&] {
        std::array<ufo::SampleBank::PlaybackState,14> voices{};watchAllocations=true;
        for(int block=0;block<1000;++block)for(int voice=0;voice<14;++voice) {
            const double source=double((uint64_t(voice)*frames/14+uint64_t(block)*64)%(frames-4096));
            const auto out=bank.read(source,0,true,.5,voices[size_t(voice)]);
            if(!std::isfinite(out[0]) || !std::isfinite(out[1]) || std::abs(out[0])>1 || std::abs(out[1])>1)finite=false;
        }
        watchAllocations=false;
    });renderer.join();
    require(finite,"concurrent page fills/evictions preserve finite bounded audio across14 voice positions");
    require(renderAllocations.load()==allocationBaseline,"concurrent miss handling still allocates no audio-thread memory");
    require(bank.streamMemoryBytes()<17*1024*1024,"seeks do not grow resident memory");
    const auto ended=bank.read(double(frames),0,true,.5);
    require(ended[0]==0 && ended[1]==0,"streamed one-shot stops at its actual endpoint");

    // Sparse fixture proves 64-bit frame and file-offset handling without
    // allocating a multi-gigabyte resident buffer or filling disk with zeroes.
    const uint64_t bigFrames=(uint64_t(1)<<32)+32768;
    const auto sparse=directory/"above-four-billion-frames.raw";
    {
        std::ofstream file(sparse,std::ios::binary);file.seekp(std::streamoff((bigFrames-8192)*8));
        std::array<float,8192*2> end{};for(size_t i=0;i<8192;++i){end[i*2]=.4f;end[i*2+1]=-.2f;}
        file.write(reinterpret_cast<const char*>(end.data()),std::streamsize(end.size()*sizeof(float)));
        require(bool(file),"create sparse source above32-bit frame range");
    }
    ufo::StreamMetadata large;large.sampleRate=192000;large.levels.push_back({sparse.string(),bigFrames});large.overview={.4f,-.2f};
    {
        auto huge=std::make_shared<ufo::StreamingSource>(large);ufo::SampleBank largeBank(huge);
        require(largeBank.frames()==bigFrames,"stream metadata preserves more than32-bit frame counts");
        require(std::filesystem::file_size(sparse)>uint64_t(4)*1024*1024*1024,"fixture has actual multi-gigabyte logical file offsets");
        require(largeBank.prewarm(1),"worker seeks beyond4GB using64-bit offsets");
        const auto tail=largeBank.read(double(bigFrames-2048),0,true,.5);
        require(std::abs(tail[0]-.4f)<1e-6 && std::abs(tail[1]+.2f)<1e-6,"far-end stereo data reads correctly without full-file allocation");
        require(largeBank.streamMemoryBytes()<17*1024*1024,"multi-gigabyte source keeps the same cache budget");
    }
    {
        auto bad=metadata;bad.levels[0].frames++;
        bool rejected=false;try{ufo::StreamingSource invalid(std::move(bad));}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"truncated cache metadata is rejected before rendering");
    }
    {
        auto bad=metadata;bad.sampleRate=std::numeric_limits<double>::quiet_NaN();
        bool rejected=false;try{ufo::StreamingSource invalid(std::move(bad));}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"nonfinite streaming metadata cannot reach index calculations");
    }
    {
        // All mips of a two-hour/192kHz source can prewarm together within the
        // fixed cache. Sparse files keep the test small while preserving offsets.
        ufo::StreamMetadata pyramids;pyramids.sampleRate=192000;pyramids.overview={0};
        uint64_t count=uint64_t(192000)*7200;
        for(int level=0;count>0 && level<32;++level,count/=2) {
            const auto mip=directory/("mip-"+std::to_string(level)+".raw");
            {std::ofstream output(mip,std::ios::binary);output.seekp(std::streamoff(count*8-1));output.put(0);require(bool(output),"write sparse long-source mip");}
            pyramids.levels.push_back({mip.string(),count});
        }
        auto source=std::make_shared<ufo::StreamingSource>(pyramids);ufo::SampleBank pyramid(source);
        require(pyramid.prewarm(.65f),"all31 long-source mips prewarm within the fixed cache");
        require(pyramid.streamMemoryBytes()<17*1024*1024,"mip count does not turn cache into full-source allocation");
        require(!pyramid.prewarm(std::numeric_limits<float>::quiet_NaN()),"invalid prewarm color cannot reach integer conversion");
    }
    {
        const auto corrupted=directory/"nonfinite.raw";std::array<float,128> bad{};bad[1]=std::numeric_limits<float>::quiet_NaN();
        {std::ofstream output(corrupted,std::ios::binary);output.write(reinterpret_cast<const char*>(bad.data()),std::streamsize(sizeof(bad)));}
        ufo::StreamMetadata broken;broken.sampleRate=48000;broken.levels.push_back({corrupted.string(),64});
        auto source=std::make_shared<ufo::StreamingSource>(broken);ufo::SampleBank badBank(source);
        require(!badBank.prewarm(0),"nonfinite cache page fails on the worker before publication");
        const auto out=badBank.read(0,0,true,.5);require(out[0]==0 && out[1]==0 && badBank.streamIoErrors()>0,"corrupt cache produces silence and explicit diagnostics");
        ufo::SampleBank::PlaybackState offlineVoice;
        const auto start=std::chrono::steady_clock::now();const auto offlineOut=badBank.read(0,0,true,.5,offlineVoice,true);
        require(offlineOut[0]==0 && offlineOut[1]==0 && std::chrono::steady_clock::now()-start<std::chrono::seconds(1),"offline I/O failure returns silence promptly instead of waiting indefinitely");
    }
    std::cout<<"PASS: "<<checks<<" streaming checks; bounded resident cache, long/multi-gigabyte files,64-bit offsets, nonblocking misses/fades, offline rendering and stereo read-ahead\n";
}
