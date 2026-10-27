#include "Sampler.h"

namespace dj
{

namespace
{
    inline void readFrame (const TrackAudio& a, double pos, float& l, float& r) noexcept
    {
        const double fl = std::floor (pos);
        const auto i = (std::int64_t) fl;
        const float t = (float) (pos - fl);
        float y[4][2];
        for (int k = 0; k < 4; ++k) a.mixFrame (i - 1 + k, y[k][0], y[k][1]);
        l = hermite (y[0][0], y[1][0], y[2][0], y[3][0], t);
        r = hermite (y[0][1], y[1][1], y[2][1], y[3][1], t);
    }
}

// Source for the stretcher: the slot's audio, wrapping at the loop length when repeating.
struct SlotSource
{
    const TrackAudio* audio;
    double loopLen;          // 0 = no wrap
    void frame (double pos, float* out) const noexcept
    {
        if (loopLen > 0.0) pos = fmodPos (pos, loopLen);
        readFrame (*audio, pos, out[0], out[1]);
    }
};

double Sampler::loopFrames (double frames, double sampleRate, double bpm) noexcept
{
    if (bpm <= 1.0 || sampleRate <= 0.0) return frames;
    const double fpb = 60.0 * sampleRate / bpm;
    const double beats = frames / fpb;
    const double whole = std::max (1.0, std::round (beats));
    // loops cut at their stated tempo are within a few milliseconds of whole beats
    return std::abs (beats - whole) <= 0.15 ? whole * fpb : frames;
}

void Sampler::prepare (double outputRate)
{
    outRate_ = outputRate;
    for (auto& s : slots_) s.stretch.prepare (outputRate, 2);
    attack_ = 1.0f / (float) std::max (1.0, outputRate * 0.0015);
    release_ = 1.0f / (float) std::max (1.0, outputRate * 0.006);
}

void Sampler::load (int slot, const TrackAudio* audio, std::uint64_t id, double bpm) noexcept
{
    if (slot < 0 || slot >= kSamplerSlots) return;
    auto& s = slots_[(std::size_t) slot];
    s.audio = audio;
    s.id = id;
    s.bpm = bpm;
    s.pos = 0.0;
    s.playing = s.stopping = s.waiting = s.stretching = false;
    s.env = 0.0f;
    s.oldLeft = 0;
    s.stretch.reset();
}

void Sampler::start (Slot& s) noexcept
{
    if (s.playing && s.env > 0.0f && ! s.stretching) { s.oldPos = s.pos; s.oldLeft = 64; }
    s.pos = 0.0;
    s.playing = true;
    s.stopping = false;
    s.waiting = false;
    s.stretching = false;      // process() resets and primes the stretcher on the first sample
    if (s.oldLeft == 0) s.env = 0.0f;
}

void Sampler::launch (int slot) noexcept
{
    auto& s = slots_[(std::size_t) slot];
    const auto& c = controls[(std::size_t) slot];
    if (c.sync.load (std::memory_order_relaxed) && c.quantize.load (std::memory_order_relaxed) && s.bpm > 1.0)
    {
        s.waiting = true;          // process() starts it on the next master beat (or now, without one)
        if (s.playing) s.stopping = true;
        return;
    }
    start (s);
}

void Sampler::padDown (int slot) noexcept
{
    if (slot < 0 || slot >= kSamplerSlots) return;
    auto& s = slots_[(std::size_t) slot];
    if (s.audio == nullptr) return;
    switch ((SampleMode) controls[(std::size_t) slot].mode.load (std::memory_order_relaxed))
    {
        case SampleMode::Trigger: launch (slot); break;       // re-press restarts
        case SampleMode::Hold: launch (slot); break;          // plays while held
        case SampleMode::OnOff:
            if (s.waiting) s.waiting = false;                 // pressed again before the beat: cancel
            else if (s.playing && ! s.stopping) s.stopping = true; // press again: stop and rewind
            else launch (slot);
            break;
    }
}

void Sampler::padUp (int slot) noexcept
{
    if (slot < 0 || slot >= kSamplerSlots) return;
    if ((SampleMode) controls[(std::size_t) slot].mode.load (std::memory_order_relaxed) == SampleMode::Hold)
        stop (slot);
}

void Sampler::stop (int slot) noexcept
{
    if (slot < 0 || slot >= kSamplerSlots) return;
    auto& s = slots_[(std::size_t) slot];
    s.waiting = false;
    if (s.playing) s.stopping = true;
}

void Sampler::stopAll() noexcept
{
    for (int i = 0; i < kSamplerSlots; ++i) stop (i);
}

void Sampler::process (float* busL, float* busR, float* const* channels, int n, double masterBpm, double masterBeat) noexcept
{
    std::fill (busL, busL + n, 0.0f);
    std::fill (busR, busR + n, 0.0f);
    for (int k = 0; k < kSamplerSlots; ++k)
    {
        auto& s = slots_[(std::size_t) k];
        auto& c = controls[(std::size_t) k];
        auto& v = view[(std::size_t) k];
        v.loaded.store (s.audio != nullptr, std::memory_order_relaxed);
        v.bpm.store (s.bpm, std::memory_order_relaxed);
        if (s.audio == nullptr || (! s.playing && ! s.waiting))
        {
            v.playing.store (false, std::memory_order_relaxed);
            v.waiting.store (false, std::memory_order_relaxed);
            continue;
        }
        const auto& a = *s.audio;
        const bool sync = c.sync.load (std::memory_order_relaxed) && s.bpm > 1.0 && masterBpm > 1.0;
        const bool repeat = c.repeat.load (std::memory_order_relaxed);
        const double pitchRatio = std::pow (2.0, (double) c.pitch.load (std::memory_order_relaxed) / 12.0);
        const double tempoRatio = sync ? masterBpm / s.bpm : 1.0;
        const double srRatio = a.sampleRate / outRate_;
        const bool stretch = c.keylock.load (std::memory_order_relaxed)
                             && (std::abs (tempoRatio - 1.0) > 1.0e-4 || std::abs (pitchRatio - 1.0) > 1.0e-6);
        const double advance = stretch ? srRatio * tempoRatio : srRatio * tempoRatio * pitchRatio;
        const double grainSpeed = srRatio * pitchRatio;
        const double len = repeat && sync ? loopFrames ((double) a.frames, a.sampleRate, s.bpm) : (double) a.frames;
        const SlotSource src { &a, repeat ? len : 0.0 };

        // a quantized launch waits for the next master beat (the old sound fades out meanwhile)
        int from = 0;
        bool launchNow = false;
        if (s.waiting)
        {
            if (! sync || masterBeat < 0.0) launchNow = true;
            else
            {
                double toNext = std::ceil (masterBeat) - masterBeat;
                if (toNext < 1.0e-6 || toNext > 1.0 - 1.0e-6) toNext = 0.0;   // on the beat
                const double offset = toNext * 60.0 * outRate_ / masterBpm;
                if (offset < (double) n) { launchNow = true; from = (int) offset; }
            }
        }

        float* ol = out (c, busL, channels, 0);
        float* orr = out (c, busR, channels, 1);
        const float gain = c.volume.load (std::memory_order_relaxed) * dbToGain (c.gainDb.load (std::memory_order_relaxed));
        for (int i = 0; i < n; ++i)
        {
            if (launchNow && i == from)
            {
                start (s);
                s.oldLeft = 0;
                if (stretch) s.env = 1.0f;   // primed below: the first transient plays at full level
            }
            if (! s.playing) continue;
            if (s.stopping) { s.env -= release_; if (s.env <= 0.0f) { s.env = 0.0f; s.playing = false; s.stopping = false; s.pos = 0.0; continue; } }
            else if (s.env < 1.0f) s.env = std::min (1.0f, s.env + attack_);
            if (stretch != s.stretching)
            {
                s.stretching = stretch;
                s.stretch.reset();
                if (stretch) prime (s, src, advance, grainSpeed);
            }
            float l, r;
            if (stretch)
            {
                float f[2];
                s.stretch.next (src, s.pos, grainSpeed, f);
                l = f[0]; r = f[1];
            }
            else
            {
                readFrame (a, s.pos, l, r);
                if (s.oldLeft > 0)
                {
                    float pl, pr;
                    readFrame (a, s.oldPos, pl, pr);
                    const float w = (float) s.oldLeft / 64.0f;
                    l = l * (1.0f - w) + pl * w;
                    r = r * (1.0f - w) + pr * w;
                    s.oldPos += advance;
                    --s.oldLeft;
                }
            }
            ol[i] += l * s.env * gain;
            orr[i] += r * s.env * gain;
            s.pos += advance;
            if (s.pos >= len)
            {
                if (repeat) s.pos = std::fmod (s.pos, len);
                else { s.playing = false; s.pos = 0.0; s.env = 0.0f; }
            }
        }
        v.playing.store (s.playing, std::memory_order_relaxed);
        v.waiting.store (s.waiting, std::memory_order_relaxed);
        v.position.store (len > 0 ? s.pos / len : 0.0, std::memory_order_relaxed);
    }
}

float* Sampler::out (const SamplerSlotControls& c, float* bus, float* const* channels, int side) noexcept
{
    const int o = std::clamp (c.output.load (std::memory_order_relaxed), 0, kNumDecks);
    return o == 0 ? bus : channels[(o - 1) * 2 + side];
}

// Runs the stretcher one hop ahead of the start, so the first output already has two overlapping grains
// (a fresh WSOLA stretcher would otherwise fade in over its first hop and soften the first transient).
void Sampler::prime (Slot& s, const SlotSource& src, double advance, double grainSpeed) noexcept
{
    const int hop = s.stretch.hop();
    double pos = s.pos - hop * advance;
    float f[2];
    for (int k = 0; k < hop; ++k) { s.stretch.next (src, pos, grainSpeed, f); pos += advance; }
}

} // namespace dj
