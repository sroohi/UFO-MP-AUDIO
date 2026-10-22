#include "SampleBank.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
constexpr double pi=3.14159265358979323846;
int checks=0;
void require(bool passed,const char* message) {
    ++checks;
    if(!passed){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}
}
std::array<std::vector<float>,2> sine(double hz,size_t frames=65536) {
    std::array<std::vector<float>,2> data;
    for(size_t channel=0;channel<2;++channel) {
        data[channel].resize(frames);
        for(size_t i=0;i<frames;++i)data[channel][i]=float((channel==0?.8:.4)*std::sin(2*pi*hz*double(i)/48000));
    }
    return data;
}
double magnitude(const ufo::SampleBank& bank,double increment,double frequency) {
    constexpr int count=4800;
    double real=0,imaginary=0;
    // Skip the FIR's source-boundary transient while retaining exact DFT bins.
    for(int i=0;i<count;++i) {
        const auto sample=bank.read(1024+double(i)*increment,0,true,increment)[0];
        const auto angle=2*pi*frequency*double(i)/48000;
        real+=sample*std::cos(angle);imaginary-=sample*std::sin(angle);
    }
    return std::sqrt(real*real+imaginary*imaginary)/count;
}
}
int main() {
    for(double rate:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),0.0}) {
        bool rejected=false;
        try{ufo::SampleBank invalid(sine(200,128),rate);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"invalid sample rates are rejected");
    }
    auto invalidData=sine(200,128);invalidData[1][3]=std::numeric_limits<float>::quiet_NaN();
    bool rejected=false;try{ufo::SampleBank invalid(std::move(invalidData),48000);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"non-finite audio is rejected");

    ufo::SampleBank high(sine(16000),48000);
    // The old ceil(log2(rate)) path stepped from -0.69 to nearly zero here.
    for(int power=-1;power<=10;++power) {
        const auto boundary=std::ldexp(1.0,power);
        for(double position:{200.,511.37,1700.2}) {
            const auto before=high.read(position,.23f,false,boundary*(1-1e-7));
            const auto after=high.read(position,.23f,false,boundary*(1+1e-7));
            for(size_t channel=0;channel<2;++channel) {
                require(std::isfinite(before[channel])&&std::isfinite(after[channel]),"mip boundary samples remain finite");
                require(std::abs(before[channel]-after[channel])<1e-5,"rate automation crosses mip boundaries continuously");
            }
        }
    }
    require(magnitude(high,2,16000)<.0005,"double-speed out-of-band source is strongly suppressed before resampling");
    ufo::SampleBank upper(sine(7000),48000);
    require(magnitude(upper,4,20000)<.0005,"quadruple-speed folded source is suppressed");
    ufo::SampleBank low(sine(200),48000);
    require(magnitude(low,2,400)>.2,"anti-alias filtering retains legal low-frequency energy");

    auto stereoData=sine(311,8192);
    ufo::SampleBank stereo(std::move(stereoData),48000);
    const auto sample=stereo.read(200,0,true,.5);
    require(std::abs(sample[0])>.1 && std::abs(sample[0]-2*sample[1])<1e-6,"stereo channel content is preserved");
    const auto end=stereo.read(double(stereo.frames()),0,true,.5);
    require(end[0]==0&&end[1]==0,"one-shot ends at the actual source endpoint");
    const auto tail=stereo.read(double(stereo.frames()-1),0,true,.5);
    require(std::isfinite(tail[0])&&std::isfinite(tail[1]),"last one-shot frame is in bounds");
    for(float color:{0.f,.5f,1.f}) {
        const auto before=stereo.read(3968-1e-6,color,false,.5);
        const auto after=stereo.read(3968+1e-6,color,false,.5);
        require(std::abs(before[0]-after[0])<1e-5,"loop endpoint crossfade is continuous across color positions");
    }
    require(std::abs(stereo.playbackIncrement(261.6255653f,48000)-1)<1e-6,"root pitch plays at original source speed");
    const auto bad=stereo.read(200,0,false,std::numeric_limits<double>::quiet_NaN());
    require(bad[0]==0&&bad[1]==0,"invalid playback rate cannot produce invalid memory access");
    const auto waveform=stereo.waveform(128);require(waveform.size()==128,"waveform produces requested bins");
    require(std::any_of(waveform.begin(),waveform.end(),[](float value){return std::abs(value)>.7f;}),"waveform reflects source amplitudes");
    std::array<std::vector<float>,2> shortest;
    for(auto& channel:shortest)channel.assign(64,0);
    shortest[0][0]=.4f;shortest[1][0]=-.8f;shortest[0][63]=-.9f;shortest[1][63]=.7f;
    ufo::SampleBank shortBank(std::move(shortest),48000);
    const auto expanded=shortBank.waveform(8192);
    require(expanded.size()==8192,"short source waveform expands to exactly requested point count");
    require(expanded.front()==-.8f && expanded.back()==-.9f,"expanded waveform retains signed strongest stereo peaks");
    require(std::all_of(expanded.begin(),expanded.end(),[](float value){return std::isfinite(value);}),"points greater than frames stay in bounds");
    require(shortBank.waveform(1)[0]==-.9f,"overview downsampling retains strongest signed peak");
    require(shortBank.waveform(0).empty(),"nonpositive waveform point count returns no bins");
    const auto native=shortBank.waveform(64);
    require(native[0]==-.8f && native[63]==-.9f,"short-source overview preserves each source frame");
    std::cout<<"PASS: "<<checks<<" sample-bank checks; mip continuity, anti-alias filtering, stereo, endpoints and invalid input\n";
}
