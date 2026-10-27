// DJ effects shared by the FX units, Pad FX, Stem FX and Sound Color FX. All in-house.
//
// Every effect is created and prepared on the message thread, then handed to the audio thread through
// a command; process() never allocates. Two families:
//   * send effects (echo, reverb...) produce a wet signal from what is fed into them, so the slot can
//     close the input and let the tail ring out after the effect is switched off (echo-out);
//   * insert effects transform the signal in place; the slot crossfades dry/processed, or for
//     "parametric" effects (filters, crush) lets the amount drive the effect itself.
#pragma once
#include "Common.h"
#include "dsp/Biquad.h"
#include "dsp/Reverb.h"
#include <memory>
#include <vector>

namespace dj
{

enum class FxType : int
{
    None = 0,
    Echo, PingPong, TapeEcho, Reverb,
    Flanger, Phaser,
    HighPass, LowPass, FilterLfo,
    Bitcrusher, Distortion,
    Gate, Roll, ReverseRoll, Braker,
    PitchShift, Noise,
    // DDJ-FLX10 BEAT FX models (manual pp.92-101), in the order of the BEAT FX SELECT knob
    LowCutEcho, MultiTapDelay, Spiral, EnigmaJet, Stretch, SlipRoll, MobiusSaw, MobiusTri,
    Count
};

// What the BEAT </> buttons change for an effect (FLX10 manual "BEAT </> range" column).
enum class BeatKind : int
{
    Time16,     // 1/16 .. 16 beats
    Time64,     // 1/16 .. 64 beats (modulation)
    Signed64,   // -64 .. -1/16, +1/16 .. +64 (direction + rate)
    Size        // 1 .. 100 % (reverb size)
};

const char* fxName (FxType t) noexcept;
BeatKind fxBeatKind (FxType t) noexcept;
// The 14 effects of the FLX10 BEAT FX SELECT knob, in knob order (MIDI notes 0x20..0x2D on channel 5).
FxType flx10BeatFx (int position) noexcept;
constexpr int kFlx10BeatFxCount = 14;
// Up to three user parameters per effect (Single FX mode knobs). Empty names are unused.
const char* fxParamName (FxType t, int index) noexcept;
float fxParamDefault (FxType t, int index) noexcept;

struct FxContext
{
    double bpm = 120.0;
    float beats = 1.0f;         // tempo multiplier (Serato "beats"); negative = falling (Signed64 effects)
    float amount = 1.0f;        // 0..1, wet level or intensity
    float params[3] { 0.5f, 0.5f, 0.5f };
};

class Effect
{
public:
    explicit Effect (FxType t) : type_ (t) {}
    virtual ~Effect() = default;

