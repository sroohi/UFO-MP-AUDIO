// Scrolling detail waveform (Serato style: centred playhead, RGB frequency colour, beat grid, cues,
// loops, slip ghost) and the whole-track overview strip.
#pragma once
#include "AppModel.h"
#include "Theme.h"

namespace app
{

class WaveformView : public juce::Component
{
public:
    WaveformView (AppModel& model, int deck);
    void refresh();                 // 60 Hz from the main timer
    void paint (juce::Graphics& g) override;
    void resized() override { image_ = {}; }
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override;

    static double& zoomSeconds();   // visible width in seconds, shared by all decks (SHIFT + rotary zooms)

private:
    void renderImage (double posSeconds, double trackSecondsPerPixel);
    // Track seconds per pixel. The view spans zoomSeconds() of real time, so a deck playing at -3 % shows
    // 3 % less track: synced decks then have identical beat spacing on screen, as in Serato.
    double trackSecondsPerPixel() const noexcept { return zoomSeconds() * frameRate_ / juce::jmax (1, getWidth()); }
    AppModel& model_;
    int deck_;
    juce::Image image_;
    // Position and tempo snapshot taken in refresh(): every lane is drawn from the same instant, not from
    // whenever its own paint() happens to run.
    double framePos_ = 0.0, frameRate_ = 1.0;
    double lastPos_ = -1.0, lastZoom_ = 0.0, lastRate_ = 1.0;
    const dj::analysis::Waveform* lastWave_ = nullptr;
    int dragStartX_ = 0;
    bool scratching_ = false;
    double dragPos_ = 0.0;
};

class OverviewView : public juce::Component
{
public:
    OverviewView (AppModel& model, int deck);
    void refresh();
    void paint (juce::Graphics& g) override;
    void resized() override { cachedFor_ = nullptr; }
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override { mouseDown (e); }
private:
    AppModel& model_;
    int deck_;
    juce::Image cache_;
    const dj::analysis::Waveform* cachedFor_ = nullptr;
    float lastX_ = -1.0f;
};

} // namespace app
