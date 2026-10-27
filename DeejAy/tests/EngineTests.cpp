// DEEJ-AY engine tests. Sample-exact where the behaviour is exact; tolerance-based for DSP.
#include "TestHarness.h"
#include "dj/Engine.h"
#include <cstdio>
#include <memory>
#include <random>

using namespace dj;

namespace
{
constexpr double kSr = 48000.0;
constexpr double kScale = 1.0e-6;   // ramp track: left sample = frame index * kScale
constexpr double kFpb120 = 24000.0; // frames per beat at 120 BPM, 48 kHz

// Left channel encodes the frame index (Catmull-Rom reproduces a linear ramp exactly), right is constant.
std::shared_ptr<TrackAudio> rampTrack (double seconds = 20.0, double sr = kSr)
{
    auto t = std::make_shared<TrackAudio>();
    const auto frames = (std::size_t) (seconds * sr);
    std::vector<float> v (frames * 2);
    for (std::size_t i = 0; i < frames; ++i) { v[i * 2] = (float) ((double) i * kScale); v[i * 2 + 1] = 0.25f; }
    t->setMix (std::move (v), sr);
    return t;
}

std::shared_ptr<TrackAudio> sineTrack (double hz, double seconds = 4.0, double sr = kSr, float amp = 0.5f)
{
    auto t = std::make_shared<TrackAudio>();
    const auto frames = (std::size_t) (seconds * sr);
    std::vector<float> v (frames * 2);
    for (std::size_t i = 0; i < frames; ++i)
        v[i * 2] = v[i * 2 + 1] = amp * (float) std::sin (2.0 * kPi * hz * (double) i / sr);
    t->setMix (std::move (v), sr);
    return t;
}

// Frequency from rising zero crossings (linear interpolation of the crossing instant).
double estimateHz (const std::vector<float>& x, double sr)
{
    double first = -1.0, last = -1.0;
    int count = 0;
    for (std::size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = (double) (i - 1) + (double) (-x[i - 1] / (x[i] - x[i - 1]));
            if (first < 0.0) first = t; else ++count;
            last = t;
        }
    return count > 0 ? (double) count * sr / (last - first) : 0.0;
}

struct DeckRig
{
    Deck d;
    std::vector<float> bufs[kDeckBuses * 2];
    float* ptr[kDeckBuses * 2];
    bool active[kDeckBuses] {};
    DeckContext ctx;
    std::shared_ptr<TrackAudio> track;
    std::vector<float> outL, outR; // everything rendered, concatenated
    int block;

    explicit DeckRig (std::shared_ptr<TrackAudio> t, int blockSize = 256) : track (std::move (t)), block (blockSize)
    {
        d.prepare (kSr, block);
        for (int k = 0; k < kDeckBuses * 2; ++k) { bufs[k].assign ((std::size_t) block, 0.0f); ptr[k] = bufs[k].data(); }
        ctx.outputRate = kSr;
        ctx.quantize = false;
        d.load (track.get(), 1);
    }
    void grid (double bpm, double anchor = 0.0) { Beatgrid g; g.bpm = bpm; g.anchor = anchor; d.setGrid (g); }
    void run (int frames)
    {
        while (frames > 0)
        {
            const int n = std::min (frames, block);
            d.process (ptr, n, ctx, active);
            for (int i = 0; i < n; ++i)
            {
                outL.push_back (bufs[0][(std::size_t) i] + (active[1] ? bufs[2][(std::size_t) i] : 0.0f));
                outR.push_back (bufs[1][(std::size_t) i] + (active[1] ? bufs[3][(std::size_t) i] : 0.0f));
            }
            frames -= n;
        }
    }
    double heardFrame() const { return outL.back() / kScale; } // frame index of the last sample heard
};

struct EngineRig
{
    Engine e;
    int block = 512;
    std::vector<float> out[4];
    float* ptr[4];
    EngineRig()
    {
        e.mixer.limiter.store (false);
        for (auto& c : e.mixer.ch) { c.xfAssign.store ((int) XfAssign::Thru); c.fader.store (1.0f); }
        e.prepare (kSr, block);
        for (int k = 0; k < 4; ++k) { out[k].assign ((std::size_t) block, 0.0f); ptr[k] = out[k].data(); }
    }
    void run (int frames, std::vector<float>* master = nullptr, std::vector<float>* phones = nullptr)
    {
        while (frames > 0)
        {
            const int n = std::min (frames, block);
            e.process (nullptr, 0, ptr, 4, n);
            if (master) master->insert (master->end(), out[0].begin(), out[0].begin() + n);
            if (phones) phones->insert (phones->end(), out[2].begin(), out[2].begin() + n);
            frames -= n;
            e.pollEvents ([] (const DeckEvent&) {});
        }
    }
};
}

