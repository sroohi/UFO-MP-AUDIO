#include "Analysis.h"
#include "Fft.h"
#include "../dsp/Biquad.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace dj::analysis
{

namespace
{
    struct OnsetEnvelope
    {
        std::vector<float> all, low; // spectral flux, full band and below 150 Hz
        std::vector<float> lowEnergy; // linear energy below 150 Hz (downbeat accent)
        double fps = 200.0;
        int hop = 240, n = 1024;
    };

    // Log-magnitude spectral flux (half-wave rectified frame-to-frame increase).
    OnsetEnvelope onsetEnvelope (const std::vector<float>& mono, double sr)
    {
        OnsetEnvelope o;
        o.hop = std::max (1, (int) std::lround (sr / 200.0));
        o.n = sr > 60000.0 ? 2048 : 1024;
        o.fps = sr / (double) o.hop;
        if ((int) mono.size() < o.n * 2) return o;
        Fft fft (o.n);
        std::vector<float> window ((std::size_t) o.n);
        for (int i = 0; i < o.n; ++i) window[(std::size_t) i] = 0.5f * (1.0f - std::cos (2.0f * (float) kPi * (float) i / (float) o.n));
        const int bins = o.n / 2;
        const int lowBins = std::max (2, (int) (150.0 * o.n / sr));
        std::vector<std::complex<float>> buf ((std::size_t) o.n);
        std::vector<float> prev ((std::size_t) bins + 1, 0.0f), cur ((std::size_t) bins + 1);
        const std::size_t frames = (mono.size() - (std::size_t) o.n) / (std::size_t) o.hop + 1;
        o.all.assign (frames, 0.0f);
        o.low.assign (frames, 0.0f);
        o.lowEnergy.assign (frames, 0.0f);
        for (std::size_t f = 0; f < frames; ++f)
        {
            const float* x = mono.data() + f * (std::size_t) o.hop;
            for (int i = 0; i < o.n; ++i) buf[(std::size_t) i] = { x[i] * window[(std::size_t) i], 0.0f };
            fft.forward (buf.data());
            float flux = 0.0f, lowFlux = 0.0f, lowE = 0.0f;
            for (int k = 1; k <= bins; ++k)
            {
                if (k <= lowBins) lowE += std::norm (buf[(std::size_t) k]);
                cur[(std::size_t) k] = std::log1p (100.0f * std::abs (buf[(std::size_t) k]));
                const float d = cur[(std::size_t) k] - prev[(std::size_t) k];
                if (d > 0.0f) { flux += d; if (k <= lowBins) lowFlux += d; }
            }
            o.all[f] = f == 0 ? 0.0f : flux;
            o.low[f] = f == 0 ? 0.0f : lowFlux;
            o.lowEnergy[f] = lowE;
            std::swap (prev, cur);
        }
        // remove the slowly varying part (local mean over +/- 0.25 s) and rectify
        auto detrend = [&] (std::vector<float>& v)
        {
            const int w = std::max (1, (int) (o.fps * 0.25));
            std::vector<double> prefix (v.size() + 1, 0.0);
            for (std::size_t i = 0; i < v.size(); ++i) prefix[i + 1] = prefix[i] + v[i];
            std::vector<float> out (v.size());
            for (std::size_t i = 0; i < v.size(); ++i)
            {
                const std::size_t a = i >= (std::size_t) w ? i - (std::size_t) w : 0;
                const std::size_t b = std::min (v.size(), i + (std::size_t) w + 1);
                const double mean = (prefix[b] - prefix[a]) / (double) (b - a);
                out[i] = std::max (0.0f, v[i] - (float) mean);
            }
            v.swap (out);
        };
        detrend (o.all);
        detrend (o.low);
        return o;
    }

    float interp (const std::vector<float>& v, double x) noexcept
    {
        if (x < 0.0) return 0.0f;
        const auto i = (std::size_t) x;
        if (i + 1 >= v.size()) return 0.0f;
        const float t = (float) (x - (double) i);
        return v[i] + t * (v[i + 1] - v[i]);
    }

    // Peakiness of the onset envelope folded at one beat period, and the phase (in envelope frames).
    struct PhaseScore { double score = 0.0; double phase = 0.0; };
    PhaseScore phaseHistogram (const std::vector<float>& env, double period)
    {
        constexpr int kBins = 96;
        std::array<double, kBins> h {};
        for (std::size_t n = 0; n < env.size(); ++n)
        {
            if (env[n] <= 0.0f) continue;
            const double p = std::fmod ((double) n, period) / period * kBins;
            const int b0 = (int) p % kBins;
            const double t = p - std::floor (p);
            h[(std::size_t) b0] += env[n] * (1.0 - t);
            h[(std::size_t) ((b0 + 1) % kBins)] += env[n] * t;
        }
        double mean = 0.0;
        for (double x : h) mean += x;
        mean /= kBins;
        if (mean <= 0.0) return {};
        PhaseScore best;
        // binomial [1 4 6 4 1] smoothing: onset frames are quantised to the envelope rate, and a sharper
        // kernel makes the score jitter between neighbouring tempos
        for (int b = 0; b < kBins; ++b)
        {
            auto at = [&] (int o) { return h[(std::size_t) ((b + o + kBins) % kBins)]; };
            const double s = at (-2) + 4.0 * at (-1) + 6.0 * at (0) + 4.0 * at (1) + at (2);
            if (s > best.score)
            {
                // parabolic refinement of the peak position
                const double ym = h[(std::size_t) ((b + kBins - 1) % kBins)], y0 = h[(std::size_t) b], yp = h[(std::size_t) ((b + 1) % kBins)];
                const double den = ym - 2.0 * y0 + yp;
                const double off = std::abs (den) > 1e-12 ? 0.5 * (ym - yp) / den : 0.0;
                best.score = s;
                best.phase = ((double) b + std::clamp (off, -0.5, 0.5)) / kBins * period;
            }
        }
        best.score /= 16.0 * mean;
        return best;
    }

    // Temperley-Kostka-Payne key profiles (D. Temperley, "Music and Probability", 2007).
    constexpr double kMajor[12] { 0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400 };
    constexpr double kMinor[12] { 0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330 };
    // Harmonic-aware templates (E. Gomez, "Tonal description of music audio signals", 2006): each pitch
    // of the profile also contributes its upper partials (octaves, 12th = +7, 17th = +4) at 0.6^(h-1),
    // so the chroma of real, harmonic-rich instruments is not pulled towards the dominant.
    void keyTemplate (int key, double* out)
    {
        static constexpr int interval[6] { 0, 0, 7, 0, 4, 7 };
        const double* base = key < 12 ? kMajor : kMinor;
        for (int p = 0; p < 12; ++p) out[p] = 0.0;
        for (int p = 0; p < 12; ++p)
            for (int h = 1; h <= 6; ++h)
                out[(p + key % 12 + interval[h - 1]) % 12] += base[p] * std::pow (0.6, h - 1);
    }

    double pearson (const double* a, const double* b, int n)
    {
        double ma = 0, mb = 0;
        for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
        ma /= n; mb /= n;
        double num = 0, da = 0, db = 0;
        for (int i = 0; i < n; ++i) { num += (a[i] - ma) * (b[i] - mb); da += (a[i] - ma) * (a[i] - ma); db += (b[i] - mb) * (b[i] - mb); }
        return da > 0 && db > 0 ? num / std::sqrt (da * db) : 0.0;
    }

    // ITU-R BS.1770-4 K-weighting coefficients at any sample rate (bilinear transform of the analog prototypes).
    struct Kw { double b[3], a[3]; };
    void kWeighting (double fs, Kw& shelf, Kw& hp)
    {
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (kPi * f0 / fs);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            shelf.b[0] = (Vh + Vb * K / Q + K * K) / a0;
            shelf.b[1] = 2.0 * (K * K - Vh) / a0;
            shelf.b[2] = (Vh - Vb * K / Q + K * K) / a0;
            shelf.a[0] = 1.0;
            shelf.a[1] = 2.0 * (K * K - 1.0) / a0;
            shelf.a[2] = (1.0 - K / Q + K * K) / a0;
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (kPi * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            hp.b[0] = 1.0; hp.b[1] = -2.0; hp.b[2] = 1.0;
            hp.a[0] = 1.0;
            hp.a[1] = 2.0 * (K * K - 1.0) / a0;
            hp.a[2] = (1.0 - K / Q + K * K) / a0;
        }
    }
}

std::vector<float> toMono (const TrackAudio& a)
{
    std::vector<float> m ((std::size_t) a.frames);
    for (std::size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (a.mix[i * 2] + a.mix[i * 2 + 1]);
    return m;
}

BpmResult detectBpm (const std::vector<float>& mono, double sr, const AnalysisSettings& s)
{
    BpmResult res;
    const auto env = onsetEnvelope (mono, sr);
    if (env.all.size() < 400) return res;

    // autocorrelation of the onset envelope
    const int maxLag = (int) (env.fps * 60.0 / 30.0) + 2;
    std::vector<float> acf ((std::size_t) maxLag + 1, 0.0f);
    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double sum = 0.0;
        for (std::size_t n = (std::size_t) lag; n < env.all.size(); ++n) sum += (double) env.all[n] * env.all[n - (std::size_t) lag];
        acf[(std::size_t) lag] = (float) (sum / (double) (env.all.size() - (std::size_t) lag));
    }

    // coarse: comb over the first four multiples of the beat lag, folded into the BPM range
    double bestBpm = 0.0, bestScore = -1.0;
    for (double bpm = s.minBpm; bpm < s.maxBpm; bpm += 0.1)
    {
        const double lag = env.fps * 60.0 / bpm;
        double sc = 0.0;
        for (int m = 1; m <= 4; ++m) sc += interp (acf, lag * m) / (double) m;
        if (sc > bestScore) { bestScore = sc; bestBpm = bpm; }
    }
    if (bestBpm <= 0.0) return res;

    // fine: phase-histogram peakiness over the whole track
    auto scoreAt = [&] (double bpm) { return phaseHistogram (env.all, env.fps * 60.0 / bpm); };
    PhaseScore best;
    double bpm = bestBpm;
    for (double b = bestBpm - 0.4; b <= bestBpm + 0.4; b += 0.005)
    {
        const auto ps = scoreAt (b);
        if (ps.score > best.score) { best = ps; bpm = b; }
    }
    if (s.snapToInteger)
    {
        for (double cand : { std::round (bpm), std::round (bpm * 2.0) / 2.0 })
        {
            if (std::abs (cand - bpm) > 0.2) continue;
            const auto ps = scoreAt (cand);
            if (ps.score >= best.score * 0.95) { best = ps; bpm = cand; break; }
        }
    }

    const double period = env.fps * 60.0 / bpm;          // envelope frames per beat
    const double beatSamples = 60.0 * sr / bpm;
    // The beat is where the kick is: off-beat hi-hats often produce more full-band flux than the kick,
    // so take the phase from the low band when it is periodic, else pick between the full-band phase
    // and the half-beat shifted one by their low-band energy.
    const auto lowPs = phaseHistogram (env.low, period);
    if (lowPs.score > 1.5)
        best.phase = lowPs.phase;
    else
    {
        auto lowAt = [&] (double ph) {
            double sum = 0.0;
            for (double p = ph; p < (double) env.low.size(); p += period) sum += interp (env.low, p);
            return sum;
        };
        if (lowAt (best.phase + 0.5 * period) > lowAt (best.phase)) best.phase = fmodPos (best.phase + 0.5 * period, period);
    }
    // envelope frame f covers [f * hop, f * hop + n); log flux peaks as a transient enters the window,
    // about a quarter window ahead of its centre
    const double firstBeat = best.phase * env.hop + env.n * 0.75;

    // downbeat: the beat of the bar where the low band hits hardest
    int bestK = 0;
    double bestLow = -1.0;
    for (int k = 0; k < 4; ++k)
    {
        double sum = 0.0;
        // linear low-band energy just after the beat: log flux would flatten the accent
        for (double p = best.phase + k * period; p + 12.0 < (double) env.lowEnergy.size(); p += 4.0 * period)
        {
            float e = 0.0f;
            for (int d = 0; d <= 10; ++d) e = std::max (e, interp (env.lowEnergy, p + d));
            sum += std::sqrt ((double) e);
        }
        if (sum > bestLow) { bestLow = sum; bestK = k; }
    }
    res.bpm = bpm;
    res.anchor = fmodPos (firstBeat + bestK * beatSamples, 4.0 * beatSamples);
    res.confidence = std::clamp ((best.score - 1.0) / 3.0, 0.0, 1.0);
    return res;
}

int detectKey (const std::vector<float>& mono, double sr, double* confidence)
{
    const int n = sr > 60000.0 ? 16384 : 8192;
    if ((int) mono.size() < n) return -1;
    Fft fft (n);
    std::vector<float> window ((std::size_t) n);
    for (int i = 0; i < n; ++i) window[(std::size_t) i] = 0.5f * (1.0f - std::cos (2.0f * (float) kPi * (float) i / (float) n));
    // bin -> pitch class weight table for 55 Hz .. 2 kHz
    struct Map { int bin, pc; float w; };
    std::vector<Map> map;
    for (int k = 1; k < n / 2; ++k)
    {
        const double f = (double) k * sr / n;
        if (f < 55.0 || f > 2000.0) continue; // fundamentals of most harmonic parts
        const double midi = 69.0 + 12.0 * std::log2 (f / 440.0);
        const double nearest = std::round (midi);
        const double d = midi - nearest;
        const float w = (float) std::pow (std::cos (kPi * d), 2.0);
        map.push_back ({ k, (int) (((long) nearest % 12 + 12) % 12), w });
    }
    std::vector<std::complex<float>> buf ((std::size_t) n);
    std::array<double, 12> chroma {};
    for (std::size_t off = 0; off + (std::size_t) n <= mono.size(); off += (std::size_t) n / 2)
    {
        double energy = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float x = mono[off + (std::size_t) i];
            energy += (double) x * x;
            buf[(std::size_t) i] = { x * window[(std::size_t) i], 0.0f };
        }
        if (energy / n < 1.0e-7) continue; // silence
        fft.forward (buf.data());
        std::array<double, 12> c {};
        for (const auto& m : map) c[(std::size_t) m.pc] += std::sqrt ((double) std::abs (buf[(std::size_t) m.bin])) * m.w;
        const double mx = *std::max_element (c.begin(), c.end());
        if (mx <= 0.0) continue;
        const double weight = std::sqrt (energy / n);
        for (int p = 0; p < 12; ++p) chroma[(std::size_t) p] += c[(std::size_t) p] / mx * weight;
    }
    double best = -2.0, second = -2.0;
    int key = -1;
    for (int k = 0; k < 24; ++k)
    {
        double prof[12];
        keyTemplate (k, prof);
        const double r = pearson (chroma.data(), prof, 12);
        if (r > best) { second = best; best = r; key = k; }
        else if (r > second) second = r;
    }
    if (confidence) *confidence = best > 0.0 ? std::clamp ((best - second) / best * 5.0, 0.0, 1.0) : 0.0;
    return key;
}

