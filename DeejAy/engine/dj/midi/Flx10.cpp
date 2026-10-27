#include "Flx10.h"
#include <chrono>

namespace dj::midi
{

namespace
{
    // MIDI channels (0-based) from the FLX10 list of MIDI messages
    constexpr int kChEffect = 4;      // channel 5
    constexpr int kChGlobal = 6;      // channel 7: browser / global mixer
    constexpr int kChFirstPad = 7;    // channel 8: deck 1 pads; deck n = 7 + 2n, +1 with SHIFT
    constexpr int kChDisplay = 15;    // channel 16: MIDI-OUT to the On Jog Displays

    // Pad note bases per hardware pad mode (manual §15): note = base + (page-1)*8 + (pad-1)
    enum HwMode { HwHotCue = 0, HwPadFx1 = 1, HwBeatJump = 2, HwSampler = 3, HwKeyboard = 4, HwPadFx2 = 5, HwBeatLoop = 6, HwKeyShift = 7 };

    double now() noexcept
    {
        return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    int relative (int v) noexcept { return v - 64; }                         // jog: 0x41.. = CW, 0x3F.. = CCW
    int browseDelta (int v) noexcept { return v < 64 ? v : v - 128; }        // browse: 0x01..0x1E CW, 0x7F..0x62 CCW

    // Beat values reachable with BEAT </> for each Beat FX kind (manual "BEAT </> range")
    double stepBeats (double current, int dir, BeatKind kind) noexcept
    {
        static constexpr double t16[] { 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 3.0 / 4, 1, 2, 4, 8, 16 };
        static constexpr double t64[] { 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 3.0 / 4, 1, 2, 4, 8, 16, 32, 64 };
        const double* t = kind == BeatKind::Time16 ? t16 : t64;
        const int n = kind == BeatKind::Time16 ? 10 : 12;
        if (kind == BeatKind::Signed64)
        {
            // -64 .. -1/16, +1/16 .. +64 as one ordered list
            double list[24];
            for (int i = 0; i < 12; ++i) { list[i] = -t64[11 - i]; list[12 + i] = t64[i]; }
            int idx = 0;
            for (int i = 0; i < 24; ++i) if (std::abs (list[i] - current) < std::abs (list[idx] - current)) idx = i;
            return list[std::clamp (idx + dir, 0, 23)];
        }
        int idx = 0;
        for (int i = 0; i < n; ++i) if (std::abs (t[i] - std::abs (current)) < std::abs (t[idx] - std::abs (current))) idx = i;
        return t[std::clamp (idx + dir, 0, n - 1)];
    }
}

const char* padModeName (PadMode m) noexcept
{
    switch (m)
    {
        case PadMode::HotCue: return "HOT CUE";
        case PadMode::PitchPlay: return "PITCH PLAY";
        case PadMode::SavedLoop: return "SAVED LOOP";
        case PadMode::PadFx: return "PAD FX";
        case PadMode::Roll: return "ROLL";
        case PadMode::BeatJump: return "BEAT JUMP";
        case PadMode::AutoLoop: return "AUTO LOOP";
        case PadMode::Sampler: return "SAMPLER";
        case PadMode::KeyShift: return "KEY SHIFT";
        case PadMode::Slicer: return "SLICER";
        case PadMode::SlicerLoop: return "SLICER LOOP";
        case PadMode::Stems: return "STEMS";
    }
    return "";
}

Flx10::Flx10 (Engine& engine, ControllerHost& host) : engine_ (engine), host_ (host)
{
    for (auto& r : msb_) r.fill (0);
}

double Flx10::rollBeats (int pad, int shift) noexcept
{
    static constexpr double base[8] { 1.0 / 8, 1.0 / 4, 1.0 / 2, 1, 2, 4, 8, 16 };
    return base[std::clamp (pad, 0, 7)] * std::pow (2.0, shift);
}

double Flx10::autoLoopBeats (int pad, int shift) noexcept
{
    return rollBeats (pad, shift);
}

void Flx10::setPadMode (int deck, PadMode mode) noexcept
{
    if (deck < 0 || deck >= kNumDecks) return;
    pads_[(std::size_t) deck].mode.store ((int) mode);
    host_.padModeChanged (deck, mode);
}

void Flx10::command (Cmd type, int deck, int index, double value, bool flag) noexcept
{
    Command c;
    c.type = type; c.deck = deck; c.index = index; c.value = value; c.flag = flag;
    engine_.postFromMidiThread (c);
}

int Flx10::otherPlayingDeck (int deck) const noexcept
{
    const int leader = engine_.view.leader.load();
    if (leader >= 0 && leader != deck) return leader;
    int best = -1;
    float bestLevel = -1.0f;
    for (int d = 0; d < kNumDecks; ++d)
    {
        if (d == deck || ! engine_.deck (d).view.playing.load()) continue;
        const float lvl = engine_.mixer.ch[(std::size_t) d].fader.load();
        if (lvl > bestLevel) { bestLevel = lvl; best = d; }
    }
    return best;
}

float Flx10::fourteenBit (int channel, int cc, int value) noexcept
{
    // MSB on CC x, LSB on CC x + 32. The MSB alone already moves the control; the LSB refines it.
    if (cc < 32)
    {
        msb_[(std::size_t) channel][(std::size_t) cc] = (std::uint8_t) value;
        return (float) (value << 7) / 16383.0f;
    }
    const int msb = msb_[(std::size_t) channel][(std::size_t) (cc - 32)];
    return (float) ((msb << 7) | value) / 16383.0f;
}

// ------------------------------------------------------------------------------------- input
void Flx10::handle (const Message& m) noexcept
{
    const int ch = m.channel();
    const int type = m.type();
    if (type == 0x90 || type == 0x80)
    {
        const bool on = type == 0x90 && m.data2 > 0;
        if (ch < kHwDecks) deckNote (ch + layerOffset(), m.data1, on);
        else if (ch == kChEffect) fxNote (m.data1, on);
        else if (ch == kChGlobal) globalNote (m.data1, on);
        else if (ch >= kChFirstPad && ch < kChFirstPad + 2 * kHwDecks)
            padNote ((ch - kChFirstPad) / 2 + layerOffset(), ((ch - kChFirstPad) & 1) != 0, m.data1, on);
        return;
    }
    if (type == 0xB0)
    {
        if (ch < kHwDecks) deckCc (ch + layerOffset(), m.data1, m.data2);
        else if (ch == kChEffect) fxCc (m.data1, m.data2);
        else if (ch == kChGlobal) globalCc (m.data1, m.data2);
    }
}

void Flx10::deckNote (int d, int note, bool on) noexcept
{
    auto& deck = engine_.deck (d);
    auto& cc = engine_.mixer.ch[(std::size_t) d];
    auto press = [&] (Cmd t, int index = 0, double value = 0.0, bool flag = false) { if (on) command (t, d, index, value, flag); };
    switch (note)
    {
        case 0x3F: shift_[(std::size_t) d].store (on); return;
        // transport
        case 0x0B: press (Cmd::PlayPause); return;
        case 0x47: press (Cmd::Stutter); return;                          // SHIFT + PLAY: stutter
        case 0x0C: command (on ? Cmd::CueDown : Cmd::CueUp, d); return;
        case 0x48: press (Cmd::JumpToStart); return;                      // SHIFT + CUE
        // jog touch
        case 0x36: command (Cmd::JogTouch, d, 0, 0.0, on); return;
        case 0x67: if (! on) command (Cmd::JogTouch, d, 0, 0.0, false); return;
        // sync / tempo / key
        case 0x58: press (Cmd::SyncOn); return;
        case 0x5C: press (Cmd::SyncOff); return;                          // SHIFT + SYNC: cancel
        case 0x78: return;                                                // long press (rekordbox sync rate)
        case 0x41: press (Cmd::TempoReset); return;
        case 0x60: press (Cmd::TempoRangeCycle); return;                  // SHIFT: +/-8 -> 16 -> 50 %
        case 0x65: press (Cmd::KeySync); return;
        case 0x64: press (Cmd::KeyReset); return;
        case 0x1A: return;
        case 0x59: press (Cmd::KeyShiftDelta, -1); return;                // Serato: MIX POINT SELECT < = key down
        case 0x5A: press (Cmd::KeyShiftDelta, 1); return;                 //         MIX POINT SELECT > = key up
        case 0x4A: press (Cmd::KeylockToggle); return;                    //         MIX POINT LINK = key lock
        case 0x49: case 0x42: case 0x43: return;
        // stems (ACTIVE PART)
        case 0x0D: press (Cmd::StemMuteToggle, 1 << (int) Stem::Drums); return;
        case 0x0E: press (Cmd::StemMuteToggle, 1 << (int) Stem::Vocals); return;
        case 0x0F: press (Cmd::StemMuteToggle, (1 << (int) Stem::Melody) | (1 << (int) Stem::Bass)); return;
        // loop slots (Serato: CUE/LOOP CALL select loop slot, SHIFT lock / delete)
        case 0x51: press (Cmd::LoopSlotSelect, -1); return;
        case 0x53: press (Cmd::LoopSlotSelect, 1); return;
        case 0x3D: press (Cmd::LoopSlotLock); return;
        case 0x3E: press (Cmd::LoopSlotDelete); return;
        // loops
        case 0x10: press (Cmd::LoopIn); return;
        case 0x11: press (Cmd::LoopOut); return;
        case 0x4C: loopInHeld_[(std::size_t) d].store (on); return;      // SHIFT + LOOP IN (in loop): jog moves the in point
        case 0x4D:                                                        // SHIFT + LOOP OUT: reloop, or jog moves the out point
            loopOutHeld_[(std::size_t) d].store (on && deck.view.loopActive.load());
            if (on && ! deck.view.loopActive.load()) command (Cmd::Reloop, d);
            return;
        case 0x14: press (Cmd::FourBeatExit); return;
        case 0x50: press (Cmd::LoopActiveToggle); return;
        // slip / reverse / quantize
        case 0x15: command (on ? Cmd::CensorDown : Cmd::CensorUp, d); return;
        case 0x38: press (Cmd::ReverseToggle); return;
        case 0x35: press (Cmd::QuantizeToggle); return;
        case 0x68: if (on) host_.tapTempo (d); return;
        case 0x40: press (Cmd::SlipToggle); return;
        case 0x17: press (Cmd::VinylToggle); return;
        // 4 BEAT JUMP: press = 4 beats, SHIFT = 16 beats, long press = fast search while held
        case 0x5E: press (Cmd::BeatJump, 0, -4.0); beatJumpHeld_[(std::size_t) d].store (on); return;
        case 0x5F: press (Cmd::BeatJump, 0, 4.0); beatJumpHeld_[(std::size_t) d].store (on); return;
        case 0x61: press (Cmd::BeatJump, 0, -16.0); return;
        case 0x62: press (Cmd::BeatJump, 0, 16.0); return;
        case 0x70: command (on ? Cmd::SearchDown : Cmd::SearchUp, d, -1); return;
        case 0x71: command (on ? Cmd::SearchDown : Cmd::SearchUp, d, 1); return;
        // deck select: state message, the selected deck's channel carries 0x7F
        case 0x3C:
            if (on) { if (d % kHwDecks == 0 || d % kHwDecks == 2) leftDeck_.store (d); else rightDeck_.store (d); resetOutputCache(); }
            return;
        case 0x72: return;
        // pad mode buttons
        case 0x1B: if (on) setPadMode (d, PadMode::HotCue); return;
        case 0x4B: if (on) setPadMode (d, PadMode::SavedLoop); return;     // hold HOT CUE (Serato: Saved Flip slot)
        case 0x69: if (on) setPadMode (d, PadMode::PitchPlay); return;
        case 0x1E: if (on) setPadMode (d, PadMode::PadFx); return;
        case 0x6B: if (on) setPadMode (d, PadMode::Roll); return;
        case 0x20: if (on) setPadMode (d, PadMode::BeatJump); return;
        case 0x6D: if (on) setPadMode (d, PadMode::AutoLoop); return;
        case 0x22: if (on) setPadMode (d, PadMode::Sampler); return;
        case 0x6F: if (on) setPadMode (d, PadMode::KeyShift); return;
        // mixer channel buttons
        case 0x54: if (on) cc.cue.store (! cc.cue.load()); return;
        case 0x39: if (on) cc.partIso.store (! cc.partIso.load()); return;  // SHIFT + CH CUE: EQ <-> Part ISO
        case 0x16: if (on) cc.xfAssign.store ((int) XfAssign::A); return;
        case 0x18: if (on) cc.xfAssign.store ((int) XfAssign::B); return;
        case 0x1D: if (on) cc.xfAssign.store ((int) XfAssign::Thru); return;
        case 0x55: case 0x56: case 0x57: return;                           // input select (PC-A / LINE / PC-B)
        // fader start
        case 0x66: if (on && faderStart.load()) command (Cmd::Play, d); return;
        case 0x5D: if (on && faderStart.load()) { command (Cmd::SyncOn, d); command (Cmd::Play, d); } return;
        case 0x52: if (on && faderStart.load()) { command (Cmd::CueDown, d); command (Cmd::CueUp, d); } return;
        default: break;
    }

    // PAGE < / > : note depends on the hardware pad mode (manual §15)
    auto page = [&] (int hw, int dir, bool shift)
    {
        if (! on) return;
        auto& ps = pads_[(std::size_t) d];
        switch (hw)
        {
            case HwKeyboard: case HwKeyShift:
                if (shift) ps.pitchOffset.store (std::clamp (ps.pitchOffset.load() + dir, -12, 12));
                else ps.pitchRange.store (std::clamp (ps.pitchRange.load() - dir, 0, 2));   // > = towards "Up"
                break;
            case HwPadFx2: ps.rollShift.store (std::clamp (ps.rollShift.load() + dir, -3, 2)); break;
            case HwBeatLoop:
                if (shift) command (Cmd::LoopShift, d, 0, dir * 1.0);
                else ps.autoLoopShift.store (std::clamp (ps.autoLoopShift.load() + dir, -3, 2));
                break;
            case HwBeatJump:
                ps.beatJumpBeats.store (std::clamp (ps.beatJumpBeats.load() * (dir > 0 ? 2.0 : 0.5), 1.0 / 32.0, 256.0));
                break;
            case HwSampler:
                engine_.samplerBank.store ((engine_.samplerBank.load() + dir + kSamplerBanks) % kSamplerBanks);
                break;
            default: break;
        }
    };
    if (note >= 0x24 && note <= 0x2B) { page (note - 0x24, -1, false); return; }
    if (note >= 0x2C && note <= 0x33) { page (note - 0x2C, 1, false); return; }
    if (note >= 0x01 && note <= 0x08) { page (note - 0x01, -1, true); return; }
    if (note == 0x09) { page (HwHotCue, 1, true); return; }
    if (note >= 0x7A && note <= 0x7F) { page (note - 0x7A + 1, 1, true); return; }
    if (note == 0x00) { page (HwKeyShift, 1, true); return; }
}

void Flx10::deckCc (int d, int cc, int v) noexcept
{
    auto& in = engine_.deck (d).inputs;
    auto& ch = engine_.mixer.ch[(std::size_t) d];
    switch (cc)
    {
        // jog wheel (relative)
        case 0x22:  // platter, Vinyl on: scratch while touched
            if (engine_.deck (d).view.scratching.load()) in.scratchTicks.fetch_add (relative (v));
            else in.bendTicks.fetch_add (relative (v));
            return;
        case 0x23: case 0x21: in.bendTicks.fetch_add (relative (v)); return;   // platter (Vinyl off) / side: pitch bend
        case 0x29: in.seekTicks.fetch_add (relative (v)); return;               // 4 BEAT JUMP + platter: fast search
        case 0x1F:                                                              // SHIFT + platter
            if (loopInHeld_[(std::size_t) d].load()) in.loopInTicks.fetch_add (relative (v));
            else if (loopOutHeld_[(std::size_t) d].load()) in.loopOutTicks.fetch_add (relative (v));
            else in.gridBpmTicks.fetch_add (relative (v));                      // beatgrid interval
            return;
        case 0x26: in.gridSlideTicks.fetch_add (relative (v)); return;          // SHIFT + side: slide the grid
        default: break;
    }
    const int base = cc < 32 ? cc : cc - 32;
    const float x = fourteenBit (d % kHwDecks, cc, v);
    switch (base)
    {
        case 0x00: case 0x05:   // TEMPO slider ('-' end = min)
            in.tempoFader.store (x);
            in.tempoFaderMoved.store (true);
            return;
        case 0x04: ch.trim.store (x); return;
        case 0x07: ch.eq[0].store (x); return;
        case 0x0B: ch.eq[1].store (x); return;
        case 0x0F: ch.eq[2].store (x); return;
        case 0x13: ch.fader.store (x); return;
        default: return;
    }
}

void Flx10::padNote (int d, bool shift, int note, bool on) noexcept
{
    const int hw = note >> 4;
    const int pad = note & 7;            // page 2 (+8) behaves like page 1 in Serato
    auto& ps = pads_[(std::size_t) d];
    const auto mode = (PadMode) ps.mode.load();
    switch (hw)
    {
        case HwHotCue:
            if (mode == PadMode::SavedLoop) { if (on) command (Cmd::LoopSlotPad, d, pad, 0.0, shift); return; }
            command (on ? Cmd::HotCueDown : Cmd::HotCueUp, d, pad, 0.0, shift);
            return;
        case HwKeyboard:
            if (! on) return;
            if (shift) command (Cmd::PitchPlayCue, d, pad);
            else command (Cmd::PitchPlay, d, pitchPlaySemitones ((PitchPlayRange) ps.pitchRange.load(), pad, ps.pitchOffset.load()));
            return;
        case HwPadFx1:
            command (on ? Cmd::PadFxDown : Cmd::PadFxUp, d, pad);
            return;
        case HwPadFx2:  // Serato: Roll
            if (on) { ps.heldRollPad.store (pad); command (Cmd::RollDown, d, 0, rollBeats (pad, ps.rollShift.load())); }
            else if (ps.heldRollPad.load() == pad) { ps.heldRollPad.store (-1); command (Cmd::RollUp, d); }
            return;
        case HwBeatJump:
        {
            const double size = ps.beatJumpBeats.load();
            switch (pad)
            {
                case 0: if (on) command (Cmd::BeatJump, d, 0, -size); return;
                case 1: if (on) ps.beatJumpBeats.store (std::max (1.0 / 32.0, size * 0.5)); return;
                case 2: if (on) ps.beatJumpBeats.store (std::min (256.0, size * 2.0)); return;
                case 3: if (on) command (Cmd::BeatJump, d, 0, size); return;
                case 4: if (on) host_.previousTrack (d); return;
                case 5: command (on ? Cmd::SearchDown : Cmd::SearchUp, d, -1); return;
                case 6: command (on ? Cmd::SearchDown : Cmd::SearchUp, d, 1); return;
                default: command (on ? Cmd::CensorDown : Cmd::CensorUp, d); return;
            }
        }
        case HwBeatLoop:  // Serato: Auto Loop (the same pad again exits)
            if (on) command (Cmd::AutoLoop, d, 0, autoLoopBeats (pad, ps.autoLoopShift.load()));
            return;
        case HwSampler:
        {
            const int slot = engine_.samplerBank.load() * kSamplerSlotsPerBank + pad;
            Command c;
            c.deck = slot;
            c.type = shift ? Cmd::SamplerStop : (on ? Cmd::SamplerPadDown : Cmd::SamplerPadUp);
            if (shift && ! on) return;
            engine_.postFromMidiThread (c);
            return;
        }
        default:  // KEY SHIFT
            if (on) command (Cmd::KeyShiftSet, d, shift ? 0 : pitchPlaySemitones ((PitchPlayRange) ps.pitchRange.load(), pad, ps.pitchOffset.load()));
            return;
    }
}

void Flx10::globalNote (int note, bool on) noexcept
{
    auto& mx = engine_.mixer;
    // BROWSE press with a deck selected: load; twice quickly on that deck = Instant Doubles
    static constexpr int loadNotes[kHwDecks] { 0x46, 0x47, 0x48, 0x49 };
    for (int hw = 0; hw < kHwDecks; ++hw)
        if (note == loadNotes[hw])
        {
            const int d = hw + layerOffset();
            if (! on) return;
            const double t = now();
            if (t - lastBrowsePress_[(std::size_t) d] < 0.45) host_.instantDoubles (d);
            else host_.loadSelected (d);
            lastBrowsePress_[(std::size_t) d] = t;
            return;
        }
    if (! on) return;
    switch (note)
    {
        case 0x5D: case 0x6D: case 0x5E: case 0x6F: return;               // SHIFT + rotary press
        case 0x65: host_.back (false); return;
        case 0x66: host_.back (true); return;
        case 0x7A: host_.view (false); return;
        case 0x68: host_.view (true); return;
        case 0x67: return;
        case 0x63: mx.masterCue.store (! mx.masterCue.load()); return;
        case 0x69: mx.samplerCue.store (! mx.samplerCue.load()); return;
        // SOUND COLOR FX: one type for all channels; pressing the active one again turns it off
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
        {
            const int current = mx.colorFx.load();
            mx.colorFx.store (current == note ? (int) ColorFxType::None : note);
            return;
        }
        // FX PART SELECT
        case 0x1B: engine_.fxPart[(int) Stem::Vocals].store (! engine_.fxPart[(int) Stem::Vocals].load()); return;
        case 0x1C: engine_.fxPart[(int) Stem::Drums].store (! engine_.fxPart[(int) Stem::Drums].load()); return;
        case 0x1D:
        {
            const bool on2 = ! engine_.fxPart[(int) Stem::Melody].load();
            engine_.fxPart[(int) Stem::Melody].store (on2);
            engine_.fxPart[(int) Stem::Bass].store (on2);
            return;
        }
        default: break;
    }
    // SHIFT + ACTIVE PART pressed twice: Part Instant Doubles (decks 1-4)
    if (note >= 0x24 && note <= 0x2F)
    {
        const int group = (note - 0x24) / 4, d = (note - 0x24) % 4 + layerOffset();
        const int mask = group == 0 ? (1 << (int) Stem::Drums) : group == 1 ? (1 << (int) Stem::Vocals) : ((1 << (int) Stem::Melody) | (1 << (int) Stem::Bass));
        host_.partInstantDoubles (d, mask);
    }
}

void Flx10::globalCc (int cc, int v) noexcept
{
    if (cc == 0x40) { host_.browse (browseDelta (v), false); return; }
    if (cc == 0x64) { host_.browse (browseDelta (v), true); return; }
    auto& mx = engine_.mixer;
    const int base = cc < 32 ? cc : cc - 32;
    if (base >= 0x20) return;
    const float x = fourteenBit (kChGlobal, cc, v);
    switch (base)
    {
        case 0x1F: mx.crossfader.store (x); return;
        case 0x08: if (applyMasterLevel.load()) mx.masterLevel.store (x); return;
        case 0x09: if (applyMasterLevel.load()) mx.boothLevel.store (x); return;
        case 0x0C: mx.phonesMix.store (x); return;
        case 0x0D: mx.phonesLevel.store (x); return;
        case 0x03: mx.samplerVolume.store (x); return;
        case 0x17: case 0x18: case 0x19: case 0x1A:
            mx.ch[(std::size_t) (base - 0x17 + layerOffset())].color.store (x * 2.0f - 1.0f);
            return;
        default: return;   // MIC EQ: processed on the unit
    }
}

void Flx10::fxNote (int note, bool on) noexcept
{
    auto& u = engine_.fxUnit (kBeatFxUnit).controls;
    // BEAT FX SELECT position (0x20..0x2D) and CH SELECT position (0x10..0x16) arrive as note-on states
    if (note >= 0x20 && note < 0x20 + kFlx10BeatFxCount)
    {
        if (! on) return;
        beatFxPosition_.store (note - 0x20);
        host_.beatFxSelect (flx10BeatFx (note - 0x20));
        return;
    }
    if (note >= 0x10 && note <= 0x16)
    {
        if (! on) return;
        static constexpr int target[7] { 0, 1, 2, 3, (int) FxTarget::Master, -1, (int) FxTarget::Sampler };
        const int pick = target[note - 0x10] >= 0 && target[note - 0x10] < kHwDecks ? target[note - 0x10] + layerOffset() : target[note - 0x10];
        for (int t = 0; t < kFxTargets; ++t) u.assign[t].store (t == pick);
        return;
    }
    if (! on) return;
    const FxType type = flx10BeatFx (beatFxPosition_.load());
    const BeatKind kind = fxBeatKind (type);
    switch (note)
    {
        case 0x4A: case 0x4B:
        {
            const int dir = note == 0x4B ? 1 : -1;
            if (kind == BeatKind::Size)
                u.params[0].store (std::clamp (u.params[0].load() + 0.1f * (float) dir, 0.01f, 1.0f));
            else
                u.beats.store ((float) stepBeats (u.beats.load(), dir, kind));
            return;
        }
        case 0x66: u.tapMode.store (false); return;                      // SHIFT + BEAT <: BPM AUTO
        case 0x6B:                                                       // SHIFT + BEAT >: TAP
        {
            const double t = now();
            if (! taps_.empty() && t - taps_.back() > 2.0) taps_.clear();
            taps_.push_back (t);
            if (taps_.size() > 8) taps_.erase (taps_.begin());
            if (taps_.size() >= 3)
            {
                const double avg = (taps_.back() - taps_.front()) / (double) (taps_.size() - 1);
                if (avg > 0.15) { u.tapBpm.store (60.0 / avg); u.tapMode.store (true); }
            }
            return;
        }
        case 0x47:
        {
            Command c;
            c.type = Cmd::FxSlotToggle;
            c.deck = kBeatFxUnit;
            c.index = 0;
            engine_.postFromMidiThread (c);
            return;
        }
        default: return;
    }
}

void Flx10::fxCc (int cc, int v) noexcept
{
    const float x = fourteenBit (kChEffect, cc, v);
    if (cc == 0x02 || cc == 0x22) engine_.fxUnit (kBeatFxUnit).controls.depth.store (x);
}

// ------------------------------------------------------------------------------------ output
void Flx10::put (std::vector<Message>& out, std::uint8_t status, std::uint8_t d1, std::uint8_t d2)
{
    const int key = (status << 8) | d1;
    const auto it = sent_.find (key);
    if (it != sent_.end() && it->second == d2) return;
    sent_[key] = d2;
    out.push_back ({ status, d1, d2 });
}

void Flx10::collectOutput (std::vector<Message>& out)
{
    if (resendAll_.exchange (false)) sent_.clear();
    const auto led = [] (bool on) -> std::uint8_t { return on ? 0x7F : 0x00; };
    for (int hw = 0; hw < kHwDecks; ++hw)
    {
        const int d = hw + layerOffset();          // engine deck shown on this hardware deck
        const auto& v = engine_.deck (d).view;
        const auto st = (std::uint8_t) (0x90 | hw);
        const bool loaded = v.loaded.load();
        const bool playing = v.playing.load();
        const double sr = v.trackSampleRate.load();
        const double pos = v.position.load();
        put (out, st, 0x0B, led (playing));
        put (out, st, 0x0C, led (loaded && ! playing && std::abs (pos - v.cuePoint.load()) < 2.0));
        put (out, st, 0x58, led (v.sync.load()));
        put (out, st, 0x41, led (loaded && std::abs (v.tempo.load()) < 1e-9));
        put (out, st, 0x65, led (v.keyShift.load() != 0));
        put (out, st, 0x4A, led (v.keylock.load()));                     // MIX POINT LINK = key lock in Serato
        put (out, st, 0x40, led (v.slip.load()));
        put (out, st, 0x35, led (engine_.view.quantize.load()));
        put (out, st, 0x15, led (v.censor.load() || v.reverse.load()));
        const bool loop = v.loopActive.load() || v.rollActive.load();
        put (out, st, 0x10, led (loop));
        put (out, st, 0x11, led (loop));
        put (out, st, 0x14, led (v.loopActive.load()));
        const bool stems = v.stemsAvailable.load();
        put (out, st, 0x0D, led (stems && ! v.stemMuted[(int) Stem::Drums].load()));
        put (out, st, 0x0E, led (stems && ! v.stemMuted[(int) Stem::Vocals].load()));
        put (out, st, 0x0F, led (stems && ! v.stemMuted[(int) Stem::Melody].load()));
        put (out, st, 0x54, led (engine_.mixer.ch[(std::size_t) d].cue.load()));

        const auto mode = (PadMode) pads_[(std::size_t) d].mode.load();
        put (out, st, 0x1B, led (mode == PadMode::HotCue || mode == PadMode::PitchPlay || mode == PadMode::SavedLoop));
        put (out, st, 0x1E, led (mode == PadMode::PadFx || mode == PadMode::Roll));
        put (out, st, 0x20, led (mode == PadMode::BeatJump || mode == PadMode::AutoLoop));
        put (out, st, 0x22, led (mode == PadMode::Sampler || mode == PadMode::KeyShift));

        // pads (page 1 notes of the active mode)
        const auto padSt = (std::uint8_t) (0x90 | (kChFirstPad + 2 * hw));
        int padBase = HwHotCue;
        switch (mode)
        {
            case PadMode::PadFx: padBase = HwPadFx1; break;
            case PadMode::Roll: padBase = HwPadFx2; break;
            case PadMode::BeatJump: padBase = HwBeatJump; break;
            case PadMode::AutoLoop: padBase = HwBeatLoop; break;
            case PadMode::Sampler: padBase = HwSampler; break;
            case PadMode::PitchPlay: padBase = HwKeyboard; break;
            case PadMode::KeyShift: padBase = HwKeyShift; break;
            default: padBase = HwHotCue; break;
        }
        for (int p = 0; p < 8; ++p)
        {
            bool lit = false;
            switch (mode)
            {
                case PadMode::HotCue: case PadMode::PitchPlay: lit = v.hotCue[p].load() >= 0.0; break;
                case PadMode::SavedLoop: lit = v.loopSlotOut[p].load() > 0.0; break;
                case PadMode::PadFx: lit = true; break;   // all eight Pad FX are always available
                case PadMode::Roll: lit = pads_[(std::size_t) d].heldRollPad.load() == p; break;
                case PadMode::AutoLoop:
                {
                    const double fpb = v.trackBpm.load() > 0 ? 60.0 * sr / v.trackBpm.load() : 0.0;
                    const double len = v.loopOut.load() - v.loopIn.load();
                    lit = v.loopActive.load() && fpb > 0 && std::abs (len - autoLoopBeats (p, pads_[(std::size_t) d].autoLoopShift.load()) * fpb) < 1.0;
                    break;
                }
                case PadMode::Sampler:
                    lit = engine_.sampler().view[(std::size_t) (engine_.samplerBank.load() * kSamplerSlotsPerBank + p)].loaded.load();
                    break;
                case PadMode::BeatJump: lit = p == 0 || p == 3; break;
                case PadMode::KeyShift:
                    lit = pitchPlaySemitones ((PitchPlayRange) pads_[(std::size_t) d].pitchRange.load(), p, pads_[(std::size_t) d].pitchOffset.load()) == v.keyShift.load();
                    break;
                default: break;
            }
            put (out, padSt, (std::uint8_t) (padBase * 16 + p), led (lit));
        }

        // On Jog Display (channel 16) and the channel meter
        const auto disp = (std::uint8_t) (0xB0 | kChDisplay);
        const auto dispNote = (std::uint8_t) (0x90 | kChDisplay);
        const int bpm10 = std::clamp ((int) std::lround (v.bpm.load() * 10.0), 0, 9999);
        put (out, disp, (std::uint8_t) (0x14 + hw), (std::uint8_t) (bpm10 >> 7));
        put (out, disp, (std::uint8_t) (0x34 + hw), (std::uint8_t) (bpm10 & 0x7F));
        // playing speed -100.0 .. +100.0 % over 0 .. 9999 (encoding to be confirmed on hardware)
        const double pct = std::clamp (v.tempo.load() * 100.0, -100.0, 100.0);
        const int sp = (int) std::lround ((pct + 100.0) / 200.0 * 9999.0);
        put (out, disp, (std::uint8_t) (0x18 + hw), (std::uint8_t) (sp >> 7));
        put (out, disp, (std::uint8_t) (0x38 + hw), (std::uint8_t) (sp & 0x7F));
        const double elapsed = sr > 0 ? pos / sr : 0.0;
        put (out, disp, (std::uint8_t) (0x42 + 2 * hw), (std::uint8_t) std::clamp ((int) (elapsed / 60.0), 0, 99));
        put (out, disp, (std::uint8_t) (0x43 + 2 * hw), (std::uint8_t) std::clamp ((int) std::fmod (elapsed, 60.0), 0, 99));
        // Digital Marker: the platter angle (one turn = 1.8 s)
        const int angle = (int) std::fmod (elapsed / 1.8 * 360.0, 360.0);
        put (out, disp, (std::uint8_t) (0x10 + hw), (std::uint8_t) (angle >> 7));
        put (out, disp, (std::uint8_t) (0x30 + hw), (std::uint8_t) (angle & 0x7F));
        put (out, dispNote, (std::uint8_t) (0x20 + hw), led (v.keylock.load()));
        put (out, dispNote, (std::uint8_t) (0x18 + hw), led (v.leader.load()));
        put (out, dispNote, (std::uint8_t) (0x1C + hw), led (v.sync.load()));
        put (out, (std::uint8_t) (0xB0 | hw), 0x4A, (std::uint8_t) std::clamp (0x0D + v.keyShift.load(), 0x01, 0x19));
        const float peak = engine_.mixerView.channelPeak[d].load();
        const float db = gainToDb (peak);
        put (out, (std::uint8_t) (0xB0 | hw), 0x02, (std::uint8_t) std::clamp ((int) ((db + 48.0f) / 48.0f * 127.0f), 0, 127));
    }
    // effects section
    const bool sel[3] { engine_.fxPart[(int) Stem::Vocals].load(), engine_.fxPart[(int) Stem::Drums].load(), engine_.fxPart[(int) Stem::Melody].load() };
    for (int i = 0; i < 3; ++i) put (out, (std::uint8_t) (0x90 | kChGlobal), (std::uint8_t) (0x1B + i), led (sel[i]));
    const int cfx = engine_.mixer.colorFx.load();
    for (int i = 0; i < kNumColorFx; ++i) put (out, (std::uint8_t) (0x90 | kChGlobal), (std::uint8_t) i, led (cfx == i));
    put (out, (std::uint8_t) (0x90 | kChEffect), 0x47, led (engine_.fxUnit (kBeatFxUnit).view.on[0].load()));
    put (out, (std::uint8_t) (0x90 | kChGlobal), 0x69, led (engine_.mixer.samplerCue.load()));
}

} // namespace dj::midi