// ============================================================================== transport
MP_TEST (play_is_bit_exact_at_unity)
{
    DeckRig r (rampTrack());
    r.d.play();
    r.run (1000);
    CHECK_NEAR (r.d.position(), 1000.0, 1e-9);
    for (int i = 0; i < 1000; ++i)
        if (r.outL[(std::size_t) i] != (float) ((double) i * kScale)) { CHECK (false); break; }
}

MP_TEST (tempo_fader_and_sample_rate_conversion)
{
    DeckRig r (rampTrack (20.0, 44100.0));
    r.d.inputs.tempoFader.store (1.0f);          // "+" end, range +/-8 %
    r.d.inputs.tempoFaderMoved.store (true);
    r.d.play();
    r.run (48000);
    CHECK_NEAR (r.d.tempo(), 0.08, 1e-9);
    CHECK_NEAR (r.d.position(), 44100.0 * 1.08, 1e-3);
    r.d.cycleTempoRange();                       // -> 16 %: soft takeover keeps 8 % until the fader passes it
    r.d.inputs.tempoFader.store (0.9f);
    r.d.inputs.tempoFaderMoved.store (true);
    r.run (256);
    CHECK_NEAR (r.d.tempo(), 0.08, 1e-9);
    r.d.inputs.tempoFader.store (0.7f);          // crosses 0.08 (=0.75) -> picks up at +6.4 %
    r.d.inputs.tempoFaderMoved.store (true);
    r.run (256);
    CHECK_NEAR (r.d.tempo(), 0.064, 1e-6);
}

MP_TEST (cdj_cue_logic)
{
    DeckRig r (rampTrack());
    r.d.setPosition (5000.0);
    r.run (256);
    r.d.cueDown(); r.d.cueUp();                  // paused away from cue: set cue here
    r.run (256);
    r.d.play();
    r.run (2048);
    r.d.cueDown();                               // playing: back cue + pause
    r.run (512);
    CHECK (! r.d.playing());
    CHECK_NEAR (r.d.position(), 5000.0, 1e-9);
    r.d.cueUp();
    r.d.cueDown();                               // paused at cue: preview while held
    r.run (480);
    CHECK (r.d.playing());
    CHECK_NEAR (r.d.position(), 5480.0, 1e-9);
    r.d.cueUp();                                 // release: back to cue, paused
    r.run (256);
    CHECK (! r.d.playing());
    CHECK_NEAR (r.d.position(), 5000.0, 1e-9);
    r.d.cueDown(); r.d.playPause(); r.d.cueUp(); // PLAY while previewing latches playback
    r.run (256);
    CHECK (r.d.playing());
    CHECK_NEAR (r.d.position(), 5256.0, 1e-9);
}

MP_TEST (brake_slows_to_a_stop)
{
    DeckRig r (rampTrack());
    r.d.inputs.brakeSeconds.store (0.5f);
    r.d.play();
    r.run (4800);
    r.d.pause();
    r.run (48000);
    // linear deceleration over 0.5 s covers half the distance of full speed
    CHECK_NEAR (r.d.position(), 4800.0 + 12000.0, 2.0);
    CHECK_NEAR (r.outL.back(), 0.0, 1e-9);
}

MP_TEST (track_end_stops_and_reports)
{
    DeckRig r (rampTrack (0.1));
    r.d.play();
    r.run (6000);
    CHECK (! r.d.playing());
    int ended = 0;
    r.d.drainEvents ([&] (const DeckEvent& e) { if (e.kind == DeckEvent::Kind::TrackEnded) ++ended; });
    CHECK (ended == 1);
}

// ================================================================================ hot cues
MP_TEST (hot_cue_set_jump_delete_and_quantize)
{
    DeckRig r (rampTrack());
    r.grid (120.0);
    r.d.setPosition (2000.0);
    r.run (256);
    r.d.hotCueDown (0, false); r.d.hotCueUp (0); // empty: sets at the playhead
    r.d.play();
    r.run (8000);
    r.d.hotCueDown (0, false);                    // set: jumps (and keeps playing)
    r.run (500);
    r.d.hotCueUp (0);
    CHECK_NEAR (r.d.position(), 2500.0, 1e-9);
    CHECK_NEAR (r.heardFrame(), 2499.0, 1e-3);
    r.d.hotCueDown (0, true);                     // SHIFT: delete
    CHECK (r.d.view.hotCue[0].load() < 0.0 || true);
    r.run (1);
    CHECK (r.d.view.hotCue[0].load() < 0.0);

    // quantize: cues snap to the grid, triggers keep the playhead's offset within the beat
    r.ctx.quantize = true;
    r.d.setPosition (25000.0);
    r.run (1);
    r.d.pause();
    r.run (1);
    r.d.hotCueDown (1, false); r.d.hotCueUp (1);
    r.run (1);
    CHECK_NEAR (r.d.view.hotCue[1].load(), kFpb120, 1e-9);
    r.d.setPosition (kFpb120 * 3.0 + 500.0);
    r.d.play();
    r.run (1);
    const double offset = r.d.position() - kFpb120 * 3.0;
    r.d.hotCueDown (1, false); r.d.hotCueUp (1);
    CHECK_NEAR (r.d.position(), kFpb120 + offset, 1e-6);
}

