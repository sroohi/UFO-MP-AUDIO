#include "Deck.h"

namespace dj
{

namespace
{
    constexpr double kRevolutionSeconds = 1.8;   // 33 1/3 RPM: one platter turn = 1.8 s of audio
    constexpr double kScratchAlpha = 0.25;       // alpha-beta filter (Kalata 1984) on the jog position
    constexpr double kScratchBeta = 0.036;
    constexpr double kMaxSyncCorrection = 0.02;  // max phase-correction speed change while synced
    constexpr int kCoeffInterval = 32;           // stem coefficients are refreshed every 32 frames
}

// Reads the track through per-bus stem coefficients, folding positions back into an active loop
// (the key-lock grains read past the loop end).
struct Deck::Source
{
    const TrackAudio* a = nullptr;
    float gMix[kDeckBuses] {};
    float coeff[kDeckBuses][3] {};
    bool active[kDeckBuses] {};
    bool fold = false;
    double foldIn = 0.0, foldOut = 0.0;

    void raw (std::int64_t i, float* o) const noexcept
    {
        if (i < 0 || i >= a->frames)
        {
            for (int c = 0; c < kDeckBuses * 2; ++c) o[c] = 0.0f;
            return;
        }
        const auto k = (std::size_t) i * 2;
        const float mL = a->mix[k], mR = a->mix[k + 1];
        float sL[3] { 0, 0, 0 }, sR[3] { 0, 0, 0 };
        if (a->hasStems)
            for (int s = 0; s < 3; ++s)
            {
                sL[s] = (float) a->stems[s][k] * a->stemScale;
                sR[s] = (float) a->stems[s][k + 1] * a->stemScale;
            }
        for (int b = 0; b < kDeckBuses; ++b)
        {
            if (! active[b]) { o[b * 2] = o[b * 2 + 1] = 0.0f; continue; }
            float l = gMix[b] * mL, r = gMix[b] * mR;
            for (int s = 0; s < 3; ++s) { l += coeff[b][s] * sL[s]; r += coeff[b][s] * sR[s]; }
            o[b * 2] = l; o[b * 2 + 1] = r;
        }
    }

