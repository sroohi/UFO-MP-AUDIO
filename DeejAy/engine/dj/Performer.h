// Performer: the performance FX section on the master bus. Everything the decks and the sampler play goes
// through it after the master FX unit and before the master level, limiter, outputs and recorder.
//
// Nine effects, each with an on switch and two parameters x / y in 0..1 (the XY pad axes):
//   Stutter      x: slice 1/4 .. 1/32 beat           y: build-up (the slice keeps halving)
//   Note Repeat  x: rate 1/2 .. 1/32 (incl. triplets) y: gate (how much of each repeat sounds)
//   Tape Stop    x: stop time 1/4 .. 4 beats         y: curve (linear .. hard brake)
//   Pitch        x: -12 .. +12 semitones             y: mix
//   Filter       x: low-pass <- centre -> high-pass  y: resonance
//   Talkbox      x: vowel A E I O U                  y: vowel sweep (off, 4 .. 1/4 beats)
//   Bitcrusher   x: bit depth 16 .. 3                y: sample-rate reduction 1x .. 32x
//   Delay        x: 1/8 .. 1 beat                    y: feedback        (tails ring out after off)
//   Reverb       x: size                             y: send            (tails ring out after off)
// Chain: time effects (tape stop > stutter > note repeat; the highest one switched on plays), pitch,
// filter, talkbox, bitcrusher, delay, reverb. Rhythmic values follow the master tempo and the leader's
// beat phase, so a 1/8 stutter lands on the grid. Switching any effect on or off crossfades over 10 ms.
//
// Real-time safe: all memory is allocated in prepare().
#pragma once
#include "Common.h"
#include "dsp/Reverb.h"
#include "dsp/Svf.h"
#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace dj
{

enum class PerformerFx : int { Stutter = 0, NoteRepeat, TapeStop, Pitch, Filter, Talkbox, Bitcrusher, Delay, Reverb };
constexpr int kNumPerformerFx = 9;

const char* performerFxName (PerformerFx fx) noexcept;
const char* performerParamName (PerformerFx fx, int axis) noexcept;      // axis 0 = x, 1 = y
std::string performerParamText (PerformerFx fx, int axis, float value);  // "1/16", "-3 st", "E", ...

// Parameter mappings (exposed for the UI and the tests)
namespace perf
{
    double stutterSliceBeats (float x) noexcept;
    double noteRepeatBeats (float x) noexcept;
    double tapeStopBeats (float x) noexcept;
    int pitchSemitones (float x) noexcept;
    double delayBeats (float x) noexcept;
    double talkboxSweepBeats (float y) noexcept;  // 0 = no sweep
    float crushBits (float x) noexcept;
    int crushDownsample (float y) noexcept;
}

struct PerformerControls
{
    std::atomic<bool> enabled { true };               // section on / bypass
    std::atomic<bool> on[kNumPerformerFx] {};
    std::atomic<float> x[kNumPerformerFx] { 0.30f, 0.70f, 0.50f, 0.50f, 0.50f, 0.00f, 0.50f, 0.55f, 0.60f };
    std::atomic<float> y[kNumPerformerFx] { 0.00f, 0.80f, 0.30f, 1.00f, 0.30f, 0.00f, 0.20f, 0.45f, 0.40f };
};

struct PerformerView
{
    std::atomic<bool> sounding[kNumPerformerFx] {};   // on, fading, or ringing out
};

class Performer
{
public:
    PerformerControls controls;
    PerformerView view;

    void prepare (double sampleRate, int maxBlock);
    void reset() noexcept;

    // `beat`: master beat position at the first sample (any origin; the leader deck's grid), or a negative
    // value when no deck leads, in which case an internal clock at `bpm` keeps time.
    void process (float* l, float* r, int n, double bpm, double beat) noexcept DJ_NONBLOCKING;

private:
    enum TimeMode { kStutter = 0, kRepeat = 1, kTape = 2, kTimeModes = 3 };
    struct TimeState
    {
        float gain = 0.0f;          // crossfade gain of this mode's output
        bool running = false;
        double start = 0.0;         // absolute ring index of the captured slice (stutter / repeat)
        double pos = 0.0;           // samples into the slice, or the tape read position (absolute)
        double rate = 1.0;
        double elapsed = 0.0;       // samples since the mode started
        double lastBeatIndex = -1.0;
        double startBeat = 0.0;
    };

    float ringAt (const std::vector<float>& ring, double index) const noexcept;
    void startTimeMode (int mode, double beat, double samplesPerBeat) noexcept;
    float pitchTap (int ch, double delay) const noexcept;

    double sr_ = 48000.0;
    int maxBlock_ = 0;
    float rampStep_ = 0.001f;
    double internalBeat_ = 0.0;

    // input ring for the time effects
    std::vector<float> ringL_, ringR_;
    std::int64_t written_ = 0;
    int ringSize_ = 1;
    std::array<TimeState, kTimeModes> time_ {};

    // per-effect on/off crossfades
    std::array<float, kNumPerformerFx> gain_ {};

    // pitch shifter: two crossfaded taps on a short delay line
    std::vector<float> pitchBuf_[2];
    int pitchWrite_ = 0;
    double pitchPhase_ = 0.0;
    double pitchWindow_ = 2048.0;

    // filter
    dsp::Svf filter_;
    float filterX_ = 0.5f;
    bool filterSet_ = false;

    // talkbox: three formant band-passes
    std::array<dsp::Svf, 3> formant_ {};
    std::array<float, 3> vowelGain_ {};
    bool vowelSet_ = false;

    // bitcrusher
    float crushHold_[2] { 0.0f, 0.0f };
    int crushCount_ = 0;

    // delay (ping-pong)
    std::vector<float> delayBuf_[2];
    int delayWrite_ = 0;
    double delaySamples_ = 0.0;
    float delayLp_[2] { 0.0f, 0.0f };
    float delayEnergy_ = 0.0f;

    // reverb (send)
    dsp::Reverb reverb_;
    std::vector<float> wetL_, wetR_;
    float reverbEnergy_ = 0.0f;
};

} // namespace dj
