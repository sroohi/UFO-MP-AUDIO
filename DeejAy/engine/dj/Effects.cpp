#include "Effects.h"

namespace dj
{

namespace
{
    constexpr double kMaxDelaySeconds = 8.0; // 16 beats at 120 BPM

    float beatSeconds (const FxContext& c) noexcept
    {
        const double bpm = c.bpm > 20.0 ? c.bpm : 120.0;
        return (float) (60.0 / bpm) * std::max (std::abs (c.beats), 1.0f / 64.0f);
    }

    // ---------------------------------------------------------------------------------------- echo
    class Echo final : public Effect
    {
    public:
        Echo() : Effect (FxType::Echo) {}
        void prepare (double sr, int) override { sr_ = sr; line_.prepare ((std::size_t) (sr * kMaxDelaySeconds)); reset(); }
        void reset() noexcept override { line_.reset(); lp_[0] = lp_[1] = 0.0f; time_ = -1.0f; }
        bool isSend() const noexcept override { return true; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float target = std::clamp (beatSeconds (c) * (float) sr_, 1.0f, (float) line_.size() - 4.0f);
            if (time_ < 0.0f) time_ = target;
            const float fb = 0.2f + 0.75f * c.params[0];
            const float damp = 0.05f + 0.6f * c.params[1];
            for (int i = 0; i < n; ++i)
            {
                time_ += 0.0005f * (target - time_);
                const float dl = line_.read (0, time_), dr = line_.read (1, time_);
                lp_[0] += (1.0f - damp) * (dl - lp_[0]);
                lp_[1] += (1.0f - damp) * (dr - lp_[1]);
                line_.write (l[i] + fb * lp_[0], r[i] + fb * lp_[1]);
                l[i] = dl; r[i] = dr;
            }
        }
    private:
        double sr_ = 48000.0;
        DelayLine line_;
        float lp_[2] { 0, 0 }, time_ = -1.0f;
    };

    class PingPong final : public Effect
    {
    public:
        PingPong() : Effect (FxType::PingPong) {}
        void prepare (double sr, int) override { sr_ = sr; line_.prepare ((std::size_t) (sr * kMaxDelaySeconds)); reset(); }
        void reset() noexcept override { line_.reset(); time_ = -1.0f; }
        bool isSend() const noexcept override { return true; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float target = std::clamp (beatSeconds (c) * (float) sr_, 1.0f, (float) line_.size() - 4.0f);
            if (time_ < 0.0f) time_ = target;
            const float fb = 0.2f + 0.75f * c.params[0];
            for (int i = 0; i < n; ++i)
            {
                time_ += 0.0005f * (target - time_);
                const float dl = line_.read (0, time_), dr = line_.read (1, time_);
                // left tap feeds right, right tap feeds left; input enters on the left
                line_.write (0.5f * (l[i] + r[i]) + fb * dr, fb * dl);
                l[i] = dl; r[i] = dr;
            }
        }
    private:
        double sr_ = 48000.0;
        DelayLine line_;
        float time_ = -1.0f;
    };

    class TapeEcho final : public Effect
    {
    public:
        TapeEcho() : Effect (FxType::TapeEcho) {}
        void prepare (double sr, int) override { sr_ = sr; line_.prepare ((std::size_t) (sr * kMaxDelaySeconds)); reset(); }
        void reset() noexcept override { line_.reset(); lp_[0] = lp_[1] = 0.0f; phase_ = 0.0; time_ = -1.0f; }
        bool isSend() const noexcept override { return true; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float target = std::clamp (beatSeconds (c) * (float) sr_, 8.0f, (float) line_.size() - 64.0f);
            if (time_ < 0.0f) time_ = target;
            const float fb = 0.3f + 0.68f * c.params[0];
            const float wow = 0.002f + 0.006f * c.params[1];
            const float drive = 1.0f + 3.0f * c.params[2];
            const double inc = 2.0 * kPi * 0.6 / sr_;
            const float damp = 0.55f;
            for (int i = 0; i < n; ++i)
            {
                time_ += 0.0002f * (target - time_); // tape-like glide when the tempo changes
                phase_ += inc; if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
                const float mod = time_ * (1.0f + wow * (float) std::sin (phase_) + 0.3f * wow * (float) std::sin (phase_ * 7.1));
                const float dl = line_.read (0, mod), dr = line_.read (1, mod);
                lp_[0] += (1.0f - damp) * (dl - lp_[0]);
                lp_[1] += (1.0f - damp) * (dr - lp_[1]);
                line_.write (std::tanh (drive * (l[i] + fb * lp_[0])) / drive, std::tanh (drive * (r[i] + fb * lp_[1])) / drive);
                l[i] = dl; r[i] = dr;
            }
        }
    private:
        double sr_ = 48000.0, phase_ = 0.0;
        DelayLine line_;
        float lp_[2] { 0, 0 }, time_ = -1.0f;
    };