    FxType type() const noexcept { return type_; }
    virtual void prepare (double sampleRate, int maxBlock) = 0;
    virtual void reset() noexcept = 0;
    // In place. Send effects receive the send signal and must return wet-only audio.
    virtual void process (float* l, float* r, int n, const FxContext& ctx) noexcept = 0;
    virtual bool isSend() const noexcept { return false; }
    // Parametric inserts: the slot runs them fully wet while on, and `amount` drives the effect.
    virtual bool amountIsParameter() const noexcept { return false; }
    // Called on the audio thread when the slot switches on (capture effects start here).
    virtual void onActivate() noexcept {}

protected:
    FxType type_;
};

std::unique_ptr<Effect> makeEffect (FxType type);

// Runs one effect with click-free on/off: send effects keep their tail after switching off (the input is
// gated, the wet output continues until it decays), inserts crossfade dry <-> processed. The effect is
// owned elsewhere; the runner only borrows it.
class FxRunner
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr_ = sampleRate;
        wl_.assign ((std::size_t) maxBlock, 0.0f);
        wr_.assign ((std::size_t) maxBlock, 0.0f);
        mix_.prepare (sampleRate, 8.0f);
        mix_.snap (0.0f);
    }
    void setEffect (Effect* fx) noexcept { fx_ = fx; running_ = false; on_ = false; mix_.snap (0.0f); silent_ = 0; }
    Effect* effect() const noexcept { return fx_; }
    void setOn (bool on) noexcept
    {
        if (fx_ == nullptr) { on_ = false; return; }
        if (on && ! on_)
        {
            if (! running_) fx_->reset();
            fx_->onActivate();
            running_ = true;
            silent_ = 0;
        }
        on_ = on;
    }
    bool on() const noexcept { return on_; }
    bool running() const noexcept { return running_; }

    void process (float* l, float* r, int n, const FxContext& ctx) noexcept
    {
        if (fx_ == nullptr || ! running_ || n <= 0) return;
        n = std::min (n, (int) wl_.size());
        mix_.target = on_ ? 1.0f : 0.0f;
        float* wl = wl_.data();
        float* wr = wr_.data();
        if (fx_->isSend())
        {
            for (int i = 0; i < n; ++i) { const float g = mix_.next(); wl[i] = l[i] * g; wr[i] = r[i] * g; }
            fx_->process (wl, wr, n, ctx);
            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                l[i] += wl[i] * ctx.amount; r[i] += wr[i] * ctx.amount;
                peak = std::max (peak, std::max (std::abs (wl[i]), std::abs (wr[i])));
            }
            if (! on_ && mix_.value < 1.0e-4f)
            {
                silent_ = peak < 1.0e-5f ? silent_ + n : 0;
                if (silent_ > (int) (sr_ * 0.5)) stop();
            }
            return;
        }
        std::copy (l, l + n, wl);
        std::copy (r, r + n, wr);
        fx_->process (wl, wr, n, ctx);
        const float wet = fx_->amountIsParameter() ? 1.0f : std::clamp (ctx.amount, 0.0f, 1.0f);
        for (int i = 0; i < n; ++i)
        {
            const float g = mix_.next() * wet;
            l[i] += g * (wl[i] - l[i]);
            r[i] += g * (wr[i] - r[i]);
        }
        if (! on_ && mix_.value < 1.0e-4f) stop();
    }

private:
    void stop() noexcept { running_ = false; silent_ = 0; mix_.snap (0.0f); fx_->reset(); }

    Effect* fx_ = nullptr;
    bool on_ = false, running_ = false;
    Smoothed mix_;
    int silent_ = 0;
    double sr_ = 48000.0;
    std::vector<float> wl_, wr_;
};

// Fractional stereo delay line with preallocated storage.
class DelayLine
{
public:
    void prepare (std::size_t maxFrames) { buf_[0].assign (maxFrames + 4, 0.0f); buf_[1].assign (maxFrames + 4, 0.0f); w_ = 0; }
    void reset() noexcept { std::fill (buf_[0].begin(), buf_[0].end(), 0.0f); std::fill (buf_[1].begin(), buf_[1].end(), 0.0f); w_ = 0; }
    std::size_t size() const noexcept { return buf_[0].size(); }
    void write (float l, float r) noexcept
    {
        buf_[0][w_] = l; buf_[1][w_] = r;
        if (++w_ >= buf_[0].size()) w_ = 0;
    }
    // Sample `delay` frames behind the most recently written frame (delay >= 1).
    float read (int ch, float delay) const noexcept
    {
        const auto size = (float) buf_[0].size();
        float p = (float) w_ - delay;
        while (p < 0.0f) p += size;
        auto i0 = (std::size_t) p;
        const float frac = p - (float) i0;
        if (i0 >= buf_[0].size()) i0 -= buf_[0].size();
        const std::size_t i1 = i0 + 1 < buf_[0].size() ? i0 + 1 : 0;
        return buf_[ch][i0] + frac * (buf_[ch][i1] - buf_[ch][i0]);
    }
private:
    std::vector<float> buf_[2];
    std::size_t w_ = 0;
};

} // namespace dj
