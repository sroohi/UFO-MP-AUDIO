#include "Performer.h"
#include <cstdio>

namespace dj
{

namespace
{
    template <typename T, std::size_t N>
    T pick (const T (&table)[N], float x) noexcept
    {
        const int i = std::clamp ((int) (std::clamp (x, 0.0f, 1.0f) * (float) N), 0, (int) N - 1);
        return table[(std::size_t) i];
    }

    std::string beatText (double beats)
    {
        char b[24];
        if (beats >= 1.0 - 1.0e-9) { std::snprintf (b, sizeof b, "%g", beats); return b; }
        for (int den : { 2, 3, 4, 6, 8, 12, 16, 24, 32, 64 })
            for (int num = 1; num < den; ++num)
                if (std::abs (beats - (double) num / den) < 1.0e-6) return std::to_string (num) + "/" + std::to_string (den);
        std::snprintf (b, sizeof b, "%.3f", beats);
        return b;
    }

    std::string percent (float v)
    {
        return std::to_string ((int) std::lround (std::clamp (v, 0.0f, 1.0f) * 100.0f)) + "%";
    }

    // Formants (Hz) and relative levels of the vowels A E I O U (adult voice averages).
    struct Vowel { float f[3]; float g[3]; };
    constexpr Vowel kVowels[5] {
        { { 800.0f, 1150.0f, 2900.0f }, { 1.0f, 0.50f, 0.25f } },
        { { 400.0f, 1700.0f, 2600.0f }, { 1.0f, 0.40f, 0.25f } },
        { { 300.0f, 2200.0f, 3000.0f }, { 1.0f, 0.30f, 0.20f } },
        { { 450.0f,  800.0f, 2830.0f }, { 1.0f, 0.60f, 0.15f } },
        { { 325.0f,  700.0f, 2530.0f }, { 1.0f, 0.40f, 0.10f } } };
    constexpr float kFormantQ = 10.0f;
    constexpr float kTalkboxMakeup = 3.0f;
    constexpr int kControlInterval = 32;    // samples between filter coefficient updates