    class ReverbFx final : public Effect
    {
    public:
        ReverbFx() : Effect (FxType::Reverb) {}
        void prepare (double sr, int) override { rev_.prepare (sr); }
        void reset() noexcept override { rev_.reset(); }
        bool isSend() const noexcept override { return true; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            rev_.set (0.55f + 0.45f * c.params[0], 0.15f + 0.6f * c.params[1], 1.0f);
            rev_.process (l, r, n);
            for (int i = 0; i < n; ++i) { l[i] *= 2.5f; r[i] *= 2.5f; }
        }
    private:
        dsp::Reverb rev_;
    };

    // ----------------------------------------------------------------------------- modulation
    class Flanger final : public Effect
    {
    public:
        Flanger() : Effect (FxType::Flanger) {}
        void prepare (double sr, int) override { sr_ = sr; line_.prepare ((std::size_t) (sr * 0.03)); reset(); }
        void reset() noexcept override { line_.reset(); phase_ = 0.0; fbState_[0] = fbState_[1] = 0.0f; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            // LFO period = beats (x4: a 1-beat flanger sweeps once per bar, as DJ flangers do)
            const double period = std::max (0.05, (double) beatSeconds (c) * 4.0);
            const double inc = 2.0 * kPi / (period * sr_);
            const float fb = 0.3f + 0.6f * c.params[0];
            const float minD = 0.0005f * (float) sr_, maxD = 0.006f * (float) sr_;
            for (int i = 0; i < n; ++i)
            {
                phase_ += inc; if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
                const float lfo = 0.5f * (1.0f - (float) std::cos (phase_));
                const float d = minD + (maxD - minD) * lfo;
                const float dl = line_.read (0, d), dr = line_.read (1, d);
                line_.write (l[i] + fb * dl, r[i] + fb * dr);
                l[i] = 0.6f * (l[i] + dl); r[i] = 0.6f * (r[i] + dr);
            }
        }
    private:
        double sr_ = 48000.0, phase_ = 0.0;
        DelayLine line_;
        float fbState_[2] { 0, 0 };
    };

    class Phaser final : public Effect
    {
    public:
        Phaser() : Effect (FxType::Phaser) {}
        void prepare (double sr, int) override { sr_ = sr; reset(); }
        void reset() noexcept override { for (auto& s : z_) s[0] = s[1] = 0.0f; fb_[0] = fb_[1] = 0.0f; phase_ = 0.0; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const double period = std::max (0.05, (double) beatSeconds (c) * 4.0);
            const double inc = 2.0 * kPi / (period * sr_);
            const float feedback = 0.2f + 0.6f * c.params[0];
            float* io[2] { l, r };
            for (int i = 0; i < n; ++i)
            {
                phase_ += inc; if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
                const float lfo = 0.5f * (1.0f - (float) std::cos (phase_));
                const float f = 200.0f * std::pow (25.0f, lfo); // 200 Hz .. 5 kHz
                const float t = std::tan ((float) kPi * f / (float) sr_);
                const float a = (t - 1.0f) / (t + 1.0f);
                for (int ch = 0; ch < 2; ++ch)
                {
                    float x = io[ch][i] + feedback * fb_[ch];
                    for (int s = 0; s < 6; ++s)
                    {
                        const float y = a * x + z_[s][ch];
                        z_[s][ch] = x - a * y;
                        x = y;
                    }
                    fb_[ch] = x;
                    io[ch][i] = 0.6f * (io[ch][i] + x);
                }
            }
        }
    private:
        double sr_ = 48000.0, phase_ = 0.0;
        float z_[6][2] {};
        float fb_[2] { 0, 0 };
    };

    // -------------------------------------------------------------------------------- filters
    class SweepFilter final : public Effect
    {
    public:
        explicit SweepFilter (FxType t) : Effect (t) {}
        void prepare (double sr, int) override { sr_ = sr; reset(); }
        void reset() noexcept override { for (auto& b : f_) b.reset(); last_ = -1.0f; }
        bool amountIsParameter() const noexcept override { return true; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float a = std::clamp (c.amount, 0.0f, 1.0f);
            const float q = 0.707f + 4.0f * c.params[0];
            if (std::abs (a - last_) > 1.0e-4f || q != lastQ_)
            {
                const bool hp = type_ == FxType::HighPass;
                const double fc = hp ? 20.0 * std::pow (500.0, (double) a) : 20000.0 * std::pow (1.0 / 250.0, (double) a);
                for (auto& b : f_) b.set (hp ? dsp::Biquad::Type::HighPass : dsp::Biquad::Type::LowPass, sr_, fc, q);
                last_ = a; lastQ_ = q;
            }
            for (int i = 0; i < n; ++i)
            {
                l[i] = f_[1].process (0, f_[0].process (0, l[i]));
                r[i] = f_[1].process (1, f_[0].process (1, r[i]));
            }
        }
    private:
        double sr_ = 48000.0;
        dsp::Biquad f_[2];
        float last_ = -1.0f, lastQ_ = 0.0f;
    };

