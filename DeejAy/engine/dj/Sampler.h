// Sample player: 4 banks (A-D) x 8 slots, Serato play modes (Trigger / Hold / On-Off), per-slot
// sync to the master tempo, repeat, pitch, gain and output (sampler bus or a mixer channel).
#pragma once
#include "Common.h"
#include "TrackAudio.h"
#include <array>
#include <atomic>

namespace dj
{

enum class SampleMode : int { Trigger = 0, Hold = 1, OnOff = 2 };

struct SamplerSlotControls
{
    std::atomic<int> mode { (int) SampleMode::Trigger };
    std::atomic<bool> repeat { false };
    std::atomic<bool> sync { false };
    std::atomic<float> volume { 1.0f };      // 0..1
    std::atomic<float> gainDb { 0.0f };
    std::atomic<float> pitch { 0.0f };       // semitones
    std::atomic<int> output { 0 };           // 0 = sampler bus, 1..4 = channel 1..4
};

struct SamplerSlotView
{
    std::atomic<bool> loaded { false };
    std::atomic<bool> playing { false };
    std::atomic<double> position { 0.0 };    // 0..1
    std::atomic<double> bpm { 0.0 };
};

class Sampler
{
public:
    void prepare (double outputRate);

    // audio thread (commands)
    void load (int slot, const TrackAudio* audio, std::uint64_t id, double bpm) noexcept;
    std::uint64_t audioId (int slot) const noexcept { return slots_[(std::size_t) slot].id; }
    void padDown (int slot) noexcept;
    void padUp (int slot) noexcept;
    void stop (int slot) noexcept;
    void stopAll() noexcept;

    // Renders all slots; `channels` holds kNumDecks stereo pairs (channel outputs are added, not cleared).
    void process (float* busL, float* busR, float* const* channels, int n, double masterBpm) noexcept DJ_NONBLOCKING;

    std::array<SamplerSlotControls, kSamplerSlots> controls;
    std::array<SamplerSlotView, kSamplerSlots> view;

private:
    struct Slot
    {
        const TrackAudio* audio = nullptr;
        std::uint64_t id = 0;
        double bpm = 0.0;
        double pos = 0.0;
        bool playing = false;
        bool stopping = false;
        float env = 0.0f;
        double oldPos = 0.0;
        int oldLeft = 0;      // declick of a retrigger
    };
    void start (Slot& s) noexcept;

    std::array<Slot, kSamplerSlots> slots_ {};
    double outRate_ = 48000.0;
    float attack_ = 1.0f / 64.0f, release_ = 1.0f / 256.0f;
};

} // namespace dj