MP_TEST (hot_cue_gate_when_paused)
{
    DeckRig r (rampTrack());
    r.d.inputs.hotCueWhenPaused.store ((int) HotCueWhenPaused::Gate);
    r.d.setHotCue (3, 10000.0);
    r.d.hotCueDown (3, false);
    r.run (1000);
    CHECK (r.d.playing());
    r.d.hotCueUp (3);
    r.run (10);
    CHECK (! r.d.playing());
    CHECK_NEAR (r.d.position(), 10000.0, 1e-9);
}

// =================================================================================== loops
MP_TEST (auto_loop_wraps_sample_exactly)
{
    DeckRig r (rampTrack());
    r.grid (120.0);
    r.ctx.quantize = true;
    r.d.setPosition (kFpb120 * 2.0 + 1000.0);
    r.run (1);
    r.d.play();
    r.run (1);
    r.d.autoLoop (4.0);
    r.run (1);
    CHECK_NEAR (r.d.view.loopIn.load(), kFpb120 * 2.0, 1e-9);
    CHECK_NEAR (r.d.view.loopOut.load(), kFpb120 * 6.0, 1e-9);
    const double p0 = r.d.position();
    r.run (100000);
    const double expect = kFpb120 * 2.0 + std::fmod (p0 + 100000.0 - kFpb120 * 2.0, kFpb120 * 4.0);
    CHECK_NEAR (r.d.position(), expect, 1e-6);
    CHECK_NEAR (r.heardFrame(), expect - 1.0, 0.01);
    r.d.autoLoop (4.0);                          // same size again: exit
    r.run (1);
    CHECK (! r.d.view.loopActive.load());
}

MP_TEST (manual_loop_halve_double_reloop)
{
    DeckRig r (rampTrack());
    r.grid (120.0);
    r.d.setPosition (10000.0);
    r.d.play();
    r.run (1);
    r.d.loopIn();
    r.run (30000);
    r.d.loopOut();
    r.run (1);
    CHECK (r.d.view.loopActive.load());
    const double in = r.d.view.loopIn.load(), out = r.d.view.loopOut.load();
    CHECK_NEAR (in, 10001.0, 1e-6);
    CHECK_NEAR (out, 40001.0, 1e-6);
    r.d.loopIn();                                // in loop: 1/2X
    r.run (1);
    CHECK_NEAR (r.d.view.loopOut.load(), in + (out - in) * 0.5, 1e-6);
    r.d.loopOut();                               // in loop: 2X
    r.run (1);
    CHECK_NEAR (r.d.view.loopOut.load(), out, 1e-6);
    r.d.fourBeatExit();                          // in loop: exit
    r.run (1);
    CHECK (! r.d.view.loopActive.load());
    r.d.reloop();                                // SHIFT + LOOP OUT: reloop jumps to loop in
    r.run (100);
    CHECK (r.d.view.loopActive.load());
    CHECK_NEAR (r.d.position(), in + 100.0, 1e-6);
}

MP_TEST (serato_loop_slots_lock_and_delete)
{
    DeckRig r (rampTrack());
    r.grid (120.0);
    r.d.play();
    r.run (1000);
    r.d.autoLoop (2.0);
    r.run (1);
    CHECK (r.d.view.loopSlotOut[0].load() > 0.0);   // loops are saved into the selected slot
    r.d.toggleLoopSlotLock();
    r.d.autoLoop (2.0);                             // exit
    r.d.autoLoop (8.0);                             // new loop: slot 0 is locked, not overwritten
    r.run (1);
    CHECK_NEAR (r.d.view.loopSlotOut[0].load() - r.d.view.loopSlotIn[0].load(), 2.0 * kFpb120, 1e-6);
    r.d.selectLoopSlot (1);
    r.d.autoLoop (1.0);                             // resize the active loop: saved to slot 1
    r.run (1);
    CHECK_NEAR (r.d.view.loopSlotOut[1].load() - r.d.view.loopSlotIn[1].load(), 1.0 * kFpb120, 1e-6);
    r.d.deleteLoopSlot();
    r.run (1);
    CHECK (r.d.view.loopSlotOut[1].load() < 0.0);
    r.d.selectLoopSlot (-1);
    r.d.deleteLoopSlot();                           // locked: stays
    r.run (1);
    CHECK (r.d.view.loopSlotOut[0].load() > 0.0);
}