double integratedLoudness (const float* x, std::int64_t frames, double sr)
{
    Kw shelf, hp;
    kWeighting (sr, shelf, hp);
    const auto block = (std::int64_t) std::lround (0.4 * sr), step = (std::int64_t) std::lround (0.1 * sr);
    if (frames < block) return -70.0;
    // K-weighted squares, summed over both channels, as a running prefix for block means
    std::vector<double> prefix ((std::size_t) frames + 1, 0.0);
    for (int ch = 0; ch < 2; ++ch)
    {
        double s1[2] {}, s2[2] {};
        for (std::int64_t i = 0; i < frames; ++i)
        {
            const double in = x[i * 2 + ch];
            const double y1 = shelf.b[0] * in + s1[0];
            s1[0] = shelf.b[1] * in - shelf.a[1] * y1 + s1[1];
            s1[1] = shelf.b[2] * in - shelf.a[2] * y1;
            const double y2 = hp.b[0] * y1 + s2[0];
            s2[0] = hp.b[1] * y1 - hp.a[1] * y2 + s2[1];
            s2[1] = hp.b[2] * y1 - hp.a[2] * y2;
            prefix[(std::size_t) i + 1] += y2 * y2;
        }
    }
    for (std::size_t i = 1; i < prefix.size(); ++i) prefix[i] += prefix[i - 1];
    std::vector<double> z;
    for (std::int64_t s = 0; s + block <= frames; s += step)
        z.push_back ((prefix[(std::size_t) (s + block)] - prefix[(std::size_t) s]) / (double) block);
    auto lufs = [] (double p) { return -0.691 + 10.0 * std::log10 (std::max (p, 1.0e-12)); };
    double sum = 0.0; int count = 0;
    for (double p : z) if (lufs (p) > -70.0) { sum += p; ++count; }
    if (count == 0) return -70.0;
    const double relGate = lufs (sum / count) - 10.0;
    sum = 0.0; count = 0;
    for (double p : z) if (lufs (p) > -70.0 && lufs (p) > relGate) { sum += p; ++count; }
    return count > 0 ? lufs (sum / count) : -70.0;
}

