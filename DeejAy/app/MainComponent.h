// Main window content. Layout follows Serato's workflow: FX bar on top, stacked waveforms, virtual decks
// either side of the mixer (FLX10 sides: decks 1/3 left, 2/4 right; 5/7 and 6/8 on the second bank),
// the Performer (master-bus FX), sampler, library at the bottom, status bar.
#pragma once
#include "AppModel.h"
#include "DeckView.h"
#include "FxView.h"
#include "LibraryView.h"
#include "MixerView.h"
#include "PerformerView.h"
#include "SamplerView.h"
#include "WaveformView.h"

namespace app
{

class MainComponent : public juce::Component, public juce::DragAndDropContainer, private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    bool keyStateChanged (bool isKeyDown) override;

private:
    // Keyboard control (the KEYS button): two decks (the left and right deck of the bank on screen), the
    // Performer and the sampler pads of the current bank. See Settings > Keyboard for the map.
    enum class KeyAction { Cue, Play, Sync, Censor, Hot1, Hot2, Hot3, Hot4, JumpBack, JumpFwd, LoopIn, LoopOut, FourBeat, BendDown, BendUp,
                           Performer, SamplerPad };
    struct KeyBind
    {
        int key;
        int side;            // 0 = left deck, 1 = right deck (deck keys)
        KeyAction action;
        int index = 0;       // Performer effect / sampler pad
        bool alt = false;    // needs Option (sampler pads)
        bool held = false;
        int deck = -1;       // the deck the press went to (the bank may change while held)
        bool latched = false;
    };

    void timerCallback() override;
    void setStatus (const juce::String& s);
    void keyAction (KeyBind& k, bool down);
    void toggleRecording();
    bool textEditing() const;
    int bankBase() const;                     // first deck on screen
    int sideDeck (int side) const;            // left / right deck for keyboard and loading
    std::vector<int> shownDecks() const;
    void cycleDeckCount();
    void updateButtons();

    AppModel model_;
    juce::TooltipWindow tooltips_ { this, 600 };
    FxUnitView fx1_ { model_, 0 }, fx2_ { model_, 1 };
    BeatFxView beatFx_ { model_ };
    std::array<std::unique_ptr<WaveformView>, dj::kNumDecks> waves_;
    std::array<std::unique_ptr<DeckView>, dj::kNumDecks> decks_;
    MixerView mixer_ { model_ };
    PerformerView performer_ { model_ };
    SamplerView sampler_ { model_ };
    LibraryView library_ { model_ };
    juce::TextButton decksBtn_ { "4 DECKS" }, bankBtn_ { "DECKS 1-4" }, keysBtn_ { "KEYS" }, settings_ { "SETTINGS" };
    juce::TextButton samplerBtn_ { "SAMPLER" }, performerBtn_ { "PERFORMER" }, spliceBtn_ { "SPLICE" };
    juce::String status_, midiStatus_;
    juce::uint32 statusTime_ = 0, lastCueKey_[2] {};
    std::vector<KeyBind> keys_;
    int tick_ = 0;
};

} // namespace app
