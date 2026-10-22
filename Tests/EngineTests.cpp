#include "Engine.h"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <chrono>
static int checks=0;
static void require(bool yes,const char* message) { ++checks; if(!yes) { std::cerr<<"FAIL: "<<message<<'\n'; std::exit(1); } }
static double render(ufo::Engine& e,int samples) {
    double energy=0;
    for(int i=0;i<samples;++i) { auto out=e.next(); require(std::isfinite(out[0])&&std::isfinite(out[1]),"finite audio"); require(std::abs(out[0])<=1&&std::abs(out[1])<=1,"bounded output"); energy+=out[0]*out[0]+out[1]*out[1]; }
    return energy;
}
static double oscillatorMagnitude(const ufo::Engine& e,double frequency,double binFrequency) {
    double real=0,imaginary=0;
    constexpr int count=4800;
    for(int i=0;i<count;++i) {
        const double sample=e.wavetableSample(0,1,double(i)*frequency/48000.0,float(frequency));
        const double angle=2*ufo::pi*binFrequency*double(i)/48000.0;
        real+=sample*std::cos(angle);imaginary-=sample*std::sin(angle);
    }
    return std::sqrt(real*real+imaginary*imaginary)/count;
}
static double nonlinearMagnitude(bool oversampled,double binFrequency) {
    ufo::StereoDecimator2x decimator;
    double real=0,imaginary=0;
    constexpr int warmup=512,count=4800;
    for(int i=0;i<warmup+count;++i) {
        auto saturate=[](double time){return float(std::tanh(3*std::sin(2*ufo::pi*7000*time)));};
        float sample=saturate(double(i)/48000);
        if(oversampled) sample=decimator.process({sample,sample},{saturate((double(i)+.5)/48000),0})[0];
        if(i>=warmup) {
            const double angle=2*ufo::pi*binFrequency*double(i)/48000;
            real+=sample*std::cos(angle);imaginary-=sample*std::sin(angle);
        }
    }
    return std::sqrt(real*real+imaginary*imaginary)/count;
}
static void oversamplingRegressions() {
    require(ufo::Engine::latencySamples()==32,"engine reports its fixed host-rate FIR latency");
    ufo::StereoDecimator2x decimator;
    int peakIndex=-1;float peak=0;
    for(int i=0;i<128;++i) {
        auto out=decimator.process(i==0?std::array<float,2>{1,0}:std::array<float,2>{0,0},{0,0});
        if(std::abs(out[0])>peak){peak=std::abs(out[0]);peakIndex=i;}
        require(out[1]==0,"decimator keeps channels isolated");
    }
    require(peakIndex==ufo::Engine::latencySamples(),"decimator impulse peak matches reported latency");
    decimator.reset();float dc=0;
    for(int i=0;i<256;++i)dc=decimator.process({1,1},{1,1})[0];
    require(std::abs(dc-1)<.00001f,"FIR decimator preserves DC gain");
    auto rms=[&](double frequency) {
        decimator.reset();double energy=0;
        for(int i=0;i<5312;++i){float a=float(std::sin(2*ufo::pi*frequency*double(i*2))),b=float(std::sin(2*ufo::pi*frequency*double(i*2+1)));auto out=decimator.process({a,a},{b,b});if(i>=512)energy+=out[0]*out[0];}
        return std::sqrt(energy/4800);
    };
    require(rms(.26)<rms(.1)*.001,"FIR removes above-output-Nyquist energy by at least 60 dB");
    const double baselineAlias=nonlinearMagnitude(false,13000),qualityAlias=nonlinearMagnitude(true,13000);
    require(baselineAlias>.01,"nonlinear reference exposes the fifth-harmonic alias");
    require(qualityAlias<baselineAlias*.03,"2x processing and FIR decimation reduce nonlinear alias by over 30 dB");
    const double normalFundamental=nonlinearMagnitude(false,7000),qualityFundamental=nonlinearMagnitude(true,7000);
    require(std::abs(qualityFundamental/normalFundamental-1)<.01,"oversampling retains nonlinear fundamental level");
    auto instrument=std::make_unique<ufo::Engine>();
    instrument->params.mix=0;instrument->params.sustain=1;instrument->params.attack=.001f;instrument->params.gain=1;instrument->params.drive=1;instrument->params.delay=0;instrument->params.reverb=0;
    instrument->params.layers[0].color=1;instrument->params.layers[0].movement=0;instrument->params.layers[0].cutoff=17600;instrument->params.layers[0].resonance=0;
    instrument->prepare(48000);instrument->noteOn(117,1);render(*instrument,4800);
    double fundamentalReal=0,fundamentalImaginary=0,aliasReal=0,aliasImaginary=0;
    for(int i=0;i<48000;++i) {
        const float sample=instrument->next()[0];
        const double carrier=2*ufo::pi*7040*double(i)/48000,alias=2*ufo::pi*12800*double(i)/48000;
        fundamentalReal+=sample*std::cos(carrier);fundamentalImaginary-=sample*std::sin(carrier);
        aliasReal+=sample*std::cos(alias);aliasImaginary-=sample*std::sin(alias);
    }
    const double actualFundamental=std::hypot(fundamentalReal,fundamentalImaginary),actualAlias=std::hypot(aliasReal,aliasImaginary);
    require(actualFundamental>1,"full engine high-register drive test remains audible");
    require(actualAlias<actualFundamental*.002,"actual instrument oversampling suppresses the fifth-harmonic folded image");
    std::cout<<"Oversampling: 13 kHz alias "<<baselineAlias<<" -> "<<qualityAlias<<"; latency "<<ufo::Engine::latencySamples()<<" samples\n";
}
static void productionRegressions() {
    auto first=std::make_unique<ufo::Engine>();auto second=std::make_unique<ufo::Engine>();auto& e=*first;auto& reference=*second;
    e.prepare(48000);
    const double fundamental=oscillatorMagnitude(e,10000,10000);
    require(fundamental>.1,"high-register source retains its fundamental");
    require(oscillatorMagnitude(e,10000,20000)>fundamental*.1,"legal upper harmonic is retained");
    require(oscillatorMagnitude(e,10000,18000)<fundamental*.001,"third harmonic folding below Nyquist is suppressed by at least 60 dB");
    require(oscillatorMagnitude(e,10000,8000)<fundamental*.001,"fourth harmonic alias is suppressed by at least 60 dB");
    for(int partial=2;partial<=16;++partial) {
        const float boundary=48000.f*.5f/float(partial);
        const float below=e.wavetableSample(0,1,.137,boundary-.001f);
        const float above=e.wavetableSample(0,1,.137,boundary+.001f);
        require(std::abs(above-below)<.00001f,"mip transitions do not step when pitch crosses a harmonic limit");
    }
    for(auto* synth:{&e,&reference}) {
        synth->params=ufo::Parameters{};synth->params.delay=0;synth->params.reverb=0;synth->params.sustain=1;synth->params.gain=.7f;
        synth->prepare(48000);synth->noteOn(69,.8f);render(*synth,4800);
    }
    e.params.gain=0;
    float changed=e.next()[0],unchanged=reference.next()[0];
    require(std::abs(unchanged)>.00001f,"automation comparison has an audible reference");
    require(std::abs(changed/unchanged)>.98f,"gain automation ramps instead of hard-muting one sample");
    render(e,14400);require(render(e,100)==0,"gain ramp reaches exact silence");

    e.params=ufo::Parameters{};auto& m=e.params.mods[0];m.type=3;m.depth=1;m.attack=.1f;m.decay=.1f;m.sustain=.4f;m.release=.05f;
    e.prepare(48000);e.noteOn(60,.8f);render(e,4800);e.noteOn(64,.8f);render(e,2400);
    require(e.voiceModulationValues(60)[0]>.6f,"older voice retains its own modulation envelope decay");
    require(e.voiceModulationValues(64)[0]>.45f && e.voiceModulationValues(64)[0]<.55f,"new voice independently attacks its modulation envelope");
    e.noteOff(60);render(e,2401);
    require(e.voiceModulationValues(60)[0]<.001f,"note off releases only that voice modulation envelope");
    require(e.voiceModulationValues(64)[0]>.9f,"held voice modulation envelope survives another voice release");

    e.params=ufo::Parameters{};auto& sequence=e.params.mods[0];sequence.type=4;sequence.sync=true;sequence.stepCount=16;sequence.rate=1;
    for(size_t i=0;i<sequence.steps.size();++i) sequence.steps[i]=float(i)/15.f*2-1;
    e.prepare(48000);e.setTempo(120);e.setTransport(.5,true,true);e.next();
    require(std::abs(e.modulationValues()[0]-sequence.steps[8])<.00001f,"16-step editable sequence follows absolute host PPQ");
    e.setTransport(.0625,true,true);e.next();
    require(std::abs(e.modulationValues()[0]-sequence.steps[1])<.00001f,"host seek retriggers the exact sequence step");
    e.setTransport(.3,false,true);e.next();float stopped=e.modulationValues()[0];render(e,5000);
    require(e.modulationValues()[0]==stopped,"synced sequence holds its phase while host is stopped");
    sequence.shift=.25f;e.reset();e.setTransport(0,false,true);e.next();
    require(std::abs(e.modulationValues()[0]-sequence.steps[4])<.00001f,"sequence shift rotates the pattern");
    sequence.type=2;sequence.stepCount=8;sequence.shape=.375f;sequence.shift=0;e.reset();int pulses=0;
    for(int step=0;step<8;++step){e.setTransport(double(step)/8,false,true);e.next();if(e.modulationValues()[0]>.5f)++pulses;}
    require(pulses==3,"Euclidean generator distributes the requested pulse count");
    sequence.type=1;e.reset();e.setTransport(3.25,false,true);e.next();float random=e.modulationValues()[0];
    e.setTransport(7.25,false,true);e.next();e.setTransport(3.25,false,true);e.next();
    require(e.modulationValues()[0]==random,"sample-and-hold is deterministic on transport seeks");
    e.params=ufo::Parameters{};e.params.mods[0].type=4;e.params.mods[0].sync=true;e.params.mods[0].rate=4;e.params.mods[0].stepCount=16;
    reference.params=e.params;e.prepare(48000);reference.prepare(48000);e.setTransport(.123,true,true);reference.setTransport(.123,true,true);
    render(e,1500);render(reference,500);reference.setTransport(.123+500.0*120/(60*48000),true,true);render(reference,1000);
    require(e.modulationValues()[0]==reference.modulationValues()[0],"host sync is independent of audio block partitioning");

    std::array<std::vector<float>,2> stereo;
    for(auto& channel:stereo)channel.resize(8192);
    for(size_t i=0;i<stereo[0].size();++i) {
        stereo[0][i]=.6f*float(std::sin(2*ufo::pi*200*double(i)/48000));
        stereo[1][i]=.6f*float(std::sin(2*ufo::pi*300*double(i)/48000));
    }
    ufo::SampleBank imported(std::move(stereo),48000);
    e.params=ufo::Parameters{};e.params.mix=0;e.params.delay=0;e.params.reverb=0;e.params.attack=.001f;e.params.sustain=1;
    e.params.layers[0].color=0;e.params.layers[0].movement=0;e.params.layers[0].oneshot=true;
    e.prepare(48000);e.setSampleBank(0,&imported);e.noteOn(60,1);
    double stereoEnergy=0,channelDifference=0;
    for(int i=0;i<7000;++i){auto out=e.next();stereoEnergy+=out[0]*out[0]+out[1]*out[1];channelDifference+=std::abs(out[0]-out[1]);}
    require(stereoEnergy>1 && channelDifference>1,"imported stereo samples preserve distinct left and right content");
    render(e,48000);require(render(e,1024)<.000001,"imported one-shot stops at its actual sample endpoint");
    e.setSampleBank(0,nullptr);

    ufo::DiffusionReverb room;room.prepare(48000);double early=0,late=0,stereoDifference=0;int occupied=0;
    for(int i=0;i<48000*3;++i) {
        auto out=room.tick(i==0?std::array<float,2>{1,0}:std::array<float,2>{0,0});
        require(std::isfinite(out[0])&&std::isfinite(out[1]),"diffusion network remains finite");
        const double power=out[0]*out[0]+out[1]*out[1];
        if(i<24000)early+=power;
        if(i>=48000*2)late+=power;
        if(i>1000 && i<12000 && power>1e-10)++occupied;
        stereoDifference+=std::abs(out[0]-out[1]);
    }
    require(early>0 && late<early*.001,"diffusion reverb impulse has a decaying stable tail");
    require(occupied>4000,"allpass diffusion produces a dense impulse response");
    require(stereoDifference>1,"reverb decorrelates its stereo output");
    room.reset();auto silent=room.tick({0,0});require(silent[0]==0&&silent[1]==0,"reverb reset clears its tail");
}
int main(int argc,char** argv) {
    auto engine=std::make_unique<ufo::Engine>(); auto& e=*engine;
    e.params.delay=0; e.params.reverb=0;
    require(render(e,1024)==0,"initial silence");
    e.noteOn(69,.8f); require(render(e,24000)>1,"note produces audio");
    e.noteOff(69); render(e,48000); require(e.activeVoices()==0,"release completes");
    require(render(e,2048)==0,"silence after release without FX");
    e.noteOn(60,.7f); e.sustain(1,true); e.noteOff(60); render(e,48000); require(e.activeVoices()==1,"pedal sustains voice");
    e.sustain(1,false); render(e,48000); require(e.activeVoices()==0,"pedal release ends voice");
    e.reset(); for(int i=0;i<32;++i)e.noteOn(40+i,.8f); require(e.activeVoices()==14,"voice stealing is bounded"); render(e,1024); e.reset();
    require(e.activeVoices()==0,"panic clears voices");
    // Both channels survive independent bends, pedal and releases.
    e.noteOn(60,.8f,1);e.noteOn(60,.8f,2);e.pitchBend(2,1);e.noteOff(60,1);render(e,48000);require(e.activeVoices()==1,"MIDI channel isolation");e.reset();
    for(double sr:{44100.,48000.,96000.}) for(int shaper=0;shaper<3;++shaper) {
        e.prepare(sr); e.params.drive=1;e.params.chorus=1;e.params.flanger=1;e.params.delay=1;e.params.reverb=1;e.params.feedback=.9f;
        for(auto& l:e.params.layers){l.shaper=shaper;l.resonance=1;l.cutoff=17600;l.color=1;}
        for(int i=0;i<4;++i){e.params.mods[size_t(i)].type=i;e.params.mods[size_t(i)].depth=1;e.params.mods[size_t(i)].destination=i;e.params.mods[size_t(i)].rate=50;}
        for(int n=36;n<50;++n)e.noteOn(n,1);
        require(render(e,int(sr*.2))>0,"all shapers render under modulation and effects");
    }
    // Pitch test: zero crossing frequency of a dark, fundamental-heavy source.
    e.params=ufo::Parameters{}; e.prepare(48000); e.params.layers[0].color=0;e.params.layers[0].movement=0;e.params.mix=0;e.params.sustain=1;e.params.delay=0;e.params.reverb=0;e.noteOn(69,1);
    render(e,4800); int crossings=0;float previous=0;
    for(int i=0;i<48000;++i){float x=e.next()[0];if(previous<=0&&x>0)++crossings;previous=x;}
    require(std::abs(crossings-440)<=2,"A4 tuning");
    e.pitchBend(1,1); render(e,4800); crossings=0; previous=0;
    for(int i=0;i<48000;++i){float x=e.next()[0];if(previous<=0&&x>0)++crossings;previous=x;}
    require(std::abs(crossings-494)<=2,"two-semitone pitch bend");
    e.reset(); e.noteOn(60,0); require(e.activeVoices()==0,"zero velocity is note off");
    e.params=ufo::Parameters{}; e.prepare(48000); e.params.delay=0; e.params.reverb=0;
    e.params.layers[0].oneshot=true; e.params.layers[1].oneshot=true;
    e.params.layers[0].attack=e.params.layers[1].attack=.01f;
    e.params.layers[0].decay=e.params.layers[1].decay=.01f;
    e.noteOn(60,1);require(render(e,960)>0,"one shot starts");render(e,48000);
    require(render(e,1024)<.000001,"one shot ends without MIDI note off");
    for(int type=0;type<5;++type) {
        e.params=ufo::Parameters{};e.prepare(48000);
        e.params.mods[0].type=type;e.params.mods[0].depth=.8f;e.params.mods[0].rate=20;
        e.noteOn(60,.8f);require(render(e,4800)>0,"all five modulation generators render");
    }
    productionRegressions();
    oversamplingRegressions();
    e.params=ufo::Parameters{};e.params.sustain=1;e.params.drive=.5f;e.params.chorus=.5f;e.params.flanger=.25f;e.params.delay=.3f;e.params.reverb=.3f;e.prepare(48000);
    for(int note=48;note<62;++note)e.noteOn(note,.7f);
    const auto started=std::chrono::steady_clock::now();double benchmarkEnergy=0;
    for(int i=0;i<48000;++i){auto out=e.next();benchmarkEnergy+=out[0]*out[0]+out[1]*out[1];}
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    require(benchmarkEnergy>0,"CPU diagnostic renders the full 14-voice effect chain");
    std::cout<<"CPU diagnostic: 1 s at 48 kHz / 14 dual-layer voices / 2x DSP + FX rendered in "<<elapsed<<" s ("<<elapsed*100<<"% of one core's realtime budget on this runner)\n";
    if(argc>1) {
        e.params=ufo::Parameters{};e.prepare(48000);e.params.chorus=.4f;e.params.reverb=.35f;
        std::ofstream file(argv[1],std::ios::binary); require(bool(file),"open demo WAV");
        auto u16=[&](uint16_t v){file.put(char(v&255));file.put(char(v>>8));};
        auto u32=[&](uint32_t v){u16(uint16_t(v));u16(uint16_t(v>>16));};
        constexpr uint32_t count=48000*8;
        file.write("RIFF",4);u32(36+count*4);file.write("WAVEfmt ",8);u32(16);u16(1);u16(2);u32(48000);u32(48000*4);u16(4);u16(16);file.write("data",4);u32(count*4);
        for(uint32_t i=0;i<count;++i){if(i%48000==0 && i<48000*6){e.noteOn(48+int(i/48000)*2,.65f);e.noteOn(55+int(i/48000)*2,.5f);}if(i%48000==36000){e.noteOff(48+int(i/48000)*2);e.noteOff(55+int(i/48000)*2);}auto out=e.next();for(float x:out)u16(uint16_t(int16_t(x*32767)));}
        require(bool(file),"write demo WAV");
    }
    std::cout<<"PASS: "<<checks<<" checks; MIDI, tuning, stereo samples, band limiting, automation ramps, polyphonic modulation, PPQ sync, oversampling and diffusion FX\n";
}
