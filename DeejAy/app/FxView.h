// FX bar: two Serato-style FX units and the DDJ-FLX10 BEAT FX unit.
#pragma once
#include "AppModel.h"
#include "Widgets.h"

namespace app
{

class FxUnitView : public juce::Component
{
public:
    FxUnitView (AppModel& model, int unit);
    void refresh();
    void resized() override;
    void paint (juce::Graphics& g) override;
private:
    void stepBeats (int dir);
    int target (int button) const;            // assign button -> FX target (channel buttons follow the deck bank)
    AppModel& model_;
    int unit_;
    juce::TextButton mode_;
    std::array<juce::ComboBox, dj::kFxSlotsPerUnit> type_;
    std::array<juce::TextButton, dj::kFxSlotsPerUnit> on_;
    std::array<Knob, dj::kFxSlotsPerUnit> level_;
    Knob depth_ { false, "DEPTH" };
    juce::TextButton less_ { "<" }, more_ { ">" }, tap_ { "TAP" };
    juce::Label beats_;
    std::array<juce::TextButton, 6> assign_;
    std::vector<double> taps_;
};

class BeatFxView : public juce::Component
{
public:
    explicit BeatFxView (AppModel& model);
    void refresh();
    void resized() override;
    void paint (juce::Graphics& g) override;
private:
    AppModel& model_;
    juce::ComboBox type_, channel_;
    juce::TextButton less_ { "BEAT <" }, more_ { "BEAT >" }, on_ { "ON" }, tap_ { "TAP" }, auto_ { "AUTO" };
    Knob depth_ { false, "LEVEL/DEPTH" };
    juce::Label info_;
    std::vector<double> taps_;
};

} // namespace app
