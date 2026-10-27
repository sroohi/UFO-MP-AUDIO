// One DJ deck: transport, cues, loops, slip, scratch, key lock and stem buses. Audio thread only,
// except the atomics in DeckView / DeckInputs, which are the message-thread interface.
//
// Positions are in track frames (the track's own sample rate). Every discontinuity (hot cue, loop wrap,
// beat jump, slip return) is crossfaded with the continuing stream over `kDeclickFrames`.
//
// A deck renders up to three stereo buses so that FX can target stems (FLX10 FX PART SELECT):
//   bus 0 = the stems selected for FX ("FX part"; the whole track when all parts are selected),
//   bus 1 = the remaining stems (silent when all parts are selected),
//   bus 2 = the Stem FX echo send.
#pragma once
#include "Common.h"
#include "Effects.h"
#include "TimeStretch.h"
#include "TrackAudio.h"
#include <array>
#include <atomic>
#include <memory>

namespace dj
{

constexpr int kDeckBuses = 3;
constexpr int kDeclickFrames = 128;

enum class HotCueWhenPaused : int { Play = 0, Gate = 1, Jump = 2 };
enum class SlicerMode : int { Off = 0, Slicer = 1, SlicerLoop = 2 };
enum class PitchPlayRange : int { Up = 0, Middle = 1, Down = 2 };

// Serato tempo ranges selectable with SHIFT + TEMPO RESET (manual p.14).
constexpr double kTempoRanges[] { 0.08, 0.16, 0.50 };
constexpr int kNumTempoRanges = 3;

// Pitch Play / Key Shift pad tables (manual: Up / Middle / Down), pads 1..8.
inline int pitchPlaySemitones (PitchPlayRange range, int pad, int offset) noexcept
{
    static constexpr int up[8] { 4, 5, 6, 7, 0, 1, 2, 3 };
    static constexpr int mid[8] { 0, 1, 2, 3, -4, -3, -2, -1 };
    static constexpr int down[8] { -3, -2, -1, 0, -7, -6, -5, -4 };
    const int* t = range == PitchPlayRange::Up ? up : range == PitchPlayRange::Middle ? mid : down;
    return t[std::clamp (pad, 0, 7)] + offset;
}

struct LoopRegion
{
    double in = -1.0, out = -1.0;
    bool valid() const noexcept { return in >= 0.0 && out > in; }
    double length() const noexcept { return out - in; }
};

struct LoopSlot
{
    LoopRegion region;
    bool locked = false;
};

struct PadFxPreset
{
    FxType type = FxType::None;
    float beats = 0.5f;
    float amount = 0.8f;
    float params[3] { 0.5f, 0.5f, 0.5f };
};

// Message-thread -> audio-thread continuous inputs (written from the UI or the MIDI thread).
struct DeckInputs
{
    std::atomic<float> tempoFader { 0.5f };      // 0..1, 0.5 = 0 %, 0 = "-" end
    std::atomic<bool> tempoFaderMoved { false }; // set by the writer, consumed by the deck (soft takeover)
    std::atomic<float> stemGain[kNumStems] { 1.0f, 1.0f, 1.0f, 1.0f }; // Part ISO knobs / UI sliders
    // jog wheel ticks accumulated since the last audio block
    std::atomic<int> scratchTicks { 0 }, bendTicks { 0 }, seekTicks { 0 };
    std::atomic<int> gridBpmTicks { 0 }, gridSlideTicks { 0 }, loopInTicks { 0 }, loopOutTicks { 0 };
    std::atomic<int> ticksPerRevolution { 720 };
    std::atomic<float> brakeSeconds { 0.0f }, startSeconds { 0.0f };
    std::atomic<int> hotCueWhenPaused { (int) HotCueWhenPaused::Play };
    std::atomic<bool> slipReverseAutoCancel { true };  // FLX10: cancels after 8 beats
};

// Audio-thread -> UI state. Written once per block.
struct DeckView
{
    std::atomic<bool> loaded { false };
    std::atomic<std::uint64_t> audioId { 0 };
    std::atomic<double> position { 0.0 };         // track frames
    std::atomic<double> slipPosition { -1.0 };    // background position while a slip action is held
    std::atomic<std::int64_t> length { 0 };
    std::atomic<double> trackSampleRate { 44100.0 };
    std::atomic<bool> playing { false };
    std::atomic<double> speed { 0.0 };            // current playback speed relative to 1.0 (0 when stopped)
    std::atomic<double> tempo { 0.0 };            // tempo fader value as a fraction (+0.05 = +5 %)
    std::atomic<int> tempoRangeIndex { 0 };
    std::atomic<double> bpm { 0.0 };              // effective BPM (track BPM * tempo)
    std::atomic<double> trackBpm { 0.0 };
    std::atomic<double> gridAnchor { 0.0 };
    std::atomic<bool> keylock { false };
    std::atomic<int> keyShift { 0 };              // semitones
    std::atomic<bool> sync { false };
    std::atomic<bool> leader { false };
    std::atomic<bool> slip { false };
    std::atomic<bool> slipActive { false };
    std::atomic<bool> reverse { false };
    std::atomic<bool> censor { false };
    std::atomic<bool> vinyl { true };
    std::atomic<bool> scratching { false };
    std::atomic<bool> jogTouched { false };
    std::atomic<double> cuePoint { 0.0 };
    std::atomic<double> hotCue[kNumHotCues];
    std::atomic<double> loopIn { -1.0 }, loopOut { -1.0 };
    std::atomic<bool> loopActive { false };
    std::atomic<bool> rollActive { false };
    std::atomic<double> rollIn { -1.0 }, rollOut { -1.0 };
    std::atomic<int> loopSlot { 0 };
    std::atomic<double> loopSlotIn[kNumSavedLoops], loopSlotOut[kNumSavedLoops];
    std::atomic<bool> loopSlotLocked[kNumSavedLoops];
    std::atomic<int> slicerMode { 0 };
    std::atomic<double> slicerDomainStart { -1.0 };
    std::atomic<double> slicerDomainBeats { 8.0 };
    std::atomic<int> activeSlice { -1 };
    std::atomic<bool> stemsAvailable { false };
    std::atomic<float> stemGain[kNumStems];       // effective (mute x ISO x Part Instant Doubles)
    std::atomic<bool> stemMuted[kNumStems];
    std::atomic<bool> stemEcho[kNumStems];
    std::atomic<bool> padFxActive[kNumPadFx];
    std::atomic<float> peak { 0.0f };
    std::atomic<bool> endReached { false };

