// RBJ "Audio EQ Cookbook" biquads (Robert Bristow-Johnson, public text) and a Linkwitz-Riley 4th-order
// crossover built from two cascaded Butterworth sections. In-house implementation, stereo, TDF-II.
#pragma once
#include "../Common.h"

namespace dj::dsp
{

class Biquad
{
public:
    enum class Type { LowPass, HighPass, BandPass, Notch, Peak, LowShelf, HighShelf, AllPass };

    void set (Type type, double sampleRate, double freq, double q, double gainDb = 0.0) noexcept
    {
        freq = std::clamp (freq, 10.0, sampleRate * 0.49);
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * freq / sampleRate;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * std::max (q, 1.0e-3));
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (type)
        {
            case Type::LowPass:  b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
            case Type::HighPass: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
            case Type::BandPass: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
            case Type::Notch:    b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
            case Type::AllPass:  b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
            case Type::Peak:
                b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
            case Type::LowShelf:
            {
                const double sq = 2 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) - (A - 1) * cw + sq); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sq);
                a0 = (A + 1) + (A - 1) * cw + sq; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sq; break;
            }
            case Type::HighShelf:
            {
                const double sq = 2 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * cw + sq); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sq);
                a0 = (A + 1) - (A - 1) * cw + sq; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sq; break;
            }
        }
        b0_ = (float) (b0 / a0); b1_ = (float) (b1 / a0); b2_ = (float) (b2 / a0);
        a1_ = (float) (a1 / a0); a2_ = (float) (a2 / a0);
    }

    void reset() noexcept { z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0.0f; }

    float process (int ch, float x) noexcept
    {
        const float y = b0_ * x + z1_[ch];
        z1_[ch] = b1_ * x - a1_ * y + z2_[ch];
        z2_[ch] = b2_ * x - a2_ * y;
        return y;
    }

private:
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float z1_[2] { 0, 0 }, z2_[2] { 0, 0 };
};

// Linkwitz-Riley 24 dB/oct low/high pair. low + high is an all-pass (flat magnitude).
class LR4
{
public:
    void set (double sampleRate, double freq) noexcept
    {
        constexpr double q = 0.70710678118654752;
        for (auto& b : lp_) b.set (Biquad::Type::LowPass, sampleRate, freq, q);
        for (auto& b : hp_) b.set (Biquad::Type::HighPass, sampleRate, freq, q);
    }
    void reset() noexcept { for (auto& b : lp_) b.reset(); for (auto& b : hp_) b.reset(); }
    void split (int ch, float x, float& low, float& high) noexcept
    {
        low = lp_[1].process (ch, lp_[0].process (ch, x));
        high = hp_[1].process (ch, hp_[0].process (ch, x));
    }
private:
    Biquad lp_[2], hp_[2];
};

} // namespace dj::dsp
