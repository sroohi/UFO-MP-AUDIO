// DDJ-FLX10 mapping tests: MIDI in -> engine state, engine state -> LEDs / jog display.
#include "TestHarness.h"
#include "dj/midi/Flx10.h"
#include <memory>

using namespace dj;
using namespace dj::midi;

namespace
{
struct Host final : ControllerHost
{
    int browsed = 0, loaded = -1, doubles = -1, prev = -1, taps = 0, partDeck = -1, partMask = 0;
    bool backShift = false;
    FxType beatFx = FxType::None;
    PadMode lastMode = PadMode::HotCue;
    void browse (int delta, bool) override { browsed += delta; }
    void loadSelected (int deck) override { loaded = deck; }
    void instantDoubles (int deck) override { doubles = deck; }
    void back (bool shift) override { backShift = shift; }
    void view (bool) override {}
    void previousTrack (int deck) override { prev = deck; }
    void tapTempo (int) override { ++taps; }
    void partInstantDoubles (int deck, int mask) override { partDeck = deck; partMask = mask; }
    void padModeChanged (int, PadMode m) override { lastMode = m; }
    void beatFxSelect (FxType t) override { beatFx = t; }
};

struct Rig
{
    Engine engine;
    Host host;
    Flx10 flx { engine, host };
    std::vector<float> out[4];
    float* ptr[4];
    Rig()
    {
        engine.prepare (48000.0, 256);
        for (int k = 0; k < 4; ++k) { out[k].assign (256, 0.0f); ptr[k] = out[k].data(); }
        auto t = std::make_shared<TrackAudio>();
        t->setMix (std::vector<float> (48000 * 20 * 2, 0.1f), 48000.0);
        engine.loadDeck (0, t);
        Command g; g.type = Cmd::DeckSetGrid; g.deck = 0; g.value = 120.0; engine.post (g);
        run();
    }
    void run (int blocks = 1) { for (int i = 0; i < blocks; ++i) engine.process (nullptr, 0, ptr, 4, 256); engine.pollEvents ([] (const DeckEvent&) {}); }
    void note (int ch, int n, bool on) { flx.handle ({ (std::uint8_t) (0x90 | ch), (std::uint8_t) n, (std::uint8_t) (on ? 0x7F : 0x00) }); }
    void press (int ch, int n) { note (ch, n, true); note (ch, n, false); }
    void cc (int ch, int n, int v) { flx.handle ({ (std::uint8_t) (0xB0 | ch), (std::uint8_t) n, (std::uint8_t) v }); }
    bool sentContains (const std::vector<Message>& m, int status, int d1, int d2)
    {
        for (const auto& x : m) if (x.status == status && x.data1 == d1 && x.data2 == d2) return true;
        return false;
    }
};
}

MP_TEST (flx10_transport_cue_and_leds)
{
    Rig r;
    std::vector<Message> leds;
    r.flx.collectOutput (leds);
    CHECK (r.sentContains (leds, 0x90, 0x0B, 0x00));                // PLAY LED off
    r.press (0, 0x0B);                                               // PLAY/PAUSE
    r.run (4);
    CHECK (r.engine.deck (0).playing());
    leds.clear();
    r.flx.collectOutput (leds);
    CHECK (r.sentContains (leds, 0x90, 0x0B, 0x7F));                // PLAY LED on
    leds.clear();
    r.flx.collectOutput (leds);
    CHECK (leds.empty());                                            // only changes are sent
    r.note (0, 0x0C, true);                                          // CUE while playing: back cue + pause
    r.run();
    r.note (0, 0x0C, false);
    r.run();
    CHECK (! r.engine.deck (0).playing());
    CHECK_NEAR (r.engine.deck (0).position(), 0.0, 1e-9);
}

MP_TEST (flx10_mixer_14bit_and_jog)
{
    Rig r;
    r.cc (0, 0x13, 0x40);                                            // CH FADER MSB
    CHECK_NEAR (r.engine.mixer.ch[0].fader.load(), (64 << 7) / 16383.0, 1e-6);
    r.cc (0, 0x33, 0x7F);                                            // LSB refines
    CHECK_NEAR (r.engine.mixer.ch[0].fader.load(), ((64 << 7) | 127) / 16383.0, 1e-6);
    r.cc (6, 0x1F, 0x7F); r.cc (6, 0x3F, 0x7F);                      // crossfader fully right
    CHECK_NEAR (r.engine.mixer.crossfader.load(), 1.0, 1e-6);
    r.cc (6, 0x17, 0x00); r.cc (6, 0x37, 0x00);                      // COLOR CH1 fully CCW
    CHECK_NEAR (r.engine.mixer.ch[0].color.load(), -1.0, 1e-6);
    r.cc (0, 0x00, 0x7F); r.cc (0, 0x20, 0x7F);                      // TEMPO slider '+' end
    CHECK (r.engine.deck (0).inputs.tempoFaderMoved.load());
    CHECK_NEAR (r.engine.deck (0).inputs.tempoFader.load(), 1.0, 1e-6);
    r.cc (0, 0x21, 0x43);                                            // jog side +3
    r.cc (0, 0x23, 0x3E);                                            // platter (vinyl off) -2
    CHECK (r.engine.deck (0).inputs.bendTicks.load() == 1);
    r.note (0, 0x36, true);                                          // touch the platter (vinyl on)
    r.run();
    CHECK (r.engine.deck (0).view.scratching.load());
    r.cc (0, 0x22, 0x45);
    CHECK (r.engine.deck (0).inputs.scratchTicks.load() == 5);
    r.cc (6, 0x40, 0x7E);                                            // browse CCW 2
    r.cc (6, 0x40, 0x03);                                            // browse CW 3
    CHECK (r.host.browsed == 1);
}

