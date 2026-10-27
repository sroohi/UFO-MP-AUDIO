// Main window content. Layout follows Serato's workflow: FX bar on top, stacked waveforms, virtual decks
// either side of the mixer (FLX10 sides: decks 1/3 left, 2/4 right), sampler, library at the bottom.
#pragma once
#include "AppModel.h"
#include "DeckView.h"
#include "FxView.h"
#include "LibraryView.h"
#include "MixerView.h"
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
    enum class KeyAction { Cue, Play, Sync, Censor, Hot1, Hot2, Hot3, Hot4, JumpBack, JumpFwd, LoopIn, LoopOut, FourBeat, BendDown, BendUp };
    struct KeyBind { int key; int deck; KeyAction action; bool held = false; };

    void timerCallback() override;
    void setStatus (const juce::String& s);
    void keyAction (const KeyBind& k, bool down);
    void toggleRecording();
    bool textEditing() const;

    AppModel model_;
    juce::TooltipWindow tooltips_ { this, 600 };
    FxUnitView fx1_ { model_, 0 }, fx2_ { model_, 1 };
    BeatFxView beatFx_ { model_ };
    std::array<std::unique_ptr<WaveformView>, dj::kNumDecks> waves_;
    std::array<std::unique_ptr<DeckView>, dj::kNumDecks> decks_;
    MixerView mixer_ { model_ };
    SamplerView sampler_ { model_ };
    LibraryView library_ { model_ };
    juce::TextButton settings_ { "SETTINGS" }, layout_ { "4 DECKS" }, stemsAll_ { "STEMS" };
    juce::String status_, midiStatus_;
    juce::uint32 statusTime_ = 0;
    std::vector<KeyBind> keys_;
    int tick_ = 0;
    bool showSampler_ = true;
};

} // namespace app
