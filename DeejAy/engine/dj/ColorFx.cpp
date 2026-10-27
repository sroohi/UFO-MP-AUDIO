#include "ColorFx.h"

namespace dj
{

namespace
{
    constexpr float kDeadZone = 0.02f;
    inline float shapeKnob (float x) noexcept { return std::abs (x) < kDeadZone ? 0.0f : x; }
}

const char* colorFxName (ColorFxType t) noexcept
{
    switch (t)
    {
        case ColorFxType::Space: return "SPACE";
        case ColorFxType::DubEcho: return "DUB ECHO";
        case ColorFxType::Crush: return "CRUSH";
        case ColorFxType::Pitch: return "PITCH";
        case ColorFxType::Noise: return "NOISE";
        case ColorFxType::Filter: return "FILTER";
        case ColorFxType::None: break;
    }
    return "OFF";
}

void ColorFx::prepare (double sampleRate, int maxBlock)
{
    sr_ = sampleRate;
    pitch_ = makeEffect (FxType::PitchShift);
    pitch_->prepare (sampleRate, maxBlock);
    wl_.assign ((std::size_t) maxBlock, 0.0f);
    wr_.assign ((std::size_t) maxBlock, 0.0f);
    reverb_.prepare (sampleRate);
    reverb_.set (0.85f, 0.35f, 1.0f);
    echo_.prepare ((std::size_t) (sampleRate * 2.5));
    crushHp_.set (dsp::Biquad::Type::HighPass, sampleRate, 20.0, 0.7);
    reset();
}

void ColorFx::reset() noexcept
{
    for (auto& f : filt_) f.reset();
    crushHp_.reset();
    noiseF_.reset();
    spaceTone_.reset();
    for (auto& f : echoTone_) f.reset();
    reverb_.reset();
    echo_.reset();
    if (pitch_ != nullptr) pitch_->reset();
    knob_ = 0.0f;
    lastFilterX_ = lastCrushX_ = lastNoiseX_ = 99.0f;
    echoTime_ = -1.0f;
    spaceTail_ = echoTail_ = 0;
}

void ColorFx::updateFilter (float x) noexcept
{
    if (std::abs (x - lastFilterX_) < 1.0e-4f) return;
    lastFilterX_ = x;
    // CCW: low-pass sweeping down to 100 Hz; CW: high-pass sweeping up to 9 kHz
    for (auto& f : filt_)
    {
        if (x < 0.0f) f.set (dsp::Biquad::Type::LowPass, sr_, 20000.0 * std::pow (100.0 / 20000.0, (double) -x), 1.1);
        else f.set (dsp::Biquad::Type::HighPass, sr_, 20.0 * std::pow (9000.0 / 20.0, (double) x), 1.1);
    }
}

void ColorFx::processPreEq (ColorFxType type, float knob, float* l, float* r, int n) noexcept
{
    if (type != ColorFxType::Noise) return;
    const float x = shapeKnob (knob);
    knob_ = x;
    if (x == 0.0f) return;
    if (std::abs (x - lastNoiseX_) > 1.0e-3f)
    {
        lastNoiseX_ = x;
        if (x < 0.0f) noiseF_.set (dsp::Biquad::Type::LowPass, sr_, 20000.0 * std::pow (300.0 / 20000.0, (double) -x), 0.9);
        else noiseF_.set (dsp::Biquad::Type::HighPass, sr_, 20.0 * std::pow (8000.0 / 20.0, (double) x), 0.9);
    }
    const float level = 0.3f * std::pow (std::abs (x), 1.5f);
    for (int i = 0; i < n; ++i)
    {
        seed_ = seed_ * 1664525u + 1013904223u;
        const float w = (float) (seed_ >> 8) / 8388608.0f - 1.0f;
        const float nz = noiseF_.process (0, w) * level;
        l[i] += nz; r[i] += nz;
    }
}

void ColorFx::processPostEq (ColorFxType type, float knob, float* l, float* r, int n) noexcept
{
    const float x = shapeKnob (knob);
    if (type == ColorFxType::Filter)
    {
        if (x == 0.0f && std::abs (lastFilterX_) < kDeadZone) return;
        updateFilter (x);
        // within 5 % of centre, crossfade so the deadzone edge does not click
        const float wet = std::min (1.0f, std::abs (x) / 0.05f);
        for (int i = 0; i < n; ++i)
        {
            const float fl = filt_[1].process (0, filt_[0].process (0, l[i]));
            const float fr = filt_[1].process (1, filt_[0].process (1, r[i]));
            l[i] += wet * (fl - l[i]);
            r[i] += wet * (fr - r[i]);
        }
        return;
    }
    if (type == ColorFxType::Crush)
    {
        if (x == 0.0f) return;
        const float a = std::abs (x);
        const float bits = 16.0f - 12.0f * a;
        const float levels = std::pow (2.0f, bits - 1.0f);
        const float step = 1.0f + 15.0f * a * a;
        if (x > 0.0f && std::abs (x - lastCrushX_) > 1.0e-3f)
        {
            lastCrushX_ = x;
            crushHp_.set (dsp::Biquad::Type::HighPass, sr_, 20.0 * std::pow (5000.0 / 20.0, (double) x), 0.8);
        }
        for (int i = 0; i < n; ++i)
        {
            crushCount_ += 1.0f;
            if (crushCount_ >= step)
            {
                crushCount_ -= step;
                crushHold_[0] = std::round (l[i] * levels) / levels;
                crushHold_[1] = std::round (r[i] * levels) / levels;
            }
            float cl = crushHold_[0], cr = crushHold_[1];
            if (x > 0.0f) { cl = crushHp_.process (0, cl); cr = crushHp_.process (1, cr); }
            const float wet = std::min (1.0f, a / 0.05f);
            l[i] += wet * (cl - l[i]);
            r[i] += wet * (cr - r[i]);
        }
        return;
    }
    if (type == ColorFxType::Pitch)
    {
        if (x == 0.0f) { pitch_->reset(); return; }
        const int m = std::min (n, (int) wl_.size());
        std::copy (l, l + m, wl_.data());
        std::copy (r, r + m, wr_.data());
        FxContext ctx;
        ctx.amount = 1.0f;
        ctx.params[0] = 0.5f + 0.5f * x;     // +/- 12 semitones at the ends
        pitch_->process (wl_.data(), wr_.data(), m, ctx);
        const float wet = std::min (1.0f, std::abs (x) / 0.08f);
        for (int i = 0; i < m; ++i)
        {
            l[i] += wet * (wl_[(std::size_t) i] - l[i]);
            r[i] += wet * (wr_[(std::size_t) i] - r[i]);
        }
    }
}

void ColorFx::processPostFader (ColorFxType type, float knob, double bpm, float* l, float* r, int n) noexcept
{
    const float x = (type == ColorFxType::Space || type == ColorFxType::DubEcho) ? shapeKnob (knob) : 0.0f;
    const int m = std::min (n, (int) wl_.size());

    // SPACE: reverb send; CCW keeps mids + lows of the reverb, CW mids + highs
    if (type == ColorFxType::Space || spaceTail_ > 0)
    {
        const float send = type == ColorFxType::Space ? std::abs (x) : 0.0f;
        if (send > 0.0f) { spaceTail_ = (int) (sr_ * 4.0); lastSpaceSign_ = x < 0.0f ? -1.0f : 1.0f; }
        if (send > 0.0f || spaceTail_ > 0)
        {
            spaceTone_.set (lastSpaceSign_ < 0.0f ? dsp::Biquad::Type::LowPass : dsp::Biquad::Type::HighPass,
                            sr_, lastSpaceSign_ < 0.0f ? 2500.0 : 600.0, 0.7);
            for (int i = 0; i < m; ++i) { wl_[(std::size_t) i] = l[i] * send; wr_[(std::size_t) i] = r[i] * send; }
            reverb_.process (wl_.data(), wr_.data(), m);
            for (int i = 0; i < m; ++i)
            {
                l[i] += 2.2f * spaceTone_.process (0, wl_[(std::size_t) i]);
                r[i] += 2.2f * spaceTone_.process (1, wr_[(std::size_t) i]);
            }
            if (send == 0.0f) spaceTail_ = std::max (0, spaceTail_ - m);
        }
    }

    // DUB ECHO: 3/4-beat echo with heavy feedback; CCW = echo on the mids, CW = echo on the highs
    if (type == ColorFxType::DubEcho || echoTail_ > 0)
    {
        const float send = type == ColorFxType::DubEcho ? std::abs (x) : 0.0f;
        if (send > 0.0f) { echoTail_ = (int) (sr_ * 6.0); lastEchoSign_ = x < 0.0f ? -1.0f : 1.0f; }
        if (send > 0.0f || echoTail_ > 0)
        {
            const double beat = 60.0 / (bpm > 20.0 ? bpm : 120.0);
            const float target = (float) std::min (beat * 0.75 * sr_, (double) echo_.size() - 8.0);
            if (echoTime_ < 0.0f) echoTime_ = target;
            for (auto& f : echoTone_)
                f.set (lastEchoSign_ < 0.0f ? dsp::Biquad::Type::BandPass : dsp::Biquad::Type::HighPass,
                       sr_, lastEchoSign_ < 0.0f ? 1000.0 : 2000.0, 0.7);
            for (int i = 0; i < m; ++i)
            {
                echoTime_ += 0.0005f * (target - echoTime_);
                const float dl = echo_.read (0, echoTime_), dr = echo_.read (1, echoTime_);
                const float tl = echoTone_[0].process (0, dl), tr = echoTone_[1].process (1, dr);
                echo_.write (l[i] * send + 0.62f * tl, r[i] * send + 0.62f * tr);
                l[i] += 1.4f * tl;
                r[i] += 1.4f * tr;
            }
            if (send == 0.0f) echoTail_ = std::max (0, echoTail_ - m);
        }
    }
}

} // namespace dj
