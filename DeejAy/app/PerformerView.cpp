#include "PerformerView.h"

namespace app
{

namespace
{
    juce::Colour fxColour (int fx)
    {
        switch ((dj::PerformerFx) fx)
        {
            case dj::PerformerFx::Stutter: case dj::PerformerFx::NoteRepeat: case dj::PerformerFx::TapeStop: return Theme::hot;
            case dj::PerformerFx::Bitcrusher: return Theme::warn;
            case dj::PerformerFx::Delay: case dj::PerformerFx::Reverb: return Theme::good;
            case dj::PerformerFx::Pitch: case dj::PerformerFx::Filter: case dj::PerformerFx::Talkbox: break;
        }
        return Theme::accent;
    }

    constexpr juce::uint32 kMomentaryMs = 300;   // held longer than this: the button acts as momentary
}

// ------------------------------------------------------------------------------- tile
PerformerTile::PerformerTile (AppModel& model, int fx)
    : model_ (model), fx_ (fx),
      button_ (dj::performerFxName ((dj::PerformerFx) fx)),
      x_ (fx == (int) dj::PerformerFx::Pitch || fx == (int) dj::PerformerFx::Filter, dj::performerParamName ((dj::PerformerFx) fx, 0)),
      y_ (false, dj::performerParamName ((dj::PerformerFx) fx, 1))
{
    auto& c = model_.engine.performer.controls;
    button_.setColour (fxColour (fx));
    button_.onPress = [this, &c] (bool)
    {
        if (onFocus) onFocus (fx_);
        wasOn_ = c.on[fx_].load();
        pressedAt_ = juce::Time::getMillisecondCounter();
        c.on[fx_].store (! wasOn_);
    };
    button_.onRelease = [this, &c]
    {
        if (! wasOn_ && juce::Time::getMillisecondCounter() - pressedAt_ > kMomentaryMs) c.on[fx_].store (false);
    };
    addAndMakeVisible (button_);
    for (auto* k : { &x_, &y_ })
    {
        k->setArcColour (fxColour (fx));
        addAndMakeVisible (k);
    }
    x_.setValue (c.x[fx].load(), juce::dontSendNotification);
    y_.setValue (c.y[fx].load(), juce::dontSendNotification);
    x_.onValueChange = [this, &c] { c.x[fx_].store ((float) x_.getValue()); if (onFocus) onFocus (fx_); };
    y_.onValueChange = [this, &c] { c.y[fx_].store ((float) y_.getValue()); if (onFocus) onFocus (fx_); };
    x_.setDoubleClickReturnValue (true, c.x[fx].load());
    y_.setDoubleClickReturnValue (true, c.y[fx].load());
}

void PerformerTile::refresh (bool focused)
{
    const auto& c = model_.engine.performer.controls;
    const bool on = c.on[fx_].load();
    const bool sounding = model_.engine.performer.view.sounding[fx_].load();
    button_.setLit (on || sounding);
    button_.setColour (on ? fxColour (fx_) : fxColour (fx_).withAlpha (0.45f));   // a ringing tail shows dimmer
    if (! x_.isMouseButtonDown()) x_.setValue (c.x[fx_].load(), juce::dontSendNotification);
    if (! y_.isMouseButtonDown()) y_.setValue (c.y[fx_].load(), juce::dontSendNotification);
    const auto xt = juce::String (dj::performerParamText ((dj::PerformerFx) fx_, 0, c.x[fx_].load()));
    const auto yt = juce::String (dj::performerParamText ((dj::PerformerFx) fx_, 1, c.y[fx_].load()));
    if (xt != xText_ || yt != yText_ || focused != focused_)
    {
        xText_ = xt; yText_ = yt; focused_ = focused;
        repaint();
    }
}

void PerformerTile::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (focused_ ? Theme::panelHi.brighter (0.08f) : Theme::panelHi.withAlpha (0.5f));
    g.fillRoundedRectangle (r, 4.0f);
    if (focused_)
    {
        g.setColour (fxColour (fx_).withAlpha (0.8f));
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
    }
    auto values = getLocalBounds().reduced (4, 2).removeFromBottom (12);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.drawText (xText_, values.removeFromLeft (values.getWidth() / 2), juce::Justification::centred);
    g.drawText (yText_, values, juce::Justification::centred);
}

void PerformerTile::paintOverChildren (juce::Graphics& g)
{
    if (keyHint_.isEmpty()) return;
    // the computer-keyboard key, in the button's corner
    g.setColour (button_.lit() ? Theme::bg.withAlpha (0.7f) : Theme::dim);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.drawText (keyHint_, button_.getBounds().reduced (6, 2), juce::Justification::centredRight);
}

void PerformerTile::resized()
{
    auto r = getLocalBounds().reduced (3);
    button_.setBounds (r.removeFromTop (juce::jmax (22, r.getHeight() * 30 / 100)));
    r.removeFromBottom (12);
    x_.setBounds (r.removeFromLeft (r.getWidth() / 2));
    y_.setBounds (r);
}