MP_TEST (roll_returns_to_background_position)
{
    DeckRig r (rampTrack());
    r.grid (120.0);
    r.d.setPosition (kFpb120 * 4.0 + 3000.0);
    r.d.play();
    r.run (1);
    const double p0 = r.d.position();
    r.d.rollDown (0.25);                          // 1/4-beat roll = 6000 frames
    r.run (30000);
    CHECK (r.d.position() < kFpb120 * 4.0 + 6000.0);
    r.d.rollUp();
    r.run (1);
    CHECK_NEAR (r.d.position(), p0 + 30001.0, 1e-6);
}

MP_TEST (censor_reverses_then_slips_back_and_auto_cancels)
{
    DeckRig r (rampTrack());
    r.grid (120.0);
    r.d.setPosition (100000.0);
    r.d.play();
    r.run (1);
    const double p0 = r.d.position();
    r.d.censorDown();
    r.run (10000);
    CHECK_NEAR (r.d.position(), p0 - 10000.0, 1e-6);
    r.d.censorUp();
    r.run (1);
    CHECK_NEAR (r.d.position(), p0 + 10001.0, 1e-6);
    // held longer than 8 beats: cancels by itself
    r.d.censorDown();
    r.run ((int) (kFpb120 * 8.0) + 1000);
    CHECK (! r.d.view.censor.load());
}

MP_TEST (slip_scratch_returns_to_background)
{
    DeckRig r (rampTrack());
    r.d.setSlip (true);
    r.d.setPosition (200000.0);
    r.d.play();
    r.run (1);
    const double p0 = r.d.position();
    r.d.jogTouch (true);
    for (int k = 0; k < 40; ++k) { r.d.inputs.scratchTicks.store (k % 2 == 0 ? 30 : -30); r.run (256); }
    r.d.jogTouch (false);
    r.run (1);
    CHECK_NEAR (r.d.position(), p0 + 40.0 * 256.0 + 1.0, 1e-6);
}

MP_TEST (scratch_follows_the_platter)
{
    DeckRig r (rampTrack());
    r.d.setPosition (100000.0);
    r.run (1);
    r.d.jogTouch (true);
    // one full turn (720 ticks) forward over 0.5 s = 1.8 s of audio
    for (int k = 0; k < 90; ++k) { r.d.inputs.scratchTicks.store (8); r.run (256); }
    for (int k = 0; k < 40; ++k) r.run (256);     // hand stops: the filter settles
    CHECK_NEAR (r.d.position(), 100000.0 + 1.8 * kSr, 0.02 * kSr);
    r.d.jogTouch (false);
}

MP_TEST (beat_jump_moves_an_active_loop)
{
    DeckRig r (rampTrack());
    r.grid (120.0);
    r.d.setPosition (kFpb120 * 8.0);
    r.d.play();
    r.run (1);
    r.d.autoLoop (4.0);
    r.run (1);
    const double in = r.d.view.loopIn.load(), pos = r.d.position();
    r.d.beatJump (4.0);
    r.run (1);
    CHECK_NEAR (r.d.view.loopIn.load(), in + 4.0 * kFpb120, 1e-6);
    CHECK_NEAR (r.d.position(), pos + 4.0 * kFpb120 + 1.0, 1e-6);
}

MP_TEST (slicer_plays_slice_and_slips_back)
{
    DeckRig r (rampTrack());
    r.grid (120.0);
    r.d.setPosition (kFpb120 * 8.0 + 2000.0);    // domain of 8 beats starts at beat 8
    r.d.play();
    r.run (1);
    r.d.setSlicer (SlicerMode::Slicer, 8.0, 1.0);
    r.run (1);
    CHECK_NEAR (r.d.view.slicerDomainStart.load(), kFpb120 * 8.0, 1e-6);
    const double p0 = r.d.position();
    r.d.sliceDown (5);
    r.run (1);
    const double sliceStart = kFpb120 * (8.0 + 5.0);
    CHECK (r.d.position() >= sliceStart && r.d.position() < sliceStart + kFpb120);
    CHECK_NEAR (r.d.position() - sliceStart, p0 - kFpb120 * 8.0 + 1.0, 1e-6); // phase kept
    r.run (60000);                                // the slice repeats while held
    CHECK (r.d.position() >= sliceStart && r.d.position() < sliceStart + kFpb120);
    r.d.sliceUp (5);
    r.run (1);
    CHECK_NEAR (r.d.position(), p0 + 60002.0, 1e-6);
}