    class FilterLfo final : public Effect
    {
    public:
        FilterLfo() : Effect (FxType::FilterLfo) {}
        void prepare (double sr, int) override { sr_ = sr; reset(); }
        void reset() noexcept override { f_.reset(); phase_ = 0.0; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const double period = std::max (0.05, (double) beatSeconds (c));
            const double inc = 2.0 * kPi / (period * sr_);
            const float q = 1.0f + 6.0f * c.params[0];
            for (int i = 0; i < n; ++i)
            {
                phase_ += inc; if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
                if ((i & 15) == 0)
                {
                    const float lfo = 0.5f * (1.0f - (float) std::cos (phase_));
                    f_.set (dsp::Biquad::Type::LowPass, sr_, 250.0 * std::pow (32.0, (double) lfo), q);
                }
                l[i] = f_.process (0, l[i]);
                r[i] = f_.process (1, r[i]);
            }
        }
    private:
        double sr_ = 48000.0, phase_ = 0.0;
        dsp::Biquad f_;
    };

    // ---------------------------------------------------------------------------- distortion
    class Bitcrusher final : public Effect
    {
    public:
        Bitcrusher() : Effect (FxType::Bitcrusher) {}
        void prepare (double, int) override { reset(); }
        void reset() noexcept override { hold_[0] = hold_[1] = 0.0f; counter_ = 0.0f; }
        bool amountIsParameter() const noexcept override { return true; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float a = std::clamp (c.amount, 0.0f, 1.0f);
            const float bits = 16.0f - 13.0f * a;
            const float levels = std::pow (2.0f, bits - 1.0f);
            const float step = 1.0f + 23.0f * a * a * (0.25f + c.params[0]); // sample-rate reduction
            for (int i = 0; i < n; ++i)
            {
                counter_ += 1.0f;
                if (counter_ >= step)
                {
                    counter_ -= step;
                    hold_[0] = std::round (l[i] * levels) / levels;
                    hold_[1] = std::round (r[i] * levels) / levels;
                }
                l[i] = hold_[0]; r[i] = hold_[1];
            }
        }
    private:
        float hold_[2] { 0, 0 }, counter_ = 0.0f;
    };

    class Distortion final : public Effect
    {
    public:
        Distortion() : Effect (FxType::Distortion) {}
        void prepare (double sr, int) override { tone_.set (dsp::Biquad::Type::LowPass, sr, 6500.0, 0.7); reset(); }
        void reset() noexcept override { tone_.reset(); }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float drive = 1.0f + 24.0f * c.params[0];
            const float norm = 1.0f / std::tanh (drive);
            for (int i = 0; i < n; ++i)
            {
                l[i] = tone_.process (0, std::tanh (drive * l[i]) * norm * 0.7f);
                r[i] = tone_.process (1, std::tanh (drive * r[i]) * norm * 0.7f);
            }
        }
    private:
        dsp::Biquad tone_;
    };

    // --------------------------------------------------------------------------- rhythmic
    class Gate final : public Effect
    {
    public:
        Gate() : Effect (FxType::Gate) {}
        void prepare (double sr, int) override { sr_ = sr; smooth_.prepare (sr, 2.0f); reset(); }
        void reset() noexcept override { phase_ = 0.0; smooth_.snap (1.0f); }
        void onActivate() noexcept override { phase_ = 0.0; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const double period = std::max (0.01, (double) beatSeconds (c));
            const double inc = 1.0 / (period * sr_);
            const double duty = 0.25 + 0.5 * c.params[0];
            for (int i = 0; i < n; ++i)
            {
                phase_ += inc; if (phase_ >= 1.0) phase_ -= 1.0;
                smooth_.target = phase_ < duty ? 1.0f : 0.0f;
                const float g = smooth_.next();
                l[i] *= g; r[i] *= g;
            }
        }
    private:
        double sr_ = 48000.0, phase_ = 0.0;
        Smoothed smooth_;
    };

