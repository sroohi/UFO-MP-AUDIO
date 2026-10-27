// Analysis tests on synthetic signals with known answers.
#include "TestHarness.h"
#include "dj/analysis/Analysis.h"
#include "dj/analysis/StemSeparator.h"
#include <random>

using namespace dj;
using namespace dj::analysis;

namespace
{
// Four-on-the-floor: a pitched kick on every beat (accented on the downbeat), noise hats on off-beats.
std::shared_ptr<TrackAudio> drumLoop (double bpm, double seconds, double sr, double firstDownbeat)
{
    const auto frames = (std::size_t) (seconds * sr);
    std::vector<float> v (frames * 2, 0.0f);
    std::mt19937 rng (3);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    const double beat = 60.0 * sr / bpm;
    for (int b = 0;; ++b)
    {
        const double start = firstDownbeat * sr + b * beat;
        if (start >= (double) frames) break;
        const float accent = (b % 4 == 0) ? 1.0f : 0.6f;
        if (start >= 0.0)
            for (int i = 0; i < (int) (0.12 * sr) && (std::size_t) start + (std::size_t) i < frames; ++i)
            {
                const double t = i / sr;
                const float k = accent * 0.8f * (float) (std::exp (-t * 30.0) * std::sin (2.0 * kPi * (50.0 + 80.0 * std::exp (-t * 40.0)) * t));
                v[((std::size_t) start + (std::size_t) i) * 2] += k;
                v[((std::size_t) start + (std::size_t) i) * 2 + 1] += k;
            }
        const double hat = start + beat * 0.5;
        if (hat >= 0.0)
            for (int i = 0; i < (int) (0.03 * sr) && (std::size_t) hat + (std::size_t) i < frames; ++i)
            {
                const float h = 0.15f * (float) std::exp (-i / sr * 120.0) * u (rng);
                v[((std::size_t) hat + (std::size_t) i) * 2] += h;
                v[((std::size_t) hat + (std::size_t) i) * 2 + 1] += h;
            }
    }
    auto t = std::make_shared<TrackAudio>();
    t->setMix (std::move (v), sr);
    return t;
}

// Chord progression of additive tones (fundamental + 4 harmonics), `secondsPerChord` each.
std::vector<float> chords (const std::vector<std::vector<int>>& prog, double sr, double secondsPerChord, int repeats)
{
    std::vector<float> m;
    for (int rep = 0; rep < repeats; ++rep)
        for (const auto& chord : prog)
        {
            const auto n = (std::size_t) (secondsPerChord * sr);
            const std::size_t off = m.size();
            m.resize (off + n, 0.0f);
            for (int midi : chord)
            {
                const double f = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
                for (int h = 1; h <= 5; ++h)
                    for (std::size_t i = 0; i < n; ++i)
                        m[off + i] += (float) (0.08 / h * std::sin (2.0 * kPi * f * h * (double) i / sr));
            }
        }
    return m;
}
}

MP_TEST (bpm_and_downbeat_of_a_four_on_the_floor_loop)
{
    const double sr = 44100.0;
    auto t = drumLoop (124.0, 40.0, sr, 0.37);
    AnalysisSettings s;
    const auto r = detectBpm (toMono (*t), sr, s);
    CHECK_NEAR (r.bpm, 124.0, 1e-9);                      // snapped to the integer
    const double bar = 4.0 * 60.0 * sr / 124.0;
    const double err = wrapHalf ((r.anchor - 0.37 * sr) / bar) * bar;
    CHECK (std::abs (err) < 0.008 * sr);                  // downbeat within 8 ms
    CHECK (r.confidence > 0.2);
}

MP_TEST (bpm_precision_and_range_folding)
{
    const double sr = 48000.0;
    AnalysisSettings s;
    auto frac = drumLoop (126.3, 60.0, sr, 0.1);
    CHECK_NEAR (detectBpm (toMono (*frac), sr, s).bpm, 126.3, 0.02);

    auto dnb = drumLoop (174.0, 40.0, sr, 0.0);
    CHECK_NEAR (detectBpm (toMono (*dnb), sr, s).bpm, 87.0, 0.02);   // default range 68-135: half time
    s.minBpm = 88.0; s.maxBpm = 175.0;
    CHECK_NEAR (detectBpm (toMono (*dnb), sr, s).bpm, 174.0, 0.02);
}

MP_TEST (key_of_minor_and_major_cadences)
{
    const double sr = 44100.0;
    // A minor: Am - Dm - E - Am
    const auto am = chords ({ { 57, 60, 64 }, { 62, 65, 69 }, { 64, 68, 71 }, { 57, 60, 64 } }, sr, 1.5, 3);
    double conf = 0.0;
    CHECK (detectKey (am, sr, &conf) == 21);
    CHECK (std::string (keyName (21)) == "Am");
    CHECK (camelotNumber (21) == 8 && camelotIsMinor (21));
    // G major: G - C - D - G
    const auto g = chords ({ { 55, 59, 62 }, { 60, 64, 67 }, { 62, 66, 69 }, { 55, 59, 62 } }, sr, 1.5, 3);
    CHECK (detectKey (g, sr) == 7);
    CHECK (camelotNumber (7) == 9 && ! camelotIsMinor (7));
}

