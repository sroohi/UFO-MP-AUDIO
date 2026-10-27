// Feed-forward compressor, log-domain peak detector with decoupled attack/release
// (Giannoulis, Massberg & Reiss, "Digital Dynamic Range Compressor Design", JAES 2012).
#pragma once
#include <cmath>
#include <algorithm>

namespace dj::dsp
{

class Compressor
{
public:
    void prepare (double sampleRate) noexcept { sr_ = sampleRate; env_ = 0.0f; update(); }
    void set (float thresholdDb, float ratio, float attackMs, float releaseMs, float makeupDb, float kneeDb = 6.0f) noexcept
    {
        thr_ = thresholdDb; ratio_ = std::max (1.0f, ratio); att_ = std::max (0.01f, attackMs); rel_ = std::max (1.0f, releaseMs);
        makeup_ = makeupDb; knee_ = std::max (0.0f, kneeDb);
        update();
    }
    // processes a stereo pair with a linked (max) detector; `scL/scR` optional sidechain
    void process (float* l, float* r, int n, const float* scL = nullptr, const float* scR = nullptr) noexcept
    {
        const float mk = dbToGain (makeup_);
        for (int i = 0; i < n; ++i)
        {
            const float dl = scL != nullptr ? scL[i] : l[i];
            const float dr = scR != nullptr ? scR[i] : r[i];
            const float x = std::max (std::abs (dl), std::abs (dr));
            const float xDb = 20.0f * std::log10 (std::max (x, 1e-9f));
            const float over = xDb - thr_;
            float yDb;
            if (2.0f * over < -knee_)      yDb = xDb;
            else if (2.0f * over > knee_)  yDb = thr_ + over / ratio_;
            else { const float t = over + knee_ * 0.5f; yDb = xDb + (1.0f / ratio_ - 1.0f) * t * t / (2.0f * knee_); }
            const float gr = xDb - yDb; // positive dB of reduction
            env_ = gr > env_ ? aAtt_ * env_ + (1.0f - aAtt_) * gr : aRel_ * env_ + (1.0f - aRel_) * gr;
            const float g = dbToGain (-env_) * mk;
            l[i] *= g; r[i] *= g;
        }
    }
    float gainReductionDb() const noexcept { return env_; }

private:
    static float dbToGain (float db) noexcept { return std::pow (10.0f, db / 20.0f); }
    void update() noexcept
    {
        // alpha = exp(-ln 9 / (Fs * T)) so the detector covers 10%..90% in T
        aAtt_ = std::exp (-std::log (9.0f) / ((float) sr_ * att_ * 0.001f));
        aRel_ = std::exp (-std::log (9.0f) / ((float) sr_ * rel_ * 0.001f));
    }
    double sr_ = 48000.0;
    float thr_ = -18.0f, ratio_ = 4.0f, att_ = 10.0f, rel_ = 100.0f, makeup_ = 0.0f, knee_ = 6.0f;
    float aAtt_ = 0.0f, aRel_ = 0.0f, env_ = 0.0f;
};

} // namespace dj::dsp