    // Captures `beats` of audio when switched on, then repeats it (forwards or backwards).
    class Roll final : public Effect
    {
    public:
        explicit Roll (bool reverse, bool slip = false)
            : Effect (slip ? FxType::SlipRoll : reverse ? FxType::ReverseRoll : FxType::Roll), reverse_ (reverse), slip_ (slip) {}
        void prepare (double sr, int) override
        {
            sr_ = sr;
            for (auto& b : buf_) b.assign ((std::size_t) (sr * kMaxDelaySeconds) + 8, 0.0f);
            reset();
        }
        void reset() noexcept override { captured_ = 0; read_ = 0; len_ = 0; }
        void onActivate() noexcept override { captured_ = 0; read_ = 0; len_ = 0; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            // SLIP ROLL re-records whenever the roll length changes
            if (slip_ && c.beats != lastBeats_) { lastBeats_ = c.beats; onActivate(); }
            if (len_ == 0)
                len_ = std::clamp<std::size_t> ((std::size_t) (beatSeconds (c) * (float) sr_), 64, buf_[0].size() - 1);
            const std::size_t fade = std::min<std::size_t> (64, len_ / 4);
            for (int i = 0; i < n; ++i)
            {
                if (captured_ < len_)
                {
                    buf_[0][captured_] = l[i]; buf_[1][captured_] = r[i];
                    ++captured_;
                    continue; // live while the first pass is captured
                }
                const std::size_t k = reverse_ ? len_ - 1 - read_ : read_;
                float g = 1.0f;
                if (read_ < fade) g = (float) read_ / (float) fade;
                else if (read_ >= len_ - fade) g = (float) (len_ - 1 - read_) / (float) fade;
                l[i] = buf_[0][k] * g; r[i] = buf_[1][k] * g;
                if (++read_ >= len_) read_ = 0;
            }
        }
    private:
        bool reverse_, slip_;
        float lastBeats_ = 0.0f;
        double sr_ = 48000.0;
        std::vector<float> buf_[2];
        std::size_t captured_ = 0, read_ = 0, len_ = 0;
    };

    // Vinyl brake: the incoming signal slows to a stop over `beats`.
    class Braker final : public Effect
    {
    public:
        Braker() : Effect (FxType::Braker) {}
        void prepare (double sr, int) override { sr_ = sr; line_.prepare ((std::size_t) (sr * kMaxDelaySeconds)); reset(); }
        void reset() noexcept override { line_.reset(); lag_ = 1.0f; speed_ = 1.0f; }
        void onActivate() noexcept override { lag_ = 1.0f; speed_ = 1.0f; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float decel = 1.0f / std::max (1.0f, beatSeconds (c) * (float) sr_);
            const float maxLag = (float) line_.size() - 8.0f;
            for (int i = 0; i < n; ++i)
            {
                line_.write (l[i], r[i]);
                speed_ = std::max (0.0f, speed_ - decel);
                lag_ = std::min (maxLag, lag_ + (1.0f - speed_));
                const float g = std::min (1.0f, speed_ * 8.0f); // fade the last 1/8 of the brake
                l[i] = line_.read (0, lag_) * g;
                r[i] = line_.read (1, lag_) * g;
            }
        }
    private:
        double sr_ = 48000.0;
        DelayLine line_;
        float lag_ = 1.0f, speed_ = 1.0f;
    };

    // Two-tap rotating delay pitch shifter (classic "harmonizer" topology).
    class PitchShift final : public Effect
    {
    public:
        PitchShift() : Effect (FxType::PitchShift) {}
        void prepare (double sr, int) override
        {
            sr_ = sr;
            window_ = (float) (0.06 * sr);
            line_.prepare ((std::size_t) (window_ * 2.0f) + 8);
            reset();
        }
        void reset() noexcept override { line_.reset(); phase_ = 0.0f; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float semis = (c.params[0] - 0.5f) * 24.0f * std::clamp (c.amount * 2.0f, 0.0f, 1.0f);
            const float ratio = std::pow (2.0f, semis / 12.0f);
            const float inc = (1.0f - ratio) / window_;
            for (int i = 0; i < n; ++i)
            {
                line_.write (l[i], r[i]);
                phase_ += inc;
                if (phase_ >= 1.0f) phase_ -= 1.0f;
                if (phase_ < 0.0f) phase_ += 1.0f;
                const float p2 = phase_ + 0.5f >= 1.0f ? phase_ - 0.5f : phase_ + 0.5f;
                const float d1 = 2.0f + phase_ * window_, d2 = 2.0f + p2 * window_;
                const float g1 = std::sin ((float) kPi * phase_), g2 = std::sin ((float) kPi * p2);
                l[i] = line_.read (0, d1) * g1 + line_.read (0, d2) * g2;
                r[i] = line_.read (1, d1) * g1 + line_.read (1, d2) * g2;
            }
        }
    private:
        double sr_ = 48000.0;
        float window_ = 2880.0f, phase_ = 0.0f;
        DelayLine line_;
    };