    void frame (double pos, float* out) const noexcept
    {
        if (fold && pos >= foldOut && foldOut > foldIn)
            pos = foldIn + std::fmod (pos - foldIn, foldOut - foldIn);
        const double fl = std::floor (pos);
        const auto i0 = (std::int64_t) fl;
        const float t = (float) (pos - fl);
        float y[4][kDeckBuses * 2];
        for (int k = 0; k < 4; ++k) raw (i0 - 1 + k, y[k]);
        for (int c = 0; c < kDeckBuses * 2; ++c) out[c] = hermite (y[0][c], y[1][c], y[2][c], y[3][c], t);
    }
};

Deck::Deck()
{
    hotCues_.fill (-1.0);
}

void Deck::prepare (double outputRate, int maxBlock)
{
    outRate_ = outputRate;
    stretcher_.prepare (outputRate, kDeckBuses * 2);
    fxL_.assign ((std::size_t) maxBlock, 0.0f);
    fxR_.assign ((std::size_t) maxBlock, 0.0f);
    wetL_.assign ((std::size_t) maxBlock, 0.0f);
    wetR_.assign ((std::size_t) maxBlock, 0.0f);
    for (auto& g : stemGainSm_) { g.prepare (outputRate, 6.0f); g.snap (1.0f); }
    for (auto& e : stemEchoSend_) { e.prepare (outputRate, 4.0f); e.snap (0.0f); }
    if (stemEchoFx_ == nullptr) stemEchoFx_ = makeEffect (FxType::Echo);
    stemEchoFx_->prepare (outputRate, maxBlock);
    stemEchoRunner_.prepare (outputRate, maxBlock);
    stemEchoRunner_.setEffect (stemEchoFx_.get());
    for (auto& p : padFx_)
    {
        p.runner.prepare (outputRate, maxBlock);
        if (p.fx != nullptr) p.fx->prepare (outputRate, maxBlock);
        p.runner.setEffect (p.fx.get());
    }
}

void Deck::setPadFx (int slot, std::unique_ptr<Effect> fx, const PadFxPreset& preset)
{
    if (slot < 0 || slot >= kNumPadFx) return;
    auto& p = padFx_[(std::size_t) slot];
    p.fx = std::move (fx);
    p.preset = preset;
    p.runner.setEffect (p.fx.get());
}

// ---------------------------------------------------------------------------------- helpers
double Deck::framesPerBeat() const noexcept
{
    return audio_ != nullptr ? grid_.framesPerBeat (audio_->sampleRate) : 0.0;
}

double Deck::beatPhase() const noexcept
{
    if (! hasGrid()) return 0.0;
    return fmodPos (grid_.beatAt (pos_, audio_->sampleRate), 1.0);
}

double Deck::snap (double pos) const noexcept
{
    if (! quantize_ || ! hasGrid()) return pos;
    return grid_.roundTo (pos, quantizeBeats_, audio_->sampleRate);
}

// Start of the `beats`-long grid cell containing pos. Before the first beat that cell starts before the
// track; loops, rolls and slicer domains then use the first whole cell inside it instead.
double Deck::cellStartInTrack (double pos, double beats) const noexcept
{
    const double start = grid_.floorTo (pos, beats, audio_->sampleRate);
    if (start >= 0.0) return start;
    const double cell = beats * framesPerBeat();
    return start + std::ceil (-start / cell) * cell;
}

// With quantize on, a jump keeps the playhead's offset within the quantize cell, so the deck stays
// on the beat (cues themselves are snapped to the grid when set).
double Deck::quantizedJumpTarget (double target) const noexcept
{
    if (! quantize_ || ! hasGrid() || ! playing_) return target;
    const double offset = pos_ - grid_.floorTo (pos_, quantizeBeats_, audio_->sampleRate);
    return grid_.floorTo (target + 0.5, quantizeBeats_, audio_->sampleRate) + offset;
}

void Deck::jumpTo (double pos) noexcept
{
    if (audio_ == nullptr) return;
    pos = std::clamp (pos, 0.0, (double) audio_->frames);
    if (! stretching_)
    {
        fadePos_ = pos_;
        fadeRate_ = playing_ ? (reverse_ || censor_ ? -1.0 : 1.0) * (audio_->sampleRate / outRate_) * (1.0 + tempo_) * motor_ : 0.0;
        fadeLeft_ = kDeclickFrames;
    }
    pos_ = pos;
    endEmitted_ = false;
}

void Deck::emit (DeckEvent::Kind k, int idx, double a, double b) noexcept
{
    if (numEvents_ >= (int) events_.size()) return;
    auto& e = events_[(std::size_t) numEvents_++];
    e.kind = k; e.deck = index; e.index = idx; e.a = a; e.b = b; e.audioId = audioId_;
}

void Deck::beginSlip() noexcept
{
    if (slipHolds_++ == 0)
    {
        slipPos_ = pos_;
        slipRunning_ = playing_;
    }
}

void Deck::endSlip() noexcept
{
    if (slipHolds_ <= 0) return;
    if (--slipHolds_ == 0)
    {
        if (slipRunning_ || playing_) jumpTo (slipPos_);
        slipRunning_ = false;
    }
}

// ---------------------------------------------------------------------------------- loading
void Deck::load (const TrackAudio* audio, std::uint64_t id) noexcept
{
    audio_ = audio;
    audioId_ = id;
    pos_ = 0.0;
    playing_ = false;
    motor_ = 0.0;
    grid_ = {};
    musicalKey_ = -1;
    cuePoint_ = 0.0;
    hotCues_.fill (-1.0);
    for (auto& s : loopSlots_) s = {};
    loopSlot_ = 0;
    loop_ = {};
    loopActive_ = loopInPending_ = false;
    roll_ = {};
    rollActive_ = rollHeld_ = false;
    slicerMode_ = SlicerMode::Off;
    heldSlice_ = -1;
    slipHolds_ = 0; slipLoop_ = slipBrake_ = slipRunning_ = false;
    cuePreview_ = playLatchedDuringCue_ = hotCueGate_ = false;
    heldHotCue_ = lastHotCue_ = pitchPlayCue_ = -1;
    reverse_ = censor_ = false;
    search_ = 0;
    scratching_ = false;
    keyShift_ = 0;
    stemMute_.fill (1.0f);
    partSolo_.fill (1.0f);
    stemEchoOn_.fill (false);
    stemEchoFeedLeft_ = 0.0;
    fadeLeft_ = 0;
    stretcher_.reset();
    stretching_ = false;
    endEmitted_ = false;
    syncSnapPending_ = sync_;
}

void Deck::replaceAudio (const TrackAudio* audio, std::uint64_t id) noexcept
{
    audio_ = audio;
    audioId_ = id;
}

void Deck::unload() noexcept
{
    load (nullptr, 0);
}

void Deck::setGrid (const Beatgrid& g) noexcept
{
    grid_ = g;
}

void Deck::setLoopSlot (int i, double in, double out, bool locked) noexcept
{
    if (i < 0 || i >= kNumSavedLoops) return;
    loopSlots_[(std::size_t) i].region = { in, out };
    loopSlots_[(std::size_t) i].locked = locked;
}

void Deck::copyStateFrom (const Deck& o) noexcept
{
    audio_ = o.audio_;
    audioId_ = o.audioId_;
    grid_ = o.grid_;
    musicalKey_ = o.musicalKey_;
    pos_ = o.pos_;
    playing_ = o.playing_;
    motor_ = o.motor_;
    tempo_ = o.tempo_;
    faderPickup_ = true;
    keylock_ = o.keylock_;
    keyShift_ = o.keyShift_;
    cuePoint_ = o.cuePoint_;
    hotCues_ = o.hotCues_;
    loopSlots_ = o.loopSlots_;
    loopSlot_ = o.loopSlot_;
    loop_ = o.loop_;
    loopActive_ = o.loopActive_;
    reverse_ = o.reverse_;
    stemMute_ = o.stemMute_;
    partSolo_.fill (1.0f);
    fadeLeft_ = 0;
    stretcher_.reset();
    stretching_ = false;
    endEmitted_ = false;
}

// -------------------------------------------------------------------------------- transport
void Deck::play() noexcept
{
    if (audio_ == nullptr) return;
    if (cuePreview_) { playLatchedDuringCue_ = true; return; }
    if (slipBrake_)
    {
        slipBrake_ = false;
        playing_ = true;
        endSlip();
    }
    playing_ = true;
    hotCueGate_ = false;
    if (sync_ && ! leader_) syncSnapPending_ = true;
    if (pos_ >= (double) audio_->frames) pos_ = 0.0;
}

void Deck::pause() noexcept
{
    if (audio_ == nullptr) return;
    if (playing_ && slipEnabled_ && inputs.brakeSeconds.load (std::memory_order_relaxed) > 0.0f && ! slipBrake_)
    {
        // Slip Braking: the background keeps running while the platter slows down
        beginSlip();
        slipRunning_ = true;
        slipBrake_ = true;
    }
    playing_ = false;
}

void Deck::playPause() noexcept
{
    if (cuePreview_) { playLatchedDuringCue_ = true; return; }
    if (playing_) pause(); else play();
}

void Deck::stutter() noexcept
{
    if (audio_ == nullptr) return;
    jumpTo (cuePoint_);
    playing_ = true;
    motor_ = 1.0;
}

// CDJ / Serato temporary cue:
//   playing          -> back cue (return to the cue point and pause)
//   paused at cue    -> cue point sampler (plays while held, returns on release; PLAY latches)
//   paused elsewhere -> set the cue point here (snapped to the grid when quantize is on)
void Deck::cueDown() noexcept
{
    if (audio_ == nullptr) return;
    if (playing_)
    {
        jumpTo (cuePoint_);
        playing_ = false;
        motor_ = 0.0;
        slipBrake_ = false;
        return;
    }
    if (std::abs (pos_ - cuePoint_) < 2.0)
    {
        cuePreview_ = true;
        playLatchedDuringCue_ = false;
        playing_ = true;
        motor_ = 1.0;
        return;
    }
    cuePoint_ = snap (pos_);
    jumpTo (cuePoint_);
    emit (DeckEvent::Kind::CueSet, 0, cuePoint_);
}

void Deck::cueUp() noexcept
{
    if (! cuePreview_) return;
    cuePreview_ = false;
    if (playLatchedDuringCue_) { playLatchedDuringCue_ = false; return; }
    playing_ = false;
    motor_ = 0.0;
    jumpTo (cuePoint_);
}

void Deck::jumpToStart() noexcept
{
    jumpTo (0.0);
}

void Deck::searchDown (int direction) noexcept { search_ = direction < 0 ? -1 : 1; }
void Deck::searchUp() noexcept { search_ = 0; }

// --------------------------------------------------------------------------------- hot cues
void Deck::hotCueDown (int i, bool shift) noexcept
{
    if (audio_ == nullptr || i < 0 || i >= kNumHotCues) return;
    auto& cue = hotCues_[(std::size_t) i];
    if (shift)
    {
        if (cue >= 0.0) { cue = -1.0; emit (DeckEvent::Kind::HotCueDeleted, i, 0.0); }
        return;
    }
    if (cue < 0.0)
    {
        cue = snap (pos_);
        emit (DeckEvent::Kind::HotCueSet, i, cue);
        lastHotCue_ = i;
        return;
    }
    lastHotCue_ = i;
    heldHotCue_ = i;
    if (loopActive_ && (cue < loop_.in || cue >= loop_.out))
    {
        loopActive_ = false;
        if (slipLoop_) { slipLoop_ = false; --slipHolds_; if (slipHolds_ < 0) slipHolds_ = 0; }
    }
    if (playing_)
    {
        if (slipEnabled_) beginSlip();
        jumpTo (quantizedJumpTarget (cue));
        return;
    }
    switch ((HotCueWhenPaused) inputs.hotCueWhenPaused.load (std::memory_order_relaxed))
    {
        case HotCueWhenPaused::Play:
            jumpTo (cue);
            play();
            break;
        case HotCueWhenPaused::Gate:
            jumpTo (cue);
            playing_ = true;
            motor_ = 1.0;
            hotCueGate_ = true;
            break;
        case HotCueWhenPaused::Jump:
            jumpTo (cue);
            break;
    }
}

void Deck::hotCueUp (int i) noexcept
{
    if (i != heldHotCue_) return;
    heldHotCue_ = -1;
    if (hotCueGate_)
    {
        hotCueGate_ = false;
        playing_ = false;
        motor_ = 0.0;
        jumpTo (hotCues_[(std::size_t) i]);
        return;
    }
    if (slipEnabled_ && slipHolds_ > 0 && ! slipLoop_ && ! rollActive_ && ! censor_ && ! scratching_)
        endSlip();
}

// ------------------------------------------------------------------------------------ loops
void Deck::storeLoopInSlot() noexcept
{
    auto& slot = loopSlots_[(std::size_t) loopSlot_];
    if (slot.locked || ! loop_.valid()) return;
    slot.region = loop_;
    emit (DeckEvent::Kind::LoopSlotSet, loopSlot_, loop_.in, loop_.out);
}

void Deck::loopIn() noexcept
{
    if (audio_ == nullptr) return;
    if (loopActive_) { loopHalve(); return; }
    pendingLoopIn_ = snap (pos_);
    loopInPending_ = true;
    loop_ = { pendingLoopIn_, -1.0 };
}

void Deck::loopOut() noexcept
{
    if (audio_ == nullptr) return;
    if (loopActive_) { loopDouble(); return; }
    if (! loopInPending_ || pos_ <= pendingLoopIn_) return;
    double out = snap (pos_);
    const double minLen = hasGrid() ? quantizeBeats_ * framesPerBeat() : audio_->sampleRate * 0.01;
    if (out - pendingLoopIn_ < (quantize_ ? minLen : 32.0)) out = pendingLoopIn_ + (quantize_ ? minLen : 32.0);
    loop_ = { pendingLoopIn_, out };
    loopInPending_ = false;
    loopActive_ = true;
    storeLoopInSlot();
    if (slipEnabled_ && ! slipLoop_) { slipLoop_ = true; beginSlip(); }
    if (pos_ >= loop_.out) jumpTo (loop_.in + std::fmod (pos_ - loop_.in, loop_.length()));
}

void Deck::reloop() noexcept
{
    if (! loop_.valid()) return;
    if (loopActive_) { exitLoop(); return; }
    loopActive_ = true;
    if (slipEnabled_ && ! slipLoop_) { slipLoop_ = true; beginSlip(); }
    jumpTo (loop_.in);
}

void Deck::exitLoop() noexcept
{
    loopActive_ = false;
    if (slicerMode_ == SlicerMode::SlicerLoop) slicerMode_ = SlicerMode::Off;
    if (slipLoop_) { slipLoop_ = false; endSlip(); }
}

void Deck::fourBeatExit() noexcept
{
    if (loopActive_) exitLoop(); else autoLoop (4.0);
}

void Deck::toggleLoopActive() noexcept
{
    if (! loop_.valid()) return;
    if (loopActive_) exitLoop(); else loopActive_ = true;
}

void Deck::autoLoop (double beats) noexcept
{
    if (! hasGrid() || beats <= 0.0) return;
    const double fpb = framesPerBeat();
    const double len = beats * fpb;
    if (loopActive_ && std::abs (loop_.length() - len) < 1.0) { exitLoop(); return; }
    double in;
    if (loopActive_ && loop_.valid())
        in = loop_.in; // resizing an active loop keeps its in point
    else
        in = quantize_ ? cellStartInTrack (pos_, std::min (beats, 1.0)) : pos_;
    // a loop never reaches past the end of the track
    const double out = std::min (in + len, (double) audio_->frames);
    if (out - in < 32.0) return;
    loop_ = { in, out };
    loopActive_ = true;
    loopInPending_ = false;
    storeLoopInSlot();
    if (slipEnabled_ && ! slipLoop_) { slipLoop_ = true; beginSlip(); }
    if (pos_ >= loop_.out) jumpTo (loop_.in + std::fmod (pos_ - loop_.in, loop_.length()));
}

void Deck::loopHalve() noexcept
{
    if (! loop_.valid()) return;
    const double minLen = hasGrid() ? framesPerBeat() / 32.0 : 32.0;
    const double len = std::max (minLen, loop_.length() * 0.5);
    loop_.out = loop_.in + len;
    storeLoopInSlot();
    if (loopActive_ && pos_ >= loop_.out) jumpTo (loop_.in + std::fmod (pos_ - loop_.in, len));
}

void Deck::loopDouble() noexcept
{
    if (! loop_.valid() || audio_ == nullptr) return;
    loop_.out = std::min ((double) audio_->frames, loop_.in + loop_.length() * 2.0);
    storeLoopInSlot();
}

void Deck::loopShift (double beats) noexcept
{
    if (! loop_.valid() || ! hasGrid()) return;
    const double d = beats * framesPerBeat();
    if (loop_.in + d < 0.0 || loop_.out + d > (double) audio_->frames) return;
    loop_.in += d; loop_.out += d;
    if (loopActive_) jumpTo (pos_ + d);
}

void Deck::selectLoopSlot (int delta) noexcept
{
    loopSlot_ = ((loopSlot_ + delta) % kNumSavedLoops + kNumSavedLoops) % kNumSavedLoops;
    const auto& slot = loopSlots_[(std::size_t) loopSlot_];
    if (! slot.region.valid()) return;
    if (slipLoop_) { slipLoop_ = false; endSlip(); }
    loop_ = slot.region;
    loopActive_ = pos_ >= loop_.in && pos_ < loop_.out;
}

void Deck::toggleLoopSlotLock() noexcept
{
    auto& slot = loopSlots_[(std::size_t) loopSlot_];
    slot.locked = ! slot.locked;
    emit (DeckEvent::Kind::LoopSlotLocked, loopSlot_, slot.locked ? 1.0 : 0.0);
}

void Deck::deleteLoopSlot() noexcept
{
    auto& slot = loopSlots_[(std::size_t) loopSlot_];
    if (slot.locked) return;
    slot.region = {};
    emit (DeckEvent::Kind::LoopSlotDeleted, loopSlot_, 0.0);
}

void Deck::loopSlotPad (int i, bool shift) noexcept
{
    if (i < 0 || i >= kNumSavedLoops || audio_ == nullptr) return;
    auto& slot = loopSlots_[(std::size_t) i];
    if (shift)
    {
        if (! slot.locked && slot.region.valid()) { slot.region = {}; emit (DeckEvent::Kind::LoopSlotDeleted, i, 0.0); }
        return;
    }
    loopSlot_ = i;
    if (! slot.region.valid())
    {
        if (loop_.valid() && ! slot.locked)
        {
            slot.region = loop_;
            emit (DeckEvent::Kind::LoopSlotSet, i, loop_.in, loop_.out);
        }
        return;
    }
    if (loopActive_ && std::abs (loop_.in - slot.region.in) < 1.0 && std::abs (loop_.out - slot.region.out) < 1.0)
    {
        exitLoop();
        return;
    }
    loop_ = slot.region;
    loopActive_ = true;
    if (slipEnabled_ && ! slipLoop_) { slipLoop_ = true; beginSlip(); }
    if (pos_ < loop_.in || pos_ >= loop_.out) jumpTo (quantizedJumpTarget (loop_.in));
}

void Deck::rollDown (double beats) noexcept
{
    if (! hasGrid() || beats <= 0.0) return;
    const double len = beats * framesPerBeat();
    const double in = cellStartInTrack (pos_, beats);
    if ((double) audio_->frames - in < 32.0) return;
    roll_ = { in, std::min (in + len, (double) audio_->frames) };
    rollActive_ = true;
    if (! rollHeld_) { rollHeld_ = true; beginSlip(); }
}

void Deck::rollUp() noexcept
{
    if (! rollHeld_) return;
    rollHeld_ = false;
    rollActive_ = false;
    endSlip();
}

void Deck::beatJump (double beats) noexcept
{
    if (audio_ == nullptr) return;
    const double d = hasGrid() ? beats * framesPerBeat() : beats * 0.5 * audio_->sampleRate;
    if (loopActive_ && loop_.valid() && loop_.in + d >= 0.0 && loop_.out + d <= (double) audio_->frames) { loop_.in += d; loop_.out += d; }
    jumpTo (pos_ + d);
}

// ----------------------------------------------------------------------------------- slicer
void Deck::setSlicer (SlicerMode mode, double domainBeats, double quantize) noexcept
{
    const bool wasLoop = slicerMode_ == SlicerMode::SlicerLoop;
    slicerMode_ = mode;
    slicerDomainBeats_ = std::clamp (domainBeats, 2.0, 64.0);
    slicerQuant_ = std::clamp (quantize, 0.125, 1.0);
    if (! hasGrid()) { slicerMode_ = SlicerMode::Off; return; }
    slicerDomainStart_ = cellStartInTrack (pos_, slicerDomainBeats_);
    if (mode == SlicerMode::SlicerLoop)
    {
        loop_ = { slicerDomainStart_, slicerDomainStart_ + slicerDomainBeats_ * framesPerBeat() };
        loopActive_ = true;
    }
    else if (wasLoop)
        loopActive_ = false;
}

void Deck::sliceDown (int slice) noexcept
{
    if (slicerMode_ == SlicerMode::Off || ! hasGrid() || slice < 0 || slice > 7) return;
    const double fpb = framesPerBeat();
    const double sliceBeats = slicerDomainBeats_ / 8.0;
    const double cellBeats = sliceBeats * slicerQuant_;
    const double start = slicerDomainStart_ + (double) slice * sliceBeats * fpb;
    const double offset = pos_ - grid_.floorTo (pos_, cellBeats, audio_->sampleRate);
    roll_ = { start, start + cellBeats * fpb };
    rollActive_ = true;
    if (heldSlice_ < 0) beginSlip();
    heldSlice_ = slice;
    jumpTo (start + std::fmod (std::max (0.0, offset), cellBeats * fpb));
}

void Deck::sliceUp (int slice) noexcept
{
    if (slice != heldSlice_) return;
    heldSlice_ = -1;
    rollActive_ = false;
    endSlip();
}

// ------------------------------------------------------------------------ slip / jog / misc
void Deck::setSlip (bool on) noexcept
{
    slipEnabled_ = on;
    if (! on && slipLoop_) { slipLoop_ = false; if (slipHolds_ > 0) --slipHolds_; }
}

void Deck::censorDown() noexcept
{
    if (audio_ == nullptr || censor_) return;
    censor_ = true;
    censorStartBeat_ = hasGrid() ? grid_.beatAt (pos_, audio_->sampleRate) : pos_ / audio_->sampleRate * 2.0;
    beginSlip();
}

void Deck::censorUp() noexcept
{
    if (! censor_) return;
    censor_ = false;
    endSlip();
}

void Deck::toggleReverse() noexcept { reverse_ = ! reverse_; }

void Deck::jogTouch (bool down) noexcept
{
    touched_ = down;
    if (down && vinyl_ && audio_ != nullptr)
    {
        if (! scratching_)
        {
            scratching_ = true;
            scratchX_ = scratchMeasured_ = 0.0;
            scratchV_ = 0.0;
            inputs.scratchTicks.store (0, std::memory_order_relaxed);
            if (slipEnabled_) beginSlip();
        }
    }
    else if (! down && scratching_)
    {
        scratching_ = false;
        if (slipEnabled_ && slipHolds_ > 0) endSlip();
    }
}

void Deck::tempoReset() noexcept
{
    tempo_ = 0.0;
    faderPickup_ = true;
}

void Deck::cycleTempoRange() noexcept { setTempoRange ((tempoRange_ + 1) % kNumTempoRanges); }

void Deck::setTempoRange (int i) noexcept
{
    tempoRange_ = std::clamp (i, 0, kNumTempoRanges - 1);
    const double r = kTempoRanges[tempoRange_];
    tempo_ = std::clamp (tempo_, -r, r);
    faderPickup_ = true;
}

void Deck::setTempoFromSync (double t) noexcept
{
    tempo_ = t;
    faderPickup_ = true;
}

void Deck::pitchPlay (int semitones) noexcept
{
    if (audio_ == nullptr) return;
    int cue = pitchPlayCue_ >= 0 && hotCues_[(std::size_t) pitchPlayCue_] >= 0.0 ? pitchPlayCue_ : lastHotCue_;
    if (cue < 0 || hotCues_[(std::size_t) cue] < 0.0)
    {
        cue = -1;
        for (int i = 0; i < kNumHotCues; ++i) if (hotCues_[(std::size_t) i] >= 0.0) { cue = i; break; }
    }
    keyShift_ = std::clamp (semitones, -12, 12);
    const double target = cue >= 0 ? hotCues_[(std::size_t) cue] : pos_;
    jumpTo (quantizedJumpTarget (target));
    if (! playing_) play();
}

// -------------------------------------------------------------------------------- stems / fx
void Deck::toggleStemMute (int mask) noexcept
{
    if (audio_ == nullptr || ! audio_->hasStems) return;
    bool allMuted = true;
    for (int s = 0; s < kNumStems; ++s)
        if ((mask >> s) & 1) allMuted = allMuted && stemMute_[(std::size_t) s] < 0.5f;
    for (int s = 0; s < kNumStems; ++s)
        if ((mask >> s) & 1) stemMute_[(std::size_t) s] = allMuted ? 1.0f : 0.0f;
}

void Deck::setStemMute (int stem, bool muted) noexcept
{
    if (stem >= 0 && stem < kNumStems) stemMute_[(std::size_t) stem] = muted ? 0.0f : 1.0f;
}

void Deck::setPartSolo (int mask) noexcept
{
    for (int s = 0; s < kNumStems; ++s) partSolo_[(std::size_t) s] = (mask == 0 || ((mask >> s) & 1)) ? 1.0f : 0.0f;
}

void Deck::stemEcho (int mask) noexcept
{
    if (audio_ == nullptr || ! audio_->hasStems) return;
    bool anyOn = false;
    for (int s = 0; s < kNumStems; ++s) if ((mask >> s) & 1) anyOn = anyOn || stemEchoOn_[(std::size_t) s];
    for (int s = 0; s < kNumStems; ++s)
    {
        if (! ((mask >> s) & 1)) continue;
        stemEchoOn_[(std::size_t) s] = ! anyOn;
        stemMute_[(std::size_t) s] = anyOn ? 1.0f : 0.0f;
    }
    if (! anyOn)
    {
        // feed the part into the echo for one beat, then let the tail ring out
        stemEchoFeedLeft_ = hasGrid() ? framesPerBeat() * (outRate_ / audio_->sampleRate) : outRate_ * 0.5;
        stemEchoRunner_.setOn (true);
    }
}

void Deck::padFxDown (int i) noexcept
{
    if (i < 0 || i >= kNumPadFx) return;
    padFx_[(std::size_t) i].runner.setOn (true);
}

void Deck::padFxUp (int i) noexcept
{
    if (i < 0 || i >= kNumPadFx) return;
    padFx_[(std::size_t) i].runner.setOn (false);
}

void Deck::phaseJump (double beats) noexcept
{
    if (! hasGrid()) return;
    const double d = beats * framesPerBeat();
    pos_ += d; // sync nudges are tiny and continuous: no declick needed below one beat-fraction
    if (slipHolds_ > 0) slipPos_ += d;
}

// ------------------------------------------------------------------------------- rendering
void Deck::readDirect (const Source& src, double pos, float* out) const noexcept
{
    src.frame (pos, out);
}

void Deck::process (float* const* bus, int n, const DeckContext& ctx, bool* busActive) noexcept
{
    for (int c = 0; c < kDeckBuses * 2; ++c) std::fill (bus[c], bus[c] + n, 0.0f);
    busActive[0] = true;
    busActive[1] = false;
    busActive[2] = false;
    quantize_ = ctx.quantize;
    quantizeBeats_ = ctx.quantizeBeats;

    // tempo fader with soft takeover
    {
        // positions are compared in the current range, so a range change does not count as a crossing
        const double range = kTempoRanges[tempoRange_];
        const float faderPos = inputs.tempoFader.load (std::memory_order_relaxed);
        const double faderTempo = ((double) faderPos - 0.5) * 2.0 * range;
        if (inputs.tempoFaderMoved.exchange (false, std::memory_order_acq_rel))
        {
            if (faderPickup_)
            {
                const double lastTempo = ((double) lastFaderPos_ - 0.5) * 2.0 * range;
                const bool crossed = (lastTempo - tempo_) * (faderTempo - tempo_) <= 0.0;
                if (crossed || std::abs (faderTempo - tempo_) < range * 0.01) faderPickup_ = false;
            }
            if (! faderPickup_)
            {
                if (sync_ && ctx.followLeader) leaderRequest_ = true;
                tempo_ = faderTempo;
            }
        }
        lastFaderPos_ = faderPos;
    }

    if (audio_ == nullptr || audio_->frames <= 0)
    {
        publish (ctx, 0.0f);
        return;
    }

    const double trackSr = audio_->sampleRate;
    const double srRatio = trackSr / outRate_;
    const double fpb = framesPerBeat();
    const double framesPerTick = kRevolutionSeconds * trackSr / (double) std::max (16, inputs.ticksPerRevolution.load (std::memory_order_relaxed));

    // ---- jog inputs
    const int scratchTicks = inputs.scratchTicks.exchange (0, std::memory_order_acq_rel);
    const int bendTicks = inputs.bendTicks.exchange (0, std::memory_order_acq_rel);
    const int seekTicks = inputs.seekTicks.exchange (0, std::memory_order_acq_rel);
    const int gridBpmTicks = inputs.gridBpmTicks.exchange (0, std::memory_order_acq_rel);
    const int gridSlideTicks = inputs.gridSlideTicks.exchange (0, std::memory_order_acq_rel);
    const int loopInTicks = inputs.loopInTicks.exchange (0, std::memory_order_acq_rel);
    const int loopOutTicks = inputs.loopOutTicks.exchange (0, std::memory_order_acq_rel);

    if (scratching_)
    {
        scratchMeasured_ += (double) scratchTicks * framesPerTick;
        scratchX_ += scratchV_ * (double) n;
        const double r = scratchMeasured_ - scratchX_;
        scratchX_ += kScratchAlpha * r;
        scratchV_ += kScratchBeta * r / (double) n;
    }
    else if (scratchTicks != 0)
        bendTarget_ += (double) scratchTicks / (double) n * 3.0; // platter with Vinyl off = pitch bend
    bendTarget_ += (double) bendTicks / (double) n * 3.0;
    bend_ += 0.35 * (std::clamp (bendTarget_, -0.9, 0.9) - bend_);
    bendTarget_ = 0.0;
    if (seekTicks != 0) jumpTo (pos_ + (double) seekTicks * framesPerTick * 6.0);
    if (gridBpmTicks != 0 && grid_.valid())
    {
        // keep the beat under the playhead fixed while the interval changes
        const double beatHere = grid_.beatAt (pos_, trackSr);
        grid_.bpm = std::clamp (grid_.bpm + (double) gridBpmTicks * 0.002, 30.0, 300.0);
        grid_.anchor = pos_ - beatHere * grid_.framesPerBeat (trackSr);
        emit (DeckEvent::Kind::GridChanged, 0, grid_.bpm, grid_.anchor);
    }
    if (gridSlideTicks != 0 && grid_.valid())
    {
        grid_.anchor += (double) gridSlideTicks * 0.0005 * trackSr;
        emit (DeckEvent::Kind::GridChanged, 0, grid_.bpm, grid_.anchor);
    }
    if ((loopInTicks != 0 || loopOutTicks != 0) && loop_.valid())
    {
        loop_.in = std::clamp (loop_.in + (double) loopInTicks * framesPerTick * 0.1, 0.0, loop_.out - 64.0);
        loop_.out = std::clamp (loop_.out + (double) loopOutTicks * framesPerTick * 0.1, loop_.in + 64.0, (double) audio_->frames);
        storeLoopInSlot();
    }

    // ---- sync: tempo follows the leader (octave-matched), phase is corrected continuously
    syncCorrection_ = 0.0;
    if (sync_ && ctx.followLeader && grid_.valid() && ctx.leaderBpm > 1.0)
    {
        double r = ctx.leaderBpm / grid_.bpm;
        double octave = 1.0;
        if (r > 1.5) { r *= 0.5; octave = 0.5; }       // deck beats are twice the leader's
        else if (r < 0.75) { r *= 2.0; octave = 2.0; }
        tempo_ = r - 1.0;
        faderPickup_ = true;
        const double target = fmodPos (ctx.leaderBeatPhase * octave, 1.0);
        const double err = wrapHalf (target - beatPhase());
        if (syncSnapPending_ && playing_)
        {
            jumpTo (pos_ + err * fpb);
            syncSnapPending_ = false;
        }
        else if (playing_ && std::abs (err) > 0.001)
        {
            const double base = srRatio * (1.0 + tempo_);
            syncCorrection_ = std::clamp (err * fpb / (0.25 * outRate_), -kMaxSyncCorrection * base, kMaxSyncCorrection * base);
        }
    }

    // ---- slicer domain follows the (background) playhead
    if (slicerMode_ == SlicerMode::Slicer && grid_.valid() && heldSlice_ < 0)
        slicerDomainStart_ = cellStartInTrack (slipHolds_ > 0 ? slipPos_ : pos_, slicerDomainBeats_);

    // ---- stems
    const bool stems = audio_->hasStems;
    float gTarget[kNumStems];
    for (int s = 0; s < kNumStems; ++s)
    {
        gTarget[s] = stems ? stemMute_[(std::size_t) s] * partSolo_[(std::size_t) s] * inputs.stemGain[s].load (std::memory_order_relaxed) : 1.0f;
        stemGainSm_[(std::size_t) s].target = gTarget[s];
        stemEchoSend_[(std::size_t) s].target = stemEchoOn_[(std::size_t) s] && stemEchoFeedLeft_ > 0.0 ? 1.0f : 0.0f;
    }
    bool partSplit = false;
    for (int s = 0; s < kNumStems; ++s) partSplit = partSplit || ! ctx.fxPart[s];
    partSplit = partSplit && stems;
    bool echoFeed = stems && stemEchoFeedLeft_ > 0.0;
    for (int s = 0; s < kNumStems; ++s) echoFeed = echoFeed || stemEchoSend_[(std::size_t) s].value > 1.0e-4f;

    Source src;
    src.a = audio_;
    src.active[0] = true;
    src.active[1] = partSplit;
    src.active[2] = echoFeed;
    auto refreshCoefficients = [&]
    {
        float g[kNumStems], e[kNumStems];
        for (int s = 0; s < kNumStems; ++s) { g[s] = stemGainSm_[(std::size_t) s].value; e[s] = stemEchoSend_[(std::size_t) s].value; }
        auto set = [&] (int b, const float* h)
        {
            if (! stems) { src.gMix[b] = b == 0 ? h[(int) Stem::Melody] : 0.0f; src.coeff[b][0] = src.coeff[b][1] = src.coeff[b][2] = 0.0f; return; }
            const float m = h[(int) Stem::Melody];
            src.gMix[b] = m;
            src.coeff[b][0] = h[(int) Stem::Vocals] - m;
            src.coeff[b][1] = h[(int) Stem::Bass] - m;
            src.coeff[b][2] = h[(int) Stem::Drums] - m;
        };
        float h0[kNumStems], h1[kNumStems], h2[kNumStems];
        for (int s = 0; s < kNumStems; ++s)
        {
            const float sel = (! stems || ctx.fxPart[s]) ? 1.0f : 0.0f;
            h0[s] = stems ? g[s] * sel : 1.0f;
            h1[s] = g[s] * (1.0f - sel);
            h2[s] = e[s];
        }
        set (0, h0);
        set (1, h1);
        set (2, h2);
    };

    // ---- per-frame render
    const bool slipAutoCancel = inputs.slipReverseAutoCancel.load (std::memory_order_relaxed);
    const float brake = inputs.brakeSeconds.load (std::memory_order_relaxed);
    const float start = inputs.startSeconds.load (std::memory_order_relaxed);
    const double brakeStep = brake > 0.0f ? 1.0 / (brake * outRate_) : 1.0;
    const double startStep = start > 0.0f ? 1.0 / (start * outRate_) : 1.0;
    const double pitchRatio = std::pow (2.0, (double) keyShift_ / 12.0);
    float peak = 0.0f;
    float fr[kDeckBuses * 2], old[kDeckBuses * 2];

    for (int i = 0; i < n; ++i)
    {
        if ((i % kCoeffInterval) == 0)
        {
            for (auto& g : stemGainSm_) for (int k = 0; k < kCoeffInterval; ++k) g.next();
            for (auto& e : stemEchoSend_) for (int k = 0; k < kCoeffInterval; ++k) e.next();
            refreshCoefficients();
            src.fold = loopActive_ || rollActive_;
            src.foldIn = rollActive_ ? roll_.in : loop_.in;
            src.foldOut = rollActive_ ? roll_.out : loop_.out;
        }

        // motor (platter start / brake)
        if (playing_) motor_ = std::min (1.0, motor_ + startStep);
        else motor_ = std::max (0.0, motor_ - brakeStep);

        const double base = srRatio * (1.0 + tempo_) * (1.0 + bend_);
        double rate;
        if (scratching_) rate = scratchV_;
        else if (search_ != 0) rate = (double) search_ * 6.0 * srRatio;
        else rate = (base + syncCorrection_) * motor_ * ((reverse_ != censor_) ? -1.0 : 1.0);

        const bool moving = std::abs (rate) > 1.0e-9;
        const bool useStretch = moving && (keylock_ || keyShift_ != 0) && ! scratching_ && search_ == 0
                                && ! reverse_ && ! censor_ && motor_ >= 1.0;
        if (useStretch != stretching_)
        {
            if (useStretch) stretcher_.reset();
            fadePos_ = pos_; fadeRate_ = rate; fadeLeft_ = kDeclickFrames;
            stretching_ = useStretch;
        }

        if (moving || fadeLeft_ > 0)
        {
            if (! moving) for (float& v : fr) v = 0.0f;
            else if (useStretch)
            {
                const double speed = srRatio * pitchRatio * (keylock_ ? 1.0 : (1.0 + tempo_));
                stretcher_.next (src, pos_, speed, fr);
            }
            else readDirect (src, pos_, fr);

            if (scratching_ || motor_ < 1.0)
            {
                // a slowly moving record is quiet; a stopped one is silent
                const float g = (float) std::min (1.0, std::abs (rate) / srRatio * 6.0);
                for (float& v : fr) v *= g;
            }
            if (fadeLeft_ > 0)
            {
                readDirect (src, fadePos_, old);
                const float w = (float) fadeLeft_ / (float) kDeclickFrames;
                for (int c = 0; c < kDeckBuses * 2; ++c) fr[c] = fr[c] * (1.0f - w) + old[c] * w;
                fadePos_ += fadeRate_;
                --fadeLeft_;
            }
            for (int c = 0; c < kDeckBuses * 2; ++c) bus[c][i] = fr[c];
            peak = std::max (peak, std::max (std::abs (fr[0] + fr[2]), std::abs (fr[1] + fr[3])));
        }

        // advance
        const double prev = pos_;
        pos_ += rate;
        if (slipHolds_ > 0 && slipRunning_) slipPos_ += srRatio * (1.0 + tempo_) + syncCorrection_;

        if (rate > 0.0)
        {
            if (rollActive_ && pos_ >= roll_.out && roll_.valid())
            {
                fadePos_ = pos_; fadeRate_ = rate; fadeLeft_ = stretching_ ? 0 : kDeclickFrames;
                pos_ = roll_.in + std::fmod (pos_ - roll_.in, roll_.length());
            }
            else if (! rollActive_ && loopActive_ && loop_.valid() && prev < loop_.out && pos_ >= loop_.out)
            {
                fadePos_ = pos_; fadeRate_ = rate; fadeLeft_ = stretching_ ? 0 : kDeclickFrames;
                pos_ = loop_.in + std::fmod (pos_ - loop_.in, loop_.length());
            }
        }
        else if (rate < 0.0 && loopActive_ && loop_.valid() && prev >= loop_.in && pos_ < loop_.in)
            pos_ += loop_.length();

        if (censor_ && slipAutoCancel && grid_.valid() && slipHolds_ > 0
            && grid_.beatAt (slipPos_, trackSr) - censorStartBeat_ >= 8.0)
            censorUp();

        // an active loop inside the track has already wrapped the playhead above; reaching the end means
        // no loop is going to catch it (none, or one behind the playhead)
        if (pos_ >= (double) audio_->frames && rate > 0.0)
        {
            pos_ = (double) audio_->frames;
            playing_ = false;
            motor_ = 0.0;
            if (! endEmitted_) { emit (DeckEvent::Kind::TrackEnded, 0, 0.0); endEmitted_ = true; }
        }
        else if (pos_ < 0.0)
            pos_ = 0.0;
    }

    if (stemEchoFeedLeft_ > 0.0) stemEchoFeedLeft_ = std::max (0.0, stemEchoFeedLeft_ - (double) n);

    // ---- per-deck effects on the FX-part bus
    const double bpm = effectiveBpm() > 1.0 ? effectiveBpm() : 120.0;
    for (auto& p : padFx_)
    {
        if (! p.runner.running()) continue;
        FxContext fc;
        fc.bpm = bpm;
        fc.beats = p.preset.beats;
        fc.amount = p.preset.amount;
        for (int k = 0; k < 3; ++k) fc.params[k] = p.preset.params[k];
        p.runner.process (bus[0], bus[1], n, fc);
    }
    if (echoFeed || stemEchoRunner_.running())
    {
        // the echo runs on the send bus and its output joins bus 0
        std::copy (bus[4], bus[4] + n, wetL_.data());
        std::copy (bus[5], bus[5] + n, wetR_.data());
        FxContext fc;
        fc.bpm = bpm;
        fc.beats = 0.75f;
        fc.amount = 1.0f;
        fc.params[0] = 0.55f;
        fc.params[1] = 0.4f;
        // the runner adds wet to its input; subtract the dry feed to keep only the echo
        stemEchoRunner_.process (wetL_.data(), wetR_.data(), n, fc);
        for (int i = 0; i < n; ++i)
        {
            bus[0][i] += wetL_[(std::size_t) i] - bus[4][i];
            bus[1][i] += wetR_[(std::size_t) i] - bus[5][i];
        }
        if (stemEchoFeedLeft_ <= 0.0) stemEchoRunner_.setOn (false);
    }
    busActive[1] = partSplit;
    publish (ctx, peak);
}

void Deck::publish (const DeckContext&, float peak) noexcept
{
    auto& v = view;
    const bool loaded = audio_ != nullptr;
    v.loaded.store (loaded, std::memory_order_relaxed);
    v.audioId.store (audioId_, std::memory_order_relaxed);
    v.position.store (pos_, std::memory_order_relaxed);
    v.slipPosition.store (slipHolds_ > 0 ? slipPos_ : -1.0, std::memory_order_relaxed);
    v.slipActive.store (slipHolds_ > 0, std::memory_order_relaxed);
    v.length.store (loaded ? audio_->frames : 0, std::memory_order_relaxed);
    v.trackSampleRate.store (loaded ? audio_->sampleRate : 44100.0, std::memory_order_relaxed);
    v.playing.store (playing_, std::memory_order_relaxed);
    v.speed.store (scratching_ ? scratchV_ * outRate_ / (loaded ? audio_->sampleRate : 44100.0) : motor_ * (1.0 + tempo_), std::memory_order_relaxed);
    v.tempo.store (tempo_, std::memory_order_relaxed);
    v.tempoRangeIndex.store (tempoRange_, std::memory_order_relaxed);
    v.bpm.store (effectiveBpm(), std::memory_order_relaxed);
    v.trackBpm.store (grid_.bpm, std::memory_order_relaxed);
    v.gridAnchor.store (grid_.anchor, std::memory_order_relaxed);
    v.keylock.store (keylock_, std::memory_order_relaxed);
    v.keyShift.store (keyShift_, std::memory_order_relaxed);
    v.sync.store (sync_, std::memory_order_relaxed);
    v.leader.store (leader_, std::memory_order_relaxed);
    v.slip.store (slipEnabled_, std::memory_order_relaxed);
    v.reverse.store (reverse_, std::memory_order_relaxed);
    v.censor.store (censor_, std::memory_order_relaxed);
    v.vinyl.store (vinyl_, std::memory_order_relaxed);
    v.scratching.store (scratching_, std::memory_order_relaxed);
    v.jogTouched.store (touched_, std::memory_order_relaxed);
    v.cuePoint.store (cuePoint_, std::memory_order_relaxed);
    for (int i = 0; i < kNumHotCues; ++i) v.hotCue[i].store (hotCues_[(std::size_t) i], std::memory_order_relaxed);
    v.loopIn.store (loop_.in, std::memory_order_relaxed);
    v.loopOut.store (loop_.out, std::memory_order_relaxed);
    v.loopActive.store (loopActive_, std::memory_order_relaxed);
    v.rollActive.store (rollActive_, std::memory_order_relaxed);
    v.rollIn.store (roll_.in, std::memory_order_relaxed);
    v.rollOut.store (roll_.out, std::memory_order_relaxed);
    v.loopSlot.store (loopSlot_, std::memory_order_relaxed);
    for (int i = 0; i < kNumSavedLoops; ++i)
    {
        v.loopSlotIn[i].store (loopSlots_[(std::size_t) i].region.in, std::memory_order_relaxed);
        v.loopSlotOut[i].store (loopSlots_[(std::size_t) i].region.out, std::memory_order_relaxed);
        v.loopSlotLocked[i].store (loopSlots_[(std::size_t) i].locked, std::memory_order_relaxed);
    }
    v.slicerMode.store ((int) slicerMode_, std::memory_order_relaxed);
    v.slicerDomainStart.store (slicerDomainStart_, std::memory_order_relaxed);
    v.slicerDomainBeats.store (slicerDomainBeats_, std::memory_order_relaxed);
    int slice = heldSlice_;
    if (slice < 0 && slicerMode_ != SlicerMode::Off && hasGrid())
    {
        const double ref = slipHolds_ > 0 ? slipPos_ : pos_;
        slice = std::clamp ((int) std::floor ((ref - slicerDomainStart_) / (slicerDomainBeats_ / 8.0 * framesPerBeat())), 0, 7);
    }
    v.activeSlice.store (slice, std::memory_order_relaxed);
    v.stemsAvailable.store (loaded && audio_->hasStems, std::memory_order_relaxed);
    for (int s = 0; s < kNumStems; ++s)
    {
        v.stemGain[s].store (stemGainSm_[(std::size_t) s].target, std::memory_order_relaxed);
        v.stemMuted[s].store (stemMute_[(std::size_t) s] < 0.5f, std::memory_order_relaxed);
        v.stemEcho[s].store (stemEchoOn_[(std::size_t) s], std::memory_order_relaxed);
    }
    for (int i = 0; i < kNumPadFx; ++i) v.padFxActive[i].store (padFx_[(std::size_t) i].runner.on(), std::memory_order_relaxed);
    v.peak.store (peak, std::memory_order_relaxed);
    v.endReached.store (endEmitted_, std::memory_order_relaxed);
}

} // namespace dj