    DeckView()
    {
        for (auto& h : hotCue) h.store (-1.0);
        for (int i = 0; i < kNumSavedLoops; ++i) { loopSlotIn[i].store (-1.0); loopSlotOut[i].store (-1.0); loopSlotLocked[i].store (false); }
        for (int s = 0; s < kNumStems; ++s) { stemGain[s].store (1.0f); stemMuted[s].store (false); stemEcho[s].store (false); }
        for (auto& p : padFxActive) p.store (false);
    }
};

// Events the deck reports so the library can persist cue and loop edits.
struct DeckEvent
{
    enum class Kind : std::uint8_t
    {
        CueSet, HotCueSet, HotCueDeleted, LoopSlotSet, LoopSlotDeleted, LoopSlotLocked, GridChanged, TrackEnded, AudioReleased
    } kind = Kind::CueSet;
    int deck = -1;
    int index = -1;
    double a = 0.0, b = 0.0;
    std::uint64_t audioId = 0;
};

// Information from the engine about the rest of the mix (sync leader, global settings).
struct DeckContext
{
    double outputRate = 48000.0;
    bool quantize = false;
    double quantizeBeats = 1.0;     // quantize resolution in beats
    // sync
    bool followLeader = false;      // this deck has sync on and a leader other than itself is playing
    double leaderBpm = 0.0;
    double leaderBeatPhase = 0.0;   // fractional beat of the leader at block start
    bool fxPart[kNumStems] { true, true, true, true };
};

class Deck
{
public:
    Deck();