MP_TEST (flx10_pads_by_mode)
{
    Rig r;
    r.press (0, 0x0B);
    r.run (2);
    // HOT CUE pad 1 (ch 8, note 0x00): sets a hot cue
    r.note (7, 0x00, true); r.note (7, 0x00, false);
    r.run();
    CHECK (r.engine.deck (0).view.hotCue[0].load() >= 0.0);
    // SHIFT + pad (ch 9) deletes it
    r.note (8, 0x00, true); r.note (8, 0x00, false);
    r.run();
    CHECK (r.engine.deck (0).view.hotCue[0].load() < 0.0);
    // BEAT LOOP (Serato Auto Loop) pad 7 = 8 beats
    r.press (0, 0x6D);
    CHECK (r.host.lastMode == PadMode::AutoLoop);
    r.note (7, 0x66, true); r.note (7, 0x66, false);
    r.run();
    CHECK (r.engine.deck (0).view.loopActive.load());
    CHECK_NEAR (r.engine.deck (0).view.loopOut.load() - r.engine.deck (0).view.loopIn.load(), 8.0 * 24000.0, 1e-6);
    // PAGE > in Auto Loop doubles the pad sizes
    r.press (0, 0x32);
    CHECK (r.flx.pads (0).autoLoopShift.load() == 1);
    // PAD FX2 (Serato Roll) pad 3 = 1/2 beat while held
    r.note (7, 0x52, true);
    r.run();
    CHECK (r.engine.deck (0).view.rollActive.load());
    r.note (7, 0x52, false);
    r.run();
    CHECK (! r.engine.deck (0).view.rollActive.load());
    // KEYBOARD (Pitch Play) "Middle" pad 5 = -4 semitones
    r.note (7, 0x44, true);
    r.run();
    CHECK (r.engine.deck (0).keyShift() == -4);
    // BEAT JUMP pad 5 = previous track (host)
    r.note (7, 0x24, true);
    CHECK (r.host.prev == 0);
    // SAMPLER pad 2 of bank A
    auto t = std::make_shared<TrackAudio>();
    t->setMix (std::vector<float> (48000 * 2, 0.2f), 48000.0);
    r.engine.loadSampler (1, t, 0.0);
    r.run();
    r.note (7, 0x31, true);
    r.run();
    CHECK (r.engine.sampler().view[1].playing.load());
}

MP_TEST (flx10_effects_section_and_stems_buttons)
{
    Rig r;
    // SOUND COLOR FX: FILTER (ch 7 note 0x05), pressing again turns it off
    r.press (6, 0x05);
    CHECK (r.engine.mixer.colorFx.load() == (int) ColorFxType::Filter);
    r.press (6, 0x05);
    CHECK (r.engine.mixer.colorFx.load() == (int) ColorFxType::None);
    // BEAT FX SELECT position 7 = FLANGER, CH SELECT = CH 2, LEVEL/DEPTH, ON
    r.note (4, 0x27, true);
    CHECK (r.host.beatFx == FxType::Flanger);
    r.note (4, 0x11, true);
    CHECK (r.engine.fxUnit (kBeatFxUnit).controls.assign[1].load());
    CHECK (! r.engine.fxUnit (kBeatFxUnit).controls.assign[(int) FxTarget::Master].load());
    r.cc (4, 0x02, 0x7F);
    CHECK_NEAR (r.engine.fxUnit (kBeatFxUnit).controls.depth.load(), 127.0 * 128.0 / 16383.0, 1e-6);
    r.press (4, 0x47);
    r.run();
    CHECK (r.engine.fxUnit (kBeatFxUnit).slotOn (0));
    // BEAT > with Flanger (1/16 .. 64): 1 -> 2 beats
    r.engine.fxUnit (kBeatFxUnit).controls.beats.store (1.0f);
    r.press (4, 0x4B);
    CHECK_NEAR (r.engine.fxUnit (kBeatFxUnit).controls.beats.load(), 2.0, 1e-9);
    // FX PART SELECT VOCAL toggles the vocal part out of the FX bus
    r.press (6, 0x1B);
    CHECK (! r.engine.fxPart[(int) Stem::Vocals].load());
    // SHIFT + ACTIVE PART INST twice on deck 3 -> Part Instant Doubles (melody + bass)
    r.press (6, 0x2E);
    CHECK (r.host.partDeck == 2 && r.host.partMask == ((1 << (int) Stem::Melody) | (1 << (int) Stem::Bass)));
    // CH CUE / SHIFT + CH CUE (Part ISO) / crossfader assign
    r.press (0, 0x54);
    CHECK (r.engine.mixer.ch[0].cue.load());
    r.press (0, 0x39);
    CHECK (r.engine.mixer.ch[0].partIso.load());
    r.note (0, 0x16, true);
    CHECK (r.engine.mixer.ch[0].xfAssign.load() == (int) XfAssign::A);
    // rotary pressed twice quickly on deck 2 = Instant Doubles
    r.press (6, 0x47);
    CHECK (r.host.loaded == 1);
    r.press (6, 0x47);
    CHECK (r.host.doubles == 1);
}

MP_TEST (flx10_jog_display_messages)
{
    Rig r;
    std::vector<Message> out;
    r.run (2);
    r.flx.collectOutput (out);
    // BPM 120.0 -> 1200 = 0x09 / 0x30 on channel 16, CC 0x14 / 0x34 for deck 1
    CHECK (r.sentContains (out, 0xBF, 0x14, 1200 >> 7));
    CHECK (r.sentContains (out, 0xBF, 0x34, 1200 & 0x7F));
    // key change amount 0 = 0x0D on the deck channel
    CHECK (r.sentContains (out, 0xB0, 0x4A, 0x0D));
}
