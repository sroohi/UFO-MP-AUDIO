#include "Engine.h"

namespace dj
{

namespace
{
    // Serato Pad FX bank A-H (hold to apply). Echoes ring out after release.
    PadFxPreset padFxPreset (int i)
    {
        PadFxPreset p;
        switch (i)
        {
            case 0: p.type = FxType::Echo;        p.beats = 0.5f;  p.amount = 0.8f; break;
            case 1: p.type = FxType::Echo;        p.beats = 0.75f; p.amount = 0.8f; break;
            case 2: p.type = FxType::Roll;        p.beats = 0.25f; p.amount = 1.0f; break;
            case 3: p.type = FxType::Roll;        p.beats = 0.125f; p.amount = 1.0f; break;
            case 4: p.type = FxType::Gate;        p.beats = 0.25f; p.amount = 1.0f; break;
            case 5: p.type = FxType::HighPass;    p.beats = 1.0f;  p.amount = 0.7f; break;
            case 6: p.type = FxType::Braker;      p.beats = 1.0f;  p.amount = 1.0f; break;
            default: p.type = FxType::Reverb;     p.beats = 1.0f;  p.amount = 0.7f; break;
        }
        for (int k = 0; k < 3; ++k) p.params[k] = fxParamDefault (p.type, k);
        return p;
    }

    inline void applyRamp (float* l, float* r, int n, float from, float to) noexcept
    {
        if (from == to)
        {
            if (from == 1.0f) return;
            for (int i = 0; i < n; ++i) { l[i] *= from; r[i] *= from; }
            return;
        }
        const float step = (to - from) / (float) std::max (1, n);
        float g = from;
        for (int i = 0; i < n; ++i) { g += step; l[i] *= g; r[i] *= g; }
    }

    inline float peakOf (const float* l, const float* r, int n) noexcept
    {
        float p = 0.0f;
        for (int i = 0; i < n; ++i) p = std::max (p, std::max (std::abs (l[i]), std::abs (r[i])));
        return p;
    }