    // message thread, no audio callback running
    void prepare (double outputRate, int maxBlock);
    void setPadFx (int index, std::unique_ptr<Effect> fx, const PadFxPreset& preset); // before prepare/start only
    // Stem echo processors are created in prepare(); nothing here allocates on the audio thread.

    // ---- audio thread ------------------------------------------------------------------------
    // Renders numFrames into the three bus pairs. busActive[1]/[2] report whether those buses carry audio.
    void process (float* const* bus, int numFrames, const DeckContext& ctx, bool* busActive) noexcept DJ_NONBLOCKING;

    // loading
    void load (const TrackAudio* audio, std::uint64_t audioId) noexcept;
    void replaceAudio (const TrackAudio* audio, std::uint64_t audioId) noexcept; // same track, stems added
    void unload() noexcept;
    const TrackAudio* audio() const noexcept { return audio_; }
    std::uint64_t audioId() const noexcept { return audioId_; }
    void setGrid (const Beatgrid& g) noexcept;
    const Beatgrid& grid() const noexcept { return grid_; }
    void setCuePoint (double pos) noexcept { cuePoint_ = pos; }
    void setHotCue (int i, double pos) noexcept { if (i >= 0 && i < kNumHotCues) hotCues_[(std::size_t) i] = pos; }
    void setLoopSlot (int i, double in, double out, bool locked) noexcept;
    void setKeyInfo (int musicalKey) noexcept { musicalKey_ = musicalKey; }
    int musicalKey() const noexcept { return musicalKey_; }

    // transport
    void playPause() noexcept;
    void play() noexcept;
    void pause() noexcept;
    void stutter() noexcept;                // SHIFT+PLAY: play from the temporary cue
    void cueDown() noexcept;
    void cueUp() noexcept;
    void jumpToStart() noexcept;            // SHIFT+CUE
    // Double-click CUE: play from the top of the track. A cue point moved by the first click is put back.
    void restartFromStart() noexcept;
    void searchDown (int direction) noexcept; // 4 BEAT JUMP long press / Beat Jump pads 6-7
    void searchUp() noexcept;

    // hot cues
    void hotCueDown (int i, bool shift) noexcept;   // shift = delete
    void hotCueUp (int i) noexcept;
    int lastHotCue() const noexcept { return lastHotCue_; }

    // loops
    void loopIn() noexcept;                 // LOOP IN / 1/2X
    void loopOut() noexcept;                // LOOP OUT / 2X
    void reloop() noexcept;                 // SHIFT + LOOP OUT
    void exitLoop() noexcept;
    void fourBeatExit() noexcept;           // 4 BEAT / EXIT
    void toggleLoopActive() noexcept;       // SHIFT + 4 BEAT / EXIT ("Active Loop")
    void autoLoop (double beats) noexcept;  // toggles a loop of `beats` (Auto Loop pads)
    void loopHalve() noexcept;
    void loopDouble() noexcept;
    void loopShift (double beats) noexcept;
    void selectLoopSlot (int delta) noexcept;   // CUE/LOOP CALL < >
    void toggleLoopSlotLock() noexcept;         // SHIFT + CUE/LOOP CALL <
    void deleteLoopSlot() noexcept;             // SHIFT + CUE/LOOP CALL >
    void loopSlotPad (int i, bool shift) noexcept; // Saved Loop pad mode
    void rollDown (double beats) noexcept;
    void rollUp() noexcept;
    void beatJump (double beats) noexcept;

    // slicer
    void setSlicer (SlicerMode mode, double domainBeats, double quantize) noexcept;
    void sliceDown (int slice) noexcept;
    void sliceUp (int slice) noexcept;

    // slip / reverse / jog
    void setSlip (bool on) noexcept;
    bool slip() const noexcept { return slipEnabled_; }
    void censorDown() noexcept;             // SLIP REVERSE hold
    void censorUp() noexcept;
    void toggleReverse() noexcept;          // SHIFT + SLIP REVERSE
    void setVinyl (bool on) noexcept { vinyl_ = on; }
    bool vinyl() const noexcept { return vinyl_; }
    void jogTouch (bool down) noexcept;

