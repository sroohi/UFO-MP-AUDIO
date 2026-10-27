// Virtual deck: track info, overview, platter, transport, loops, tempo, key, stems and the 8 performance
// pads in every Serato pad mode (the FLX10 shares the pad mode with this view).
#pragma once
#include "AppModel.h"
#include "MidiHost.h"
#include "Widgets.h"
#include "WaveformView.h"

namespace app
{

class Platter : public juce::Component
{
public:
    Platter (AppModel& model, int deck) : model_ (model), deck_ (deck) {}
    void refresh() { repaint(); }
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
private:
    float angleAt (juce::Point<float> p) const;
    AppModel& model_;
    int deck_;
    float lastAngle_ = 0.0f;
};

class DeckView : public juce::Component, public juce::DragAndDropTarget, public juce::FileDragAndDropTarget
{
public:
    DeckView (AppModel& model, int deck);
    void refresh();                       // 30 Hz
    void trackChanged();
    void padModeChanged();
    void paint (juce::Graphics& g) override;
    void resized() override;

    // library rows are dragged in as "track:<id>"
    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString().startsWith ("track:"); }
    void itemDropped (const SourceDetails& d) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override;

    // pad dispatch (also used by the keyboard)
    void padDown (int pad, bool shift);
    void padUp (int pad);

private:
    void cmd (dj::Cmd type, int index = 0, double value = 0.0, bool flag = false) { model_.engine.deckCommand (type, deck_, index, value, flag); }
    dj::midi::PadMode mode() const;
    void setMode (dj::midi::PadMode m);
    void addToggle (juce::TextButton& b, const juce::String& text, juce::Colour on, std::function<void()> click);
    void loadWhenImported (std::string path, int attempts);

    AppModel& model_;
    int deck_;
    OverviewView overview_;
    Platter platter_;
    PadButton cue_ { "CUE" }, play_ { "PLAY" }, censor_ { "CENSOR" };
    juce::TextButton sync_, keylock_, slip_, vinyl_, reverse_, quantize_, tempoRange_, tempoReset_;
    juce::TextButton loopIn_, loopOut_, reloop_, halve_, dbl_, autoLoop_, autoLess_, autoMore_, jumpBack_, jumpFwd_, jumpLess_, jumpMore_;
    juce::TextButton keyDown_, keyUp_, keySync_, stems_[dj::kNumStems], stemsAnalyse_;
    juce::Slider tempo_;
    juce::ComboBox padMode_;
    std::array<PadButton, 8> pads_;
    double autoLoopBeats_ = 4.0, jumpBeats_ = 4.0;
    juce::String bpmText_, keyText_, timeText_, tempoText_;
};

} // namespace app
