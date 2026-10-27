// DEEJ-AY by MP Audio — the real-time DJ engine. One instance per app.
//
// Threading contract:
//   * prepare() runs on the message thread while no audio callback is active.
//   * process() runs on the audio thread: no allocation, no locks, no I/O.
//   * post() and the "message thread" helpers run on one thread (the UI / MIDI dispatcher). MIDI input
//     arriving on another thread is forwarded through the app's own queue, or writes only the atomic
//     inputs (faders, knobs, jog ticks), which are safe from any thread.
//   * Audio memory (TrackAudio) and effect instances are owned on the message thread and freed only
//     after the audio thread reports it no longer uses them (AudioReleased / retired effects).
#pragma once
#include "ColorFx.h"
#include "Deck.h"
#include "FxUnit.h"
#include "Mixer.h"
#include "Recorder.h"
#include "Sampler.h"
#include "SpscFifo.h"
#include <map>
#include <memory>
#include <vector>

namespace dj
{

enum class Cmd : std::uint16_t
{
    // deck data
    DeckLoad, DeckReplaceAudio, DeckUnload, DeckSetGrid, DeckSetCuePoint, DeckSetHotCue, DeckSetLoopSlot, DeckSetKey,
    InstantDouble, PartInstantDouble,
    // transport
    PlayPause, Play, Pause, Stutter, CueDown, CueUp, JumpToStart, SearchDown, SearchUp, SetPosition,
    HotCueDown, HotCueUp,
    // loops
    LoopIn, LoopOut, Reloop, LoopExit, FourBeatExit, LoopActiveToggle, AutoLoop, LoopHalve, LoopDouble, LoopShift,
    LoopSlotSelect, LoopSlotLock, LoopSlotDelete, LoopSlotPad,
    RollDown, RollUp, BeatJump,
    SlicerSet, SliceDown, SliceUp,
    // performance
    SlipSet, SlipToggle, CensorDown, CensorUp, ReverseToggle, VinylSet, VinylToggle, JogTouch,
    TempoReset, TempoRangeCycle, TempoRangeSet,
    SyncOn, SyncOff, SyncToggle, SetLeader,
    KeylockSet, KeylockToggle, KeyShiftSet, KeyShiftDelta, KeySync, KeyReset, PitchPlay, PitchPlayCue,
    StemMuteToggle, StemMuteSet, StemEcho, PartSolo,
    PadFxDown, PadFxUp,
    // sampler
    SamplerLoad, SamplerPadDown, SamplerPadUp, SamplerStop, SamplerStopAll,
    // fx units
    FxInstall, FxSlotOn, FxSlotToggle,
    // global
    QuantizeSet, QuantizeToggle, QuantizeBeats
};

struct Command
{
    Cmd type = Cmd::Play;
    int deck = 0;           // deck index, or FX unit / sampler slot for those commands
    int index = 0;          // hot cue / slot / pad / target / source deck
    double value = 0.0, value2 = 0.0;
    bool flag = false;
    const TrackAudio* audio = nullptr;
    std::uint64_t id = 0;
    Effect* fx = nullptr;
    FxType fxType = FxType::None;
};

struct OutputRouting
{
    std::atomic<int> masterL { 0 }, masterR { 1 };
    std::atomic<int> phonesL { 2 }, phonesR { 3 };
    std::atomic<int> boothL { -1 }, boothR { -1 };
};

struct EngineView
{
    std::atomic<int> leader { -1 };
    std::atomic<double> masterBpm { 0.0 };
    std::atomic<bool> quantize { true };
    std::atomic<double> quantizeBeats { 1.0 };
    std::atomic<double> cpuLoad { 0.0 };
};

class Engine
{
public:
    Engine();
    ~Engine();

    void prepare (double sampleRate, int maxBlock);
    double sampleRate() const noexcept { return sampleRate_; }

    // ---- audio thread -------------------------------------------------------------------------
    void process (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numFrames) noexcept DJ_NONBLOCKING;

    // ---- message thread -----------------------------------------------------------------------
    bool post (const Command& c) noexcept { return commands_.push (c); }
    // The MIDI input thread has its own queue so controller latency does not depend on the UI thread.
    bool postFromMidiThread (const Command& c) noexcept { return midiCommands_.push (c); }
    bool deckCommand (Cmd type, int deck, int index = 0, double value = 0.0, bool flag = false) noexcept
    {
        Command c; c.type = type; c.deck = deck; c.index = index; c.value = value; c.flag = flag;
        return post (c);
    }