    // tempo / key
    void tempoReset() noexcept;
    void cycleTempoRange() noexcept;
    void setTempoRange (int index) noexcept;
    double tempo() const noexcept { return tempo_; }
    void setTempoFromSync (double tempo) noexcept; // fader is soft-taken-over afterwards
    void setKeylock (bool on) noexcept { keylock_ = on; }
    bool keylock() const noexcept { return keylock_; }
    void setKeyShift (int semitones) noexcept { keyShift_ = std::clamp (semitones, -12, 12); }
    int keyShift() const noexcept { return keyShift_; }
    // How far the deck sounds from the track's key: key shift, plus the tempo fader's pitch when key lock
    // is off (+6 % on a turntable is about a semitone up).
    double soundingSemitones() const noexcept { return keyShift_ + (keylock_ ? 0.0 : 12.0 * std::log2 (std::max (0.01, 1.0 + tempo_))); }
    void pitchPlay (int semitones) noexcept;        // jump to the Pitch Play cue at a key
    void setPitchPlayCue (int i) noexcept { pitchPlayCue_ = i; }

    // stems
    void toggleStemMute (int stemMask) noexcept;    // ACTIVE PART buttons / Stems pads
    void setStemMute (int stem, bool muted) noexcept;
    void setPartSolo (int stemMask) noexcept;       // Part Instant Doubles: play only these parts (0 = off)
    void stemEcho (int stemMask) noexcept;          // Stem FX echo-out (toggle)

    // pad FX
    void padFxDown (int i) noexcept;
    void padFxUp (int i) noexcept;

    // sync support
    bool playing() const noexcept { return playing_; }
    bool hasGrid() const noexcept { return audio_ != nullptr && grid_.valid(); }
    double effectiveBpm() const noexcept { return grid_.valid() ? grid_.bpm * (1.0 + tempo_) : 0.0; }
    double beatPhase() const noexcept; // fractional beat at the current position
    void phaseJump (double beats) noexcept;
    void setSyncState (bool sync, bool leader) noexcept { sync_ = sync; leader_ = leader; if (sync && ! leader) syncSnapPending_ = true; }
    // Set when a synced follower's tempo fader takes over: the engine makes this deck the leader.
    bool takeLeaderRequest() noexcept { const bool r = leaderRequest_; leaderRequest_ = false; return r; }
    bool sync() const noexcept { return sync_; }
    double position() const noexcept { return pos_; }
    void setPosition (double pos) noexcept { jumpTo (pos); }

    // full state copy for Instant Doubles (same audio pointer)
    void copyStateFrom (const Deck& other) noexcept;

    DeckInputs inputs;
    DeckView view;
    int index = 0;

    // events (cue edits) are drained by the engine after process()
    template <typename Fn> void drainEvents (Fn&& fn) noexcept
    {
        for (int i = 0; i < numEvents_; ++i) fn (events_[(std::size_t) i]);
        numEvents_ = 0;
    }

private:
    struct Source; // reads the track through stems coefficients and loop folding

    void jumpTo (double pos) noexcept;      // crossfaded jump
    double framesPerBeat() const noexcept;
    double snap (double pos) const noexcept;  // quantized cue placement
    double quantizedJumpTarget (double target) const noexcept;
    double cellStartInTrack (double pos, double beats) const noexcept;
    void beginSlip() noexcept;
    void endSlip() noexcept;
    void storeLoopInSlot() noexcept;
    void emit (DeckEvent::Kind k, int idx, double a, double b = 0.0) noexcept;
    void updateStemTargets() noexcept;
    void publish (const DeckContext& ctx, float peak) noexcept;
    void readDirect (const Source& src, double pos, float* out) const noexcept;

    double outRate_ = 48000.0;
    const TrackAudio* audio_ = nullptr;
    std::uint64_t audioId_ = 0;
    Beatgrid grid_;
    int musicalKey_ = -1;

