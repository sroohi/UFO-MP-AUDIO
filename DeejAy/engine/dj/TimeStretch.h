// Key lock / key shift: WSOLA (Verhelst & Roelands 1993) driven directly from random-access track audio.
//
// A DJ deck already knows where in the track it should be at every output sample, so instead of a
// streaming stretcher with its own input FIFO, each grain is cut from the track at the deck's
// current position (plus the best-matching offset within +/- `tolerance`), read at the pitch ratio,
// Hann-windowed and overlap-added at 50 %. Tempo comes from the deck timeline; pitch is the grain read
// speed. Jumps (hot cues, loops, beat jumps) are simply the next grain, so they crossfade over one hop.
//
// The stretcher is multi-channel: a deck renders several stem buses (FX-part / rest / stem-FX send)
// and all of them must use the same grain offsets or they would phase against each other when summed.
// In-house implementation; no third-party code.
#pragma once
#include "Common.h"
#include <array>
#include <vector>

namespace dj
{

class TimeStretcher
{
public:
    static constexpr int kMaxChannels = 6;

    void prepare (double outputSampleRate, int numChannels)
    {
        ch_ = std::clamp (numChannels, 1, kMaxChannels);
        hop_ = std::max (64, (int) std::lround (0.023 * outputSampleRate));
        n_ = hop_ * 2;
        tol_ = std::max (16, (int) std::lround (0.012 * outputSampleRate));
        window_.resize ((std::size_t) n_);
        for (int k = 0; k < n_; ++k)
            window_[(std::size_t) k] = 0.5f * (1.0f - std::cos (2.0f * (float) kPi * (float) k / (float) n_));
        ola_.assign ((std::size_t) (n_ * ch_), 0.0f);
        ref_.assign ((std::size_t) hop_, 0.0f);
        cand_.assign ((std::size_t) (hop_ + 2 * tol_ + 8), 0.0f);
        reset();
    }

    void reset() noexcept
    {
        std::fill (ola_.begin(), ola_.end(), 0.0f);
        idx_ = hop_;
        hasPrev_ = false;
    }

    int hop() const noexcept { return hop_; }
    int channels() const noexcept { return ch_; }

    // One output frame into out[0..channels). `nominal` is the track position that should be heard now,
    // `speed` the grain read speed in track frames per output frame (pitch ratio * trackRate / outputRate).
    // Src must provide `void frame (double pos, float* out) const noexcept` filling `channels()` values.
    template <typename Src>
    void next (const Src& src, double nominal, double speed, float* out) noexcept
    {
        if (idx_ >= hop_)
        {
            makeGrain (src, nominal, speed);
            idx_ = 0;
        }
        const float* f = &ola_[(std::size_t) (idx_ * ch_)];
        for (int c = 0; c < ch_; ++c) out[c] = f[c];
        ++idx_;
    }

private:
    template <typename Src>
    float mono (const Src& src, double pos) const noexcept
    {
        std::array<float, kMaxChannels> f {};
        src.frame (pos, f.data());
        float s = 0.0f;
        for (int c = 0; c < ch_; ++c) s += f[(std::size_t) c];
        return s;
    }

    template <typename Src>
    void makeGrain (const Src& src, double nominal, double speed) noexcept
    {
        // shift the overlap-add buffer by one hop
        const auto hopSamples = (std::size_t) (hop_ * ch_);
        std::copy (ola_.begin() + (std::ptrdiff_t) hopSamples, ola_.end(), ola_.begin());
        std::fill (ola_.end() - (std::ptrdiff_t) hopSamples, ola_.end(), 0.0f);

        double start = nominal;
        if (hasPrev_)
        {
            // natural continuation of the previous grain, compared over the overlap region
            const double cont = prevStart_ + (double) hop_ * prevSpeed_;
            for (int k = 0; k < hop_; ++k)
                ref_[(std::size_t) k] = mono (src, cont + (double) k * speed);
            const double base = nominal - (double) tol_ * speed;
            const int candLen = hop_ + 2 * tol_;
            for (int j = 0; j < candLen; ++j)
                cand_[(std::size_t) j] = mono (src, base + (double) j * speed);

            // coarse search (step 4, decimate 4), then refine (+/-4, decimate 2)
            int best = tol_;
            float bestScore = -1.0e30f;
            for (int j0 = 0; j0 <= 2 * tol_; j0 += 4)
            {
                const float s = score (j0, 4);
                if (s > bestScore) { bestScore = s; best = j0; }
            }
            const int lo = std::max (0, best - 4), hi = std::min (2 * tol_, best + 4);
            bestScore = -1.0e30f;
            int refined = best;
            for (int j0 = lo; j0 <= hi; ++j0)
            {
                const float s = score (j0, 2);
                if (s > bestScore) { bestScore = s; refined = j0; }
            }
            start = base + (double) refined * speed;
        }

        std::array<float, kMaxChannels> f {};
        for (int k = 0; k < n_; ++k)
        {
            src.frame (start + (double) k * speed, f.data());
            const float w = window_[(std::size_t) k];
            float* o = &ola_[(std::size_t) (k * ch_)];
            for (int c = 0; c < ch_; ++c) o[c] += f[(std::size_t) c] * w;
        }
        prevStart_ = start;
        prevSpeed_ = speed;
        hasPrev_ = true;
    }

    // Normalised cross-correlation of the reference against the candidate starting at j0.
    float score (int j0, int step) const noexcept
    {
        float xy = 0.0f, yy = 1.0e-9f;
        for (int k = 0; k < hop_; k += step)
        {
            const float y = cand_[(std::size_t) (j0 + k)];
            xy += ref_[(std::size_t) k] * y;
            yy += y * y;
        }
        return xy / std::sqrt (yy);
    }

    int ch_ = 2, n_ = 2048, hop_ = 1024, tol_ = 512, idx_ = 1024;
    std::vector<float> window_, ola_, ref_, cand_;
    double prevStart_ = 0.0, prevSpeed_ = 1.0;
    bool hasPrev_ = false;
};

} // namespace dj
