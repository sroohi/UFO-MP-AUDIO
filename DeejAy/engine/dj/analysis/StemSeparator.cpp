#include "StemSeparator.h"
#include "Fft.h"
#include <algorithm>
#include <array>
#include <complex>

namespace dj::analysis
{

namespace
{
    using cf = std::complex<float>;
    constexpr int kMedian = 17;          // median filter length (frames / bins)
    constexpr int kHalf = kMedian / 2;
}

std::unique_ptr<StemSeparator> makeDefaultStemSeparator() { return std::make_unique<DspStemSeparator>(); }

bool DspStemSeparator::separate (const TrackAudio& a, StemSeparation& out, const Progress& progress)
{
    const int n = a.sampleRate > 60000.0 ? 8192 : 4096;
    const int hop = n / 4;
    const int bins = n / 2 + 1;
    const std::int64_t frames = a.frames;
    if (frames <= 0) return false;

    Fft fft (n);
    std::vector<float> win ((std::size_t) n);
    for (int i = 0; i < n; ++i) win[(std::size_t) i] = std::sqrt (0.5f * (1.0f - std::cos (2.0f * (float) kPi * (float) i / (float) n)));

    out.vocals.assign ((std::size_t) frames * 2, 0.0f);
    out.bass.assign ((std::size_t) frames * 2, 0.0f);
    out.drums.assign ((std::size_t) frames * 2, 0.0f);

    // frequency-dependent masks that do not depend on the signal
    std::vector<float> bassMask ((std::size_t) bins), vocalBand ((std::size_t) bins);
    for (int k = 0; k < bins; ++k)
    {
        const double f = (double) k * a.sampleRate / n;
        bassMask[(std::size_t) k] = f < 120.0 ? 1.0f : f > 250.0 ? 0.0f : (float) (0.5 * (1.0 + std::cos (kPi * (f - 120.0) / 130.0)));
        double vb = 0.0;
        if (f > 180.0 && f < 8000.0) vb = 1.0;
        else if (f > 120.0 && f <= 180.0) vb = (f - 120.0) / 60.0;
        else if (f >= 8000.0 && f < 11000.0) vb = 1.0 - (f - 8000.0) / 3000.0;
        vocalBand[(std::size_t) k] = (float) vb;
    }

    // ring of the last kMedian frames: spectra (L, R) and mono magnitude
    struct Frame { std::vector<cf> l, r; std::vector<float> mag; std::int64_t start = 0; bool valid = false; };
    std::array<Frame, kMedian> ring;
    for (auto& f : ring) { f.l.resize ((std::size_t) bins); f.r.resize ((std::size_t) bins); f.mag.resize ((std::size_t) bins); }

    std::vector<cf> buf ((std::size_t) n);
    std::vector<cf> outV ((std::size_t) n), outB ((std::size_t) n), outD ((std::size_t) n);
    std::vector<float> column (kMedian), rowH ((std::size_t) bins), rowP ((std::size_t) bins);

    const std::int64_t firstStart = -(std::int64_t) (n - hop);
    const std::int64_t totalFrames = (frames - firstStart) / hop + 1;
    auto sample = [&] (std::int64_t i, int ch) { return i >= 0 && i < frames ? a.mix[(std::size_t) i * 2 + (std::size_t) ch] : 0.0f; };

    auto analyse = [&] (std::int64_t t, Frame& fr)
    {
        fr.start = firstStart + t * hop;
        fr.valid = true;
        // pack L + iR into one complex FFT, then split the two real spectra
        for (int i = 0; i < n; ++i)
            buf[(std::size_t) i] = { sample (fr.start + i, 0) * win[(std::size_t) i], sample (fr.start + i, 1) * win[(std::size_t) i] };
        fft.forward (buf.data());
        for (int k = 0; k < bins; ++k)
        {
            const cf z = buf[(std::size_t) k], zc = std::conj (buf[(std::size_t) ((n - k) % n)]);
            fr.l[(std::size_t) k] = 0.5f * (z + zc);
            fr.r[(std::size_t) k] = cf (0.0f, -0.5f) * (z - zc);
            fr.mag[(std::size_t) k] = 0.5f * (std::abs (fr.l[(std::size_t) k]) + std::abs (fr.r[(std::size_t) k]));
        }
    };

    // spec holds the packed L + iR spectrum (see pack); the real part of the inverse is L, the imaginary R
    auto synthesise = [&] (const Frame& fr, std::vector<cf>& spec, std::vector<float>& dst)
    {
        fft.inverse (spec.data());
        const float scale = 0.5f / (float) n; // sqrt-Hann^2 at 75 % overlap sums to 2
        for (int i = 0; i < n; ++i)
        {
            const std::int64_t s = fr.start + i;
            if (s < 0 || s >= frames) continue;
            dst[(std::size_t) s * 2] += spec[(std::size_t) i].real() * win[(std::size_t) i] * scale;
            dst[(std::size_t) s * 2 + 1] += spec[(std::size_t) i].imag() * win[(std::size_t) i] * scale;
        }
    };

    auto pack = [&] (std::vector<cf>& dstSpec, const std::vector<cf>& l, const std::vector<cf>& r)
    {
        // real signals a, b with spectra A, B: FFT(a + i b)[k] = A[k] + i B[k], and A[n-k] = conj(A[k])
        for (int k = 0; k < bins; ++k) dstSpec[(std::size_t) k] = l[(std::size_t) k] + cf (0.0f, 1.0f) * r[(std::size_t) k];
        for (int k = 1; k < n / 2; ++k)
            dstSpec[(std::size_t) (n - k)] = std::conj (l[(std::size_t) k]) + cf (0.0f, 1.0f) * std::conj (r[(std::size_t) k]);
    };

    std::vector<cf> vl ((std::size_t) bins), vr ((std::size_t) bins), bl ((std::size_t) bins), br ((std::size_t) bins), dl ((std::size_t) bins), dr ((std::size_t) bins);

    for (std::int64_t t = 0; t < totalFrames + kHalf; ++t)
    {
        if (t < totalFrames) analyse (t, ring[(std::size_t) (t % kMedian)]);
        else ring[(std::size_t) (t % kMedian)].valid = false;
        const std::int64_t c = t - kHalf;     // frame whose masks are now computable
        if (c < 0) continue;
        const Frame& fr = ring[(std::size_t) (c % kMedian)];
        if (! fr.valid) continue;

        // harmonic = median across time, percussive = median across frequency
        for (int k = 0; k < bins; ++k)
        {
            int m = 0;
            for (int j = 0; j < kMedian; ++j)
            {
                const auto& o = ring[(std::size_t) j];
                if (o.valid && std::llabs (o.start - fr.start) <= (std::int64_t) kHalf * hop) column[(std::size_t) m++] = o.mag[(std::size_t) k];
            }
            std::nth_element (column.begin(), column.begin() + m / 2, column.begin() + m);
            rowH[(std::size_t) k] = column[(std::size_t) (m / 2)];
        }
        for (int k = 0; k < bins; ++k)
        {
            int m = 0;
            for (int j = std::max (0, k - kHalf); j <= std::min (bins - 1, k + kHalf); ++j) column[(std::size_t) m++] = fr.mag[(std::size_t) j];
            std::nth_element (column.begin(), column.begin() + m / 2, column.begin() + m);
            rowP[(std::size_t) k] = column[(std::size_t) (m / 2)];
        }

        for (int k = 0; k < bins; ++k)
        {
            const float h = rowH[(std::size_t) k], p = rowP[(std::size_t) k];
            const float pm = (p * p) / (h * h + p * p + 1.0e-12f);
            const float hm = 1.0f - pm;
            const cf L = fr.l[(std::size_t) k], R = fr.r[(std::size_t) k];
            dl[(std::size_t) k] = L * pm;
            dr[(std::size_t) k] = R * pm;
            const float bm = hm * bassMask[(std::size_t) k];
            bl[(std::size_t) k] = L * bm;
            br[(std::size_t) k] = R * bm;
            // centre-ness: 1 when L and R are identical in magnitude and phase
            const float el = std::norm (L), er = std::norm (R);
            const float centre = 2.0f * std::abs (L * std::conj (R)) / (el + er + 1.0e-12f);
            const float cv = std::clamp ((centre - 0.72f) / 0.28f, 0.0f, 1.0f);
            const float vm = hm * (1.0f - bassMask[(std::size_t) k]) * vocalBand[(std::size_t) k] * cv * cv;
            vl[(std::size_t) k] = L * vm;
            vr[(std::size_t) k] = R * vm;
        }
        pack (outV, vl, vr); synthesise (fr, outV, out.vocals);
        pack (outB, bl, br); synthesise (fr, outB, out.bass);
        pack (outD, dl, dr); synthesise (fr, outD, out.drums);

        if (progress && (c & 63) == 0 && ! progress ((double) c / (double) totalFrames)) return false;
    }
    if (progress) progress (1.0);
    return true;
}

} // namespace dj::analysis
