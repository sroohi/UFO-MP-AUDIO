// State-variable filter (Andrew Simper / Cytomic, "Solving the continuous SVF equations", 2013 — public paper).
#pragma once
#include <cmath>
#include <algorithm>

namespace dj::dsp
{

class Svf
{
public:
    enum class Mode { LowPass, BandPass, HighPass };

    void prepare (double sampleRate) noexcept { sr_ = sampleRate; reset(); update(); }
    void reset() noexcept { ic1_[0] = ic2_[0] = ic1_[1] = ic2_[1] = 0.0f; }

    void set (float cutoffHz, float resonance, Mode mode) noexcept
    {
        cutoff_ = std::clamp (cutoffHz, 20.0f, (float) (sr_ * 0.45));
        q_ = std::clamp (resonance, 0.5f, 20.0f);
        mode_ = mode;
        update();
    }

    float process (int ch, float v0) noexcept
    {
        float& ic1 = ic1_[ch];
        float& ic2 = ic2_[ch];
        const float v3 = v0 - ic2;
        const float v1 = a1_ * ic1 + a2_ * v3;
        const float v2 = ic2 + a2_ * ic1 + a3_ * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        switch (mode_)
        {
            case Mode::LowPass:  return v2;
            case Mode::BandPass: return v1;
            case Mode::HighPass: return v0 - k_ * v1 - v2;
        }
        return v2;
    }

private:
    void update() noexcept
    {
        const float g = std::tan (3.14159265f * cutoff_ / (float) sr_);
        k_ = 1.0f / q_;
        a1_ = 1.0f / (1.0f + g * (g + k_));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    double sr_ = 48000.0;
    float cutoff_ = 1000.0f, q_ = 0.707f;
    Mode mode_ = Mode::LowPass;
    float a1_ = 0, a2_ = 0, a3_ = 0, k_ = 1;
    float ic1_[2] { 0, 0 }, ic2_[2] { 0, 0 };
};

} // namespace dj::dsp
