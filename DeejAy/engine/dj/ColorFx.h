// SOUND COLOR FX (DDJ-FLX10 manual pp.56-57, 101): one type selected for all channels, each channel's
// COLOR knob (-1 .. +1, centre = off) drives it. Placement follows the signal-flow diagram:
// NOISE before the EQ, FILTER / CRUSH / PITCH after the EQ, DUB ECHO / SPACE after the channel fader
// (their tails keep ringing when the knob returns to centre or the fader closes).
#pragma once
#include "Effects.h"
#include "dsp/Biquad.h"
#include "dsp/Reverb.h"
#include <memory>
#include <vector>

namespace dj
{

enum class ColorFxType : int { None = -1, Space = 0, DubEcho = 1, Crush = 2, Pitch = 3, Noise = 4, Filter = 5 };
constexpr int kNumColorFx = 6;
const char* colorFxName (ColorFxType t) noexcept;

class ColorFx
{
public:
    void prepare (double sampleRate, int maxBlock);
    void reset() noexcept;

    void processPreEq (ColorFxType type, float knob, float* l, float* r, int n) noexcept;
    void processPostEq (ColorFxType type, float knob, float* l, float* r, int n) noexcept;
    void processPostFader (ColorFxType type, float knob, double bpm, float* l, float* r, int n) noexcept;

private:
    void updateFilter (float x) noexcept;

    double sr_ = 48000.0;
    float knob_ = 0.0f;        // smoothed knob shared by all stages of the selected type
    // FILTER
    dsp::Biquad filt_[2];
    float lastFilterX_ = 99.0f;
    // CRUSH
    dsp::Biquad crushHp_;
    float crushHold_[2] { 0, 0 }, crushCount_ = 0.0f, lastCrushX_ = 99.0f;
    // PITCH
    std::unique_ptr<Effect> pitch_;
    std::vector<float> wl_, wr_;
    // NOISE
    dsp::Biquad noiseF_;
    std::uint32_t seed_ = 0x9e3779b9u;
    float lastNoiseX_ = 99.0f;
    // SPACE
    dsp::Reverb reverb_;
    dsp::Biquad spaceTone_;
    int spaceTail_ = 0;
    float lastSpaceSign_ = 0.0f;
    // DUB ECHO
    DelayLine echo_;
    dsp::Biquad echoTone_[2];
    float echoTime_ = -1.0f;
    int echoTail_ = 0;
    float lastEchoSign_ = 0.0f;
};

} // namespace dj