    class Noise final : public Effect
    {
    public:
        Noise() : Effect (FxType::Noise) {}
        void prepare (double sr, int) override { sr_ = sr; reset(); }
        void reset() noexcept override { f_.reset(); seed_ = 0x12345678u; last_ = -1.0f; }
        bool amountIsParameter() const noexcept override { return true; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float a = std::clamp (c.amount, 0.0f, 1.0f);
            if (std::abs (c.params[0] - last_) > 1.0e-4f)
            {
                last_ = c.params[0];
                f_.set (dsp::Biquad::Type::BandPass, sr_, 300.0 * std::pow (40.0, (double) last_), 0.8);
            }
            const float level = 0.35f * a * a;
            for (int i = 0; i < n; ++i)
            {
                seed_ = seed_ * 1664525u + 1013904223u;
                const float w = (float) (seed_ >> 8) / 8388608.0f - 1.0f;
                const float nz = f_.process (0, w) * level;
                l[i] += nz; r[i] += nz;
            }
        }
    private:
        double sr_ = 48000.0;
        dsp::Biquad f_;
        std::uint32_t seed_ = 0x12345678u;
        float last_ = -1.0f;
    };
    // ------------------------------------------------------------------- FLX10 BEAT FX models
    // LOW CUT ECHO: echo whose repeats are high-passed, so the kick does not build up.
    class LowCutEcho final : public Effect
    {
    public:
        LowCutEcho() : Effect (FxType::LowCutEcho) {}
        void prepare (double sr, int) override
        {
            sr_ = sr; line_.prepare ((std::size_t) (sr * kMaxDelaySeconds));
            for (auto& h : hp_) h.set (dsp::Biquad::Type::HighPass, sr, 350.0, 0.7);
            reset();
        }
        void reset() noexcept override { line_.reset(); for (auto& h : hp_) h.reset(); time_ = -1.0f; }
        bool isSend() const noexcept override { return true; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float target = std::clamp (beatSeconds (c) * (float) sr_, 1.0f, (float) line_.size() - 4.0f);
            if (time_ < 0.0f) time_ = target;
            const float fb = 0.35f + 0.55f * c.params[0];
            for (int i = 0; i < n; ++i)
            {
                time_ += 0.0005f * (target - time_);
                const float dl = hp_[1].process (0, hp_[0].process (0, line_.read (0, time_)));
                const float dr = hp_[1].process (1, hp_[0].process (1, line_.read (1, time_)));
                line_.write (l[i] + fb * dl, r[i] + fb * dr);
                l[i] = dl; r[i] = dr;
            }
        }
    private:
        double sr_ = 48000.0;
        DelayLine line_;
        dsp::Biquad hp_[2];
        float time_ = -1.0f;
    };

    // MULTI TAP DELAY: up to 7 repeats spaced in 1/8-of-the-beat-length units, decaying.
    class MultiTapDelay final : public Effect
    {
    public:
        MultiTapDelay() : Effect (FxType::MultiTapDelay) {}
        void prepare (double sr, int) override { sr_ = sr; line_.prepare ((std::size_t) (sr * kMaxDelaySeconds)); reset(); }
        void reset() noexcept override { line_.reset(); spacing_ = -1.0f; }
        bool isSend() const noexcept override { return true; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float target = std::clamp (beatSeconds (c) * (float) sr_ / 8.0f, 1.0f, ((float) line_.size() - 8.0f) / 7.0f);
            if (spacing_ < 0.0f) spacing_ = target;
            for (int i = 0; i < n; ++i)
            {
                spacing_ += 0.0005f * (target - spacing_);
                line_.write (l[i], r[i]);
                float wl = 0.0f, wr = 0.0f, g = 1.0f;
                for (int t = 1; t <= 7; ++t)
                {
                    wl += g * line_.read (0, spacing_ * (float) t);
                    wr += g * line_.read (1, spacing_ * (float) t);
                    g *= 0.78f;
                }
                l[i] = wl * 0.45f; r[i] = wr * 0.45f;
            }
        }
    private:
        double sr_ = 48000.0;
        DelayLine line_;
        float spacing_ = -1.0f;
    };