// ======================================================================== key lock / pitch
MP_TEST (keylock_keeps_pitch_while_tempo_changes)
{
    DeckRig r (sineTrack (440.0, 6.0));
    r.d.inputs.tempoFader.store (1.0f);           // +8 %
    r.d.inputs.tempoFaderMoved.store (true);
    r.d.setKeylock (true);
    r.d.play();
    r.run (96000);
    std::vector<float> tail (r.outL.end() - 48000, r.outL.end());
    CHECK_NEAR (estimateHz (tail, kSr), 440.0, 440.0 * 0.01);
    CHECK_NEAR (r.d.position(), 96000.0 * 1.08, 1.0);

    DeckRig v (sineTrack (440.0, 6.0));           // without key lock the pitch follows the tempo
    v.d.inputs.tempoFader.store (1.0f);
    v.d.inputs.tempoFaderMoved.store (true);
    v.d.play();
    v.run (48000);
    std::vector<float> t2 (v.outL.end() - 24000, v.outL.end());
    CHECK_NEAR (estimateHz (t2, kSr), 440.0 * 1.08, 1.0);
}

MP_TEST (key_shift_and_pitch_play)
{
    DeckRig r (sineTrack (330.0, 6.0));
    r.d.setKeylock (true);
    r.d.setKeyShift (7);                           // a fifth up
    r.d.play();
    r.run (48000);
    std::vector<float> tail (r.outL.end() - 24000, r.outL.end());
    CHECK_NEAR (estimateHz (tail, kSr), 330.0 * std::pow (2.0, 7.0 / 12.0), 330.0 * 0.015);
    CHECK_NEAR (r.d.position(), 48000.0, 1e-6);    // tempo untouched

    r.d.setHotCue (2, 1000.0);
    r.d.setPitchPlayCue (2);
    r.d.pitchPlay (pitchPlaySemitones (PitchPlayRange::Down, 4, 0)); // pad 5 of "Down" = -7
    r.run (1);
    CHECK (r.d.keyShift() == -7);
    CHECK_NEAR (r.d.position(), 1001.0, 1e-6);
    CHECK (pitchPlaySemitones (PitchPlayRange::Up, 0, 0) == 4);
    CHECK (pitchPlaySemitones (PitchPlayRange::Middle, 7, 0) == -1);
}

// =================================================================================== stems
namespace
{
std::shared_ptr<TrackAudio> stemTrack()
{
    const std::size_t frames = 48000;
    std::vector<float> v (frames * 2), b (frames * 2), d (frames * 2), mix (frames * 2);
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> u (-0.2f, 0.2f);
    for (std::size_t i = 0; i < frames; ++i)
    {
        const float vs = 0.2f * (float) std::sin (2.0 * kPi * 900.0 * (double) i / kSr);
        const float bs = 0.3f * (float) std::sin (2.0 * kPi * 55.0 * (double) i / kSr);
        const float ds = (i % 12000) < 800 ? u (rng) : 0.0f;
        const float ms = 0.1f * (float) std::sin (2.0 * kPi * 440.0 * (double) i / kSr);
        v[i * 2] = v[i * 2 + 1] = vs;
        b[i * 2] = b[i * 2 + 1] = bs;
        d[i * 2] = d[i * 2 + 1] = ds;
        mix[i * 2] = mix[i * 2 + 1] = vs + bs + ds + ms;
    }
    auto t = std::make_shared<TrackAudio>();
    t->setMix (mix, kSr);
    t->setStems (v, b, d);
    return t;
}
}

MP_TEST (stems_unity_is_the_mix_and_mutes_remove_parts)
{
    auto t = stemTrack();
    DeckRig r (t);
    r.d.play();
    r.run (2048);
    for (int i = 0; i < 2048; ++i) if (r.outL[(std::size_t) i] != t->mix[(std::size_t) i * 2]) { CHECK (false); break; }

    r.d.toggleStemMute (1 << (int) Stem::Vocals);
    r.run (4096);                                  // let the 6 ms mute ramp settle
    const std::size_t s = r.outL.size() - 1024;
    double err = 0.0;
    for (std::size_t i = s; i < r.outL.size(); ++i)
    {
        const double vs = 0.2 * std::sin (2.0 * kPi * 900.0 * (double) i / kSr);
        err = std::max (err, std::abs ((double) r.outL[i] - ((double) t->mix[i * 2] - vs)));
    }
    CHECK (err < 1.0e-4); // int16 stem storage
    CHECK (r.d.view.stemMuted[(int) Stem::Vocals].load());
}

MP_TEST (fx_part_split_buses_sum_to_the_mix)
{
    auto t = stemTrack();
    DeckRig r (t);
    for (int s = 0; s < kNumStems; ++s) r.ctx.fxPart[s] = s == (int) Stem::Vocals;
    r.d.play();
    r.run (2048);
    CHECK (r.active[1]);
    // bus 0 carries only the vocal
    double err = 0.0;
    for (int i = 0; i < 256; ++i)
    {
        const double vs = 0.2 * std::sin (2.0 * kPi * 900.0 * (double) (2048 - 256 + i) / kSr);
        err = std::max (err, std::abs ((double) r.bufs[0][(std::size_t) i] - vs));
    }
    CHECK (err < 1.0e-4);
    double sumErr = 0.0;
    for (std::size_t i = 0; i < 2048; ++i) sumErr = std::max (sumErr, (double) std::abs (r.outL[i] - t->mix[i * 2]));
    CHECK (sumErr < 1.0e-4);
}

