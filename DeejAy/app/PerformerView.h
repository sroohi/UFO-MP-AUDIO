// Performer panel: the master-bus performance FX (stutter, note repeat, tape stop, pitch, filter, talkbox,
// bitcrusher, delay, reverb). Everything the decks and the sampler play goes through it.
//
// A tile's button latches on a click and acts as a momentary switch when held (release turns it off).
// The XY pad drives the two parameters of the last touched effect and plays it while touched.
#pragma once
#include "AppModel.h"
#include "Widgets.h"

namespace app
{

class PerformerTile : public juce::Component
{
public:
    PerformerTile (AppModel& model, int fx);
    std::function<void (int fx)> onFocus;
    void refresh (bool focused);
    void setKeyHint (const juce::String& k) { keyHint_ = k; }
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void resized() override;

private:
    AppModel& model_;
    int fx_;
    PadButton button_;
    Knob x_, y_;
    juce::uint32 pressedAt_ = 0;
    bool wasOn_ = false, focused_ = false;
    juce::String keyHint_, xText_, yText_;
};

class XYPad : public juce::Component
{
public:
    explicit XYPad (AppModel& model) : model_ (model) {}
    void setEffect (int fx) { if (fx != fx_) { fx_ = fx; repaint(); } }
    void refresh() { repaint(); }
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    void set (juce::Point<float> p);
    AppModel& model_;
    int fx_ = 0;
    bool switchedOn_ = false;
};

class PerformerView : public juce::Component
{
public:
    explicit PerformerView (AppModel& model);
    void refresh();
    void setKeyHints (const std::array<juce::String, dj::kNumPerformerFx>& hints);
    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    AppModel& model_;
    juce::TextButton enable_ { "ON" };
    std::array<std::unique_ptr<PerformerTile>, dj::kNumPerformerFx> tiles_;
    XYPad xy_;
    int focus_ = 0;
};

} // namespace app