    // Pitch class of a key's relative major (A minor -> C), for harmonic matching.
    inline int relativeMajorPc (int key) noexcept { return key < 12 ? key : (key - 12 + 3) % 12; }
}

Engine::Engine()
{
    for (int d = 0; d < kNumDecks; ++d)
    {
        decks_.push_back (std::make_unique<Deck>());
        decks_.back()->index = d;
        for (int p = 0; p < kNumPadFx; ++p)
        {
            const auto preset = padFxPreset (p);
            decks_.back()->setPadFx (p, makeEffect (preset.type), preset);
        }
    }
    // FLX10 BEAT FX unit: one effect, starts on ECHO (knob position 2), assigned to master
    fxUnits_[kBeatFxUnit].controls.multi.store (false);
    fxUnits_[kBeatFxUnit].controls.depth.store (0.5f);
    fxUnits_[kBeatFxUnit].controls.assign[(int) FxTarget::Master].store (true);
    for (int u = 0; u < kTotalFxUnits; ++u)
        fxUnits_[(std::size_t) u].controls.placement.store ((int) FxPlacement::PostFader);
}

Engine::~Engine()
{
    recorder.stop();
}

void Engine::prepare (double sampleRate, int maxBlock)
{
    sampleRate_ = sampleRate;
    maxBlock_ = std::max (64, maxBlock);
    for (auto& d : decks_) d->prepare (sampleRate, maxBlock_);
    for (auto& u : fxUnits_) u.prepare (sampleRate, maxBlock_);
    for (auto& e : ownedFx_) e->prepare (sampleRate, maxBlock_);
    sampler_.prepare (sampleRate);
    for (auto& c : colorFx_) c.prepare (sampleRate, maxBlock_);
    for (auto& e : eqA_) e.prepare (sampleRate);
    for (auto& e : eqB_) e.prepare (sampleRate);
    limiter_.prepare (sampleRate);
    performer.prepare (sampleRate, maxBlock_);
    // gain ramps start from the current control values (no fade-in on the first block)
    for (int ch = 0; ch < kNumDecks; ++ch)
    {
        auto& cc = mixer.ch[(std::size_t) ch];
        lastTrim_[(std::size_t) ch] = trimGain (cc.trim.load()) * dbToGain (cc.autoGainDb.load());
        lastFader_[(std::size_t) ch] = channelFaderGain (cc.fader.load(), mixer.chFaderCurve.load());
        const auto assign = (XfAssign) cc.xfAssign.load();
        const float xf = mixer.xfReverse.load() ? 1.0f - mixer.crossfader.load() : mixer.crossfader.load();
        lastXf_[(std::size_t) ch] = assign == XfAssign::Thru ? 1.0f : crossfaderGain (assign == XfAssign::A ? xf : 1.0f - xf, mixer.xfCurve.load(), mixer.xfCutLag.load());
    }
    lastMaster_ = mixer.masterLevel.load();
    lastSamplerVol_ = mixer.samplerVolume.load();
    for (auto& deckBufs : deckBus_) for (auto& b : deckBufs) b.assign ((std::size_t) maxBlock_, 0.0f);
    for (auto* v : { &masterL_, &masterR_, &cueL_, &cueR_, &smpL_, &smpR_ }) v->assign ((std::size_t) maxBlock_, 0.0f);
    // default FX unit contents (Serato defaults: Echo / Reverb / HPF on FX1, Flanger / Phaser / Roll on FX2)
    if (fxUnits_[0].slotType (0) == FxType::None)
    {
        static constexpr FxType defaults[kTotalFxUnits][kFxSlotsPerUnit] {
            { FxType::Echo, FxType::Reverb, FxType::HighPass },
            { FxType::Flanger, FxType::Phaser, FxType::Roll },
            { FxType::Echo, FxType::None, FxType::None } };
        for (int u = 0; u < kTotalFxUnits; ++u)
            for (int s = 0; s < kFxSlotsPerUnit; ++s)
                if (defaults[u][s] != FxType::None)
                {
                    for (int t = 0; t < kFxTargets; ++t)
                    {
                        auto fx = makeEffect (defaults[u][s]);
                        fx->prepare (sampleRate, maxBlock_);
                        fxUnits_[(std::size_t) u].installEffect (s, t, fx.get(), defaults[u][s]);
                        ownedFx_.push_back (std::move (fx));
                    }
                }
    }
}

// ------------------------------------------------------------------------- message thread
std::uint64_t Engine::holdAudio (std::shared_ptr<const TrackAudio> audio)
{
    const auto id = ++nextAudioId_;
    held_[id] = { std::move (audio), 1 };
    return id;
}

void Engine::releaseAudio (std::uint64_t id)
{
    auto it = held_.find (id);
    if (it == held_.end()) return;
    if (--it->second.refs <= 0) held_.erase (it);
}

std::shared_ptr<const TrackAudio> Engine::audioFor (std::uint64_t id) const
{
    const auto it = held_.find (id);
    return it == held_.end() ? nullptr : it->second.audio;
}

void Engine::loadDeck (int deck, std::shared_ptr<const TrackAudio> audio)
{
    if (deck < 0 || deck >= kNumDecks) return;
    Command c;
    c.type = audio != nullptr ? Cmd::DeckLoad : Cmd::DeckUnload;
    c.deck = deck;
    c.audio = audio.get();
    c.id = audio != nullptr ? holdAudio (std::move (audio)) : 0;
    slotAudio_[(std::size_t) deck] = c.id;
    if (! post (c) && c.id != 0) releaseAudio (c.id);
}

void Engine::replaceDeckAudio (std::uint64_t oldId, std::shared_ptr<const TrackAudio> audio)
{
    if (oldId == 0 || audio == nullptr) return;
    for (int d = 0; d < kNumDecks; ++d)
    {
        if (slotAudio_[(std::size_t) d] != oldId) continue;
        Command c;
        c.type = Cmd::DeckReplaceAudio;
        c.deck = d;
        c.audio = audio.get();
        c.id = holdAudio (audio);
        slotAudio_[(std::size_t) d] = c.id;
        if (! post (c)) releaseAudio (c.id);
    }
}

void Engine::loadSampler (int slot, std::shared_ptr<const TrackAudio> audio, double bpm)
{
    if (slot < 0 || slot >= kSamplerSlots) return;
    Command c;
    c.type = Cmd::SamplerLoad;
    c.deck = slot;
    c.audio = audio.get();
    c.value = bpm;
    c.id = audio != nullptr ? holdAudio (std::move (audio)) : 0;
    samplerAudio_[(std::size_t) slot] = c.id;
    if (! post (c) && c.id != 0) releaseAudio (c.id);
}

void Engine::instantDouble (int fromDeck, int toDeck, int partMask)
{
    if (fromDeck == toDeck || fromDeck < 0 || toDeck < 0 || fromDeck >= kNumDecks || toDeck >= kNumDecks) return;
    const auto id = slotAudio_[(std::size_t) fromDeck];
    auto it = held_.find (id);
    if (id == 0 || it == held_.end()) return;
    ++it->second.refs;
    slotAudio_[(std::size_t) toDeck] = id;
    Command c;
    c.type = partMask != 0 ? Cmd::PartInstantDouble : Cmd::InstantDouble;
    c.deck = toDeck;
    c.index = fromDeck;
    c.value = (double) partMask;
    c.id = id;
    if (! post (c)) releaseAudio (id);
}

void Engine::setFxType (int unit, int slot, FxType type)
{
    if (unit < 0 || unit >= kTotalFxUnits || slot < 0 || slot >= kFxSlotsPerUnit) return;
    for (int t = 0; t < kFxTargets; ++t)
    {
        Command c;
        c.type = Cmd::FxInstall;
        c.deck = unit;
        c.index = slot;
        c.value = (double) t;
        c.fxType = type;
        if (type != FxType::None)
        {
            auto fx = makeEffect (type);
            fx->prepare (sampleRate_, maxBlock_);
            c.fx = fx.get();
            ownedFx_.push_back (std::move (fx));
        }
        post (c);
    }
}

void Engine::retireEffect (Effect* fx)
{
    for (auto it = ownedFx_.begin(); it != ownedFx_.end(); ++it)
        if (it->get() == fx) { ownedFx_.erase (it); return; }
}

// --------------------------------------------------------------------------- audio thread
void Engine::releaseFromAudioThread (std::uint64_t id) noexcept
{
    if (id == 0) return;
    DeckEvent e;
    e.kind = DeckEvent::Kind::AudioReleased;
    e.audioId = id;
    events_.push (e);
}

void Engine::syncOn (int d) noexcept
{
    auto& deck = *decks_[(std::size_t) d];
    if (! deck.hasGrid()) return;
    const bool leaderUsable = leader_ >= 0 && leader_ != d && decks_[(std::size_t) leader_]->hasGrid() && decks_[(std::size_t) leader_]->playing();
    if (! leaderUsable)
    {
        // Serato Smart Sync: the other playing deck becomes the leader (preferring one already synced)
        int best = -1;
        for (int k = 0; k < kNumDecks; ++k)
        {
            if (k == d) continue;
            auto& o = *decks_[(std::size_t) k];
            if (! o.hasGrid() || ! o.playing()) continue;
            if (best < 0 || (o.sync() && ! decks_[(std::size_t) best]->sync())) best = k;
        }
        leader_ = best >= 0 ? best : d;
        if (best >= 0) decks_[(std::size_t) best]->setSyncState (true, true);
    }
    deck.setSyncState (true, leader_ == d);
}

void Engine::syncOff (int d) noexcept
{
    decks_[(std::size_t) d]->setSyncState (false, false);
    if (leader_ != d) return;
    leader_ = -1;
    for (int k = 0; k < kNumDecks; ++k)
        if (k != d && decks_[(std::size_t) k]->sync() && decks_[(std::size_t) k]->hasGrid()) { leader_ = k; break; }
}

void Engine::updateLeader() noexcept
{
    for (int d = 0; d < kNumDecks; ++d)
        if (decks_[(std::size_t) d]->takeLeaderRequest()) leader_ = d;
    if (leader_ >= 0)
    {
        auto& l = *decks_[(std::size_t) leader_];
        if (! l.hasGrid() || ! l.sync())
            leader_ = -1;
        else if (! l.playing())
            // hand over to a playing synced deck, if any
            for (int k = 0; k < kNumDecks; ++k)
                if (k != leader_ && decks_[(std::size_t) k]->sync() && decks_[(std::size_t) k]->playing() && decks_[(std::size_t) k]->hasGrid())
                { leader_ = k; break; }
    }
    if (leader_ < 0)
        for (int k = 0; k < kNumDecks; ++k)
            if (decks_[(std::size_t) k]->sync() && decks_[(std::size_t) k]->hasGrid()) { leader_ = k; break; }
    for (int d = 0; d < kNumDecks; ++d)
    {
        auto& deck = *decks_[(std::size_t) d];
        deck.setSyncState (deck.sync(), deck.sync() && d == leader_);
    }
}

void Engine::keySync (int d) noexcept
{
    auto& deck = *decks_[(std::size_t) d];
    if (deck.musicalKey() < 0) return;
    int target = -1;
    if (leader_ >= 0 && leader_ != d && decks_[(std::size_t) leader_]->musicalKey() >= 0) target = leader_;
    if (target < 0)
        for (int k = 0; k < kNumDecks; ++k)
            if (k != d && decks_[(std::size_t) k]->playing() && decks_[(std::size_t) k]->musicalKey() >= 0) { target = k; break; }
    if (target < 0) return;
    auto& t = *decks_[(std::size_t) target];
    const int targetKey = transposeKey (t.musicalKey(), (int) std::lround (t.soundingSemitones()));
    int diff = relativeMajorPc (targetKey) - relativeMajorPc (deck.musicalKey());
    diff = ((diff % 12) + 12) % 12;
    if (diff > 6) diff -= 12;
    deck.setKeyShift (diff);
    deck.setKeylock (true);
}

// The deck that keeps time for tempo-synced master effects: the sync leader while it plays, otherwise
// the loudest playing deck with a beatgrid. -1 when nothing with a grid plays.
int Engine::clockDeck() const noexcept
{
    if (leader_ >= 0 && decks_[(std::size_t) leader_]->playing() && decks_[(std::size_t) leader_]->hasGrid()) return leader_;
    int best = -1;
    float bestLevel = -1.0f;
    for (int d = 0; d < kNumDecks; ++d)
    {
        const auto& deck = *decks_[(std::size_t) d];
        if (! deck.playing() || ! deck.hasGrid()) continue;
        const float level = mixer.ch[(std::size_t) d].fader.load (std::memory_order_relaxed);
        if (level > bestLevel) { bestLevel = level; best = d; }
    }
    return best;
}

double Engine::unitBpm (int unit) const noexcept
{
    const auto& u = fxUnits_[(std::size_t) unit];
    if (u.controls.tapMode.load (std::memory_order_relaxed)) return u.controls.tapBpm.load (std::memory_order_relaxed);
    int only = -1, count = 0;
    for (int ch = 0; ch < kNumDecks; ++ch)
        if (u.controls.assign[ch].load (std::memory_order_relaxed)) { only = ch; ++count; }
    if (count == 1 && decks_[(std::size_t) only]->effectiveBpm() > 1.0) return decks_[(std::size_t) only]->effectiveBpm();
    if (leader_ >= 0 && decks_[(std::size_t) leader_]->effectiveBpm() > 1.0) return decks_[(std::size_t) leader_]->effectiveBpm();
    for (const auto& d : decks_) if (d->playing() && d->effectiveBpm() > 1.0) return d->effectiveBpm();
    return 120.0;
}

void Engine::applyDeckCommand (Deck& d, const Command& c) noexcept
{
    switch (c.type)
    {
        case Cmd::DeckLoad:
        {
            const auto old = d.audioId();
            if (d.sync() && leader_ == d.index) syncOff (d.index);
            d.load (c.audio, c.id);
            releaseFromAudioThread (old);
            break;
        }
        case Cmd::DeckReplaceAudio:
        {
            const auto old = d.audioId();
            d.replaceAudio (c.audio, c.id);
            releaseFromAudioThread (old);
            break;
        }
        case Cmd::DeckUnload:
        {
            const auto old = d.audioId();
            syncOff (d.index);
            d.unload();
            releaseFromAudioThread (old);
            break;
        }
        case Cmd::DeckSetGrid: { Beatgrid g; g.bpm = c.value; g.anchor = c.value2; d.setGrid (g); break; }
        case Cmd::DeckSetCuePoint: d.setCuePoint (c.value); break;
        case Cmd::DeckSetHotCue: d.setHotCue (c.index, c.value); break;
        case Cmd::DeckSetLoopSlot: d.setLoopSlot (c.index, c.value, c.value2, c.flag); break;
        case Cmd::DeckSetKey: d.setKeyInfo ((int) c.value); break;
        case Cmd::InstantDouble:
        case Cmd::PartInstantDouble:
        {
            auto& src = *decks_[(std::size_t) std::clamp (c.index, 0, kNumDecks - 1)];
            if (src.audio() == nullptr) { releaseFromAudioThread (c.id); break; }
            const auto old = d.audioId();
            d.copyStateFrom (src);
            releaseFromAudioThread (old);
            if (c.type == Cmd::PartInstantDouble)
            {
                const int mask = (int) c.value;
                d.setPartSolo (mask);
                for (int s = 0; s < kNumStems; ++s) if ((mask >> s) & 1) src.setStemMute (s, true);
            }
            break;
        }
        case Cmd::PlayPause: d.playPause(); break;
        case Cmd::Play: d.play(); break;
        case Cmd::Pause: d.pause(); break;
        case Cmd::Stutter: d.stutter(); break;
        case Cmd::CueDown: d.cueDown(); break;
        case Cmd::CueUp: d.cueUp(); break;
        case Cmd::JumpToStart: d.jumpToStart(); break;
        case Cmd::RestartFromStart: d.restartFromStart(); break;
        case Cmd::SearchDown: d.searchDown (c.index); break;
        case Cmd::SearchUp: d.searchUp(); break;
        case Cmd::SetPosition: d.setPosition (c.value); break;
        case Cmd::HotCueDown: d.hotCueDown (c.index, c.flag); break;
        case Cmd::HotCueUp: d.hotCueUp (c.index); break;
        case Cmd::LoopIn: d.loopIn(); break;
        case Cmd::LoopOut: d.loopOut(); break;
        case Cmd::Reloop: d.reloop(); break;
        case Cmd::LoopExit: d.exitLoop(); break;
        case Cmd::FourBeatExit: d.fourBeatExit(); break;
        case Cmd::LoopActiveToggle: d.toggleLoopActive(); break;
        case Cmd::AutoLoop: d.autoLoop (c.value); break;
        case Cmd::LoopHalve: d.loopHalve(); break;
        case Cmd::LoopDouble: d.loopDouble(); break;
        case Cmd::LoopShift: d.loopShift (c.value); break;
        case Cmd::LoopSlotSelect: d.selectLoopSlot (c.index); break;
        case Cmd::LoopSlotLock: d.toggleLoopSlotLock(); break;
        case Cmd::LoopSlotDelete: d.deleteLoopSlot(); break;
        case Cmd::LoopSlotPad: d.loopSlotPad (c.index, c.flag); break;
        case Cmd::RollDown: d.rollDown (c.value); break;
        case Cmd::RollUp: d.rollUp(); break;
        case Cmd::BeatJump: d.beatJump (c.value); break;
        case Cmd::SlicerSet: d.setSlicer ((SlicerMode) c.index, c.value, c.value2); break;
        case Cmd::SliceDown: d.sliceDown (c.index); break;
        case Cmd::SliceUp: d.sliceUp (c.index); break;
        case Cmd::SlipSet: d.setSlip (c.flag); break;
        case Cmd::SlipToggle: d.setSlip (! d.slip()); break;
        case Cmd::CensorDown: d.censorDown(); break;
        case Cmd::CensorUp: d.censorUp(); break;
        case Cmd::ReverseToggle: d.toggleReverse(); break;
        case Cmd::VinylSet: d.setVinyl (c.flag); break;
        case Cmd::VinylToggle: d.setVinyl (! d.vinyl()); break;
        case Cmd::JogTouch: d.jogTouch (c.flag); break;
        case Cmd::TempoReset: d.tempoReset(); break;
        case Cmd::TempoRangeCycle: d.cycleTempoRange(); break;
        case Cmd::TempoRangeSet: d.setTempoRange (c.index); break;
        case Cmd::SyncOn: syncOn (d.index); break;
        case Cmd::SyncOff: syncOff (d.index); break;
        case Cmd::SyncToggle: if (d.sync()) syncOff (d.index); else syncOn (d.index); break;
        case Cmd::SetLeader: if (d.hasGrid()) { d.setSyncState (true, true); leader_ = d.index; } break;
        case Cmd::KeylockSet: d.setKeylock (c.flag); break;
        case Cmd::KeylockToggle: d.setKeylock (! d.keylock()); break;
        case Cmd::KeyShiftSet: d.setKeyShift (c.index); if (c.index != 0) d.setKeylock (true); break;
        case Cmd::KeyShiftDelta: d.setKeyShift (d.keyShift() + c.index); d.setKeylock (true); break;
        case Cmd::KeySync: keySync (d.index); break;
        case Cmd::KeyReset: d.setKeyShift (0); break;
        case Cmd::PitchPlay: d.setKeylock (true); d.pitchPlay (c.index); break;
        case Cmd::PitchPlayCue: d.setPitchPlayCue (c.index); break;
        case Cmd::StemMuteToggle: d.toggleStemMute (c.index); break;
        case Cmd::StemMuteSet: d.setStemMute (c.index, c.flag); break;
        case Cmd::StemEcho: d.stemEcho (c.index); break;
        case Cmd::PartSolo: d.setPartSolo (c.index); break;
        case Cmd::PadFxDown: d.padFxDown (c.index); break;
        case Cmd::PadFxUp: d.padFxUp (c.index); break;
        default: break;
    }
}

void Engine::applyCommand (const Command& c) noexcept
{
    switch (c.type)
    {
        case Cmd::SamplerLoad:
        {
            const auto old = sampler_.audioId (std::clamp (c.deck, 0, kSamplerSlots - 1));
            sampler_.load (c.deck, c.audio, c.id, c.value);
            releaseFromAudioThread (old);
            return;
        }
        case Cmd::SamplerPadDown: sampler_.padDown (c.deck); return;
        case Cmd::SamplerPadUp: sampler_.padUp (c.deck); return;
        case Cmd::SamplerStop: sampler_.stop (c.deck); return;
        case Cmd::SamplerStopAll: sampler_.stopAll(); return;
        case Cmd::FxInstall:
        {
            if (c.deck < 0 || c.deck >= kTotalFxUnits) return;
            auto& u = fxUnits_[(std::size_t) c.deck];
            const int target = (int) c.value;
            Effect* old = u.installed (c.index, target);
            u.installEffect (c.index, target, c.fx, c.fxType);
            if (old != nullptr && old != c.fx) retiredFx_.push (old);
            return;
        }
        case Cmd::FxSlotOn:
            if (c.deck >= 0 && c.deck < kTotalFxUnits) fxUnits_[(std::size_t) c.deck].setSlotOn (c.index, c.flag);
            return;
        case Cmd::FxSlotToggle:
            if (c.deck >= 0 && c.deck < kTotalFxUnits)
                fxUnits_[(std::size_t) c.deck].setSlotOn (c.index, ! fxUnits_[(std::size_t) c.deck].slotOn (c.index));
            return;
        case Cmd::QuantizeSet: quantize_ = c.flag; return;
        case Cmd::QuantizeToggle: quantize_ = ! quantize_; return;
        case Cmd::QuantizeBeats: quantizeBeats_ = std::clamp (c.value, 0.125, 1.0); return;
        default: break;
    }
    if (c.deck >= 0 && c.deck < kNumDecks) applyDeckCommand (*decks_[(std::size_t) c.deck], c);
}

void Engine::process (const float* const*, int, float* const* outputs, int numOutputs, int numFrames) noexcept
{
    Command c;
    while (commands_.pop (c)) applyCommand (c);
    while (midiCommands_.pop (c)) applyCommand (c);
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr) std::fill (outputs[ch], outputs[ch] + numFrames, 0.0f);