Waveform buildWaveform (const float* x, std::int64_t frames, double sr, double binsPerSecond)
{
    Waveform w;
    w.binsPerSecond = binsPerSecond;
    const auto bins = (std::size_t) std::max<std::int64_t> (1, (std::int64_t) std::ceil ((double) frames / sr * binsPerSecond));
    std::vector<float> pk (bins, 0.0f), lo (bins, 0.0f), mi (bins, 0.0f), hi (bins, 0.0f);
    std::vector<int> cnt (bins, 0);
    // three bands, each 24 dB/oct: < 200 Hz, 200 Hz - 2.5 kHz, > 2.5 kHz
    dsp::Biquad lp[2], hpf[2], mbHp[2], mbLp[2];
    for (auto& b : lp) b.set (dsp::Biquad::Type::LowPass, sr, 200.0, 0.7071);
    for (auto& b : hpf) b.set (dsp::Biquad::Type::HighPass, sr, 2500.0, 0.7071);
    for (auto& b : mbHp) b.set (dsp::Biquad::Type::HighPass, sr, 200.0, 0.7071);
    for (auto& b : mbLp) b.set (dsp::Biquad::Type::LowPass, sr, 2500.0, 0.7071);
    const double framesPerBin = sr / binsPerSecond;
    for (std::int64_t i = 0; i < frames; ++i)
    {
        const float m = 0.5f * (x[i * 2] + x[i * 2 + 1]);
        const float l = lp[1].process (0, lp[0].process (0, m));
        const float h = hpf[1].process (0, hpf[0].process (0, m));
        const float md = mbLp[1].process (0, mbLp[0].process (0, mbHp[1].process (0, mbHp[0].process (0, m))));
        const auto b = std::min (bins - 1, (std::size_t) ((double) i / framesPerBin));
        pk[b] = std::max (pk[b], std::abs (m));
        lo[b] += l * l; mi[b] += md * md; hi[b] += h * h;
        ++cnt[b];
    }
    float maxPk = 1.0e-6f, maxBand = 1.0e-9f;
    for (std::size_t b = 0; b < bins; ++b)
    {
        if (cnt[b] > 0) { lo[b] = std::sqrt (lo[b] / cnt[b]); mi[b] = std::sqrt (mi[b] / cnt[b]); hi[b] = std::sqrt (hi[b] / cnt[b]); }
        maxPk = std::max (maxPk, pk[b]);
        maxBand = std::max ({ maxBand, lo[b], mi[b], hi[b] });
    }
    auto q = [] (float v, float mx) { return (std::uint8_t) std::lround (255.0f * std::sqrt (std::clamp (v / mx, 0.0f, 1.0f))); };
    w.peak.resize (bins); w.low.resize (bins); w.mid.resize (bins); w.high.resize (bins);
    for (std::size_t b = 0; b < bins; ++b)
    {
        w.peak[b] = (std::uint8_t) std::lround (255.0f * std::clamp (pk[b] / maxPk, 0.0f, 1.0f));
        w.low[b] = q (lo[b], maxBand);
        w.mid[b] = q (mi[b], maxBand);
        w.high[b] = q (hi[b], maxBand);
    }
    return w;
}

