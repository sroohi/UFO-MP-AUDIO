// Iterative radix-2 complex FFT with precomputed twiddles. In-house; used by offline analysis only.
#pragma once
#include <cmath>
#include <complex>
#include <vector>

namespace dj::analysis
{

class Fft
{
public:
    explicit Fft (int size) : n_ (size), twiddle_ ((std::size_t) size / 2), rev_ ((std::size_t) size)
    {
        int bits = 0;
        while ((1 << bits) < n_) ++bits;
        for (int i = 0; i < n_; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev_[(std::size_t) i] = r;
        }
        for (int k = 0; k < n_ / 2; ++k)
            twiddle_[(std::size_t) k] = std::polar (1.0f, (float) (-2.0 * 3.14159265358979323846 * k / n_));
    }

    int size() const noexcept { return n_; }

    void forward (std::complex<float>* x) const noexcept { transform (x, false); }
    // Unnormalised inverse: divide by size() to round-trip.
    void inverse (std::complex<float>* x) const noexcept { transform (x, true); }

private:
    void transform (std::complex<float>* x, bool inverse) const noexcept
    {
        for (int i = 0; i < n_; ++i)
        {
            const int r = rev_[(std::size_t) i];
            if (r > i) std::swap (x[i], x[r]);
        }
        for (int len = 2; len <= n_; len <<= 1)
        {
            const int half = len / 2, step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int k = 0; k < half; ++k)
                {
                    std::complex<float> w = twiddle_[(std::size_t) (k * step)];
                    if (inverse) w = std::conj (w);
                    const std::complex<float> t = w * x[i + k + half];
                    x[i + k + half] = x[i + k] - t;
                    x[i + k] += t;
                }
        }
    }

    int n_;
    std::vector<std::complex<float>> twiddle_;
    std::vector<int> rev_;
};

} // namespace dj::analysis
