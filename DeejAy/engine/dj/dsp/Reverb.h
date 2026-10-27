// Freeverb (Jezar at Dreampoint, public domain) — 8 parallel combs + 4 series allpasses per channel.
#pragma once
#include <vector>
#include <array>
#include <algorithm>

namespace dj::dsp
{

class Reverb
{
public:
    void prepare (double sampleRate)
    {
        const double scale = sampleRate / 44100.0;
        static constexpr int combTunings[8] { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        static constexpr int apTunings[4] { 556, 441, 341, 225 };
        for (int ch = 0; ch < 2; ++ch)
        {
            const int spread = ch == 0 ? 0 : 23;
            for (std::size_t i = 0; i < 8; ++i) combs_[ch][i].resize ((std::size_t) ((combTunings[i] + spread) * scale));
            for (std::size_t i = 0; i < 4; ++i) aps_[ch][i].resize ((std::size_t) ((apTunings[i] + spread) * scale));
        }
        reset();
    }
    void reset() noexcept
    {
        for (auto& c : combs_) for (auto& x : c) x.clear();
        for (auto& a : aps_) for (auto& x : a) x.clear();
    }
    void set (float roomSize, float damping, float mix) noexcept
    {
        feedback_ = 0.7f + std::clamp (roomSize, 0.0f, 1.0f) * 0.28f;
        damp_ = std::clamp (damping, 0.0f, 1.0f) * 0.4f;
        mix_ = std::clamp (mix, 0.0f, 1.0f);
    }
    void process (float* l, float* r, int n) noexcept
    {
        float* io[2] { l, r };
        for (int i = 0; i < n; ++i)
        {
            const float input = (l[i] + r[i]) * 0.015f;
            for (int ch = 0; ch < 2; ++ch)
            {
                float out = 0.0f;
                for (auto& c : combs_[ch]) out += c.process (input, feedback_, damp_);
                for (auto& a : aps_[ch]) out = a.process (out);
                io[ch][i] = io[ch][i] * (1.0f - mix_) + out * mix_;
            }
        }
    }

private:
    struct Comb
    {
        std::vector<float> buf; std::size_t idx = 0; float store = 0.0f;
        void resize (std::size_t n) { buf.assign (std::max<std::size_t> (1, n), 0.0f); idx = 0; store = 0; }
        void clear() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); store = 0; }
        float process (float in, float fb, float damp) noexcept
        {
            const float out = buf[idx];
            store = out * (1.0f - damp) + store * damp;
            buf[idx] = in + store * fb;
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };
    struct AllPass
    {
        std::vector<float> buf; std::size_t idx = 0;
        void resize (std::size_t n) { buf.assign (std::max<std::size_t> (1, n), 0.0f); idx = 0; }
        void clear() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); }
        float process (float in) noexcept
        {
            const float b = buf[idx];
            const float out = -in + b;
            buf[idx] = in + b * 0.5f;
            if (++idx >= buf.size()) idx = 0;
            return out;
        }
    };
    std::array<Comb, 8> combs_[2];
    std::array<AllPass, 4> aps_[2];
    float feedback_ = 0.84f, damp_ = 0.2f, mix_ = 0.0f;
};

} // namespace dj::dsp