    int done = 0;
    float* chunk[32];
    while (done < numFrames)
    {
        const int n = std::min (maxBlock_, numFrames - done);
        const int m = std::min (numOutputs, 32);
        for (int ch = 0; ch < m; ++ch) chunk[ch] = outputs[ch] != nullptr ? outputs[ch] + done : nullptr;
        processBlock (chunk, m, n);
        done += n;
    }
}

void Engine::processBlock (float* const* outputs, int numOutputs, int n) noexcept
{
    updateLeader();

    // ---- sync context
    DeckContext ctx;
    ctx.outputRate = sampleRate_;
    ctx.quantize = quantize_;
    ctx.quantizeBeats = quantizeBeats_;
    for (int s = 0; s < kNumStems; ++s) ctx.fxPart[s] = fxPart[s].load (std::memory_order_relaxed);
    double leaderBpm = 0.0, leaderBeat = 0.0;
    bool leaderPlaying = false;
    if (leader_ >= 0)
    {
        const auto& l = *decks_[(std::size_t) leader_];
        leaderBpm = l.effectiveBpm();
        leaderPlaying = l.playing();
        if (l.hasGrid()) leaderBeat = fmodPos (l.grid().beatAt (l.position(), l.audio()->sampleRate), 4.0);
    }
    // master clock for the sampler and the Performer: the clock deck's tempo and beat at this block's start
    const int cd = clockDeck();
    double clockBpm = unitBpm (0), clockBeat = -1.0;
    if (cd >= 0)
    {
        const auto& d = *decks_[(std::size_t) cd];
        clockBpm = d.effectiveBpm();
        clockBeat = fmodPos (d.grid().beatAt (d.position(), d.audio()->sampleRate), 4.0);
    }
    view.leader.store (leader_, std::memory_order_relaxed);
    view.masterBpm.store (leaderBpm, std::memory_order_relaxed);
    view.quantize.store (quantize_, std::memory_order_relaxed);
    view.quantizeBeats.store (quantizeBeats_, std::memory_order_relaxed);

    // ---- Part ISO: the EQ knobs of a channel in Part ISO mode drive its deck's stems
    for (int ch = 0; ch < kNumDecks; ++ch)
    {
        auto& cc = mixer.ch[(std::size_t) ch];
        auto& in = decks_[(std::size_t) ch]->inputs;
        if (cc.partIso.load (std::memory_order_relaxed))
        {
            const float hi = isoGain (cc.eq[0].load (std::memory_order_relaxed));
            const float mid = isoGain (cc.eq[1].load (std::memory_order_relaxed));
            const float low = isoGain (cc.eq[2].load (std::memory_order_relaxed));
            in.stemGain[(int) Stem::Vocals].store (hi, std::memory_order_relaxed);   // Serato: HI = Vocal
            in.stemGain[(int) Stem::Melody].store (mid, std::memory_order_relaxed);  //         MID = Bass + Melody
            in.stemGain[(int) Stem::Bass].store (mid, std::memory_order_relaxed);
            in.stemGain[(int) Stem::Drums].store (low, std::memory_order_relaxed);   //         LOW = Drums
        }
        else
            for (int s = 0; s < kNumStems; ++s) in.stemGain[s].store (1.0f, std::memory_order_relaxed);
    }

    // ---- decks
    bool busActive[kNumDecks][kDeckBuses];
    for (int d = 0; d < kNumDecks; ++d)
    {
        auto& deck = *decks_[(std::size_t) d];
        DeckContext dc = ctx;
        dc.followLeader = deck.sync() && leader_ >= 0 && leader_ != d && leaderPlaying && leaderBpm > 1.0;
        dc.leaderBpm = leaderBpm;
        dc.leaderBeatPhase = leaderBeat;
        float* bufs[kDeckBuses * 2];
        for (int k = 0; k < kDeckBuses * 2; ++k) bufs[k] = deckBus_[d][k].data();
        deck.process (bufs, n, dc, busActive[d]);
        deck.drainEvents ([this] (const DeckEvent& e) { events_.push (e); });
    }

    // ---- sampler (slots routed to a channel are mixed into that deck's FX-part bus)
    float* chanInject[kNumDecks * 2];
    for (int d = 0; d < kNumDecks; ++d) { chanInject[d * 2] = deckBus_[d][0].data(); chanInject[d * 2 + 1] = deckBus_[d][1].data(); }
    sampler_.process (smpL_.data(), smpR_.data(), chanInject, n, clockBpm, clockBeat);

    std::fill (masterL_.begin(), masterL_.begin() + n, 0.0f);
    std::fill (masterR_.begin(), masterR_.begin() + n, 0.0f);
    std::fill (cueL_.begin(), cueL_.begin() + n, 0.0f);
    std::fill (cueR_.begin(), cueR_.begin() + n, 0.0f);

    const auto colorType = (ColorFxType) mixer.colorFx.load (std::memory_order_relaxed);
    const auto eqMode = (EqMode) mixer.eqMode.load (std::memory_order_relaxed);
    float xf = mixer.crossfader.load (std::memory_order_relaxed);
    if (mixer.xfReverse.load (std::memory_order_relaxed)) xf = 1.0f - xf;
    const float xfCurve = mixer.xfCurve.load (std::memory_order_relaxed);
    const float xfLag = mixer.xfCutLag.load (std::memory_order_relaxed);
    const float chCurve = mixer.chFaderCurve.load (std::memory_order_relaxed);
    double unitBpms[kTotalFxUnits];
    for (int u = 0; u < kTotalFxUnits; ++u)
    {
        unitBpms[u] = unitBpm (u);
        fxUnits_[(std::size_t) u].view.bpm.store (unitBpms[u], std::memory_order_relaxed);
    }

    auto runUnits = [&] (int target, FxPlacement stage, bool anyStage, float* l, float* r)
    {
        for (int u = 0; u < kTotalFxUnits; ++u)
        {
            auto& unit = fxUnits_[(std::size_t) u];
            if (! anyStage && unit.controls.placement.load (std::memory_order_relaxed) != (int) stage) continue;
            if (! unit.active (target)) continue;
            unit.process (target, l, r, n, unitBpms[u]);
        }
    };

    // ---- channel strips
    for (int ch = 0; ch < kNumDecks; ++ch)
    {
        auto& cc = mixer.ch[(std::size_t) ch];
        auto& deck = *decks_[(std::size_t) ch];
        float* aL = deckBus_[ch][0].data();
        float* aR = deckBus_[ch][1].data();
        float* bL = deckBus_[ch][2].data();
        float* bR = deckBus_[ch][3].data();
        const bool useB = busActive[ch][1];

        const float trim = trimGain (cc.trim.load (std::memory_order_relaxed)) * dbToGain (cc.autoGainDb.load (std::memory_order_relaxed));
        applyRamp (aL, aR, n, lastTrim_[(std::size_t) ch], trim);
        if (useB) applyRamp (bL, bR, n, lastTrim_[(std::size_t) ch], trim);
        lastTrim_[(std::size_t) ch] = trim;

        const float color = cc.color.load (std::memory_order_relaxed);
        const double bpm = deck.effectiveBpm() > 1.0 ? deck.effectiveBpm() : 120.0;
        colorFx_[(std::size_t) ch].processPreEq (colorType, color, aL, aR, n);
        runUnits (ch, FxPlacement::PreEq, false, aL, aR);

        float knobs[3];
        const bool iso = cc.partIso.load (std::memory_order_relaxed);
        for (int b = 0; b < 3; ++b) knobs[b] = iso ? 0.5f : cc.eq[b].load (std::memory_order_relaxed);
        eqA_[(std::size_t) ch].process (knobs, eqMode, aL, aR, n);
        if (useB) eqB_[(std::size_t) ch].process (knobs, eqMode, bL, bR, n);

        colorFx_[(std::size_t) ch].processPostEq (colorType, color, aL, aR, n);
        runUnits (ch, FxPlacement::PreFader, false, aL, aR);

        float pk = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float l = aL[i] + (useB ? bL[i] : 0.0f), r = aR[i] + (useB ? bR[i] : 0.0f);
            pk = std::max (pk, std::max (std::abs (l), std::abs (r)));
            if (cc.cue.load (std::memory_order_relaxed)) { cueL_[(std::size_t) i] += l; cueR_[(std::size_t) i] += r; }
        }
        mixerView.channelPeak[ch].store (pk, std::memory_order_relaxed);

        const float fader = channelFaderGain (cc.fader.load (std::memory_order_relaxed), chCurve);
        applyRamp (aL, aR, n, lastFader_[(std::size_t) ch], fader);
        if (useB) applyRamp (bL, bR, n, lastFader_[(std::size_t) ch], fader);
        lastFader_[(std::size_t) ch] = fader;

        colorFx_[(std::size_t) ch].processPostFader (colorType, color, bpm, aL, aR, n);
        runUnits (ch, FxPlacement::PostFader, false, aL, aR);

        const auto assign = (XfAssign) cc.xfAssign.load (std::memory_order_relaxed);
        const float xg = assign == XfAssign::Thru ? 1.0f : crossfaderGain (assign == XfAssign::A ? xf : 1.0f - xf, xfCurve, xfLag);
        const float x0 = lastXf_[(std::size_t) ch];
        const float step = (xg - x0) / (float) n;
        float g = x0;
        for (int i = 0; i < n; ++i)
        {
            g += step;
            masterL_[(std::size_t) i] += (aL[i] + (useB ? bL[i] : 0.0f)) * g;
            masterR_[(std::size_t) i] += (aR[i] + (useB ? bR[i] : 0.0f)) * g;
        }
        lastXf_[(std::size_t) ch] = xg;
    }

    // ---- sampler bus
    runUnits ((int) FxTarget::Sampler, FxPlacement::PostFader, true, smpL_.data(), smpR_.data());
    const float smpVol = mixer.samplerVolume.load (std::memory_order_relaxed);
    applyRamp (smpL_.data(), smpR_.data(), n, lastSamplerVol_, smpVol);
    lastSamplerVol_ = smpVol;
    const bool smpCue = mixer.samplerCue.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        masterL_[(std::size_t) i] += smpL_[(std::size_t) i];
        masterR_[(std::size_t) i] += smpR_[(std::size_t) i];
        if (smpCue) { cueL_[(std::size_t) i] += smpL_[(std::size_t) i]; cueR_[(std::size_t) i] += smpR_[(std::size_t) i]; }
    }

    // ---- master
    runUnits ((int) FxTarget::Master, FxPlacement::PostFader, true, masterL_.data(), masterR_.data());
    performer.process (masterL_.data(), masterR_.data(), n, clockBpm, clockBeat);   // the whole mix
    const float masterLevel = mixer.masterLevel.load (std::memory_order_relaxed);
    applyRamp (masterL_.data(), masterR_.data(), n, lastMaster_, masterLevel);
    lastMaster_ = masterLevel;
    float prePeak = peakOf (masterL_.data(), masterR_.data(), n);
    if (mixer.limiter.load (std::memory_order_relaxed)) limiter_.process (masterL_.data(), masterR_.data(), n, 0.977f);
    if (mixer.masterMono.load (std::memory_order_relaxed))
        for (int i = 0; i < n; ++i) { const float m = 0.5f * (masterL_[(std::size_t) i] + masterR_[(std::size_t) i]); masterL_[(std::size_t) i] = masterR_[(std::size_t) i] = m; }
    float pl = 0.0f, pr = 0.0f;
    for (int i = 0; i < n; ++i) { pl = std::max (pl, std::abs (masterL_[(std::size_t) i])); pr = std::max (pr, std::abs (masterR_[(std::size_t) i])); }
    mixerView.masterPeak[0].store (pl, std::memory_order_relaxed);
    mixerView.masterPeak[1].store (pr, std::memory_order_relaxed);
    mixerView.limiterReductionDb.store (limiter_.reductionDb(), std::memory_order_relaxed);
    mixerView.clip.store (prePeak > 1.0f, std::memory_order_relaxed);
    recorder.push (masterL_.data(), masterR_.data(), n);

    // ---- outputs
    auto out = [&] (int ch) -> float* { return ch >= 0 && ch < numOutputs ? outputs[ch] : nullptr; };
    if (float* o = out (routing.masterL.load (std::memory_order_relaxed))) for (int i = 0; i < n; ++i) o[i] += masterL_[(std::size_t) i];
    if (float* o = out (routing.masterR.load (std::memory_order_relaxed))) for (int i = 0; i < n; ++i) o[i] += masterR_[(std::size_t) i];
    const float booth = mixer.boothLevel.load (std::memory_order_relaxed);
    if (booth > 0.0f)
    {
        if (float* o = out (routing.boothL.load (std::memory_order_relaxed))) for (int i = 0; i < n; ++i) o[i] += masterL_[(std::size_t) i] * booth;
        if (float* o = out (routing.boothR.load (std::memory_order_relaxed))) for (int i = 0; i < n; ++i) o[i] += masterR_[(std::size_t) i] * booth;
    }
    float* hl = out (routing.phonesL.load (std::memory_order_relaxed));
    float* hr = out (routing.phonesR.load (std::memory_order_relaxed));
    if (hl != nullptr || hr != nullptr)
    {
        const bool hw = mixer.phonesMixInHardware.load (std::memory_order_relaxed);
        const bool masterCue = mixer.masterCue.load (std::memory_order_relaxed);
        const float mix = hw ? 0.0f : mixer.phonesMix.load (std::memory_order_relaxed);
        const float level = hw ? 1.0f : mixer.phonesLevel.load (std::memory_order_relaxed);
        const bool split = mixer.phonesSplit.load (std::memory_order_relaxed);
        const bool invert = mixer.splitInvert.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            float cl = cueL_[(std::size_t) i], cr = cueR_[(std::size_t) i];
            if (masterCue) { cl += masterL_[(std::size_t) i]; cr += masterR_[(std::size_t) i]; }
            float l, r;
            if (split)
            {
                const float cm = 0.5f * (cl + cr), mm = 0.5f * (masterL_[(std::size_t) i] + masterR_[(std::size_t) i]);
                l = invert ? mm : cm;
                r = invert ? cm : mm;
            }
            else
            {
                l = cl * (1.0f - mix) + masterL_[(std::size_t) i] * mix;
                r = cr * (1.0f - mix) + masterR_[(std::size_t) i] * mix;
            }
            if (hl != nullptr) hl[i] += std::clamp (l * level, -1.0f, 1.0f);
            if (hr != nullptr) hr[i] += std::clamp (r * level, -1.0f, 1.0f);
        }
    }
}

} // namespace dj