MP_TEST (loudness_matches_bs1770_calibration)
{
    for (double sr : { 44100.0, 48000.0 })
    {
        const auto frames = (std::size_t) (sr * 10.0);
        std::vector<float> x (frames * 2);
        for (std::size_t i = 0; i < frames; ++i) x[i * 2] = x[i * 2 + 1] = 0.1f * (float) std::sin (2.0 * kPi * 997.0 * (double) i / sr);
        CHECK_NEAR (integratedLoudness (x.data(), (std::int64_t) frames, sr), -20.0, 0.1);
    }
}

MP_TEST (waveform_colours_follow_the_spectrum)
{
    const double sr = 48000.0;
    auto make = [&] (double hz) {
        std::vector<float> x ((std::size_t) sr * 2);
        for (std::size_t i = 0; i < x.size() / 2; ++i) x[i * 2] = x[i * 2 + 1] = 0.5f * (float) std::sin (2.0 * kPi * hz * (double) i / sr);
        return buildWaveform (x.data(), (std::int64_t) x.size() / 2, sr, 100.0);
    };
    const auto lo = make (50.0), hi = make (9000.0);
    CHECK (lo.size() == 100);
    CHECK (lo.low[50] > 3 * lo.high[50] && lo.low[50] > 2 * lo.mid[50]);
    CHECK (hi.high[50] > 3 * hi.low[50] && hi.high[50] > 2 * hi.mid[50]);
    CHECK (lo.peak[50] == 255);
}

MP_TEST (dsp_stems_route_drums_bass_and_centre_vocal)
{
    const double sr = 44100.0;
    const auto frames = (std::size_t) (sr * 8.0);
    std::vector<float> mix (frames * 2, 0.0f);
    std::vector<float> clickMask (frames, 0.0f);
    std::mt19937 rng (11);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    for (std::size_t i = 0; i < frames; ++i)
    {
        const double t = (double) i / sr;
        // centre "vocal": 330 Hz with harmonics and vibrato
        const double ph = 2.0 * kPi * 330.0 * t + 0.6 * std::sin (2.0 * kPi * 5.0 * t);
        const float voc = 0.12f * (float) (std::sin (ph) + 0.5 * std::sin (2 * ph) + 0.25 * std::sin (3 * ph));
        // hard-left "guitar": 523 Hz
        const float gtr = 0.12f * (float) std::sin (2.0 * kPi * 523.0 * t);
        // centre bass 55 Hz
        const float bass = 0.25f * (float) std::sin (2.0 * kPi * 55.0 * t);
        mix[i * 2] = voc + gtr + bass;
        mix[i * 2 + 1] = voc + bass;
    }
    for (std::size_t b = 0; b * 11025 < frames; ++b)        // noise clicks every 1/4 s
        for (std::size_t i = 0; i < 400 && b * 11025 + i < frames; ++i)
        {
            const float c = 0.4f * u (rng) * (float) std::exp (-(double) i / 80.0);
            mix[(b * 11025 + i) * 2] += c;
            mix[(b * 11025 + i) * 2 + 1] += c;
            clickMask[b * 11025 + i] = 1.0f;
        }
    TrackAudio a;
    a.setMix (mix, sr);
    StemSeparation st;
    DspStemSeparator sep;
    REQUIRE (sep.separate (a, st, {}));
    REQUIRE (st.drums.size() == mix.size());

    auto energy = [&] (const std::vector<float>& x, bool inClicks) {
        double e = 0.0;
        for (std::size_t i = (std::size_t) sr; i < frames - (std::size_t) sr; ++i)
            if ((clickMask[i] > 0.0f) == inClicks) e += (double) x[i * 2] * x[i * 2] + (double) x[i * 2 + 1] * x[i * 2 + 1];
        return e;
    };
    // drums: energy concentrated in the clicks
    const double dIn = energy (st.drums, true), dOut = energy (st.drums, false);
    CHECK (dIn > 3.0 * dOut);
    // bass stem: the 55 Hz tone, little of the rest -> correlate with the bass signal
    double num = 0.0, den1 = 0.0, den2 = 0.0;
    for (std::size_t i = (std::size_t) sr; i < frames - (std::size_t) sr; ++i)
    {
        const double ref = 0.25 * std::sin (2.0 * kPi * 55.0 * (double) i / sr);
        num += st.bass[i * 2 + 1] * ref; den1 += st.bass[i * 2 + 1] * st.bass[i * 2 + 1]; den2 += ref * ref;
    }
    CHECK (num / std::sqrt (den1 * den2) > 0.9);
    // vocal stem: the centred voice survives, the hard-left guitar mostly does not
    double vL = 0.0, vR = 0.0;
    for (std::size_t i = (std::size_t) sr; i < frames - (std::size_t) sr; ++i) { vL += st.vocals[i * 2] * st.vocals[i * 2]; vR += st.vocals[i * 2 + 1] * st.vocals[i * 2 + 1]; }
    CHECK (vR > 0.0);
    CHECK (vL / vR < 1.5);                                  // the left-only guitar adds little to the left vocal channel
}
