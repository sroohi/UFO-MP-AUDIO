#include "Mixer.h"

namespace dj
{

float channelFaderGain (float position, float curve) noexcept
{
    const float p = std::clamp (position, 0.0f, 1.0f);
    const float exponent = 2.6f - 2.3f * std::clamp (curve, 0.0f, 1.0f); // 2.6 (slow) .. 0.3 (fast)
    return std::pow (p, exponent);
}

float crossfaderGain (float distance, float curve, float cutLag) noexcept
{
    const float lag = std::clamp (cutLag, 0.0f, 0.2f);
    const float d = std::clamp (distance / (1.0f - lag), 0.0f, 1.0f);
    const float w = 1.0f - 0.96f * std::clamp (curve, 0.0f, 1.0f); // width of the fade region
    const float t = std::clamp ((d - (1.0f - w)) / w, 0.0f, 1.0f);
    return std::cos (0.5f * (float) kPi * t);
}

float trimGain (float knob) noexcept
{
    const float k = std::clamp (knob, 0.0f, 1.0f);
    if (k <= 0.0005f) return 0.0f;
    if (k >= 0.5f) return dbToGain ((k - 0.5f) * 24.0f);
    return std::pow (2.0f * k, 2.0f); // -inf .. 0 dB
}

float isoGain (float knob) noexcept
{
    const float k = std::clamp (knob, 0.0f, 1.0f);
    if (k >= 0.5f) return 1.0f + (k - 0.5f) * 2.0f * (dbToGain (6.0f) - 1.0f);
    return std::pow (2.0f * k, 2.0f);
}

// ------------------------------------------------------------------------------------- EQ
void ChannelEq::prepare (double sampleRate)
{
    sr_ = sampleRate;
    xLow_.set (sampleRate, 250.0);
    xHigh_.set (sampleRate, 3000.0);
    align_.set (sampleRate, 3000.0);
    for (auto& g : g_) { g.prepare (sampleRate, 10.0f); g.snap (1.0f); }
    for (auto& l : last_) l = -1.0f;
    reset();
}

void ChannelEq::reset() noexcept
{
    low_.reset(); mid_.reset(); high_.reset();
    xLow_.reset(); xHigh_.reset(); align_.reset();
}

void ChannelEq::process (const float* k, EqMode mode, float* l, float* r, int n) noexcept
{
    if (mode != lastMode_) { reset(); lastMode_ = mode; for (auto& x : last_) x = -1.0f; }
    if (mode == EqMode::Classic)
    {
        // DJM-style: HI shelf 13 kHz, MID peak 1 kHz, LOW shelf 70 Hz; -26 dB fully CCW, +6 dB fully CW
        auto db = [] (float knob) { return knob >= 0.5f ? (knob - 0.5f) * 12.0f : -26.0f * (1.0f - 2.0f * knob); };
        bool flat = true;
        for (int b = 0; b < 3; ++b) flat = flat && std::abs (k[b] - 0.5f) < 0.002f;
        if (flat && last_[0] >= 0.0f && std::abs (last_[0] - 0.5f) < 0.002f && std::abs (last_[1] - 0.5f) < 0.002f && std::abs (last_[2] - 0.5f) < 0.002f)
            return;
        if (k[0] != last_[0]) high_.set (dsp::Biquad::Type::HighShelf, sr_, 13000.0, 0.6, db (k[0]));
        if (k[1] != last_[1]) mid_.set (dsp::Biquad::Type::Peak, sr_, 1000.0, 0.55, db (k[1]));
        if (k[2] != last_[2]) low_.set (dsp::Biquad::Type::LowShelf, sr_, 70.0, 0.6, db (k[2]));
        for (int b = 0; b < 3; ++b) last_[b] = k[b];
        for (int i = 0; i < n; ++i)
        {
            l[i] = high_.process (0, mid_.process (0, low_.process (0, l[i])));
            r[i] = high_.process (1, mid_.process (1, low_.process (1, r[i])));
        }
        return;
    }
    // Isolator: three Linkwitz-Riley bands summed back with gains; full kill at CCW
    for (int b = 0; b < 3; ++b) g_[b].target = isoGain (k[b]);
    for (int i = 0; i < n; ++i)
    {
        const float gh = g_[0].next(), gm = g_[1].next(), gl = g_[2].next();
        float io[2] { l[i], r[i] };
        for (int c = 0; c < 2; ++c)
        {
            float lo, rest, mi, hi, loA, loB;
            xLow_.split (c, io[c], lo, rest);
            xHigh_.split (c, rest, mi, hi);
            align_.split (c, lo, loA, loB);         // all-pass the low band like the other two
            io[c] = gl * (loA + loB) + gm * mi + gh * hi;
        }
        l[i] = io[0]; r[i] = io[1];
    }
}

// -------------------------------------------------------------------------------- limiter
void Limiter::prepare (double sampleRate)
{
    dl_.fill (0.0f); dr_.fill (0.0f); req_.fill (1.0f);
    idx_ = 0;
    gain_ = 1.0f;
    release_ = std::exp (-1.0f / (float) (sampleRate * 0.15));
}

void Limiter::process (float* l, float* r, int n, float ceiling) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float pk = std::max (std::abs (l[i]), std::abs (r[i]));
        req_[(std::size_t) idx_] = pk > ceiling ? ceiling / pk : 1.0f;
        float need = 1.0f;
        for (float q : req_) need = std::min (need, q);
        // attack within the lookahead window, release slowly
        if (need < gain_) gain_ += (need - gain_) * (1.0f / (float) (kLookahead / 2));
        else gain_ = need + (gain_ - need) * release_;
        const float ol = dl_[(std::size_t) idx_], orr = dr_[(std::size_t) idx_];
        dl_[(std::size_t) idx_] = l[i];
        dr_[(std::size_t) idx_] = r[i];
        idx_ = (idx_ + 1) % kLookahead;
        l[i] = std::clamp (ol * gain_, -ceiling, ceiling);
        r[i] = std::clamp (orr * gain_, -ceiling, ceiling);
    }
}

} // namespace dj
