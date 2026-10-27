#include "WaveformView.h"

namespace app
{

namespace
{
    // Max peak and mean colour of the bins covering [t0, t1) seconds.
    struct Column { float peak = 0.0f; juce::Colour colour; };
    Column column (const dj::analysis::Waveform& w, double t0, double t1)
    {
        Column c;
        const auto n = (std::int64_t) w.size();
        auto b0 = (std::int64_t) std::floor (t0 * w.binsPerSecond);
        auto b1 = (std::int64_t) std::floor (t1 * w.binsPerSecond);
        if (b1 < 0 || b0 >= n) return c;
        b0 = juce::jlimit<std::int64_t> (0, n - 1, b0);
        b1 = juce::jlimit<std::int64_t> (b0, n - 1, b1);
        int lo = 0, mi = 0, hi = 0, pk = 0, cnt = 0;
        for (auto b = b0; b <= b1; ++b)
        {
            pk = juce::jmax (pk, (int) w.peak[(std::size_t) b]);
            lo += w.low[(std::size_t) b]; mi += w.mid[(std::size_t) b]; hi += w.high[(std::size_t) b];
            ++cnt;
        }
        c.peak = (float) pk / 255.0f;
        c.colour = Theme::waveform ((std::uint8_t) (lo / cnt), (std::uint8_t) (mi / cnt), (std::uint8_t) (hi / cnt));
        return c;
    }
}

double& WaveformView::zoomSeconds()
{
    static double z = 8.0;
    return z;
}

WaveformView::WaveformView (AppModel& model, int deck) : model_ (model), deck_ (deck)
{
    setOpaque (true);
}

void WaveformView::refresh()
{
    const auto& v = model_.engine.deck (deck_).view;
    const double sr = v.trackSampleRate.load();
    framePos_ = v.position.load();
    frameRate_ = juce::jlimit (0.1, 3.0, 1.0 + v.tempo.load());
    const double pos = sr > 0 ? framePos_ / sr : 0.0;
    const auto* wave = model_.decks[(std::size_t) deck_].detail.get();
    if (std::abs (pos - lastPos_) > 1.0e-4 || wave != lastWave_ || std::abs (zoomSeconds() - lastZoom_) > 1.0e-9
        || std::abs (frameRate_ - lastRate_) > 1.0e-9)
        repaint();
}

void WaveformView::renderImage (double posSeconds, double spp)
{
    const int w = getWidth(), h = getHeight();
    if (w <= 0 || h <= 0) return;
    if (! image_.isValid() || image_.getWidth() != w || image_.getHeight() != h)
        image_ = juce::Image (juce::Image::RGB, w, h, false);
    image_.clear (image_.getBounds(), Theme::bg);
    juce::Image::BitmapData px (image_, juce::Image::BitmapData::writeOnly);
    const auto* wave = model_.decks[(std::size_t) deck_].detail.get();
    if (wave == nullptr || wave->size() == 0) return;
    const float mid = (float) h * 0.5f;
    for (int x = 0; x < w; ++x)
    {
        const double t = posSeconds + (x - w / 2) * spp;
        const auto c = column (*wave, t, t + spp);
        if (c.peak <= 0.0f) continue;
        const int half = (int) (c.peak * (mid - 1.0f));
        const auto col = x < w / 2 ? c.colour.withMultipliedBrightness (0.65f) : c.colour; // played part is dimmer
        for (int y = (int) mid - half; y <= (int) mid + half; ++y) px.setPixelColour (x, y, col);
    }
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto& v = model_.engine.deck (deck_).view;
    const auto& dt = model_.decks[(std::size_t) deck_];
    const double sr = v.trackSampleRate.load();
    const double posFrames = framePos_;
    const double pos = sr > 0 ? posFrames / sr : 0.0;
    lastPos_ = pos;
    lastWave_ = dt.detail.get();
    lastZoom_ = zoomSeconds();
    lastRate_ = frameRate_;
    const double spp = trackSecondsPerPixel();
    renderImage (pos, spp);
    if (image_.isValid()) g.drawImageAt (image_, 0, 0);
    else g.fillAll (Theme::bg);

    const int w = getWidth(), h = getHeight();
    auto xOf = [&] (double frames) { return (float) (w / 2 + (frames / sr - pos) / spp); };

    if (sr > 0 && v.loaded.load())
    {
        // loop / roll regions
        auto region = [&] (double in, double out, juce::Colour c)
        {
            if (in < 0 || out <= in) return;
            const float x0 = xOf (in), x1 = xOf (out);
            if (x1 < 0 || x0 > (float) w) return;
            g.setColour (c.withAlpha (0.22f));
            g.fillRect (juce::Rectangle<float> (x0, 0, x1 - x0, (float) h));
            g.setColour (c);
            g.drawVerticalLine ((int) x0, 0, (float) h);
            g.drawVerticalLine ((int) x1, 0, (float) h);
        };
        if (v.loopActive.load()) region (v.loopIn.load(), v.loopOut.load(), Theme::good);
        if (v.rollActive.load()) region (v.rollIn.load(), v.rollOut.load(), Theme::warn);

        // beat grid: bars bright, beats dim
        const double bpm = v.trackBpm.load();
        if (bpm > 0.0)
        {
            const double fpb = 60.0 * sr / bpm, anchor = v.gridAnchor.load();
            const double firstBeat = std::floor ((posFrames - spp * 0.5 * w * sr - anchor) / fpb);
            const auto length = (double) v.length.load();
            for (double b = firstBeat; ; b += 1.0)
            {
                const double f = anchor + b * fpb;
                const float x = xOf (f);
                if (x > (float) w || f > length) break;
                if (x < 0 || f < 0.0) continue;
                const bool bar = ((long long) std::llround (b) % 4 + 4) % 4 == 0;
                g.setColour (bar ? juce::Colours::white.withAlpha (0.55f) : juce::Colours::white.withAlpha (0.16f));
                g.drawVerticalLine ((int) x, bar ? 0.0f : (float) h * 0.82f, (float) h);
                if (bar) g.drawVerticalLine ((int) x, 0.0f, (float) h * 0.18f);
            }
        }

        // cue point and hot cues
        const float cx = xOf (v.cuePoint.load());
        g.setColour (Theme::hot);
        g.drawVerticalLine ((int) cx, 0, (float) h);
        const auto* track = model_.library.find (dt.trackId);
        for (int i = 0; i < dj::kNumHotCues; ++i)
        {
            const double hc = v.hotCue[i].load();
            if (hc < 0) continue;
            const float x = xOf (hc);
            if (x < -20.0f || x > (float) (w + 20)) continue;
            const auto col = Theme::hotCue (i, track != nullptr ? track->hotCues[(std::size_t) i].color : 0);
            g.setColour (col);
            g.drawVerticalLine ((int) x, 0, (float) h);
            g.fillRect (juce::Rectangle<float> (x, 0, 14, 12));
            g.setColour (Theme::bg);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText (juce::String (i + 1), juce::Rectangle<float> (x, 0, 14, 12), juce::Justification::centred);
        }

        // slip: where playback will resume
        const double slip = v.slipPosition.load();
        if (slip >= 0.0)
        {
            g.setColour (Theme::accent.withAlpha (0.8f));
            const float sx = xOf (slip);
            for (int y = 0; y < h; y += 6) g.drawVerticalLine ((int) sx, (float) y, (float) y + 3.0f);
        }
    }

    // playhead and deck badge
    g.setColour (juce::Colours::white);
    g.fillRect (w / 2 - 1, 0, 2, h);
    g.setColour (Theme::deck (deck_));
    g.fillRect (0, 0, 4, h);
    if (dt.loading)
    {
        g.setColour (Theme::dim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Loading / analysing " + dt.title + " ...", getLocalBounds().reduced (10, 0), juce::Justification::centredLeft);
    }
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    dragStartX_ = e.x;
    dragPos_ = model_.engine.deck (deck_).view.position.load();
    // while playing, dragging the waveform scratches (Serato behaviour); while paused it seeks
    scratching_ = model_.engine.deck (deck_).view.playing.load();
    if (scratching_) model_.engine.deckCommand (dj::Cmd::JogTouch, deck_, 0, 0.0, true);
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    const auto& v = model_.engine.deck (deck_).view;
    const double sr = v.trackSampleRate.load();
    const double spp = trackSecondsPerPixel();
    const int dx = e.x - dragStartX_;
    if (scratching_)
    {
        // dragging left moves the record forward: convert pixels to jog ticks (one turn = 1.8 s)
        const int ticksPerRev = model_.engine.deck (deck_).inputs.ticksPerRevolution.load();
        const double seconds = -dx * spp;
        model_.engine.deck (deck_).inputs.scratchTicks.fetch_add ((int) std::lround (seconds / 1.8 * ticksPerRev));
        dragStartX_ = e.x;
        return;
    }
    model_.engine.deckCommand (dj::Cmd::SetPosition, deck_, 0, juce::jmax (0.0, dragPos_ - dx * spp * sr));
}

void WaveformView::mouseUp (const juce::MouseEvent&)
{
    if (scratching_) model_.engine.deckCommand (dj::Cmd::JogTouch, deck_, 0, 0.0, false);
    scratching_ = false;
}

void WaveformView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    zoomSeconds() = juce::jlimit (1.0, 40.0, zoomSeconds() * (w.deltaY > 0 ? 0.85 : 1.0 / 0.85));
}

// ---------------------------------------------------------------------------- overview
OverviewView::OverviewView (AppModel& model, int deck) : model_ (model), deck_ (deck) {}

void OverviewView::refresh()
{
    const auto& v = model_.engine.deck (deck_).view;
    const auto len = (double) v.length.load();
    const float x = len > 0 ? (float) (v.position.load() / len) * (float) getWidth() : 0.0f;
    if (std::abs (x - lastX_) >= 0.5f || model_.decks[(std::size_t) deck_].overview.get() != cachedFor_) repaint();
}

void OverviewView::paint (juce::Graphics& g)
{
    const int w = getWidth(), h = getHeight();
    g.fillAll (Theme::panel);
    const auto* wave = model_.decks[(std::size_t) deck_].overview.get();
    if (wave != cachedFor_ || ! cache_.isValid() || cache_.getWidth() != w)
    {
        cachedFor_ = wave;
        cache_ = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        if (wave != nullptr && wave->size() > 0)
        {
            juce::Graphics ig (cache_);
            const double total = (double) wave->size();
            for (int x = 0; x < w; ++x)
            {
                const auto b0 = (std::size_t) ((double) x / w * total), b1 = (std::size_t) juce::jmax ((double) b0, (double) (x + 1) / w * total - 1.0);
                int pk = 0, lo = 0, mi = 0, hi = 0, n = 0;
                for (auto b = b0; b <= b1 && b < wave->size(); ++b)
                { pk = juce::jmax (pk, (int) wave->peak[b]); lo += wave->low[b]; mi += wave->mid[b]; hi += wave->high[b]; ++n; }
                if (n == 0) continue;
                const float ph = (float) pk / 255.0f * (float) h * 0.5f;
                ig.setColour (Theme::waveform ((std::uint8_t) (lo / n), (std::uint8_t) (mi / n), (std::uint8_t) (hi / n)));
                ig.drawVerticalLine (x, (float) h * 0.5f - ph, (float) h * 0.5f + ph);
            }
        }
    }
    g.drawImageAt (cache_, 0, 0);
    const auto& v = model_.engine.deck (deck_).view;
    const double len = (double) v.length.load();
    if (len <= 0) return;
    const float x = (float) (v.position.load() / len) * (float) w;
    lastX_ = x;
    g.setColour (Theme::bg.withAlpha (0.45f));
    g.fillRect (juce::Rectangle<float> (0, 0, x, (float) h));                  // played part
    // last 30 s warning (Serato flashes the overview)
    const double remaining = (len - v.position.load()) / juce::jmax (1.0, v.trackSampleRate.load());
    if (v.playing.load() && remaining < 30.0 && (juce::Time::getMillisecondCounter() / 400) % 2 == 0)
    {
        g.setColour (Theme::bad.withAlpha (0.25f));
        g.fillRect (getLocalBounds());
    }
    const auto* track = model_.library.find (model_.decks[(std::size_t) deck_].trackId);
    for (int i = 0; i < dj::kNumHotCues; ++i)
    {
        const double hc = v.hotCue[i].load();
        if (hc < 0) continue;
        g.setColour (Theme::hotCue (i, track != nullptr ? track->hotCues[(std::size_t) i].color : 0));
        g.fillRect ((float) (hc / len) * (float) w - 1.0f, 0.0f, 2.0f, (float) h);
    }
    if (v.loopActive.load())
    {
        g.setColour (Theme::good.withAlpha (0.4f));
        const float x0 = (float) (v.loopIn.load() / len) * (float) w, x1 = (float) (v.loopOut.load() / len) * (float) w;
        g.fillRect (juce::Rectangle<float> (x0, 0, juce::jmax (2.0f, x1 - x0), (float) h));
    }
    g.setColour (juce::Colours::white);
    g.fillRect (x - 1.0f, 0.0f, 2.0f, (float) h);
}

void OverviewView::mouseDown (const juce::MouseEvent& e)
{
    // needle search: click / drag in the overview to jump (paused decks, or always with SHIFT)
    const auto& v = model_.engine.deck (deck_).view;
    if (v.playing.load() && ! e.mods.isShiftDown()) return;
    const double len = (double) v.length.load();
    model_.engine.deckCommand (dj::Cmd::SetPosition, deck_, 0, juce::jlimit (0.0, len, (double) e.x / juce::jmax (1, getWidth()) * len));
}

} // namespace app