TrackAnalysis analyseTrack (const TrackAudio& a, const AnalysisSettings& s, const Progress& progress)
{
    TrackAnalysis r;
    if (a.frames <= 0) return r;
    auto report = [&] (double p) { return ! progress || progress (p); };
    const auto mono = toMono (a);
    if (! report (0.05)) return r;
    r.beat = detectBpm (mono, a.sampleRate, s);
    if (! report (0.45)) return r;
    r.key = detectKey (mono, a.sampleRate, &r.keyConfidence);
    if (! report (0.7)) return r;
    r.loudnessLufs = integratedLoudness (a.mix.data(), a.frames, a.sampleRate);
    r.autoGainDb = r.loudnessLufs > -69.0 ? std::clamp (s.targetLufs - r.loudnessLufs, -12.0, 12.0) : 0.0;
    for (float v : a.mix) r.peak = std::max (r.peak, (double) std::abs (v));
    if (! report (0.8)) return r;
    r.detail = buildWaveform (a.mix.data(), a.frames, a.sampleRate, s.detailBinsPerSecond);
    r.overview = buildWaveform (a.mix.data(), a.frames, a.sampleRate, (double) s.overviewBins / std::max (1.0, a.lengthSeconds()));
    report (1.0);
    return r;
}

} // namespace dj::analysis
