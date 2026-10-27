// FX units. Two Serato-style units (FX1 / FX2: Multi FX = three effects with their own levels, or
// Single FX = one effect with three parameters) and one DDJ-FLX10-style BEAT FX unit (one effect,
// BEAT </>, LEVEL/DEPTH, CH SELECT incl. MASTER and SAMPLER).
//
// A unit can be assigned to several targets at once (channels 1-4, master, sampler). Each target has
// its own effect instances, so echoes on two decks do not share a delay line. Instances are created
// on the message thread (Engine::setFxType) and installed through the command queue.
#pragma once
#include "Effects.h"
#include <array>
#include <atomic>

namespace dj
{

enum class FxTarget : int { Ch1 = 0, Ch2, Ch3, Ch4, Master, Sampler };
constexpr int kFxTargets = 6;
enum class FxPlacement : int { PreEq = 0, PreFader = 1, PostFader = 2 };

constexpr int kBeatFxUnit = 2;      // engine index of the FLX10 BEAT FX unit
constexpr int kTotalFxUnits = 3;

struct FxUnitControls
{
    std::atomic<bool> multi { true };                       // Multi FX (3 slots) / Single FX
    std::atomic<float> depth { 1.0f };                      // unit depth (Multi) or effect amount (Single)
    std::atomic<float> slotLevel[kFxSlotsPerUnit] { 0.8f, 0.8f, 0.8f };
    std::atomic<float> params[3] { 0.5f, 0.5f, 0.5f };      // Single FX parameters
    std::atomic<float> beats { 1.0f };                      // beat multiplier (negative: falling)
    std::atomic<bool> assign[kFxTargets] { false, false, false, false, false, false };
    std::atomic<int> placement { (int) FxPlacement::PostFader };
    std::atomic<bool> tapMode { false };                    // FLX10 SHIFT+BEAT >: TAP; SHIFT+BEAT <: AUTO
    std::atomic<double> tapBpm { 120.0 };
};

struct FxUnitView
{
    std::atomic<int> type[kFxSlotsPerUnit] { 0, 0, 0 };
    std::atomic<bool> on[kFxSlotsPerUnit] { false, false, false };
    std::atomic<double> bpm { 120.0 };
};

class FxUnit
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        for (auto& row : runners_) for (auto& r : row) r.prepare (sampleRate, maxBlock);
    }

    // ---- audio thread (via commands)
    void installEffect (int slot, int target, Effect* fx, FxType type) noexcept
    {
        if (slot < 0 || slot >= kFxSlotsPerUnit || target < 0 || target >= kFxTargets) return;
        runners_[(std::size_t) slot][(std::size_t) target].setEffect (fx);
        types_[(std::size_t) slot] = type;
        view.type[slot].store ((int) type, std::memory_order_relaxed);
    }
    Effect* installed (int slot, int target) const noexcept { return runners_[(std::size_t) slot][(std::size_t) target].effect(); }
    void setSlotOn (int slot, bool on) noexcept
    {
        if (slot < 0 || slot >= kFxSlotsPerUnit) return;
        slotOn_[(std::size_t) slot] = on;
        view.on[slot].store (on, std::memory_order_relaxed);
    }
    bool slotOn (int slot) const noexcept { return slotOn_[(std::size_t) slot]; }
    FxType slotType (int slot) const noexcept { return types_[(std::size_t) slot]; }

    // Is anything (an active slot or a ringing tail) to be processed for this target?
    bool active (int target) const noexcept
    {
        for (int s = 0; s < kFxSlotsPerUnit; ++s)
            if (runners_[(std::size_t) s][(std::size_t) target].running() || (slotOn_[(std::size_t) s] && controls.assign[target].load (std::memory_order_relaxed)))
                return true;
        return false;
    }

    void process (int target, float* l, float* r, int n, double bpm) noexcept
    {
        const bool assigned = controls.assign[target].load (std::memory_order_relaxed);
        const bool multi = controls.multi.load (std::memory_order_relaxed);
        const float depth = controls.depth.load (std::memory_order_relaxed);
        const float beats = controls.beats.load (std::memory_order_relaxed);
        for (int s = 0; s < kFxSlotsPerUnit; ++s)
        {
            auto& runner = runners_[(std::size_t) s][(std::size_t) target];
            const bool want = assigned && slotOn_[(std::size_t) s] && (multi || s == 0);
            if (want != runner.on()) runner.setOn (want);
            if (! runner.running()) continue;
            FxContext ctx;
            ctx.bpm = bpm;
            ctx.beats = beats;
            ctx.amount = multi ? controls.slotLevel[s].load (std::memory_order_relaxed) * depth : depth;
            for (int k = 0; k < 3; ++k)
                ctx.params[k] = multi ? fxParamDefault (types_[(std::size_t) s], k) : controls.params[k].load (std::memory_order_relaxed);
            runner.process (l, r, n, ctx);
        }
    }

    FxUnitControls controls;
    FxUnitView view;

private:
    std::array<std::array<FxRunner, kFxTargets>, kFxSlotsPerUnit> runners_ {};
    std::array<bool, kFxSlotsPerUnit> slotOn_ {};
    std::array<FxType, kFxSlotsPerUnit> types_ {};
};

} // namespace dj