    // transport
    double pos_ = 0.0;
    bool playing_ = false;
    double motor_ = 0.0;            // 0..1 platter speed (brake / start)
    double tempo_ = 0.0;            // fraction
    int tempoRange_ = 0;
    bool faderPickup_ = false;      // soft takeover after reset / sync
    float lastFaderPos_ = 0.5f;
    bool quantize_ = false;
    double quantizeBeats_ = 1.0;
    bool endEmitted_ = false;
    double syncCorrection_ = 0.0;
    double bend_ = 0.0, bendTarget_ = 0.0;
    bool keylock_ = false;
    int keyShift_ = 0;
    bool sync_ = false, leader_ = false;
    bool syncSnapPending_ = false;
    bool leaderRequest_ = false;

    // cue logic (CDJ / Serato)
    double cuePoint_ = 0.0;
    double cueBeforeLastSet_ = -1.0; // cue point the last CUE press replaced (-1: it did not set one)
    bool cuePreview_ = false;       // CUE held: playing as a preview
    bool playLatchedDuringCue_ = false;
    std::array<double, kNumHotCues> hotCues_ {};
    int heldHotCue_ = -1;
    bool hotCueGate_ = false;       // hot cue pressed while paused in Gate mode
    int lastHotCue_ = -1;
    int pitchPlayCue_ = -1;

    // loops
    LoopRegion loop_;
    bool loopActive_ = false;
    bool loopInPending_ = false;    // LOOP IN pressed, waiting for LOOP OUT
    double pendingLoopIn_ = -1.0;
    LoopRegion roll_;
    bool rollActive_ = false;
    std::array<LoopSlot, kNumSavedLoops> loopSlots_ {};
    int loopSlot_ = 0;

    // slicer
    SlicerMode slicerMode_ = SlicerMode::Off;
    double slicerDomainBeats_ = 8.0, slicerQuant_ = 1.0;
    double slicerDomainStart_ = -1.0;
    int heldSlice_ = -1;

    // slip
    bool slipEnabled_ = false;
    int slipHolds_ = 0;
    double slipPos_ = 0.0;
    bool slipRunning_ = false;      // the background position advances (deck was playing / slip braking)
    bool slipLoop_ = false;         // the active loop holds a slip session
    bool slipBrake_ = false;        // paused with slip on: PLAY resumes at the background position
    bool rollHeld_ = false;

    // reverse / censor / search / scratch
    bool reverse_ = false;
    bool censor_ = false;
    double censorStartBeat_ = 0.0;
    int search_ = 0;
    bool vinyl_ = true;
    bool touched_ = false;
    bool scratching_ = false;
    double scratchX_ = 0.0, scratchV_ = 0.0, scratchMeasured_ = 0.0;

    // stems
    std::array<float, kNumStems> stemMute_ { 1, 1, 1, 1 };   // 0 = muted
    std::array<float, kNumStems> partSolo_ { 1, 1, 1, 1 };
    std::array<Smoothed, kNumStems> stemGainSm_ {};
    std::array<Smoothed, kNumStems> stemEchoSend_ {};
    std::array<bool, kNumStems> stemEchoOn_ {};
    double stemEchoFeedLeft_ = 0.0; // frames of feed left for the echo-out
    std::unique_ptr<Effect> stemEchoFx_;
    FxRunner stemEchoRunner_;

    // pad FX
    struct PadFxSlot
    {
        std::unique_ptr<Effect> fx;
        PadFxPreset preset;
        FxRunner runner;
    };
    std::array<PadFxSlot, kNumPadFx> padFx_ {};

    // rendering
    TimeStretcher stretcher_;
    bool stretching_ = false;
    double fadePos_ = 0.0, fadeRate_ = 0.0;
    int fadeLeft_ = 0;
    std::vector<float> scratchBuf_;
    std::vector<float> fxL_, fxR_, wetL_, wetR_;

    std::array<DeckEvent, 32> events_ {};
    int numEvents_ = 0;
};

} // namespace dj