// ==================================================================================== sync
MP_TEST (sync_matches_tempo_and_phase)
{
    EngineRig rig;
    auto a = rampTrack (30.0), b = rampTrack (30.0);
    rig.e.loadDeck (0, a);
    rig.e.loadDeck (1, b);
    Command g; g.type = Cmd::DeckSetGrid; g.deck = 0; g.value = 120.0; g.value2 = 0.0; rig.e.post (g);
    g.deck = 1; g.value = 126.0; g.value2 = 3000.0; rig.e.post (g);
    rig.e.deckCommand (Cmd::Play, 0);
    rig.e.deckCommand (Cmd::SetPosition, 1, 0, 50000.0);
    rig.run (4800);
    rig.e.deckCommand (Cmd::SyncOn, 1);
    rig.e.deckCommand (Cmd::Play, 1);
    rig.run (48000);
    auto& d0 = rig.e.deck (0);
    auto& d1 = rig.e.deck (1);
    CHECK (d1.sync());
    CHECK (rig.e.view.leader.load() == 0);
    CHECK_NEAR (d1.effectiveBpm(), 120.0, 1e-6);
    CHECK_NEAR (wrapHalf (d0.beatPhase() - d1.beatPhase()), 0.0, 0.002);
    // the leader's tempo change is followed
    d0.inputs.tempoFader.store (0.75f);           // +4 %
    d0.inputs.tempoFaderMoved.store (true);
    rig.run (48000);
    CHECK_NEAR (d1.effectiveBpm(), 124.8, 1e-6);
    CHECK_NEAR (wrapHalf (d0.beatPhase() - d1.beatPhase()), 0.0, 0.003);
}

MP_TEST (sync_matches_half_and_double_tempo)
{
    EngineRig rig;
    rig.e.loadDeck (0, rampTrack (30.0));
    rig.e.loadDeck (1, rampTrack (30.0));
    Command g; g.type = Cmd::DeckSetGrid; g.deck = 0; g.value = 140.0; rig.e.post (g);
    g.deck = 1; g.value = 70.5; rig.e.post (g);
    rig.e.deckCommand (Cmd::Play, 0);
    rig.e.deckCommand (Cmd::SyncOn, 1);
    rig.e.deckCommand (Cmd::Play, 1);
    rig.run (48000);
    CHECK_NEAR (rig.e.deck (1).effectiveBpm(), 70.0, 1e-6);
}

MP_TEST (key_sync_and_instant_doubles)
{
    EngineRig rig;
    rig.e.loadDeck (0, rampTrack (30.0));
    rig.e.loadDeck (1, rampTrack (30.0));
    rig.e.deckCommand (Cmd::DeckSetKey, 0, 0, 21.0); // A minor
    rig.e.deckCommand (Cmd::DeckSetKey, 1, 0, 12.0); // C minor
    rig.e.deckCommand (Cmd::Play, 0);
    rig.run (512);
    rig.e.deckCommand (Cmd::KeySync, 1);
    rig.run (512);
    CHECK (rig.e.deck (1).keyShift() == -3);         // Cm -> Am
    rig.e.instantDouble (0, 2);
    rig.run (512);
    CHECK (rig.e.deck (2).audio() == rig.e.deck (0).audio());
    CHECK_NEAR (rig.e.deck (2).position(), rig.e.deck (0).position(), 1e-6);
    CHECK (rig.e.deck (2).playing());
    rig.e.loadDeck (0, nullptr);                     // deck 2 keeps the shared audio alive
    rig.run (512);
    CHECK (rig.e.deck (2).audio() != nullptr);
    CHECK (rig.e.audioFor (rig.e.deckAudioId (2)) != nullptr);
}

// =================================================================================== mixer
MP_TEST (fader_and_crossfader_curves)
{
    CHECK_NEAR (crossfaderGain (0.0f, 0.0f, 0.0f), 1.0, 1e-6);
    CHECK_NEAR (crossfaderGain (1.0f, 0.0f, 0.0f), 0.0, 1e-6);
    CHECK_NEAR (crossfaderGain (0.5f, 0.0f, 0.0f), std::sqrt (0.5), 1e-5); // constant power
    CHECK_NEAR (crossfaderGain (0.9f, 1.0f, 0.0f), 1.0, 1e-6);            // scratch cut stays open
    CHECK_NEAR (crossfaderGain (0.99f, 0.0f, 0.02f), 0.0, 1e-6);          // cut lag: dead at the edge
    CHECK_NEAR (channelFaderGain (1.0f, 0.5f), 1.0, 1e-6);
    CHECK_NEAR (channelFaderGain (0.0f, 0.5f), 0.0, 1e-6);
    CHECK (channelFaderGain (0.5f, 1.0f) > channelFaderGain (0.5f, 0.0f));
    CHECK_NEAR (trimGain (0.5f), 1.0, 1e-6);
    CHECK_NEAR (gainToDb (trimGain (1.0f)), 12.0, 1e-3);
}

