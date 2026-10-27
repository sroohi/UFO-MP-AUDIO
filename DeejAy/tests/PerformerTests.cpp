// Performer (master-bus performance FX) tests.
#include "TestHarness.h"
#include "dj/Engine.h"
#include "dj/Performer.h"
#include <cmath>
#include <memory>
#include <random>
#include <vector>

using namespace dj;

namespace
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

struct PerfRig
{
    Performer p;
    double beat = 0.0;
    double bpm = 120.0;
    PerfRig() { p.prepare (kSr, kBlock); }

    // runs `in` through the performer in blocks, advancing a beat clock (pass clock = false for "no leader")
    void run (std::vector<float>& l, std::vector<float>& r, bool clock = true)
    {
        for (std::size_t i = 0; i < l.size(); i += kBlock)
        {
            const int n = (int) std::min<std::size_t> (kBlock, l.size() - i);
            p.process (l.data() + i, r.data() + i, n, bpm, clock ? beat : -1.0);
            beat += n * bpm / 60.0 / kSr;
        }
    }
    void set (PerformerFx fx, bool on, float x = -1.0f, float y = -1.0f)
    {
        if (x >= 0.0f) p.controls.x[(int) fx].store (x);
        if (y >= 0.0f) p.controls.y[(int) fx].store (y);
        p.controls.on[(int) fx].store (on);
    }
};

std::vector<float> sine (double hz, int n, float amp = 0.5f)
{
    std::vector<float> v ((std::size_t) n);
    for (int i = 0; i < n; ++i) v[(std::size_t) i] = amp * (float) std::sin (2.0 * kPi * hz * i / kSr);
    return v;
}

double rms (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double s = 0.0;
    for (std::size_t i = from; i < to; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / (double) std::max<std::size_t> (1, to - from));
}

int zeroCrossings (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    int z = 0;
    for (std::size_t i = from + 1; i < to; ++i) if ((v[i - 1] < 0.0f) != (v[i] < 0.0f)) ++z;
    return z;
}

bool finite (const std::vector<float>& v)
{
    for (float x : v) if (! std::isfinite (x)) return false;
    return true;
}
}

MP_TEST (performer_is_transparent_when_nothing_is_on)
{
    PerfRig r;
    auto l = sine (440.0, 48000), rr = sine (550.0, 48000);
    const auto l0 = l, r0 = rr;
    r.run (l, rr);
    bool same = true;
    for (std::size_t i = 0; i < l.size(); ++i) same = same && l[i] == l0[i] && rr[i] == r0[i];
    CHECK (same);
}

MP_TEST (performer_stutter_repeats_a_grid_slice)
{
    PerfRig r;
    // a ramp makes every sample unique, so repetition is unambiguous
    std::vector<float> l (96000), rr;
    for (std::size_t i = 0; i < l.size(); ++i) l[i] = (float) i / 96000.0f;
    rr = l;
    r.set (PerformerFx::Stutter, true, 0.30f, 0.0f);   // 1/8 beat at 120 BPM = 3000 samples
    CHECK_NEAR (perf::stutterSliceBeats (0.30f), 0.125, 1e-12);
    r.run (l, rr);
    const std::size_t slice = 3000;
    double worst = 0.0;
    for (std::size_t i = 6000; i < 90000; ++i) worst = std::max (worst, (double) std::abs (l[i] - l[i + slice]));
    CHECK (worst < 1e-6);
    // the slice is the first 1/8 beat of the track (pressed on the beat)
    CHECK_NEAR (l[6000 + 1500], 1500.0f / 96000.0f, 1e-6);
}

MP_TEST (performer_stutter_build_up_shortens_the_slice)
{
    PerfRig r;
    std::vector<float> l (5 * 24000), rr;
    for (std::size_t i = 0; i < l.size(); ++i) l[i] = (float) i / (float) l.size();
    rr = l;
    r.set (PerformerFx::Stutter, true, 0.0f, 1.0f);    // 1/4 beat, halving every beat
    r.run (l, rr);
    // beat 3..4: two halvings -> 1/16 beat = 1500 samples
    double worst = 0.0;
    for (std::size_t i = 3 * 24000 + 200; i < 4 * 24000 - 1500 - 200; ++i) worst = std::max (worst, (double) std::abs (l[i] - l[i + 1500]));
    CHECK (worst < 1e-6);
}