    // Loads shared audio into a deck / sampler slot; the engine keeps it alive until released.
    void loadDeck (int deck, std::shared_ptr<const TrackAudio> audio);
    void replaceDeckAudio (std::uint64_t oldId, std::shared_ptr<const TrackAudio> audio); // stems arrived
    void loadSampler (int slot, std::shared_ptr<const TrackAudio> audio, double bpm);
    void instantDouble (int fromDeck, int toDeck, int partMask = 0);
    std::uint64_t deckAudioId (int deck) const { return slotAudio_[(std::size_t) deck]; }
    std::shared_ptr<const TrackAudio> audioFor (std::uint64_t id) const;
    // Creates the effect instances for every target and installs them in an FX unit slot.
    void setFxType (int unit, int slot, FxType type);
    // Drains audio-thread events; frees released audio / effects. Returns cue/loop/grid/end events.
    template <typename Fn> void pollEvents (Fn&& fn)
    {
        DeckEvent e;
        while (events_.pop (e))
        {
            if (e.kind == DeckEvent::Kind::AudioReleased) { releaseAudio (e.audioId); continue; }
            fn (e);
        }
        Effect* fx;
        while (retiredFx_.pop (fx)) retireEffect (fx);
    }

    Deck& deck (int i) noexcept { return *decks_[(std::size_t) i]; }
    FxUnit& fxUnit (int i) noexcept { return fxUnits_[(std::size_t) i]; }
    Sampler& sampler() noexcept { return sampler_; }
    MixerControls mixer;
    MixerView mixerView;
    OutputRouting routing;
    EngineView view;
    std::atomic<bool> fxPart[kNumStems] { true, true, true, true };   // FX PART SELECT
    std::atomic<int> samplerBank { 0 };                                // shared by UI and pads
    Recorder recorder;

private:
    void applyCommand (const Command& c) noexcept;
    void applyDeckCommand (Deck& d, const Command& c) noexcept;
    void updateLeader() noexcept;
    void syncOn (int d) noexcept;
    void syncOff (int d) noexcept;
    void keySync (int d) noexcept;
    double unitBpm (int unit) const noexcept;
    void processBlock (float* const* outputs, int numOutputs, int n) noexcept;
    void releaseAudio (std::uint64_t id);
    void retireEffect (Effect* fx);
    void releaseFromAudioThread (std::uint64_t id) noexcept;
    std::uint64_t holdAudio (std::shared_ptr<const TrackAudio> audio);

    double sampleRate_ = 48000.0;
    int maxBlock_ = 1024;
    std::vector<std::unique_ptr<Deck>> decks_;
    std::array<FxUnit, kTotalFxUnits> fxUnits_ {};
    Sampler sampler_;
    std::array<ColorFx, kNumDecks> colorFx_ {};
    std::array<ChannelEq, kNumDecks> eqA_ {}, eqB_ {};
    Limiter limiter_;

    // audio-thread state
    bool quantize_ = true;
    double quantizeBeats_ = 1.0;
    int leader_ = -1;
    std::array<float, kNumDecks> lastTrim_ {}, lastFader_ {}, lastXf_ {};
    float lastMaster_ = 1.0f, lastSamplerVol_ = 0.8f;
    std::vector<float> deckBus_[kNumDecks][kDeckBuses * 2];
    std::vector<float> masterL_, masterR_, cueL_, cueR_, smpL_, smpR_;

    SpscFifo<Command> commands_ { 2048 };
    SpscFifo<Command> midiCommands_ { 2048 };
    SpscFifo<DeckEvent> events_ { 1024 };
    SpscFifo<Effect*> retiredFx_ { 256 };

    // message-thread ownership
    std::uint64_t nextAudioId_ = 0;
    struct Held { std::shared_ptr<const TrackAudio> audio; int refs = 0; };
    std::map<std::uint64_t, Held> held_;
    std::array<std::uint64_t, kNumDecks> slotAudio_ {};
    std::array<std::uint64_t, kSamplerSlots> samplerAudio_ {};
    std::vector<std::unique_ptr<Effect>> ownedFx_;
};

} // namespace dj