    // SPIRAL: diffused echo whose delay time glides when BEAT changes (the pitch bends with it);
    // LEVEL/DEPTH raises both the wet level and the feedback.
    class Spiral final : public Effect
    {
    public:
        Spiral() : Effect (FxType::Spiral) {}
        void prepare (double sr, int) override
        {
            sr_ = sr; line_.prepare ((std::size_t) (sr * kMaxDelaySeconds));
            static constexpr double apMs[3] { 4.7, 8.9, 13.1 };
            for (int k = 0; k < 3; ++k) { ap_[k].prepare ((std::size_t) (sr * 0.02)); apLen_[k] = (float) (sr * apMs[k] * 0.001); }
            reset();
        }
        void reset() noexcept override { line_.reset(); for (auto& a : ap_) a.reset(); time_ = -1.0f; }
        bool isSend() const noexcept override { return true; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const float target = std::clamp (beatSeconds (c) * (float) sr_, 16.0f, (float) line_.size() - 4.0f);
            if (time_ < 0.0f) time_ = target;
            const float fb = 0.45f + 0.5f * std::clamp (c.amount, 0.0f, 1.0f);
            for (int i = 0; i < n; ++i)
            {
                time_ += 0.00004f * (target - time_); // slow glide: audible pitch bend on BEAT changes
                float dl = line_.read (0, time_), dr = line_.read (1, time_);
                for (int k = 0; k < 3; ++k)
                {
                    const float bl = ap_[k].read (0, apLen_[k]), br = ap_[k].read (1, apLen_[k]);
                    const float ol = -0.6f * dl + bl, orr = -0.6f * dr + br;
                    ap_[k].write (dl + 0.6f * ol, dr + 0.6f * orr);
                    dl = ol; dr = orr;
                }
                line_.write (l[i] + fb * dl, r[i] + fb * dr);
                l[i] = dl; r[i] = dr;
            }
        }
    private:
        double sr_ = 48000.0;
        DelayLine line_, ap_[3];
        float apLen_[3] { 200, 400, 600 };
        float time_ = -1.0f;
    };

    // ENIGMA JET: "barber-pole" flanger. Two taps sweep the delay in one direction and crossfade, so the
    // comb appears to rise (BEAT > 0) or fall (BEAT < 0) forever.
    class EnigmaJet final : public Effect
    {
    public:
        EnigmaJet() : Effect (FxType::EnigmaJet) {}
        void prepare (double sr, int) override { sr_ = sr; line_.prepare ((std::size_t) (sr * 0.03)); reset(); }
        void reset() noexcept override { line_.reset(); phase_ = 0.0; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const double period = std::max (0.05, (double) beatSeconds (c));
            const double inc = (c.beats < 0.0f ? -1.0 : 1.0) / (period * sr_);
            const float minD = 0.0003f * (float) sr_, span = 0.008f * (float) sr_;
            const float fb = 0.5f;
            for (int i = 0; i < n; ++i)
            {
                phase_ += inc;
                if (phase_ >= 1.0) phase_ -= 1.0;
                if (phase_ < 0.0) phase_ += 1.0;
                const float p1 = (float) phase_, p2 = p1 + 0.5f >= 1.0f ? p1 - 0.5f : p1 + 0.5f;
                // decreasing delay = rising comb
                const float d1 = minD + span * (1.0f - p1), d2 = minD + span * (1.0f - p2);
                const float g1 = std::sin ((float) kPi * p1), g2 = std::sin ((float) kPi * p2);
                const float wl = line_.read (0, d1) * g1 + line_.read (0, d2) * g2;
                const float wr = line_.read (1, d1) * g1 + line_.read (1, d2) * g2;
                line_.write (l[i] + fb * wl, r[i] + fb * wr);
                l[i] = 0.55f * (l[i] + wl); r[i] = 0.55f * (r[i] + wr);
            }
        }
    private:
        double sr_ = 48000.0, phase_ = 0.0;
        DelayLine line_;
    };

    // STRETCH: from every retrigger point (each BEAT length) the live input is replayed slower but at
    // the original pitch (granular overlap-add); LEVEL/DEPTH sets how much slower (1x .. 4x).
    class Stretch final : public Effect
    {
    public:
        Stretch() : Effect (FxType::Stretch) {}
        void prepare (double sr, int) override
        {
            sr_ = sr;
            line_.prepare ((std::size_t) (sr * kMaxDelaySeconds));
            grain_ = std::max (256, (int) (0.04 * sr));
            reset();
        }
        void reset() noexcept override { line_.reset(); onActivate(); }
        void onActivate() noexcept override { lag_ = 1.0f; sinceTrigger_ = 0; grainPhase_ = 0; lagA_ = lagB_ = 1.0f; }
        bool amountIsParameter() const noexcept override { return true; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const int cycle = std::max (grain_, (int) (beatSeconds (c) * (float) sr_));
            const float slow = 1.0f + 3.0f * std::clamp (c.amount, 0.0f, 1.0f);
            const float maxLag = (float) line_.size() - (float) grain_ - 8.0f;
            const int half = grain_ / 2;
            for (int i = 0; i < n; ++i)
            {
                line_.write (l[i], r[i]);
                if (sinceTrigger_++ >= cycle) { sinceTrigger_ = 0; lag_ = 1.0f; }
                // the read head advances at 1/slow, so it falls behind the write head by (1 - 1/slow) per frame
                lag_ = std::min (maxLag, lag_ + (1.0f - 1.0f / slow));
                if (grainPhase_ == 0) lagA_ = lag_ + (float) half;
                if (grainPhase_ == half) lagB_ = lag_ + (float) half;
                // two grains offset by half a grain; within a grain the audio plays at natural speed
                const float pa = (float) grainPhase_ / (float) grain_;
                const float pb = pa + 0.5f >= 1.0f ? pa - 0.5f : pa + 0.5f;
                const float ga = std::sin ((float) kPi * pa), gb = std::sin ((float) kPi * pb);
                const float da = std::max (1.0f, lagA_ - (float) grainPhase_);
                const float db = std::max (1.0f, lagB_ - (float) ((grainPhase_ + half) % grain_));
                l[i] = line_.read (0, da) * ga + line_.read (0, db) * gb;
                r[i] = line_.read (1, da) * ga + line_.read (1, db) * gb;
                if (++grainPhase_ >= grain_) grainPhase_ = 0;
            }
        }
    private:
        double sr_ = 48000.0;
        DelayLine line_;
        int grain_ = 2048, grainPhase_ = 0, sinceTrigger_ = 0;
        float lag_ = 1.0f, lagA_ = 1.0f, lagB_ = 1.0f;
    };

