// 4-channel DJ mixer: trim, 3-band EQ (DJM-style -26..+6 dB, or a full-kill isolator), Part ISO,
// SOUND COLOR FX, channel fader curve, crossfader assign / curve / cut lag / reverse, cue, headphones,
// master + booth, peak limiter. The FLX10 sends every knob and fader as 14-bit MIDI; the mixing
// itself happens here when the channel input switch is on PC-A/PC-B (manual p.174 signal flow).
#pragma once
#include "ColorFx.h"
#include "dsp/Biquad.h"
#include <array>
#include <atomic>

namespace dj
{

enum class EqMode : int { Classic = 0, Isolator = 1 };
enum class XfAssign : int { A = 0, Thru = 1, B = 2 };

struct ChannelControls
{
    std::atomic<float> trim { 0.5f };          // 0..1, 0.5 = 0 dB, 1 = +12 dB, 0 = off
    std::atomic<float> eq[3] { 0.5f, 0.5f, 0.5f }; // HI, MID, LOW; 0.5 = flat
    std::atomic<float> fader { 1.0f };
    std::atomic<float> color { 0.0f };         // SOUND COLOR FX knob, -1..+1
    std::atomic<bool> cue { false };
    std::atomic<int> xfAssign { (int) XfAssign::Thru };
    std::atomic<bool> partIso { false };       // SHIFT + CH CUE: EQ knobs drive stem volumes
    std::atomic<float> autoGainDb { 0.0f };    // from loudness analysis (added to trim)
};

struct MixerControls
{
    std::array<ChannelControls, kNumDecks> ch;
    std::atomic<float> crossfader { 0.5f };    // 0 = A (left), 1 = B (right)
    std::atomic<float> xfCurve { 0.75f };      // 0 = smooth (constant power) .. 1 = sharp cut
    std::atomic<float> xfCutLag { 0.02f };     // fraction of travel that is dead at each end
    std::atomic<bool> xfReverse { false };     // "hamster"
    std::atomic<float> chFaderCurve { 0.5f };  // 0 = slow .. 1 = fast
    std::atomic<float> masterLevel { 1.0f };   // software master (FLX10 applies its own in hardware)
    std::atomic<float> boothLevel { 0.0f };
    std::atomic<float> phonesMix { 0.0f };     // 0 = cue, 1 = master
    std::atomic<float> phonesLevel { 0.8f };
    std::atomic<bool> phonesMixInHardware { true }; // FLX10: HP MIX / LEVEL act on the unit
    std::atomic<bool> phonesSplit { false };   // MONO SPLIT: cue left, master right
    std::atomic<bool> splitInvert { false };
    std::atomic<bool> masterCue { false };     // MASTER CUE button
    std::atomic<float> samplerVolume { 0.8f };
    std::atomic<bool> samplerCue { false };
    std::atomic<int> colorFx { (int) ColorFxType::None };
    std::atomic<int> eqMode { (int) EqMode::Classic };
    std::atomic<bool> limiter { true };
    std::atomic<bool> masterMono { false };
};

struct MixerView
{
    std::atomic<float> channelPeak[kNumDecks] { 0, 0, 0, 0 };
    std::atomic<float> masterPeak[2] { 0, 0 };
    std::atomic<float> limiterReductionDb { 0.0f };
    std::atomic<bool> clip { false };
};

// Gain of a channel fader position under a curve setting.
float channelFaderGain (float position, float curve) noexcept;
// Gain of one crossfader side. `distance` = 0 at the channel's own side, 1 at the far side.
float crossfaderGain (float distance, float curve, float cutLag) noexcept;
// Trim knob 0..1 -> linear gain.
float trimGain (float knob) noexcept;
// Part ISO knob 0..1 -> stem gain (0 at fully CCW, 1 at centre, +6 dB fully CW).
float isoGain (float knob) noexcept;

class ChannelEq
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void process (const float* knobs, EqMode mode, float* l, float* r, int n) noexcept;
private:
    double sr_ = 48000.0;
    float last_[3] { -1, -1, -1 };
    EqMode lastMode_ = EqMode::Classic;
    dsp::Biquad low_, mid_, high_;
    dsp::LR4 xLow_, xHigh_, align_;   // isolator: split at 250 Hz and 3 kHz, plus phase alignment
    Smoothed g_[3];
};

// Lookahead peak limiter with a short delay so gain reduction is applied before the peak arrives.
class Limiter
{
public:
    void prepare (double sampleRate);
    void process (float* l, float* r, int n, float ceiling) noexcept;
    float reductionDb() const noexcept { return gainToDb (gain_); }
private:
    static constexpr int kLookahead = 64;
    std::array<float, kLookahead> dl_ {}, dr_ {}, req_ {};
    int idx_ = 0;
    float gain_ = 1.0f, release_ = 0.9999f;
};

} // namespace dj