    inline float ramp (float g, float target, float step) noexcept
    {
        return g < target ? std::min (target, g + step) : std::max (target, g - step);
    }
}

namespace perf
{
    double stutterSliceBeats (float x) noexcept { static constexpr double t[] { 0.25, 0.125, 1.0 / 16, 1.0 / 32 }; return pick (t, x); }
    double noteRepeatBeats (float x) noexcept
    {
        static constexpr double t[] { 0.5, 0.25, 1.0 / 6, 0.125, 1.0 / 12, 1.0 / 16, 1.0 / 24, 1.0 / 32 };
        return pick (t, x);
    }
    double tapeStopBeats (float x) noexcept { static constexpr double t[] { 0.25, 0.5, 1.0, 2.0, 4.0 }; return pick (t, x); }
    int pitchSemitones (float x) noexcept { return (int) std::lround ((std::clamp (x, 0.0f, 1.0f) - 0.5f) * 24.0f); }
    double delayBeats (float x) noexcept { static constexpr double t[] { 0.125, 0.25, 0.375, 0.5, 0.75, 1.0 }; return pick (t, x); }
    double talkboxSweepBeats (float y) noexcept { static constexpr double t[] { 0.0, 4.0, 2.0, 1.0, 0.5, 0.25 }; return pick (t, y); }
    float crushBits (float x) noexcept { return 16.0f - std::clamp (x, 0.0f, 1.0f) * 13.0f; }
    int crushDownsample (float y) noexcept { return 1 + (int) std::lround (std::clamp (y, 0.0f, 1.0f) * 31.0f); }
}

const char* performerFxName (PerformerFx fx) noexcept
{
    switch (fx)
    {
        case PerformerFx::Stutter:    return "STUTTER";
        case PerformerFx::NoteRepeat: return "NOTE REPEAT";
        case PerformerFx::TapeStop:   return "TAPE STOP";
        case PerformerFx::Pitch:      return "PITCH";
        case PerformerFx::Filter:     return "FILTER";
        case PerformerFx::Talkbox:    return "TALKBOX";
        case PerformerFx::Bitcrusher: return "BITCRUSH";
        case PerformerFx::Delay:      return "DELAY";
        case PerformerFx::Reverb:     return "REVERB";
    }
    return "";
}

const char* performerParamName (PerformerFx fx, int axis) noexcept
{
    static constexpr const char* names[kNumPerformerFx][2] {
        { "SLICE", "BUILD" }, { "RATE", "GATE" }, { "TIME", "CURVE" }, { "PITCH", "MIX" }, { "CUTOFF", "RES" },
        { "VOWEL", "SWEEP" }, { "BITS", "RATE" }, { "TIME", "FEEDBACK" }, { "SIZE", "SEND" } };
    return names[(std::size_t) fx][axis == 0 ? 0 : 1];
}

std::string performerParamText (PerformerFx fx, int axis, float v)
{
    using namespace perf;
    switch (fx)
    {
        case PerformerFx::Stutter:    return axis == 0 ? beatText (stutterSliceBeats (v)) : (v < 0.05f ? std::string ("off") : percent (v));
        case PerformerFx::NoteRepeat: return axis == 0 ? beatText (noteRepeatBeats (v)) : percent (v);
        case PerformerFx::TapeStop:   return axis == 0 ? beatText (tapeStopBeats (v)) + " beat" : percent (v);
        case PerformerFx::Pitch:
        {
            if (axis != 0) return percent (v);
            const int s = pitchSemitones (v);
            return (s > 0 ? "+" : "") + std::to_string (s) + " st";
        }
        case PerformerFx::Filter:
            if (axis != 0) return percent (v);
            if (std::abs (v - 0.5f) < 0.02f) return "open";
            return (v < 0.5f ? "LP " : "HP ") + percent (std::abs (v - 0.5f) * 2.0f);
        case PerformerFx::Talkbox:
        {
            if (axis == 0) return std::string (1, "AEIOU"[std::clamp ((int) std::lround (v * 4.0f), 0, 4)]);
            const double s = talkboxSweepBeats (v);
            return s <= 0.0 ? std::string ("off") : beatText (s) + " beat";
        }
        case PerformerFx::Bitcrusher:
            return axis == 0 ? std::to_string ((int) std::lround (crushBits (v))) + " bit" : std::to_string (crushDownsample (v)) + "x";
        case PerformerFx::Delay:      return axis == 0 ? beatText (delayBeats (v)) : percent (v);
        case PerformerFx::Reverb:     return percent (v);
    }
    return {};
}

// ------------------------------------------------------------------------------------------------
void Performer::prepare (double sampleRate, int maxBlock)
{
    sr_ = sampleRate;
    maxBlock_ = std::max (1, maxBlock);
    rampStep_ = (float) (1.0 / (0.010 * sr_));
    ringSize_ = (int) (12.0 * sr_);
    ringL_.assign ((std::size_t) ringSize_, 0.0f);
    ringR_.assign ((std::size_t) ringSize_, 0.0f);
    pitchWindow_ = std::round (0.045 * sr_);
    for (auto& b : pitchBuf_) b.assign ((std::size_t) pitchWindow_ * 2 + 8, 0.0f);
    for (auto& b : delayBuf_) b.assign ((std::size_t) (4.5 * sr_), 0.0f);
    filter_.prepare (sr_);
    for (auto& f : formant_) f.prepare (sr_);
    reverb_.prepare (sr_);
    wetL_.assign ((std::size_t) maxBlock_, 0.0f);
    wetR_.assign ((std::size_t) maxBlock_, 0.0f);
    reset();
}

void Performer::reset() noexcept
{
    std::fill (ringL_.begin(), ringL_.end(), 0.0f);
    std::fill (ringR_.begin(), ringR_.end(), 0.0f);
    written_ = 0;
    time_ = {};
    gain_ = {};
    for (auto& b : pitchBuf_) std::fill (b.begin(), b.end(), 0.0f);
    pitchWrite_ = 0;
    pitchPhase_ = 0.0;
    filter_.reset();
    filterX_ = 0.5f;
    filterSet_ = false;
    for (auto& f : formant_) f.reset();
    vowelSet_ = false;
    crushHold_[0] = crushHold_[1] = 0.0f;
    crushCount_ = 0;
    for (auto& b : delayBuf_) std::fill (b.begin(), b.end(), 0.0f);
    delayWrite_ = 0;
    delaySamples_ = 0.0;
    delayLp_[0] = delayLp_[1] = 0.0f;
    delayEnergy_ = 0.0f;
    reverb_.reset();
    reverbEnergy_ = 0.0f;
    internalBeat_ = 0.0;
}

float Performer::ringAt (const std::vector<float>& ring, double index) const noexcept
{
    const double fl = std::floor (index);
    const auto i0 = (std::int64_t) fl;
    if (i0 < written_ - ringSize_ + 1 || i0 >= written_) return 0.0f;
    const float frac = (float) (index - fl);
    const float a = ring[(std::size_t) (i0 % ringSize_)];
    if (i0 + 1 >= written_ || frac <= 0.0f) return a;
    const float b = ring[(std::size_t) ((i0 + 1) % ringSize_)];
    return a + (b - a) * frac;
}

void Performer::startTimeMode (int mode, double beat, double spb) noexcept
{
    auto& t = time_[(std::size_t) mode];
    t.running = true;
    t.elapsed = 0.0;
    t.startBeat = beat;
    t.rate = 1.0;
    const double now = (double) written_;   // index the next input sample will get
    switch (mode)
    {
        case kStutter:
        {
            // repeat the slice that started at the last grid line, from where the music is now
            const double phase = fmodPos (beat, perf::stutterSliceBeats (controls.x[(int) PerformerFx::Stutter].load (std::memory_order_relaxed))) * spb;
            t.start = now - phase;
            t.pos = phase;
            break;
        }
        case kRepeat:
        {
            const double phase = fmodPos (beat, 1.0) * spb;
            t.start = now - phase;
            t.pos = phase;
            t.lastBeatIndex = std::floor (beat);
            break;
        }
        default:
            t.pos = now;
            break;
    }
}

float Performer::pitchTap (int ch, double delay) const noexcept
{
    const auto& b = pitchBuf_[ch];
    const int size = (int) b.size();
    double p = (double) pitchWrite_ - delay;
    while (p < 0.0) p += size;
    const int i0 = (int) p;
    const float frac = (float) (p - i0);
    const float a = b[(std::size_t) (i0 % size)], c = b[(std::size_t) ((i0 + 1) % size)];
    return a + (c - a) * frac;
}

void Performer::process (float* l, float* r, int n, double bpm, double beat) noexcept
{
    if (ringL_.empty() || n <= 0) return;
    n = std::min (n, maxBlock_);
    if (! (bpm > 1.0)) bpm = 120.0;
    bpm = std::clamp (bpm, 40.0, 300.0);
    const double spb = 60.0 * sr_ / bpm;   // samples per beat
    const double bps = 1.0 / spb;
    double beat0;
    if (beat >= 0.0) beat0 = beat;
    else beat0 = internalBeat_;
    internalBeat_ = beat0 + n * bps;

    const bool enabled = controls.enabled.load (std::memory_order_relaxed);
    bool on[kNumPerformerFx];
    float px[kNumPerformerFx], py[kNumPerformerFx];
    for (int k = 0; k < kNumPerformerFx; ++k)
    {
        on[k] = enabled && controls.on[k].load (std::memory_order_relaxed);
        px[k] = std::clamp (controls.x[k].load (std::memory_order_relaxed), 0.0f, 1.0f);
        py[k] = std::clamp (controls.y[k].load (std::memory_order_relaxed), 0.0f, 1.0f);
    }

    // ---- time effects: the highest one switched on plays
    int want = -1;
    if (on[(int) PerformerFx::TapeStop]) want = kTape;
    else if (on[(int) PerformerFx::Stutter]) want = kStutter;
    else if (on[(int) PerformerFx::NoteRepeat]) want = kRepeat;
    for (int m = 0; m < kTimeModes; ++m)
        if (m == want && ! time_[(std::size_t) m].running) startTimeMode (m, beat0, spb);

    const double sliceBeats0 = perf::stutterSliceBeats (px[(int) PerformerFx::Stutter]);
    const float build = py[(int) PerformerFx::Stutter];
    const double repeatLen = std::max (16.0, perf::noteRepeatBeats (px[(int) PerformerFx::NoteRepeat]) * spb);
    const double gateOpen = std::max (64.0, (double) py[(int) PerformerFx::NoteRepeat] * repeatLen);
    const double tapeLen = std::max (64.0, perf::tapeStopBeats (px[(int) PerformerFx::TapeStop]) * spb);
    const double tapeCurve = 1.0 + 2.0 * py[(int) PerformerFx::TapeStop];
    const double fade = 0.0015 * sr_;

    // ---- pitch
    const int semis = perf::pitchSemitones (px[(int) PerformerFx::Pitch]);
    const double pitchRatio = std::pow (2.0, semis / 12.0);
    const float pitchMix = py[(int) PerformerFx::Pitch];

    // ---- bitcrusher
    const float levels = std::pow (2.0f, perf::crushBits (px[(int) PerformerFx::Bitcrusher]) - 1.0f);
    const int downsample = perf::crushDownsample (py[(int) PerformerFx::Bitcrusher]);

    // ---- delay
    const double delayWant = std::clamp (perf::delayBeats (px[(int) PerformerFx::Delay]) * spb, 16.0, (double) delayBuf_[0].size() - 4.0);
    if (delaySamples_ <= 0.0) delaySamples_ = delayWant;
    const float feedback = py[(int) PerformerFx::Delay] * 0.92f;
    const bool delayRun = on[(int) PerformerFx::Delay] || gain_[(int) PerformerFx::Delay] > 0.0f || delayEnergy_ > 1.0e-6f;
    float delayPeak = 0.0f;

    const double sweep = perf::talkboxSweepBeats (py[(int) PerformerFx::Talkbox]);
    const float reverbSend = 0.25f + 0.75f * py[(int) PerformerFx::Reverb];

    for (int i = 0; i < n; ++i)
    {
        const double b = beat0 + i * bps;
        const float inL = l[i], inR = r[i];
        ringL_[(std::size_t) (written_ % ringSize_)] = inL;
        ringR_[(std::size_t) (written_ % ringSize_)] = inR;
        ++written_;

        // time effects
        float accL = 0.0f, accR = 0.0f, gsum = 0.0f;
        for (int m = 0; m < kTimeModes; ++m)
        {
            auto& t = time_[(std::size_t) m];
            if (! t.running) continue;
            t.gain = ramp (t.gain, m == want ? 1.0f : 0.0f, rampStep_);
            float ol = 0.0f, orr = 0.0f;
            if (m == kStutter)
            {
                double sliceBeats = sliceBeats0;
                if (build > 0.05f)
                {
                    const double every = 1.0 + (1.0 - build) * 7.0;   // beats between halvings
                    const int halvings = std::min (8, (int) (t.elapsed * bps / every));
                    sliceBeats = std::max (1.0 / 64.0, sliceBeats / (double) (1 << halvings));
                }
                const double slice = std::max (16.0, sliceBeats * spb);
                if (t.pos >= slice) t.pos = std::fmod (t.pos, slice);
                const double f = std::min (fade, slice * 0.25);
                const float env = (float) std::min (1.0, std::min (t.pos / f, (slice - t.pos) / f));
                ol = ringAt (ringL_, t.start + t.pos) * env;
                orr = ringAt (ringR_, t.start + t.pos) * env;
                t.pos += 1.0;
            }
            else if (m == kRepeat)
            {
                const double bi = std::floor (b);
                if (bi != t.lastBeatIndex)
                {
                    // a new beat: capture it and repeat its start
                    t.lastBeatIndex = bi;
                    const double phase = fmodPos (b, 1.0) * spb;
                    t.start = (double) written_ - 1.0 - phase;
                    t.pos = phase;
                }
                const double p = std::fmod (t.pos, repeatLen);
                double env = std::min (1.0, p / fade);
                if (p > gateOpen - fade) env = std::min (env, std::max (0.0, (gateOpen - p) / fade));
                ol = ringAt (ringL_, t.start + p) * (float) env;
                orr = ringAt (ringR_, t.start + p) * (float) env;
                t.pos += 1.0;
            }
            else
            {
                const double prog = std::min (1.0, t.elapsed / tapeLen);
                t.rate = std::pow (1.0 - prog, tapeCurve);
                const float env = (float) std::min (1.0, t.rate * 4.0);   // a stopped tape is silent
                ol = ringAt (ringL_, t.pos) * env;
                orr = ringAt (ringR_, t.pos) * env;
                t.pos += t.rate;
            }
            t.elapsed += 1.0;
            accL += ol * t.gain;
            accR += orr * t.gain;
            gsum += t.gain;
            if (t.gain <= 0.0f && m != want) t.running = false;
        }
        float sL = inL * (1.0f - std::min (1.0f, gsum)) + accL;
        float sR = inR * (1.0f - std::min (1.0f, gsum)) + accR;

        // pitch (the delay line always runs so switching on never plays stale audio)
        {
            auto& g = gain_[(int) PerformerFx::Pitch];
            g = ramp (g, on[(int) PerformerFx::Pitch] ? 1.0f : 0.0f, rampStep_);
            const int size = (int) pitchBuf_[0].size();
            pitchBuf_[0][(std::size_t) pitchWrite_] = sL;
            pitchBuf_[1][(std::size_t) pitchWrite_] = sR;
            if (g > 0.0f && semis != 0)
            {
                pitchPhase_ += (1.0 - pitchRatio) / pitchWindow_;
                pitchPhase_ -= std::floor (pitchPhase_);
                float wl = 0.0f, wr = 0.0f;
                for (int k = 0; k < 2; ++k)
                {
                    double ph = pitchPhase_ + 0.5 * k;
                    ph -= std::floor (ph);
                    const double delay = 1.0 + ph * pitchWindow_;
                    const float w = (float) std::sin (kPi * ph);
                    wl += pitchTap (0, delay) * w * w;
                    wr += pitchTap (1, delay) * w * w;
                }
                const float mix = g * pitchMix;
                sL = sL * (1.0f - mix) + wl * mix;
                sR = sR * (1.0f - mix) + wr * mix;
            }
            pitchWrite_ = (pitchWrite_ + 1) % size;
        }

        // filter
        {
            auto& g = gain_[(int) PerformerFx::Filter];
            g = ramp (g, on[(int) PerformerFx::Filter] ? 1.0f : 0.0f, rampStep_);
            if (g > 0.0f)
            {
                if (i % kControlInterval == 0 || ! filterSet_)
                {
                    filterSet_ = true;
                    filterX_ += (px[(int) PerformerFx::Filter] - filterX_) * 0.15f;
                    const float x = filterX_;
                    const float q = 0.707f + py[(int) PerformerFx::Filter] * 7.0f;
                    if (x < 0.5f)
                        filter_.set (20000.0f * std::pow (60.0f / 20000.0f, std::clamp ((0.48f - x) / 0.48f, 0.0f, 1.0f)), q, dsp::Svf::Mode::LowPass);
                    else
                        filter_.set (20.0f * std::pow (12000.0f / 20.0f, std::clamp ((x - 0.52f) / 0.48f, 0.0f, 1.0f)), q, dsp::Svf::Mode::HighPass);
                }
                const float fl = filter_.process (0, sL), fr = filter_.process (1, sR);
                sL = sL * (1.0f - g) + fl * g;
                sR = sR * (1.0f - g) + fr * g;
            }
            else if (filterSet_) { filter_.reset(); filterSet_ = false; filterX_ = 0.5f; }
        }

        // talkbox
        {
            auto& g = gain_[(int) PerformerFx::Talkbox];
            g = ramp (g, on[(int) PerformerFx::Talkbox] ? 1.0f : 0.0f, rampStep_);
            if (g > 0.0f)
            {
                if (i % kControlInterval == 0 || ! vowelSet_)
                {
                    // vowel position 0..4 (A E I O U): the knob, or a sweep locked to the beat
                    double v = px[(int) PerformerFx::Talkbox] * 4.0;
                    if (sweep > 0.0) v = 2.0 - 2.0 * std::cos (2.0 * kPi * b / sweep);
                    v = std::clamp (v, 0.0, 4.0);
                    const int a = std::min (3, (int) v);
                    const float t = (float) (v - a);
                    for (int k = 0; k < 3; ++k)
                    {
                        const float f = kVowels[a].f[k] + (kVowels[a + 1].f[k] - kVowels[a].f[k]) * t;
                        formant_[(std::size_t) k].set (f, kFormantQ, dsp::Svf::Mode::BandPass);
                        vowelGain_[(std::size_t) k] = (kVowels[a].g[k] + (kVowels[a + 1].g[k] - kVowels[a].g[k]) * t) / kFormantQ;
                    }
                    vowelSet_ = true;
                }
                // drive adds the harmonics the formants shape, like a voice box fed by a synth
                const float dl = std::tanh (2.0f * sL), dr = std::tanh (2.0f * sR);
                float wl = 0.0f, wr = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    wl += formant_[(std::size_t) k].process (0, dl) * vowelGain_[(std::size_t) k];
                    wr += formant_[(std::size_t) k].process (1, dr) * vowelGain_[(std::size_t) k];
                }
                sL = sL * (1.0f - g) + wl * kTalkboxMakeup * g;
                sR = sR * (1.0f - g) + wr * kTalkboxMakeup * g;
            }
            else if (vowelSet_) { for (auto& f : formant_) f.reset(); vowelSet_ = false; }
        }

        // bitcrusher
        {
            auto& g = gain_[(int) PerformerFx::Bitcrusher];
            g = ramp (g, on[(int) PerformerFx::Bitcrusher] ? 1.0f : 0.0f, rampStep_);
            if (g > 0.0f)
            {
                if (crushCount_ <= 0)
                {
                    crushHold_[0] = std::round (sL * levels) / levels;
                    crushHold_[1] = std::round (sR * levels) / levels;
                    crushCount_ = downsample;
                }
                --crushCount_;
                sL = sL * (1.0f - g) + crushHold_[0] * g;
                sR = sR * (1.0f - g) + crushHold_[1] * g;
            }
        }

        // delay (send, ping-pong)
        {
            auto& g = gain_[(int) PerformerFx::Delay];
            g = ramp (g, on[(int) PerformerFx::Delay] ? 1.0f : 0.0f, rampStep_);
            if (delayRun)
            {
                const int size = (int) delayBuf_[0].size();
                delaySamples_ += (delayWant - delaySamples_) * 0.0005;
                double rp = (double) delayWrite_ - delaySamples_;
                if (rp < 0.0) rp += size;
                const int i0 = (int) rp;
                const float fr = (float) (rp - i0);
                auto tap = [&] (int ch)
                {
                    const auto& buf = delayBuf_[ch];
                    const float a = buf[(std::size_t) (i0 % size)], c = buf[(std::size_t) ((i0 + 1) % size)];
                    return a + (c - a) * fr;
                };
                const float dL = tap (0), dR = tap (1);
                delayLp_[0] += 0.35f * (dL - delayLp_[0]);
                delayLp_[1] += 0.35f * (dR - delayLp_[1]);
                const float in = 0.5f * (sL + sR) * g;
                delayBuf_[0][(std::size_t) delayWrite_] = in + feedback * delayLp_[1];
                delayBuf_[1][(std::size_t) delayWrite_] = feedback * delayLp_[0];
                delayWrite_ = (delayWrite_ + 1) % size;
                sL += dL * 0.7f;
                sR += dR * 0.7f;
                delayPeak = std::max (delayPeak, std::max (std::abs (dL), std::abs (dR)));
            }
        }

        // reverb send (rendered per block below)
        {
            auto& g = gain_[(int) PerformerFx::Reverb];
            g = ramp (g, on[(int) PerformerFx::Reverb] ? 1.0f : 0.0f, rampStep_);
            wetL_[(std::size_t) i] = sL * g * reverbSend;
            wetR_[(std::size_t) i] = sR * g * reverbSend;
        }

        l[i] = sL;
        r[i] = sR;
    }
    if (delayRun) delayEnergy_ = delayPeak;