    // MOBIUS (SAW / TRI): a Shepard tone - octave-spaced oscillators under a fixed bell envelope that
    // glide endlessly up (BEAT > 0) or down (BEAT < 0). It sounds even with no input.
    class Mobius final : public Effect
    {
    public:
        explicit Mobius (bool tri) : Effect (tri ? FxType::MobiusTri : FxType::MobiusSaw), tri_ (tri) {}
        void prepare (double sr, int) override { sr_ = sr; reset(); }
        void reset() noexcept override { glide_ = 0.0; for (auto& p : osc_) p = 0.0; }
        void process (float* l, float* r, int n, const FxContext& c) noexcept override
        {
            const double period = std::max (0.05, (double) beatSeconds (c)) * 4.0; // one octave per 4x BEAT
            const double inc = (c.beats < 0.0f ? -1.0 : 1.0) / (period * sr_);
            for (int i = 0; i < n; ++i)
            {
                glide_ += inc;
                if (glide_ >= 1.0) glide_ -= 1.0;
                if (glide_ < 0.0) glide_ += 1.0;
                float s = 0.0f;
                for (int k = 0; k < kOctaves; ++k)
                {
                    const double oct = (double) k + glide_;                        // 0 .. kOctaves
                    const double f = 40.0 * std::pow (2.0, oct);
                    const double x = (oct - kOctaves * 0.5) / (kOctaves * 0.22);
                    const float amp = (float) std::exp (-0.5 * x * x);
                    osc_[k] += f / sr_;
                    if (osc_[k] >= 1.0) osc_[k] -= 1.0;
                    const float ph = (float) osc_[k];
                    const float w = tri_ ? 4.0f * std::abs (ph - 0.5f) - 1.0f : 2.0f * ph - 1.0f;
                    s += amp * w;
                }
                s *= 0.12f;
                l[i] += s; r[i] += s;
            }
        }
    private:
        static constexpr int kOctaves = 8;
        bool tri_;
        double sr_ = 48000.0, glide_ = 0.0;
        double osc_[kOctaves] {};
    };
}

const char* fxName (FxType t) noexcept
{
    switch (t)
    {
        case FxType::None: return "-";
        case FxType::Echo: return "Echo";
        case FxType::PingPong: return "Ping Pong";
        case FxType::TapeEcho: return "Tape Echo";
        case FxType::Reverb: return "Reverb";
        case FxType::Flanger: return "Flanger";
        case FxType::Phaser: return "Phaser";
        case FxType::HighPass: return "HPF";
        case FxType::LowPass: return "LPF";
        case FxType::FilterLfo: return "Filter LFO";
        case FxType::Bitcrusher: return "Bit Crush";
        case FxType::Distortion: return "Distortion";
        case FxType::Gate: return "Trans";
        case FxType::Roll: return "Roll";
        case FxType::ReverseRoll: return "Reverse Roll";
        case FxType::Braker: return "Braker";
        case FxType::PitchShift: return "Pitch";
        case FxType::Noise: return "Noise";
        case FxType::LowCutEcho: return "Low Cut Echo";
        case FxType::MultiTapDelay: return "Multi Tap Delay";
        case FxType::Spiral: return "Spiral";
        case FxType::EnigmaJet: return "Enigma Jet";
        case FxType::Stretch: return "Stretch";
        case FxType::SlipRoll: return "Slip Roll";
        case FxType::MobiusSaw: return "Mobius (Saw)";
        case FxType::MobiusTri: return "Mobius (Tri)";
        case FxType::Count: break;
    }
    return "?";
}