MP_TEST (performer_note_repeat_recaptures_each_beat_and_gates)
{
    PerfRig r;
    std::vector<float> l (4 * 24000), rr;
    for (std::size_t i = 0; i < l.size(); ++i) l[i] = 0.1f + (float) i / (float) l.size();
    rr = l;
    r.set (PerformerFx::NoteRepeat, true, 0.70f, 0.5f);  // 1/16 beat = 1500 samples, half open
    CHECK_NEAR (perf::noteRepeatBeats (0.70f), 1.0 / 16, 1e-12);
    r.run (l, rr);
    // within beat 2 every repeat starts with the beat's first sample
    const std::size_t beat2 = 2 * 24000;
    CHECK_NEAR (l[beat2 + 1500 + 200], 0.1f + (float) (beat2 + 200) / (float) l.size(), 1e-5);
    CHECK_NEAR (l[beat2 + 3000 + 200], 0.1f + (float) (beat2 + 200) / (float) l.size(), 1e-5);
    // the gate closes after half of each repeat
    CHECK_NEAR (l[beat2 + 1500 + 1100], 0.0f, 1e-6);
    // the next beat captures new material
    CHECK_NEAR (l[3 * 24000 + 200], 0.1f + (float) (3 * 24000 + 200) / (float) l.size(), 1e-5);
}

MP_TEST (performer_tape_stop_winds_down_to_silence_and_recovers)
{
    PerfRig r;
    auto l = sine (220.0, 4 * 24000, 0.8f), rr = l;
    r.set (PerformerFx::TapeStop, true, 0.5f, 0.0f);    // 1 beat
    CHECK_NEAR (perf::tapeStopBeats (0.5f), 1.0, 1e-12);
    r.run (l, rr);
    CHECK (rms (l, 0, 2400) > 0.3);                     // still playing at the start
    CHECK (rms (l, 26000, 48000) < 1e-6);               // stopped after one beat
    // the frequency falls as the tape slows (fewer zero crossings in the second half of the stop)
    CHECK (zeroCrossings (l, 12000, 18000) < zeroCrossings (l, 0, 6000));
    r.set (PerformerFx::TapeStop, false);
    auto l2 = sine (220.0, 24000, 0.8f), r2 = l2;
    r.run (l2, r2);
    CHECK (rms (l2, 2400, 24000) > 0.5);                // back to the live signal
}

MP_TEST (performer_pitch_shifts_an_octave)
{
    PerfRig r;
    auto l = sine (440.0, 48000), rr = l;
    r.set (PerformerFx::Pitch, true, 1.0f, 1.0f);       // +12 semitones, fully wet
    CHECK (perf::pitchSemitones (1.0f) == 12);
    r.run (l, rr);
    const int z = zeroCrossings (l, 9600, 48000);       // 0.8 s
    CHECK_NEAR (z, 2.0 * 880.0 * 0.8, 2.0 * 880.0 * 0.8 * 0.04);
    CHECK (rms (l, 9600, 48000) > 0.25);
}

MP_TEST (performer_filter_cuts_highs_and_lows)
{
    {
        PerfRig r;
        auto hi = sine (8000.0, 24000), hr = hi;
        r.set (PerformerFx::Filter, true, 0.10f, 0.0f);  // low-pass, well closed
        r.run (hi, hr);
        CHECK (rms (hi, 4800, 24000) < 0.35 * 0.1);      // > 20 dB down
    }
    {
        PerfRig r;
        auto lo = sine (60.0, 24000), lr = lo;
        r.set (PerformerFx::Filter, true, 0.95f, 0.0f);  // high-pass, well closed
        r.run (lo, lr);
        CHECK (rms (lo, 4800, 24000) < 0.35 * 0.1);
    }
}

MP_TEST (performer_bitcrusher_quantizes)
{
    PerfRig r;
    auto l = sine (100.0, 24000), rr = l;
    r.set (PerformerFx::Bitcrusher, true, 1.0f, 0.0f);  // 3 bits: steps of 1/4
    r.run (l, rr);
    bool quantized = true;
    for (std::size_t i = 2400; i < l.size(); ++i)
    {
        const float q = l[i] * 4.0f;
        quantized = quantized && std::abs (q - std::round (q)) < 1e-5f;
    }
    CHECK (quantized);
}

MP_TEST (performer_delay_and_reverb_ring_out_after_switching_off)
{
    for (auto fx : { PerformerFx::Delay, PerformerFx::Reverb })
    {
        PerfRig r;
        auto l = sine (330.0, 24000, 0.5f), rr = l;
        r.set (fx, true, 0.5f, 0.6f);
        r.run (l, rr);
        r.set (fx, false);
        std::vector<float> sl (24000, 0.0f), sr (24000, 0.0f);   // silence in, tails out
        r.run (sl, sr);
        CHECK (rms (sl, 0, 12000) > 0.005);
        CHECK (r.p.view.sounding[(int) fx].load());
    }
}

