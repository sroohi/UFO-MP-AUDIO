#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "SampleBank.h"

namespace ufo {
constexpr double pi = 3.14159265358979323846;
inline float clamp(float x, float lo, float hi) { return std::clamp(x, lo, hi); }
struct Layer {
    float color = .25f, movement = .15f, attack = .1f, decay = 1.f;
    float cutoff = 6000.f, resonance = .15f, slew = .15f, tilt = .5f, spread = 3.f;
    int bank = 0, octave = 0, pitch = 0, shaper = 0, filter = 0;
    bool oneshot = false;
};
struct Mod {
    int type = 0, destination = 0; // LFO, random, Euclidean, envelope, sequence
    float rate = 1.f, depth = 0.f, shape = .5f;
    bool sync = false;
    std::array<float,16> steps {0,.5f,-.25f,1,.25f,-.5f,.75f,-1,0,.5f,-.25f,1,.25f,-.5f,.75f,-1};
    int stepCount = 8;
    float shift = 0, attack = .02f, decay = .4f, sustain = .5f, release = .3f;
};
struct Parameters {
    std::array<Layer, 2> layers {};
    std::array<Mod, 4> mods {};
    float mix = .5f, attack = .015f, decay = .4f, sustain = .7f, release = .5f;
    float drive = 0.f, chorus = 0.f, flanger = 0.f, delay = .15f, delayTime = .3f;
    float feedback = .3f, reverb = .15f, gain = .5f;
};
class Envelope {
    enum Stage { idle, attack, decay, sustain, release } stage = idle;
    float value = 0.f, releaseStep = 0.f;
public:
    void on() { stage = attack; }
    void off(float seconds, float sr) { if (stage != idle && stage != release) { releaseStep = value / std::max(1.f, seconds * sr); stage = release; } }
    void reset() { stage = idle; value = 0; }
    bool active() const { return stage != idle; }
    float next(float a, float d, float s, float sr) {
        if (stage == attack) { value += 1.f / std::max(1.f, a * sr); if (value >= 1) { value = 1; stage = decay; } }
        else if (stage == decay) { value -= (1.f-s) / std::max(1.f, d*sr); if (value <= s) { value = s; stage = sustain; } }
        else if (stage == sustain) value = s;
        else if (stage == release) { value -= releaseStep; if (value <= 0) reset(); }
        return value;
    }
};
struct Filter {
    float ic1 = 0, ic2 = 0;
    void reset() { ic1 = ic2 = 0; }
    float tick(float x, float hz, float q, int mode, float sr) {
        float g = std::tan(float(pi) * clamp(hz, 16, sr*.45f) / sr);
        float k = 2.f - 1.95f * clamp(q, 0, 1);
        float a = 1.f / (1.f + g*(g+k));
        float v1 = a*(ic1 + g*(x-ic2));
        float v2 = ic2 + g*v1;
        ic1 = 2*v1-ic1; ic2 = 2*v2-ic2;
        return mode == 1 ? v1 : mode == 2 ? x-k*v1-v2 : v2;
    }
};
// Two input all-pass stages feed an orthogonal four-line feedback network.
// Its feedback norm is < 1, including at maximum wet level.
class DiffusionReverb {
    struct Line {
        std::vector<float> data;
        size_t position = 0;
        float read() const { return data[position]; }
        void write(float value) { data[position]=value; position=(position+1)%data.size(); }
        void prepare(size_t length) { data.assign(std::max(size_t(1),length),0); position=0; }
        void reset() { std::fill(data.begin(),data.end(),0); position=0; }
        float allpass(float x) { float y=read()-.65f*x; write(x+.65f*y); return y; }
    };
    std::array<std::array<Line,2>,2> diffusers;
    std::array<Line,4> lines;
    std::array<float,4> damping {};
public:
    void prepare(float sr) {
        constexpr std::array<int,4> lengths {1423,1559,1613,1747};
        for(size_t i=0;i<4;++i) lines[i].prepare(size_t(float(lengths[i])*sr/48000.f));
        for(size_t c=0;c<2;++c) for(size_t i=0;i<2;++i) diffusers[c][i].prepare(size_t(float((i==0?113:337)+int(c)*37)*sr/48000.f));
        damping={};
    }
    void reset() { for(auto& line:lines) line.reset(); for(auto& channel:diffusers) for(auto& line:channel) line.reset(); damping={}; }
    std::array<float,2> tick(std::array<float,2> input) {
        for(size_t c=0;c<2;++c) for(auto& diffuser:diffusers[c]) input[c]=diffuser.allpass(input[c]);
        std::array<float,4> y {};
        for(size_t i=0;i<4;++i) { y[i]=lines[i].read(); damping[i]+=.35f*(y[i]-damping[i]); }
        const auto& d=damping;
        lines[0].write(input[0]*.25f+.39f*(d[0]+d[1]+d[2]+d[3]));
        lines[1].write(input[1]*.25f+.39f*(d[0]-d[1]+d[2]-d[3]));
        lines[2].write(input[0]*.25f+.39f*(d[0]+d[1]-d[2]-d[3]));
        lines[3].write(input[1]*.25f+.39f*(d[0]-d[1]-d[2]+d[3]));
        return { .5f*(y[0]+y[1]-y[2]-y[3]), .5f*(y[0]-y[1]+y[2]-y[3]) };
    }
};
// 2:1 linear-phase anti-alias decimator. Evaluate at the even internal phase:
// a 129-tap FIR has exactly 64 high-rate / 32 host-rate samples of delay.
class StereoDecimator2x {
    static constexpr size_t taps=129;
    std::array<float,taps> coefficients {};
    std::array<std::array<float,2>,taps> history {};
    size_t position=0;
public:
    static constexpr int latencySamples() { return int((taps-1)/4); }
    StereoDecimator2x() {
        double total=0;
        constexpr double cutoff=.225;
        for(size_t i=0;i<taps;++i) {
            double n=double(i)-double(taps-1)*.5;
            double sinc=n==0 ? 2*cutoff : std::sin(2*pi*cutoff*n)/(pi*n);
            double window=.42-.5*std::cos(2*pi*double(i)/double(taps-1))+.08*std::cos(4*pi*double(i)/double(taps-1));
            coefficients[i]=float(sinc*window);total+=coefficients[i];
        }
        for(auto& value:coefficients)value=float(double(value)/total);
    }
    void reset() { history={};position=0; }
    std::array<float,2> process(std::array<float,2> first,std::array<float,2> second) {
        history[position]=first;
        std::array<float,2> output {};
        size_t index=position;
        for(float coefficient:coefficients) {
            output[0]+=coefficient*history[index][0];output[1]+=coefficient*history[index][1];
            index=index==0 ? taps-1 : index-1;
        }
        position=(position+1)%taps;history[position]=second;position=(position+1)%taps;
        return output;
    }
};
class Engine {
    static constexpr int frames = 32, length = 512, harmonics = 16;
    using Bank = std::array<std::array<float, length+1>, frames>;
    // One mip for each retained harmonic count; heap storage keeps plugin stack use small.
    std::vector<Bank> banks;
    struct Voice {
        int note = -1, channel = 1;
        uint64_t age = 0;
        float velocity = 0, time = 0;
        Envelope env;
        std::array<Envelope,4> modEnvelopes;
        std::array<float,4> modValues {};
        std::array<double, 2> phase {}, samplePosition {};
        std::array<SampleBank::PlaybackState,2> streamState{};
        std::array<float, 2> gate {};
        std::array<std::array<Filter,2>, 2> filters {};
        std::array<std::array<std::array<Filter,3>,2>, 2> resonators {};
    };
    std::array<Voice,14> voices {};
    std::array<float,16> bends {}, smoothedBends {};
    std::array<bool,16> pedal {};
    std::array<bool,14> deferred {};
    std::array<const SampleBank*,2> sampleBanks {};
    bool nonRealtimeMode=false;
    float sr = 96000, outputRate = 48000, tempo = 120, smoothingCoefficient = .002f;
    StereoDecimator2x decimator;
    Parameters smoothed;
    std::array<float,2> smoothedTuning {};
    uint64_t counter = 0;
    std::array<double,4> freeCycles {};
    std::array<float,4> lastModValues {};
    std::array<float,2> displayPositions{},displayColors{};
    double ppqPosition = 0;
    bool transportPlaying = false, transportValid = false;
    std::array<std::vector<float>,2> delayBuffer, chorusBuffer;
    DiffusionReverb reverb;
    size_t delayPos = 0, chorusPos = 0;
    double fxPhase = 0;
    void smooth(float& current,float target) const {
        current += smoothingCoefficient*(target-current);
        if(std::abs(target-current)<.000001f) current=target;
    }
    void smoothControls() {
        for(size_t l=0;l<2;++l) {
            const auto& target=params.layers[l]; auto& p=smoothed.layers[l];
            p.bank=target.bank; p.octave=target.octave; p.pitch=target.pitch; p.shaper=target.shaper; p.filter=target.filter; p.oneshot=target.oneshot;
            smooth(p.color,target.color); smooth(p.movement,target.movement); smooth(p.attack,target.attack); smooth(p.decay,target.decay);
            smooth(p.cutoff,target.cutoff); smooth(p.resonance,target.resonance); smooth(p.slew,target.slew); smooth(p.tilt,target.tilt); smooth(p.spread,target.spread);
            smooth(smoothedTuning[l],float(target.octave*12+target.pitch));
        }
        for(size_t i=0;i<4;++i) {
            const auto& target=params.mods[i]; auto& m=smoothed.mods[i];
            m.type=target.type; m.destination=target.destination; m.sync=target.sync; m.stepCount=target.stepCount;
            // Synced rate changes select a musical division immediately, preserving PPQ alignment.
            m.rate=target.rate; smooth(m.depth,target.depth); smooth(m.shape,target.shape); smooth(m.shift,target.shift);
            smooth(m.attack,target.attack); smooth(m.decay,target.decay); smooth(m.sustain,target.sustain); smooth(m.release,target.release);
            for(size_t step=0;step<m.steps.size();++step) smooth(m.steps[step],target.steps[step]);
        }
        smooth(smoothed.mix,params.mix); smooth(smoothed.attack,params.attack); smooth(smoothed.decay,params.decay); smooth(smoothed.sustain,params.sustain); smooth(smoothed.release,params.release);
        smooth(smoothed.drive,params.drive); smooth(smoothed.chorus,params.chorus); smooth(smoothed.flanger,params.flanger); smooth(smoothed.delay,params.delay); smooth(smoothed.delayTime,params.delayTime);
        smooth(smoothed.feedback,params.feedback); smooth(smoothed.reverb,params.reverb); smooth(smoothed.gain,params.gain);
        for(size_t c=0;c<bends.size();++c) smooth(smoothedBends[c],bends[c]);
    }
    float read(const std::vector<float>& b, size_t pos, float lag) const {
        double index = double(pos) - lag;
        while (index < 0) index += double(b.size());
        auto i = size_t(index) % b.size();
        float f = float(index - std::floor(index));
        return b[i]*(1-f) + b[(i+1)%b.size()]*f;
    }
    float tableSample(int bank,int retained,float color,double phase) const {
        if(retained<=0) return 0;
        float frame=clamp(color,0,1)*float(frames-1);
        auto f=size_t(frame), next=std::min(f+1,size_t(frames-1));
        phase-=std::floor(phase);
        double position=phase*length;
        auto i=std::min(size_t(position),size_t(length-1));
        float t=float(position-double(i)), blend=frame-float(f);
        const auto& b=banks[size_t(std::clamp(bank,0,3)*harmonics+std::clamp(retained,1,harmonics)-1)];
        float a=b[f][i]+t*(b[f][i+1]-b[f][i]);
        float c=b[next][i]+t*(b[next][i+1]-b[next][i]);
        return a+blend*(c-a);
    }
    static float randomAt(int slot,double cycles) {
        auto cycle=int64_t(std::floor(cycles));
        uint64_t x=uint64_t(cycle)^uint64_t(slot+1)*0x9e3779b97f4a7c15ULL;
        x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; x^=x>>31;
        return float(uint32_t(x>>32))/float(UINT32_MAX)*2-1;
    }
    void releaseVoice(Voice& voice) {
        voice.env.off(params.release,sr);
        for(size_t i=0;i<4;++i) voice.modEnvelopes[i].off(params.mods[i].release,sr);
    }
public:
    Parameters params;
    Engine() : banks(size_t(4*harmonics)) {
        for(int b=0;b<4;++b) for(int f=0;f<frames;++f) {
            float color=float(f)/float(frames-1), peak=0;
            for(int i=0;i<length;++i) {
                double phase=2*pi*i/length; float v=0;
                for(int h=1;h<=harmonics;++h) {
                    float amp=std::pow(float(h),-(2.5f-1.4f*color));
                    if(b==1 && h%2==0) amp*=.05f;
                    if(b==2) amp*=.5f+.5f*std::cos(float(h)*color*2.f);
                    if(b==3) amp*=std::sin(float(h)*1.7f+color*5.f);
                    v+=amp*float(std::sin(phase*h)); banks[size_t(b*harmonics+h-1)][size_t(f)][size_t(i)]=v;
                }
                peak=std::max(peak,std::abs(v));
            }
            for(int h=1;h<=harmonics;++h) {
                auto& table=banks[size_t(b*harmonics+h-1)][size_t(f)];
                for(int i=0;i<length;++i) table[size_t(i)]/=std::max(.01f,peak);
                table[size_t(length)]=table[0];
            }
        }
        prepare(48000);
    }
    void prepare(double sampleRate) {
        outputRate=float(sampleRate); sr=outputRate*2;
        smoothingCoefficient=1-std::exp(-1.f/(sr*.01f));
        for(size_t c=0;c<2;++c) { delayBuffer[c].assign(size_t(sr*2.1),0); chorusBuffer[c].assign(size_t(sr*.1),0); }
        reverb.prepare(sr); reset();
    }
    void reset() {
        voices={}; bends={}; smoothedBends={}; pedal={}; deferred={}; freeCycles={}; lastModValues={};
        delayPos=chorusPos=0; fxPhase=0; smoothed=params;
        for(size_t l=0;l<2;++l) smoothedTuning[l]=float(params.layers[l].octave*12+params.layers[l].pitch);
        for(auto* buffers:{&delayBuffer,&chorusBuffer}) for(auto& b:*buffers) std::fill(b.begin(),b.end(),0);
        reverb.reset();decimator.reset();displayPositions={};displayColors={};
    }
    static constexpr int latencySamples() { return StereoDecimator2x::latencySamples(); }
    void setNonRealtime(bool enabled) {nonRealtimeMode=enabled;}
    void setSampleBank(size_t layer,const SampleBank* bank) {
        if(layer>=sampleBanks.size() || sampleBanks[layer]==bank)return;
        sampleBanks[layer]=bank;
        for(auto& voice:voices){voice.samplePosition[layer]=0;voice.streamState[layer]={};}
    }
    void setTempo(float bpm) { if(std::isfinite(bpm) && bpm>0) tempo=bpm; }
    void setTransport(double ppq,bool playing,bool valid) { transportValid=valid&&std::isfinite(ppq); transportPlaying=playing; if(transportValid) ppqPosition=ppq; }
    std::array<float,4> modulationValues() const { return lastModValues; }
    std::array<float,2> playbackPositions() const { return displayPositions; }
    std::array<float,2> playbackColors() const { return displayColors; }
    std::vector<float> waveformPreview(int bank,float color,int points) const {
        std::vector<float> result(size_t(std::max(1,points)));
        for(size_t i=0;i<result.size();++i)result[i]=tableSample(bank,harmonics,color,double(i)/double(result.size()));
        return result;
    }
    std::array<float,4> voiceModulationValues(int note,int channel=1) const {
        for(const auto& v:voices) if(v.note==note && v.channel==channel && v.env.active()) return v.modValues;
        return {};
    }
    // Shared by the oscillator and actual waveform preview; frequency controls the Nyquist-safe mip.
    float wavetableSample(int bank,float color,double phase,float frequency) const {
        if(!(frequency>0) || frequency>=outputRate*.5f) return 0;
        const float budget=outputRate*.5f/frequency;
        if(budget>=float(harmonics+1)) return tableSample(bank,harmonics,color,phase);
        int count=std::min(harmonics,int(budget));
        if(count<1) return 0;
        // Each partial fades across its entire adjacent mip interval. This makes
        // both sides of every harmonic-count boundary meet at the same table.
        float fade=clamp(budget-float(count),0,1);
        float a=tableSample(bank,count-1,color,phase), b=tableSample(bank,count,color,phase);
        return a+fade*(b-a);
    }
    void pitchBend(int channel,float value) { bends[size_t(std::clamp(channel,1,16)-1)]=clamp(value,-1,1)*2; }
    void sustain(int channel,bool down) {
        auto c=size_t(std::clamp(channel,1,16)-1); pedal[c]=down;
        if(!down) for(size_t i=0;i<voices.size();++i) if(deferred[i] && voices[i].channel==channel) { releaseVoice(voices[i]); deferred[i]=false; }
    }
    void noteOn(int note,float velocity,int channel=1) {
        if(velocity<=0) { noteOff(note,channel); return; }
        size_t index=0; bool found=false;
        for(size_t i=0;i<voices.size();++i) if(!voices[i].env.active()) { index=i; found=true; break; }
        if(!found) for(size_t i=1;i<voices.size();++i) if(voices[i].age<voices[index].age) index=i;
        auto& v=voices[index]; v=Voice{}; deferred[index]=false;
        v.note=note; v.channel=std::clamp(channel,1,16); v.velocity=clamp(velocity,0,1); v.age=++counter; v.env.on();
        for(auto& envelope:v.modEnvelopes) envelope.on();
    }
    void noteOff(int note,int channel=1) {
        for(size_t i=0;i<voices.size();++i) if(voices[i].note==note && voices[i].channel==channel) {
            if(pedal[size_t(std::clamp(channel,1,16)-1)]) deferred[i]=true;
            else releaseVoice(voices[i]);
        }
    }
    int activeVoices() const { int n=0; for(const auto& v:voices) if(v.env.active()) ++n; return n; }
    std::array<float,2> next() {
        const auto first=nextInternal();
        const auto second=nextInternal();
        auto filtered=decimator.process(first,second);
        // Safety only; normal synthesis leaves headroom. FIR ringing must never
        // turn an extreme saturated patch into an out-of-range host sample.
        for(auto& value:filtered)value=clamp(value,-1,1);
        return filtered;
    }
private:
    std::array<float,2> nextInternal() {
        smoothControls();
        std::array<float,8> modulation {};
        for(size_t i=0;i<4;++i) {
            const auto& m=smoothed.mods[i];
            double cycles=m.sync && transportValid ? ppqPosition*m.rate : freeCycles[i];
            freeCycles[i]+=(m.sync ? tempo/60*m.rate : m.rate)/sr;
            float phase=float(cycles-std::floor(cycles));
            float value=0;
            if(m.type==0) { float triangle=1-4*std::abs(phase-.5f); value=std::copysign(std::pow(std::abs(triangle),std::pow(4.f,m.shape-.5f)),triangle); }
            else if(m.type==1) value=randomAt(int(i),cycles);
            else if(m.type==2 || m.type==4) {
                int count=std::clamp(m.stepCount,1,16), rotation=int(clamp(m.shift,0,1)*float(count))%count;
                int step=(std::min(count-1,int(phase*float(count)))+rotation)%count;
                if(m.type==2) { int hits=std::clamp(int(std::round(m.shape*float(count))),0,count); value=hits>0 && (step*hits)%count<hits ? 1.f:0.f; }
                else value=clamp(m.steps[size_t(step)],-1,1);
            }
            lastModValues[i]=value;
            if(m.type!=3) modulation[size_t(std::clamp(m.destination,0,7))]+=value*m.depth;
        }
        std::array<float,2> dry {};
        uint64_t displayAge=0;
        for(auto& v:voices) if(v.env.active()) {
            auto voiceModulation=modulation;
            float env=v.env.next(smoothed.attack,smoothed.decay,smoothed.sustain,sr), envelopeGain=0;
            for(size_t i=0;i<4;++i) {
                const auto& m=smoothed.mods[i];
                v.modValues[i]=v.modEnvelopes[i].next(m.attack,m.decay,m.sustain,sr);
                if(m.type==3) {
                    lastModValues[i]=std::max(lastModValues[i],v.modValues[i]);
                    if(m.destination==7) envelopeGain+=v.modValues[i]*m.depth;
                    else voiceModulation[size_t(std::clamp(m.destination,0,7))]+=v.modValues[i]*m.depth;
                }
            }
            std::array<float,2> sum {};
            for(size_t l=0;l<2;++l) {
                const auto& p=smoothed.layers[l];
                float ad=v.time<p.attack ? v.time/std::max(.001f,p.attack) : std::max(0.f,1-(v.time-p.attack)/std::max(.001f,p.decay));
                float color=p.color+ad*p.movement+voiceModulation[l];
                float frequency=440.f*std::pow(2.f,(float(v.note-69)+smoothedTuning[l]+smoothedBends[size_t(v.channel-1)]+voiceModulation[4+l]*12)/12.f);
                std::array<float,2> source {};
                if(sampleBanks[l]) {
                    double increment=sampleBanks[l]->playbackIncrement(frequency,sr);
                    source=sampleBanks[l]->read(v.samplePosition[l],color,p.oneshot,increment,v.streamState[l],nonRealtimeMode);
                    v.samplePosition[l]+=increment;
                } else {
                    float x=(!p.oneshot || v.time<p.attack+p.decay) ? wavetableSample(p.bank,color,v.phase[l],frequency) : 0;
                    source={x,x};
                }
                if(v.age>=displayAge) {
                    displayColors[l]=clamp(color,0,1);
                    if(sampleBanks[l]) {
                        const double frames=double(sampleBanks[l]->frames()),window=std::min(4096.0,frames);
                        const double base=displayColors[l]*(frames-window),fade=std::min(128.0,window*.25);
                        const double position=p.oneshot ? v.samplePosition[l] : std::fmod(v.samplePosition[l],window-fade);
                        displayPositions[l]=clamp(float((base+position)/frames),0,1);
                    } else displayPositions[l]=float(v.phase[l]);
                }
                v.phase[l]+=std::min(.49f,frequency/sr); v.phase[l]-=std::floor(v.phase[l]);
                float cutoff=p.cutoff*std::pow(2.f,voiceModulation[2+l]*4);
                if(p.shaper==1) v.gate[l]+=(env-v.gate[l])*(1-std::exp(-1.f/(sr*std::max(.02f,p.slew))));
                float mix=clamp(smoothed.mix+voiceModulation[6],0,1);
                float weight=l==0 ? std::cos(mix*float(pi)*.5f) : std::sin(mix*float(pi)*.5f);
                for(size_t c=0;c<2;++c) {
                    float x=source[c];
                    if(p.shaper==0) x=v.filters[l][c].tick(x,cutoff,p.resonance,p.filter,sr);
                    else if(p.shaper==1) x=v.filters[l][c].tick(x,cutoff*v.gate[l],p.resonance,0,sr)*v.gate[l];
                    else {
                        float out=0;
                        for(size_t band=0;band<3;++band) {
                            float hz=clamp(cutoff,159,2518)*std::pow(p.spread,float(int(band)-1));
                            float spectralWeight=band==0 ? 1-p.tilt : band==2 ? p.tilt : .5f;
                            out+=spectralWeight*v.resonators[l][c][band].tick(x,hz,.8f,1,sr);
                        }
                        x=out*.5f;
                    }
                    sum[c]+=x*weight;
                }
            }
            for(size_t c=0;c<2;++c) dry[c]+=sum[c]*env*v.velocity*.15f*clamp(1+envelopeGain,0,2);
            displayAge=std::max(displayAge,v.age);
            v.time+=1/sr;
        }
        float drive=1+smoothed.drive*15;
        std::array<float,2> effected {};
        fxPhase+=.3/sr; if(fxPhase>=1) fxPhase-=1;
        for(size_t c=0;c<2;++c) {
            float x=std::tanh(dry[c]*drive)/std::sqrt(drive);
            float sweep=float(std::sin(2*pi*(fxPhase+double(c)*.25)));
            float ch=read(chorusBuffer[c],chorusPos,sr*(.015f+.004f*sweep));
            float fl=read(chorusBuffer[c],chorusPos,sr*(.003f+.002f*sweep));
            chorusBuffer[c][chorusPos]=x+fl*smoothed.flanger*.5f;
            x+=ch*smoothed.chorus*.5f+fl*smoothed.flanger*.5f;
            float echo=read(delayBuffer[c],delayPos,sr*clamp(smoothed.delayTime,.001f,2));
            delayBuffer[c][delayPos]=std::tanh(x+echo*clamp(smoothed.feedback,0,.9f));
            effected[c]=x+echo*smoothed.delay;
        }
        auto room=reverb.tick(effected);
        std::array<float,2> output {};
        for(size_t c=0;c<2;++c) output[c]=std::tanh((effected[c]+room[c]*smoothed.reverb)*smoothed.gain*clamp(1+modulation[7],0,2));
        delayPos=(delayPos+1)%delayBuffer[0].size(); chorusPos=(chorusPos+1)%chorusBuffer[0].size();
        if(transportValid && transportPlaying) ppqPosition+=double(tempo)/(60*double(sr));
        return output;
    }
};
}