BeatKind fxBeatKind (FxType t) noexcept
{
    switch (t)
    {
        case FxType::Reverb: return BeatKind::Size;
        case FxType::Flanger: case FxType::Phaser: return BeatKind::Time64;
        case FxType::EnigmaJet: case FxType::MobiusSaw: case FxType::MobiusTri: return BeatKind::Signed64;
        default: return BeatKind::Time16;
    }
}

FxType flx10BeatFx (int position) noexcept
{
    static constexpr FxType order[kFlx10BeatFxCount] {
        FxType::LowCutEcho, FxType::Echo, FxType::MultiTapDelay, FxType::Spiral, FxType::Reverb, FxType::Gate,
        FxType::EnigmaJet, FxType::Flanger, FxType::Phaser, FxType::Stretch, FxType::SlipRoll, FxType::Roll,
        FxType::MobiusSaw, FxType::MobiusTri };
    return position >= 0 && position < kFlx10BeatFxCount ? order[position] : FxType::None;
}

const char* fxParamName (FxType t, int i) noexcept
{
    switch (t)
    {
        case FxType::Echo:       return i == 0 ? "Feedback" : i == 1 ? "Damping" : "";
        case FxType::PingPong:   return i == 0 ? "Feedback" : "";
        case FxType::TapeEcho:   return i == 0 ? "Feedback" : i == 1 ? "Wow" : "Drive";
        case FxType::Reverb:     return i == 0 ? "Size" : i == 1 ? "Damping" : "";
        case FxType::Flanger:    return i == 0 ? "Feedback" : "";
        case FxType::Phaser:     return i == 0 ? "Feedback" : "";
        case FxType::HighPass:
        case FxType::LowPass:    return i == 0 ? "Resonance" : "";
        case FxType::FilterLfo:  return i == 0 ? "Resonance" : "";
        case FxType::Bitcrusher: return i == 0 ? "Downsample" : "";
        case FxType::Distortion: return i == 0 ? "Drive" : "";
        case FxType::Gate:       return i == 0 ? "Duty" : "";
        case FxType::PitchShift: return i == 0 ? "Pitch" : "";
        case FxType::Noise:      return i == 0 ? "Colour" : "";
        case FxType::LowCutEcho: return i == 0 ? "Feedback" : "";
        case FxType::Spiral:     return "";
        case FxType::Stretch:    return "";
        default: return "";
    }
}

float fxParamDefault (FxType t, int i) noexcept
{
    if (t == FxType::PitchShift && i == 0) return 0.75f; // +6 semitones
    if (t == FxType::HighPass || t == FxType::LowPass) return i == 0 ? 0.15f : 0.5f;
    if (t == FxType::Distortion && i == 0) return 0.3f;
    return 0.5f;
}

std::unique_ptr<Effect> makeEffect (FxType type)
{
    switch (type)
    {
        case FxType::Echo: return std::make_unique<Echo>();
        case FxType::PingPong: return std::make_unique<PingPong>();
        case FxType::TapeEcho: return std::make_unique<TapeEcho>();
        case FxType::Reverb: return std::make_unique<ReverbFx>();
        case FxType::Flanger: return std::make_unique<Flanger>();
        case FxType::Phaser: return std::make_unique<Phaser>();
        case FxType::HighPass: return std::make_unique<SweepFilter> (FxType::HighPass);
        case FxType::LowPass: return std::make_unique<SweepFilter> (FxType::LowPass);
        case FxType::FilterLfo: return std::make_unique<FilterLfo>();
        case FxType::Bitcrusher: return std::make_unique<Bitcrusher>();
        case FxType::Distortion: return std::make_unique<Distortion>();
        case FxType::Gate: return std::make_unique<Gate>();
        case FxType::Roll: return std::make_unique<Roll> (false);
        case FxType::ReverseRoll: return std::make_unique<Roll> (true);
        case FxType::Braker: return std::make_unique<Braker>();
        case FxType::PitchShift: return std::make_unique<PitchShift>();
        case FxType::Noise: return std::make_unique<Noise>();
        case FxType::LowCutEcho: return std::make_unique<LowCutEcho>();
        case FxType::MultiTapDelay: return std::make_unique<MultiTapDelay>();
        case FxType::Spiral: return std::make_unique<Spiral>();
        case FxType::EnigmaJet: return std::make_unique<EnigmaJet>();
        case FxType::Stretch: return std::make_unique<Stretch>();
        case FxType::SlipRoll: return std::make_unique<Roll> (false, true);
        case FxType::MobiusSaw: return std::make_unique<Mobius> (false);
        case FxType::MobiusTri: return std::make_unique<Mobius> (true);
        case FxType::None:
        case FxType::Count: break;
    }
    return nullptr;
}

} // namespace dj