    // ---- reverb
    if (on[(int) PerformerFx::Reverb] || gain_[(int) PerformerFx::Reverb] > 0.0f || reverbEnergy_ > 1.0e-6f)
    {
        reverb_.set (0.3f + 0.7f * px[(int) PerformerFx::Reverb], 0.35f, 1.0f);
        reverb_.process (wetL_.data(), wetR_.data(), n);
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            l[i] += wetL_[(std::size_t) i];
            r[i] += wetR_[(std::size_t) i];
            peak = std::max (peak, std::max (std::abs (wetL_[(std::size_t) i]), std::abs (wetR_[(std::size_t) i])));
        }
        reverbEnergy_ = peak;
    }

    // ---- view
    view.sounding[(int) PerformerFx::Stutter].store (time_[kStutter].running, std::memory_order_relaxed);
    view.sounding[(int) PerformerFx::NoteRepeat].store (time_[kRepeat].running, std::memory_order_relaxed);
    view.sounding[(int) PerformerFx::TapeStop].store (time_[kTape].running, std::memory_order_relaxed);
    for (auto fx : { PerformerFx::Pitch, PerformerFx::Filter, PerformerFx::Talkbox, PerformerFx::Bitcrusher })
        view.sounding[(int) fx].store (gain_[(int) fx] > 0.0f, std::memory_order_relaxed);
    view.sounding[(int) PerformerFx::Delay].store (gain_[(int) PerformerFx::Delay] > 0.0f || delayEnergy_ > 1.0e-4f, std::memory_order_relaxed);
    view.sounding[(int) PerformerFx::Reverb].store (gain_[(int) PerformerFx::Reverb] > 0.0f || reverbEnergy_ > 1.0e-4f, std::memory_order_relaxed);
}

} // namespace dj
