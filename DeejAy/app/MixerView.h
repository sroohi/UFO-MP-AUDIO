// On-screen 4-channel mixer, bound both ways to the engine's mixer atomics (the FLX10 moves them too).
#pragma once
#include "AppModel.h"
#include "Widgets.h"

namespace app
{

class ChannelStrip : public juce::Component
{
public:
    ChannelStrip (AppModel& model, int channel);
    void refresh();
    void resized() override;
    void paint (juce::Graphics& g) override;
private:
    static void bind (juce::Slider& s, std::atomic<float>& target, float scale = 1.0f, float offset = 0.0f);
    static void sync (juce::Slider& s, const std::atomic<float>& source, float scale = 1.0f, float offset = 0.0f);
    AppModel& model_;
    int ch_;
    Knob trim_ { false, "TRIM" }, hi_ { true, "HI" }, mid_ { true, "MID" }, low_ { true, "LOW" }, color_ { true, "COLOR" };
    juce::TextButton cue_ { "CUE" }, iso_ { "ISO" }, xfA_ { "A" }, xfThru_ { "THRU" }, xfB_ { "B" };
    juce::Slider fader_;
    Meter meter_;
};

class MixerView : public juce::Component
{
public:
    explicit MixerView (AppModel& model);
    void refresh();
    // the channel strips shown, left to right (FLX10 order: 3 1 2 4, or 7 5 6 8 on the second bank)
    void setStrips (const std::vector<int>& order);
    void resized() override;
    void paint (juce::Graphics& g) override;
    std::function<void()> onRecordToggled;
private:
    AppModel& model_;
    std::vector<int> order_ { 2, 0, 1, 3 };
    std::array<std::unique_ptr<ChannelStrip>, dj::kNumDecks> strips_;
    std::array<juce::TextButton, dj::kNumColorFx> cfx_;
    juce::Slider crossfader_;
    Knob master_ { false, "MASTER" }, booth_ { false, "BOOTH" }, phonesMix_ { true, "CUE/MST" }, phonesLevel_ { false, "PHONES" }, samplerVol_ { false, "SAMPLER" };
    juce::TextButton masterCue_ { "MST CUE" }, samplerCue_ { "SMP CUE" }, rec_ { "REC" };
    Meter masterL_, masterR_;
    juce::String recText_;
};

} // namespace app