MP_TEST (performer_talkbox_shapes_without_blowing_up)
{
    PerfRig r;
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> u (-0.5f, 0.5f);
    std::vector<float> l (48000), rr (48000);
    for (std::size_t i = 0; i < l.size(); ++i) { l[i] = u (rng); rr[i] = u (rng); }
    const double inRms = rms (l, 4800, 48000);
    r.set (PerformerFx::Talkbox, true, 0.0f, 0.6f);     // vowel sweep on
    r.run (l, rr);
    const double outRms = rms (l, 4800, 48000);
    CHECK (finite (l));
    CHECK (outRms > inRms * 0.1);
    CHECK (outRms < inRms * 3.0);
}

MP_TEST (performer_everything_at_once_stays_finite_and_bounded)
{
    PerfRig r;
    std::mt19937 rng (3);
    std::uniform_real_distribution<float> u (-0.9f, 0.9f);
    std::vector<float> l (96000), rr (96000);
    for (std::size_t i = 0; i < l.size(); ++i) { l[i] = u (rng); rr[i] = u (rng); }
    for (int k = 0; k < kNumPerformerFx; ++k) r.p.controls.on[k].store (true);
    r.p.controls.y[(int) PerformerFx::Delay].store (1.0f);
    r.run (l, rr, false);                                  // no leader: internal clock
    float peak = 0.0f;
    for (float x : l) peak = std::max (peak, std::abs (x));
    CHECK (finite (l) && finite (rr));
    CHECK (peak < 20.0f);
}

MP_TEST (performer_parameter_texts)
{
    CHECK (performerParamText (PerformerFx::Stutter, 0, 0.30f) == "1/8");
    CHECK (performerParamText (PerformerFx::NoteRepeat, 0, 0.30f) == "1/6");
    CHECK (performerParamText (PerformerFx::Pitch, 0, 0.0f) == "-12 st");
    CHECK (performerParamText (PerformerFx::Pitch, 0, 0.75f) == "+6 st");
    CHECK (performerParamText (PerformerFx::Filter, 0, 0.5f) == "open");
    CHECK (performerParamText (PerformerFx::Talkbox, 0, 0.5f) == "I");
    CHECK (performerParamText (PerformerFx::Talkbox, 1, 0.0f) == "off");
    CHECK (performerParamText (PerformerFx::Bitcrusher, 0, 1.0f) == "3 bit");
    CHECK (performerParamText (PerformerFx::Delay, 0, 0.40f) == "3/8");
}

MP_TEST (performer_runs_on_the_engine_master_bus)
{
    Engine e;
    e.mixer.limiter.store (false);
    e.prepare (kSr, kBlock);
    auto t = std::make_shared<TrackAudio>();
    std::vector<float> mix (48000 * 10 * 2);
    for (std::size_t i = 0; i < mix.size() / 2; ++i) mix[i * 2] = mix[i * 2 + 1] = 0.5f * (float) std::sin (2.0 * kPi * 200.0 * (double) i / kSr);
    t->setMix (std::move (mix), kSr);
    e.loadDeck (0, t);
    for (auto& c : e.mixer.ch) { c.xfAssign.store ((int) XfAssign::Thru); c.fader.store (1.0f); }
    Command g; g.type = Cmd::DeckSetGrid; g.deck = 0; g.value = 120.0; e.post (g);
    e.deckCommand (Cmd::Play, 0);
    std::vector<float> out[4];
    float* ptr[4];
    for (int k = 0; k < 4; ++k) { out[k].assign (kBlock, 0.0f); ptr[k] = out[k].data(); }
    auto blockRms = [&]
    {
        double s = 0.0;
        for (float x : out[0]) s += (double) x * x;
        return std::sqrt (s / kBlock);
    };
    for (int i = 0; i < 20; ++i) { for (auto& o : out) std::fill (o.begin(), o.end(), 0.0f); e.process (nullptr, 0, ptr, 4, kBlock); }
    CHECK (blockRms() > 0.1);
    e.performer.controls.x[(int) PerformerFx::TapeStop].store (0.0f);   // 1/4 beat
    e.performer.controls.on[(int) PerformerFx::TapeStop].store (true);
    for (int i = 0; i < 40; ++i) { for (auto& o : out) std::fill (o.begin(), o.end(), 0.0f); e.process (nullptr, 0, ptr, 4, kBlock); }
    CHECK (blockRms() < 1e-6);                                       // the whole mix is stopped
    CHECK (e.deck (0).playing());                                    // while the deck keeps playing
}