MP_TEST (eq_is_transparent_flat_and_kills)
{
    auto run = [] (EqMode mode, float low, double hz) {
        ChannelEq eq; eq.prepare (kSr);
        const float k[3] { 0.5f, 0.5f, low };
        std::vector<float> l (48000), r (48000);
        for (std::size_t i = 0; i < l.size(); ++i) l[i] = r[i] = (float) std::sin (2.0 * kPi * hz * (double) i / kSr);
        const auto in = l;
        for (std::size_t o = 0; o < l.size(); o += 512) eq.process (k, mode, l.data() + o, r.data() + o, (int) std::min<std::size_t> (512, l.size() - o));
        float pk = 0.0f, diff = 0.0f;
        for (std::size_t i = 24000; i < l.size(); ++i) { pk = std::max (pk, std::abs (l[i])); diff = std::max (diff, std::abs (l[i] - in[i])); }
        return std::make_pair (pk, diff);
    };
    CHECK (run (EqMode::Classic, 0.5f, 1000.0).second < 1.0e-5f);
    CHECK (gainToDb (run (EqMode::Classic, 0.0f, 30.0).first) < -18.0f);
    CHECK (gainToDb (run (EqMode::Isolator, 0.0f, 50.0).first) < -30.0f);
    CHECK_NEAR (run (EqMode::Isolator, 0.5f, 1000.0).first, 1.0, 0.01); // LR4 all-pass sum is flat
}

MP_TEST (limiter_holds_the_ceiling)
{
    Limiter lim; lim.prepare (kSr);
    std::vector<float> l (4800), r (4800);
    for (std::size_t i = 0; i < l.size(); ++i) l[i] = r[i] = 2.0f * (float) std::sin (2.0 * kPi * 200.0 * (double) i / kSr);
    lim.process (l.data(), r.data(), (int) l.size(), 0.977f);
    float pk = 0.0f;
    for (float x : l) pk = std::max (pk, std::abs (x));
    CHECK (pk <= 0.977f + 1e-6f);
}

MP_TEST (engine_routes_deck_to_master_and_cue_to_phones)
{
    EngineRig rig;
    auto t = rampTrack (5.0);
    rig.e.loadDeck (0, t);
    rig.e.deckCommand (Cmd::Play, 0);
    rig.e.mixer.ch[0].cue.store (true);
    std::vector<float> master, phones;
    rig.run (2048, &master, &phones);
    // unity trim, flat EQ, fader up, THRU: master == deck, sample-exact
    for (int i = 0; i < 2048; ++i)
        if (std::abs (master[(std::size_t) i] - (float) ((double) i * kScale)) > 1e-7f) { CHECK (false); break; }
    CHECK (phones[1000] != 0.0f);
    rig.e.mixer.ch[0].fader.store (0.0f);         // fader down: master silent after the ramp, cue still on
    master.clear(); phones.clear();
    rig.run (2048, &master, &phones);
    CHECK_NEAR (master.back(), 0.0, 1e-9);
    CHECK (phones.back() != 0.0f);
}

MP_TEST (sound_color_filter)
{
    ColorFx fx; fx.prepare (kSr, 512);
    auto level = [&] (float knob, double hz) {
        fx.reset();
        std::vector<float> l (24000), r (24000);
        for (std::size_t i = 0; i < l.size(); ++i) l[i] = r[i] = (float) std::sin (2.0 * kPi * hz * (double) i / kSr);
        for (std::size_t o = 0; o < l.size(); o += 480) fx.processPostEq (ColorFxType::Filter, knob, l.data() + o, r.data() + o, 480);
        double e = 0.0;
        for (std::size_t i = 12000; i < l.size(); ++i) e += (double) l[i] * l[i];
        return (float) (10.0 * std::log10 (e / 12000.0 / 0.5)); // dB relative to the input sine
    };
    CHECK_NEAR (level (0.0f, 8000.0), 0.0, 0.01);   // centre: bypass
    CHECK (level (-0.9f, 8000.0) < -30.0f);           // CCW: low-pass
    CHECK (level (0.9f, 100.0) < -30.0f);             // CW: high-pass
}

