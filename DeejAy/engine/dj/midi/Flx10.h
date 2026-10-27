// Pioneer DJ DDJ-FLX10 support with Serato DJ Pro semantics.
//
// MIDI numbers come from AlphaTheta's "DDJ-FLX10 List of MIDI Messages" (E1); behaviour follows the
// Serato column of the DDJ-FLX10 instruction manual (DRI1822-D). See docs/CONTROLLER-FLX10.md for the
// full table and the items that still need checking on hardware (pad colour palette, the jog-display
// key code table, the playing-speed encoding).
//
// Threading: handle() runs on the MIDI input thread. It writes the engine's atomic inputs directly and
// posts discrete actions through Engine::postFromMidiThread(). Anything that touches the library or the
// UI goes through ControllerHost, whose implementation must marshal to the message thread.
// collectOutput() runs on the message thread (timer) and returns LED / jog-display messages that
// changed since the last call.
#pragma once
#include "../Engine.h"
#include <array>
#include <atomic>
#include <map>
#include <chrono>
#include <vector>

namespace dj::midi
{

struct Message
{
    std::uint8_t status = 0, data1 = 0, data2 = 0;
    int channel() const noexcept { return status & 0x0F; }
    int type() const noexcept { return status & 0xF0; }
};

// Pad modes. The first nine exist on the FLX10's buttons in Serato; the rest are on-screen modes.
enum class PadMode : int
{
    HotCue, PitchPlay, SavedLoop, PadFx, Roll, BeatJump, AutoLoop, Sampler, KeyShift,
    Slicer, SlicerLoop, Stems
};
const char* padModeName (PadMode m) noexcept;

// Library / UI side of the controller. Calls arrive on the MIDI thread.
class ControllerHost
{
public:
    virtual ~ControllerHost() = default;
    virtual void browse (int delta, bool shift) = 0;           // rotary turn (SHIFT: waveform zoom)
    virtual void loadSelected (int deck) = 0;                  // rotary press with deck selected
    virtual void instantDoubles (int toDeck) = 0;              // rotary pressed twice on an idle deck
    virtual void back (bool shift) = 0;                        // BACK (SHIFT: load to Prepare)
    virtual void view (bool shift) = 0;                        // VIEW (SHIFT: cycle panels)
    virtual void previousTrack (int deck) = 0;                 // Beat Jump pad 5
    virtual void tapTempo (int deck) = 0;                      // SHIFT + QUANTIZE
    virtual void partInstantDoubles (int toDeck, int stemMask) = 0;
    virtual void padModeChanged (int deck, PadMode mode) = 0;
    virtual void beatFxSelect (FxType type) = 0;              // BEAT FX SELECT knob (creates effects)
};

struct PadState
{
    std::atomic<int> mode { (int) PadMode::HotCue };
    std::atomic<int> rollShift { 0 };          // Roll sizes x 2^shift (PAGE < >)
    std::atomic<int> autoLoopShift { 0 };      // Auto Loop sizes x 2^shift
    std::atomic<double> beatJumpBeats { 4.0 }; // Beat Jump pads 1/4
    std::atomic<int> pitchRange { 1 };         // 0 Up, 1 Middle, 2 Down
    std::atomic<int> pitchOffset { 0 };        // SHIFT + PAGE: semitone shift of the table
    std::atomic<int> heldRollPad { -1 };
};

class Flx10
{
public:
    Flx10 (Engine& engine, ControllerHost& host);

    // MIDI thread
    void handle (const Message& m) noexcept;

    // message thread: changed LED / display messages since the last call (send them to the FLX10)
    void collectOutput (std::vector<Message>& out);
    // forget what was sent (e.g. after reconnecting or a deck switch) so everything is resent; any thread
    void resetOutputCache() noexcept { resendAll_.store (true); }

    PadState& pads (int deck) noexcept { return pads_[(std::size_t) deck]; }
    void setPadMode (int deck, PadMode mode) noexcept;
    bool shiftHeld (int deck) const noexcept { return shift_[(std::size_t) deck].load(); }
    int activeDeck (int side) const noexcept { return side == 0 ? leftDeck_.load() : rightDeck_.load(); }

    // Roll / Auto Loop pad sizes in beats (pads 0..7) for a range shift.
    static double rollBeats (int pad, int shift) noexcept;
    static double autoLoopBeats (int pad, int shift) noexcept;

    // Tunables (Settings)
    std::atomic<bool> faderStart { true };
    std::atomic<bool> applyMasterLevel { false };   // the FLX10 applies MASTER/BOOTH level in hardware

private:
    void deckNote (int deck, int note, bool on) noexcept;
    void deckCc (int deck, int cc, int value) noexcept;
    void padNote (int deck, bool shift, int note, bool on) noexcept;
    void globalNote (int note, bool on) noexcept;
    void globalCc (int cc, int value) noexcept;
    void fxNote (int note, bool on) noexcept;
    void fxCc (int cc, int value) noexcept;
    float fourteenBit (int channel, int cc, int value) noexcept; // assembles MSB/LSB pairs
    void command (Cmd type, int deck, int index = 0, double value = 0.0, bool flag = false) noexcept;
    int otherPlayingDeck (int deck) const noexcept;
    void put (std::vector<Message>& out, std::uint8_t status, std::uint8_t d1, std::uint8_t d2);

    Engine& engine_;
    ControllerHost& host_;
    std::array<PadState, kNumDecks> pads_;
    std::array<std::atomic<bool>, kNumDecks> shift_ {};
    std::array<std::atomic<bool>, kNumDecks> loopInHeld_ {}, loopOutHeld_ {}, beatJumpHeld_ {};
    std::atomic<int> leftDeck_ { 0 }, rightDeck_ { 1 };
    std::array<std::array<std::uint8_t, 128>, 16> msb_ {};
    std::array<double, kNumDecks> lastBrowsePress_ {};   // seconds, for the double-press Instant Doubles
    std::atomic<int> beatFxPosition_ { 1 };
    std::vector<double> taps_;
    std::map<int, std::uint8_t> sent_;       // message thread only
    std::atomic<bool> resendAll_ { true };
};

} // namespace dj::midi
