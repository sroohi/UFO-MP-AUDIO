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

void Sampler::prepare (double outputRate)
{
    outRate_ = outputRate;
    attack_ = 1.0f / (float) std::max (1.0, outputRate * 0.0015);
    release_ = 1.0f / (float) std::max (1.0, outputRate * 0.006);
}

void Sampler::load (int slot, const TrackAudio* audio, std::uint64_t id, double bpm) noexcept
{
    if (slot < 0 || slot >= kSamplerSlots) return;
    auto& s = slots_[(std::size_t) slot];
    s = {};
    s.audio = audio;
    s.id = id;
    s.bpm = bpm;
}

void Sampler::start (Slot& s) noexcept
{
    if (s.playing && s.env > 0.0f) { s.oldPos = s.pos; s.oldLeft = 64; }
    s.pos = 0.0;
    s.playing = true;
    s.stopping = false;
    if (s.oldLeft == 0) s.env = 0.0f;
}

void Sampler::padDown (int slot) noexcept
{
    if (slot < 0 || slot >= kSamplerSlots) return;
    auto& s = slots_[(std::size_t) slot];
    if (s.audio == nullptr) return;
    switch ((SampleMode) controls[(std::size_t) slot].mode.load (std::memory_order_relaxed))
    {
        case SampleMode::Trigger: start (s); break;           // re-press restarts
        case SampleMode::Hold: start (s); break;              // plays while held
        case SampleMode::OnOff:
            if (s.playing && ! s.stopping) s.stopping = true; // press again: stop and rewind
            else start (s);
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
    if (s.playing) s.stopping = true;
}

void Sampler::stopAll() noexcept
{
    for (int i = 0; i < kSamplerSlots; ++i) stop (i);
}

void Sampler::process (float* busL, float* busR, float* const* channels, int n, double masterBpm) noexcept
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
        if (s.audio == nullptr || ! s.playing)
        {
            v.playing.store (false, std::memory_order_relaxed);
            continue;
        }
        const auto& a = *s.audio;
        const bool sync = c.sync.load (std::memory_order_relaxed) && s.bpm > 1.0 && masterBpm > 1.0;
        const double rate = a.sampleRate / outRate_ * std::pow (2.0, (double) c.pitch.load (std::memory_order_relaxed) / 12.0)
                            * (sync ? masterBpm / s.bpm : 1.0);
        const bool repeat = c.repeat.load (std::memory_order_relaxed);
        const float gain = c.volume.load (std::memory_order_relaxed) * dbToGain (c.gainDb.load (std::memory_order_relaxed));
        const int out = std::clamp (c.output.load (std::memory_order_relaxed), 0, kNumDecks);
        float* ol = out == 0 ? busL : channels[(out - 1) * 2];
        float* orr = out == 0 ? busR : channels[(out - 1) * 2 + 1];
        const double len = (double) a.frames;
        for (int i = 0; i < n && s.playing; ++i)
        {
            if (s.stopping) { s.env -= release_; if (s.env <= 0.0f) { s.env = 0.0f; s.playing = false; s.stopping = false; s.pos = 0.0; break; } }
            else if (s.env < 1.0f) s.env = std::min (1.0f, s.env + attack_);
            float l, r;
            readFrame (a, s.pos, l, r);
            if (s.oldLeft > 0)
            {
                float pl, pr;
                readFrame (a, s.oldPos, pl, pr);
                const float w = (float) s.oldLeft / 64.0f;
                l = l * (1.0f - w) + pl * w;
                r = r * (1.0f - w) + pr * w;
                s.oldPos += rate;
                --s.oldLeft;
            }
            ol[i] += l * s.env * gain;
            orr[i] += r * s.env * gain;
            s.pos += rate;
            if (s.pos >= len)
            {
                if (repeat) s.pos -= len;
                else { s.playing = false; s.pos = 0.0; s.env = 0.0f; }
            }
        }
        v.playing.store (s.playing, std::memory_order_relaxed);
        v.position.store (len > 0 ? s.pos / len : 0.0, std::memory_order_relaxed);
    }
}

} // namespace dj