// ----------------------------------------------------------------------------- XY pad
void XYPad::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (Theme::bg);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (Theme::line);
    for (int i = 1; i < 4; ++i)
    {
        g.drawVerticalLine ((int) (r.getX() + r.getWidth() * (float) i / 4.0f), r.getY(), r.getBottom());
        g.drawHorizontalLine ((int) (r.getY() + r.getHeight() * (float) i / 4.0f), r.getX(), r.getRight());
    }
    const auto& c = model_.engine.performer.controls;
    const float x = c.x[fx_].load(), y = c.y[fx_].load();
    const auto col = fxColour (fx_);
    const juce::Point<float> p (r.getX() + x * r.getWidth(), r.getBottom() - y * r.getHeight());
    g.setColour (col.withAlpha (c.on[fx_].load() ? 0.9f : 0.4f));
    g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (p));
    g.setColour (Theme::dim);
    g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
    auto label = r.reduced (4.0f);
    g.drawText (juce::String (dj::performerFxName ((dj::PerformerFx) fx_)) + "  X " + dj::performerParamName ((dj::PerformerFx) fx_, 0)
                    + "  Y " + dj::performerParamName ((dj::PerformerFx) fx_, 1),
                label.removeFromTop (12.0f), juce::Justification::centredLeft);
}

void XYPad::set (juce::Point<float> p)
{
    auto& c = model_.engine.performer.controls;
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    c.x[fx_].store (juce::jlimit (0.0f, 1.0f, (p.x - r.getX()) / r.getWidth()));
    c.y[fx_].store (juce::jlimit (0.0f, 1.0f, (r.getBottom() - p.y) / r.getHeight()));
    repaint();
}

void XYPad::mouseDown (const juce::MouseEvent& e)
{
    auto& c = model_.engine.performer.controls;
    set (e.position);
    switchedOn_ = ! c.on[fx_].load();
    c.on[fx_].store (true);
}

void XYPad::mouseDrag (const juce::MouseEvent& e) { set (e.position); }

void XYPad::mouseUp (const juce::MouseEvent&)
{
    // touched to play it: lifting the finger stops it (an effect that was already on stays on)
    if (switchedOn_) model_.engine.performer.controls.on[fx_].store (false);
    switchedOn_ = false;
    repaint();
}

// ------------------------------------------------------------------------------ panel
PerformerView::PerformerView (AppModel& model) : model_ (model), xy_ (model)
{
    styleToggle (enable_, Theme::hot);
    enable_.setTooltip ("Performer on / bypass. Delay and reverb tails ring out when it is bypassed.");
    enable_.onClick = [this] { auto& e = model_.engine.performer.controls.enabled; e.store (! e.load()); };
    addAndMakeVisible (enable_);
    for (int i = 0; i < dj::kNumPerformerFx; ++i)
    {
        tiles_[(std::size_t) i] = std::make_unique<PerformerTile> (model, i);
        tiles_[(std::size_t) i]->onFocus = [this] (int fx) { focus_ = fx; xy_.setEffect (fx); };
        addAndMakeVisible (*tiles_[(std::size_t) i]);
    }
    addAndMakeVisible (xy_);
}

void PerformerView::setKeyHints (const std::array<juce::String, dj::kNumPerformerFx>& hints)
{
    for (int i = 0; i < dj::kNumPerformerFx; ++i) tiles_[(std::size_t) i]->setKeyHint (hints[(std::size_t) i]);
}

void PerformerView::refresh()
{
    const bool on = model_.engine.performer.controls.enabled.load();
    enable_.setToggleState (on, juce::dontSendNotification);
    enable_.setButtonText (on ? "ON" : "BYPASS");
    for (int i = 0; i < dj::kNumPerformerFx; ++i) tiles_[(std::size_t) i]->refresh (i == focus_);
    xy_.refresh();
}

void PerformerView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
    auto head = getLocalBounds().reduced (6, 3).removeFromLeft (84);
    g.setColour (Theme::hot);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("PERFORMER", head.removeFromTop (14), juce::Justification::topLeft);
    g.setColour (Theme::dim);
    g.setFont (juce::FontOptions (9.0f));
    g.drawFittedText ("whole mix\nclick: latch\nhold: momentary", head.removeFromTop (40), juce::Justification::topLeft, 3);
}

void PerformerView::resized()
{
    auto r = getLocalBounds().reduced (4, 3);
    auto head = r.removeFromLeft (84);
    enable_.setBounds (head.removeFromBottom (22).reduced (2, 0));
    xy_.setBounds (r.removeFromRight (juce::jmin (r.getHeight() + 40, 150)).reduced (2, 0));
    const int w = r.getWidth() / dj::kNumPerformerFx;
    for (auto& t : tiles_) t->setBounds (r.removeFromLeft (w));
}

} // namespace app