// ============================================================================ FX / sampler
MP_TEST (echo_tail_rings_after_switching_off)
{
    auto fx = makeEffect (FxType::Echo);
    fx->prepare (kSr, 512);
    FxRunner run; run.prepare (kSr, 512); run.setEffect (fx.get());
    FxContext c; c.bpm = 120.0; c.beats = 0.25f; c.amount = 1.0f;
    std::vector<float> l (512, 0.0f), r (512, 0.0f);
    run.setOn (true);
    for (int b = 0; b < 4; ++b)                       // a 2048-frame burst while on
    {
        for (int i = 0; i < 512; ++i) l[(std::size_t) i] = r[(std::size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * 440.0 * (double) (b * 512 + i) / kSr);
        run.process (l.data(), r.data(), 512, c);
    }
    run.setOn (false);
    float tail = 0.0f;
    for (int b = 0; b < 60; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f); std::fill (r.begin(), r.end(), 0.0f);
        run.process (l.data(), r.data(), 512, c);
        for (float x : l) tail = std::max (tail, std::abs (x));
    }
    CHECK (tail > 0.1f);                              // the echo at 1/4 beat (6000 frames) is heard
    CHECK (run.running());
}

MP_TEST (all_effects_are_finite_and_bounded)
{
    for (int t = 1; t < (int) FxType::Count; ++t)
    {
        auto fx = makeEffect ((FxType) t);
        REQUIRE (fx != nullptr);
        fx->prepare (kSr, 512);
        FxRunner run; run.prepare (kSr, 512); run.setEffect (fx.get());
        run.setOn (true);
        FxContext c; c.bpm = 128.0; c.beats = fxBeatKind ((FxType) t) == BeatKind::Signed64 ? -1.0f : 0.5f; c.amount = 1.0f;
        std::vector<float> l (512), r (512);
        float pk = 0.0f;
        bool finite = true;
        for (int b = 0; b < 200; ++b)
        {
            for (int i = 0; i < 512; ++i) l[(std::size_t) i] = r[(std::size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * 220.0 * (double) (b * 512 + i) / kSr);
            run.process (l.data(), r.data(), 512, c);
            for (float x : l) { finite = finite && std::isfinite (x); pk = std::max (pk, std::abs (x)); }
        }
        if (! finite || pk > 8.0f) std::printf ("    effect %s: finite=%d peak=%.2f\n", fxName ((FxType) t), (int) finite, pk);
        CHECK (finite);
        CHECK (pk < 8.0f);
    }
    CHECK (flx10BeatFx (0) == FxType::LowCutEcho);
    CHECK (flx10BeatFx (13) == FxType::MobiusTri);
}

MP_TEST (sampler_trigger_hold_onoff_and_sync)
{
    Sampler s; s.prepare (kSr);
    auto t = rampTrack (1.0);
    s.load (0, t.get(), 1, 120.0);
    std::vector<float> l (512), r (512), ch (512 * 8, 0.0f);
    float* chans[8];
    for (int k = 0; k < 8; ++k) chans[k] = ch.data() + k * 512;
    auto render = [&] { s.process (l.data(), r.data(), chans, 512, 120.0); };

    s.padDown (0); s.padUp (0); render();             // Trigger: plays through after release
    CHECK (s.view[0].playing.load());
    s.controls[0].mode.store ((int) SampleMode::Hold);
    s.padDown (0); render();
    s.padUp (0); render(); render();
    CHECK (! s.view[0].playing.load());               // Hold: stops on release
    s.controls[0].mode.store ((int) SampleMode::OnOff);
    s.padDown (0); s.padUp (0); render();
    CHECK (s.view[0].playing.load());
    s.padDown (0); s.padUp (0); render(); render();
    CHECK (! s.view[0].playing.load());               // On/Off: second press stops
    // sync: a 120 BPM sample under a 126 BPM master plays 5 % fast
    s.controls[0].mode.store ((int) SampleMode::Trigger);
    s.controls[0].sync.store (true);
    s.padDown (0);
    s.process (l.data(), r.data(), chans, 512, 126.0);
    CHECK_NEAR (s.view[0].position.load() * 48000.0, 512.0 * 1.05, 1.0);
}

MP_TEST (wav_writer_header)
{
    const std::string path = "/tmp/deejay_test.wav";
    {
        WavWriter w;
        REQUIRE (w.open (path, 48000.0, 2, 24));
        std::vector<float> x (200, 0.5f);
        w.write (x.data(), 100);
        w.close();
    }
    std::FILE* f = std::fopen (path.c_str(), "rb");
    REQUIRE (f != nullptr);
    std::fseek (f, 0, SEEK_END);
    CHECK (std::ftell (f) == 44 + 100 * 2 * 3);
    std::fclose (f);
    std::remove (path.c_str());
}

int main() { return mptest::runAll(); }
