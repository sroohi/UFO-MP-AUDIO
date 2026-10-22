#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>
#include <memory>
#include "StreamingSource.h"

namespace ufo {
// Built on a loader thread, immutable after construction. Audio owns no buffers.
class SampleBank {
    using Stereo = std::array<std::vector<float>, 2>;
    std::vector<Stereo> levels;
    std::vector<float> overview;
    double rate;
    std::shared_ptr<StreamingSource> stream;
    float interpolated(size_t level,size_t channel,double index) const {
        const auto& data=levels[level][channel];
        index=std::clamp(index,0.0,double(data.size()-1));
        const auto i=size_t(index);const auto next=std::min(i+1,data.size()-1);
        return data[i]+float(index-double(i))*(data[next]-data[i]);
    }
public:
    struct PlaybackState {std::array<float,2> last{};float gain=0;bool starving=false;};
    explicit SampleBank(std::shared_ptr<StreamingSource> source)
        :overview(source ? source->overview() : std::vector<float>{}),rate(source ? source->sourceRate() : 0),stream(std::move(source)) {
        if(!stream)throw std::invalid_argument("Missing streaming source");
    }
    bool isStreaming() const {return bool(stream);}
    uint64_t streamUnderruns() const {return stream ? stream->underruns() : 0;}
    uint64_t streamIoErrors() const {return stream ? stream->ioErrors() : 0;}
    size_t streamMemoryBytes() const {return stream ? stream->memoryBytes() : 0;}
    bool prewarm(float color,int timeoutMs=5000) const {return !stream || stream->prewarm(color,timeoutMs);}
    SampleBank(Stereo channels,double sampleRate) : rate(sampleRate) {
        if(!std::isfinite(sampleRate) || sampleRate<8000 || sampleRate>384000 || channels[0].size()<64 || channels[0].size()!=channels[1].size())
            throw std::invalid_argument("Unsupported or empty stereo sample data");
        float peak=1;
        for(const auto& channel:channels) for(float value:channel) {
            if(!std::isfinite(value)) throw std::invalid_argument("Audio contains non-finite samples");
            peak=std::max(peak,std::abs(value));
        }
        if(peak>1) for(auto& channel:channels) for(auto& value:channel)value/=peak;
        // Compute once on the loader thread. UI refresh cost depends on this
        // bounded overview, never on the duration or sample rate of the source.
        const size_t bins=std::min(size_t(4096),channels[0].size());
        overview.resize(bins);
        for(size_t bin=0;bin<bins;++bin) {
            const size_t first=bin*channels[0].size()/bins,last=(bin+1)*channels[0].size()/bins;
            float strongest=0;
            for(size_t i=first;i<last;++i)for(size_t channel=0;channel<2;++channel)
                if(std::abs(channels[channel][i])>std::abs(strongest))strongest=channels[channel][i];
            overview[bin]=strongest;
        }
        levels.push_back(std::move(channels));
        // Windowed-sinc low-pass before each 2:1 reduction; never decimate raw audio.
        constexpr int taps=47,centre=taps/2;
        std::array<double,taps> kernel{};double total=0;
        for(int i=0;i<taps;++i) {
            const int n=i-centre;
            const double sinc=n==0 ? .44 : std::sin(.44*3.141592653589793*n)/(3.141592653589793*n);
            const double window=.42-.5*std::cos(2*3.141592653589793*i/(taps-1))+.08*std::cos(4*3.141592653589793*i/(taps-1));
            kernel[size_t(i)]=sinc*window;total+=sinc*window;
        }
        for(auto& k:kernel)k/=total;
        while(levels.size()<12 && levels.back()[0].size()>=2) {
            Stereo reduced;
            const auto& previous=levels.back();const auto count=previous[0].size()/2;
            for(size_t channel=0;channel<2;++channel) {
                reduced[channel].resize(count);
                for(size_t i=0;i<count;++i) {
                    double value=0;
                    for(int tap=0;tap<taps;++tap) {
                        const auto source=std::clamp<std::ptrdiff_t>(std::ptrdiff_t(i*2)+tap-centre,0,std::ptrdiff_t(previous[channel].size()-1));
                        value+=previous[channel][size_t(source)]*kernel[size_t(tap)];
                    }
                    reduced[channel][i]=float(value);
                }
            }
            levels.push_back(std::move(reduced));
        }
    }
    double sourceRate() const { return rate; }
    uint64_t frames() const { return stream ? stream->frameCount() : uint64_t(levels[0][0].size()); }
    double playbackIncrement(float hz,double outputSampleRate) const {
        return rate/outputSampleRate*double(hz)/261.6255653005986;
    }
    std::array<float,2> read(double position,float color,bool oneshot,double increment=1) const {
        std::array<float,2> result{};const bool ready=readImpl(position,color,oneshot,increment,result);
        if(!ready && stream)stream->reportUnderrun();
        return result;
    }
    std::array<float,2> read(double position,float color,bool oneshot,double increment,PlaybackState& state,bool nonRealtime=false) const {
        if(!stream)return read(position,color,oneshot,increment);
        std::array<float,2> result{};const bool ready=readImpl(position,color,oneshot,increment,result,nonRealtime);
        if(!ready){state.starving=true;stream->reportUnderrun();}
        if(state.starving) {
            state.gain=std::max(0.f,state.gain-1.f/64);
            // Finish fading the old waveform before admitting newly arrived
            // samples, even when a fast disk recovers after only one miss.
            if(state.gain==0 && ready)state.starving=false;
        } else {state.gain=std::min(1.f,state.gain+1.f/64);state.last=result;}
        for(size_t channel=0;channel<2;++channel)result[channel]=state.last[channel]*state.gain;
        return result;
    }
private:
    bool readImpl(double position,float color,bool oneshot,double increment,std::array<float,2>& out,bool nonRealtime=false) const {
        if(!std::isfinite(position) || !std::isfinite(color) || !std::isfinite(increment) || increment<=0)return true;
        const double frameCount=double(frames());
        const double window=std::min(4096.0,frameCount);
        const double base=std::clamp(double(color),0.0,1.0)*(frameCount-window);
        double source=base+position,blend=1,other=0;
        if(oneshot) { if(source>=frameCount || source<0)return true; }
        else {
            const double fade=std::min(128.0,window*.25),period=window-fade;
            const double loop=std::fmod(std::max(0.0,position),period);
            source=base+loop;
            if(loop<fade) { blend=.5-.5*std::cos(3.141592653589793*loop/fade);other=source+period; }
        }
        // Crossfade a level ahead of the needed cutoff. Both contributing mips
        // are already band-limited for this rate, including at octave boundaries.
        // This deliberately softens the top octave near unity rather than letting
        // an unsafe brighter mip alias while it fades out.
        const size_t levelCount=stream ? stream->levelCount() : levels.size();
        const double target=std::clamp(std::log2(increment)+1.0,0.0,double(levelCount-1));
        const size_t lo=size_t(std::floor(target)),hi=std::min(lo+1,levelCount-1);
        const float levelBlend=float(target-double(lo));
        const double lowDivisor=std::ldexp(1.0,int(lo)),highDivisor=std::ldexp(1.0,int(hi));
        if(stream) {
            const auto sampleAt=[&](double index,std::array<float,2>& sample) {
                std::array<float,2> low{},high{};
                const bool lowReady=stream->sample(lo,index/lowDivisor,low,nonRealtime);
                const bool highReady=levelBlend==0 || hi==lo ? true : stream->sample(hi,index/highDivisor,high,nonRealtime);
                if(!lowReady || !highReady)return false;
                for(size_t channel=0;channel<2;++channel)sample[channel]=levelBlend==0 || hi==lo ? low[channel] : low[channel]+levelBlend*(high[channel]-low[channel]);
                return true;
            };
            std::array<float,2> a{},b{};const bool first=sampleAt(source,a),second=blend>=1 || sampleAt(other,b);
            if(!first || !second)return false;
            for(size_t channel=0;channel<2;++channel)out[channel]=blend<1 ? float(a[channel]*blend+b[channel]*(1-blend)) : a[channel];
        } else for(size_t channel=0;channel<2;++channel) {
            const auto sampleAt=[&](double index) {
                const float low=interpolated(lo,channel,index/lowDivisor);
                const float high=interpolated(hi,channel,index/highDivisor);
                return low+levelBlend*(high-low);
            };
            const float a=sampleAt(source);
            out[channel]=blend<1 ? float(a*blend+sampleAt(other)*(1-blend)) : a;
        }
        return true;
    }
public:
    std::vector<float> waveform(int points) const {
        if(points<=0)return {};
        std::vector<float> result(size_t(points),0);
        if(overview.empty())return result;
        for(int point=0;point<points;++point) {
            const size_t first=size_t(point)*overview.size()/size_t(points);
            const size_t last=std::max(first+1,size_t(point+1)*overview.size()/size_t(points));
            float strongest=0;
            for(size_t i=first;i<std::min(last,overview.size());++i)
                if(std::abs(overview[i])>std::abs(strongest))strongest=overview[i];
            result[size_t(point)]=strongest;
        }
        return result;
    }
};
}
