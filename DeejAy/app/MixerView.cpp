#include "MixerView.h"

namespace app
{

// ----------------------------------------------------------------------------- channel strip
void ChannelStrip::bind (juce::Slider& s, std::atomic<float>& target, float scale, float offset)
{
    s.onValueChange = [&s, &target, scale, offset] { target.store ((float) s.getValue() * scale + offset); };
}

void ChannelStrip::sync (juce::Slider& s, const std::atomic<float>& source, float scale, float offset)
{
    if (s.isMouseButtonDown()) return;
    const double v = ((double) source.load() - offset) / scale;
    if (std::abs (v - s.getValue()) > 1.0e-4) s.setValue (v, juce::dontSendNotification);
}

ChannelStrip::ChannelStrip (AppModel& model, int channel) : model_ (model), ch_ (channel)
{
    auto& c = model_.engine.mixer.ch[(std::size_t) ch_];
    const auto dc = Theme::deck (ch_);
    for (auto* k : { &trim_, &hi_, &mid_, &low_, &color_ }) { k->setArcColour (dc); addAndMakeVisible (k); }
    trim_.setDoubleClickReturnValue (true, 0.5);
    trim_.setValue (0.5, juce::dontSendNotification);
    bind (trim_, c.trim);
    bind (hi_, c.eq[0]);
    bind (mid_, c.eq[1]);
    bind (low_, c.eq[2]);
    bind (color_, c.color, 2.0f, -1.0f);
    fader_.setSliderStyle (juce::Slider::LinearVertical);
    fader_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    fader_.setRange (0.0, 1.0);
    fader_.setColour (juce::Slider::thumbColourId, dc);
    fader_.setColour (juce::Slider::trackColourId, Theme::panelHi);
    bind (fader_, c.fader);
    addAndMakeVisible (fader_);
    addAndMakeVisible (meter_);
    styleToggle (cue_, Theme::hot);
    cue_.onClick = [&c] { c.cue.store (! c.cue.load()); };
    styleToggle (iso_, Theme::good);
    iso_.setTooltip ("Part ISO: the EQ knobs control Vocal / Bass+Melody / Drums (SHIFT + CH CUE on the FLX10)");
    iso_.onClick = [&c] { c.partIso.store (! c.partIso.load()); };
    for (auto* b : { &cue_, &iso_, &xfA_, &xfThru_, &xfB_ }) addAndMakeVisible (b);
    styleToggle (xfA_, Theme::accent); styleToggle (xfThru_, Theme::accent); styleToggle (xfB_, Theme::accent);
    xfA_.onClick = [&c] { c.xfAssign.store ((int) dj::XfAssign::A); };
    xfThru_.onClick = [&c] { c.xfAssign.store ((int) dj::XfAssign::Thru); };
    xfB_.onClick = [&c] { c.xfAssign.store ((int) dj::XfAssign::B); };
}

void ChannelStrip::refresh()
{
    auto& c = model_.engine.mixer.ch[(std::size_t) ch_];
    sync (trim_, c.trim);
    sync (hi_, c.eq[0]);
    sync (mid_, c.eq[1]);
    sync (low_, c.eq[2]);
    sync (color_, c.color, 2.0f, -1.0f);
    sync (fader_, c.fader);
    cue_.setToggleState (c.cue.load(), juce::dontSendNotification);
    iso_.setToggleState (c.partIso.load(), juce::dontSendNotification);
    const int a = c.xfAssign.load();
    xfA_.setToggleState (a == (int) dj::XfAssign::A, juce::dontSendNotification);
    xfThru_.setToggleState (a == (int) dj::XfAssign::Thru, juce::dontSendNotification);
    xfB_.setToggleState (a == (int) dj::XfAssign::B, juce::dontSendNotification);
    meter_.setPeak (model_.engine.mixerView.channelPeak[ch_].load());
}

void ChannelStrip::paint (juce::Graphics& g)
{
    g.setColour (Theme::deck (ch_));
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("CH " + juce::String (ch_ + 1), getLocalBounds().removeFromTop (14), juce::Justification::centred);
}

void ChannelStrip::resized()
{
    auto r = getLocalBounds().reduced (2);
    r.removeFromTop (14);
    // knobs in a 2-column grid: TRIM | HI, MID | LOW, COLOR | (ISO / CUE)
    const int kh = juce::jlimit (30, 52, r.getHeight() / 6);
    auto row = [&] { return r.removeFromTop (kh); };
    auto two = [] (juce::Rectangle<int> a, juce::Component& x, juce::Component& y)
    {
        x.setBounds (a.removeFromLeft (a.getWidth() / 2));
        y.setBounds (a);
    };
    two (row(), trim_, hi_);
    two (row(), mid_, low_);
    auto last = row();
    color_.setBounds (last.removeFromLeft (last.getWidth() / 2));
    iso_.setBounds (last.removeFromTop (last.getHeight() / 2).reduced (2, 2));
    cue_.setBounds (last.reduced (2, 2));
    auto xf = r.removeFromBottom (18);
    const int w = xf.getWidth() / 3;
    xfA_.setBounds (xf.removeFromLeft (w));
    xfThru_.setBounds (xf.removeFromLeft (w));
    xfB_.setBounds (xf);
    r.removeFromBottom (2);
    meter_.setBounds (r.removeFromRight (6).reduced (0, 2));
    fader_.setBounds (r);
}

// ------------------------------------------------------------------------------------ mixer
MixerView::MixerView (AppModel& model) : model_ (model)
{
    for (int c = 0; c < dj::kNumDecks; ++c)
    {
        strips_[(std::size_t) c] = std::make_unique<ChannelStrip> (model, c);
        addAndMakeVisible (*strips_[(std::size_t) c]);
    }
    for (int i = 0; i < dj::kNumColorFx; ++i)
    {
        auto& b = cfx_[(std::size_t) i];
        b.setButtonText (dj::colorFxName ((dj::ColorFxType) i));
        styleToggle (b, Theme::accent);
        b.onClick = [this, i]
        {
            auto& t = model_.engine.mixer.colorFx;
            t.store (t.load() == i ? (int) dj::ColorFxType::None : i);
        };
        addAndMakeVisible (b);
    }
    crossfader_.setSliderStyle (juce::Slider::LinearHorizontal);
    crossfader_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    crossfader_.setRange (0.0, 1.0);
    crossfader_.setValue (0.5, juce::dontSendNotification);
    crossfader_.setDoubleClickReturnValue (true, 0.5);
    crossfader_.onValueChange = [this] { model_.engine.mixer.crossfader.store ((float) crossfader_.getValue()); };
    addAndMakeVisible (crossfader_);
    auto& mx = model_.engine.mixer;
    master_.setValue (mx.masterLevel.load(), juce::dontSendNotification);
    master_.onValueChange = [this] { model_.engine.mixer.masterLevel.store ((float) master_.getValue()); };
    booth_.onValueChange = [this] { model_.engine.mixer.boothLevel.store ((float) booth_.getValue()); };
    phonesMix_.onValueChange = [this] { model_.engine.mixer.phonesMix.store ((float) phonesMix_.getValue()); };
    phonesLevel_.setValue (mx.phonesLevel.load(), juce::dontSendNotification);
    phonesLevel_.onValueChange = [this] { model_.engine.mixer.phonesLevel.store ((float) phonesLevel_.getValue()); };
    samplerVol_.setValue (mx.samplerVolume.load(), juce::dontSendNotification);
    samplerVol_.onValueChange = [this] { model_.engine.mixer.samplerVolume.store ((float) samplerVol_.getValue()); };
    for (auto* k : { &master_, &booth_, &phonesMix_, &phonesLevel_, &samplerVol_ }) addAndMakeVisible (k);
    styleToggle (masterCue_, Theme::hot);
    masterCue_.onClick = [this] { model_.engine.mixer.masterCue.store (! model_.engine.mixer.masterCue.load()); };
    styleToggle (samplerCue_, Theme::hot);
    samplerCue_.onClick = [this] { model_.engine.mixer.samplerCue.store (! model_.engine.mixer.samplerCue.load()); };
    styleToggle (rec_, Theme::bad);
    rec_.onClick = [this] { if (onRecordToggled) onRecordToggled(); };
    for (auto* b : { &masterCue_, &samplerCue_, &rec_ }) addAndMakeVisible (b);
    addAndMakeVisible (masterL_);
    addAndMakeVisible (masterR_);
}

void MixerView::setStrips (const std::vector<int>& order)
{
    order_ = order;
    for (int c = 0; c < dj::kNumDecks; ++c)
        strips_[(std::size_t) c]->setVisible (std::find (order_.begin(), order_.end(), c) != order_.end());
    resized();
}

void MixerView::refresh()
{
    for (auto& s : strips_) if (s->isVisible()) s->refresh();
    auto& mx = model_.engine.mixer;
    const int cfx = mx.colorFx.load();
    for (int i = 0; i < dj::kNumColorFx; ++i) cfx_[(std::size_t) i].setToggleState (cfx == i, juce::dontSendNotification);
    if (! crossfader_.isMouseButtonDown()) crossfader_.setValue (mx.crossfader.load(), juce::dontSendNotification);
    if (! phonesMix_.isMouseButtonDown()) phonesMix_.setValue (mx.phonesMix.load(), juce::dontSendNotification);
    if (! phonesLevel_.isMouseButtonDown()) phonesLevel_.setValue (mx.phonesLevel.load(), juce::dontSendNotification);
    if (! samplerVol_.isMouseButtonDown()) samplerVol_.setValue (mx.samplerVolume.load(), juce::dontSendNotification);
    masterCue_.setToggleState (mx.masterCue.load(), juce::dontSendNotification);
    samplerCue_.setToggleState (mx.samplerCue.load(), juce::dontSendNotification);
    masterL_.setPeak (model_.engine.mixerView.masterPeak[0].load());
    masterR_.setPeak (model_.engine.mixerView.masterPeak[1].load());
    const bool rec = model_.engine.recorder.recording();
    rec_.setToggleState (rec, juce::dontSendNotification);
    rec_.setButtonText (rec ? "REC " + Theme::formatTime (model_.engine.recorder.seconds()).upToFirstOccurrenceOf (".", false, false) : juce::String ("REC"));
}

void MixerView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
    g.setColour (Theme::line);
    g.drawRect (getLocalBounds());
}

void MixerView::resized()
{
    auto r = getLocalBounds().reduced (4);
    auto cfxRow = r.removeFromTop (22);
    const int bw = cfxRow.getWidth() / dj::kNumColorFx;
    for (auto& b : cfx_) b.setBounds (cfxRow.removeFromLeft (bw).reduced (1, 0));
    r.removeFromTop (4);
    crossfader_.setBounds (r.removeFromBottom (26).reduced (r.getWidth() / 6, 0));
    auto masterCol = r.removeFromRight (64);
    const int kh = juce::jlimit (30, 46, (masterCol.getHeight() - 70) / 5);
    for (auto* k : { &master_, &booth_, &phonesMix_, &phonesLevel_, &samplerVol_ }) k->setBounds (masterCol.removeFromTop (kh));
    for (auto* b : { &masterCue_, &samplerCue_, &rec_ }) b->setBounds (masterCol.removeFromTop (20).reduced (0, 1));
    auto meters = masterCol.reduced (16, 4);
    masterL_.setBounds (meters.removeFromLeft (meters.getWidth() / 2).reduced (2, 0));
    masterR_.setBounds (meters.reduced (2, 0));
    // FLX10 channel order 3-1-2-4 in four-deck mode
    const int sw = r.getWidth() / juce::jmax (1, (int) order_.size());
    for (int c : order_) strips_[(std::size_t) c]->setBounds (r.removeFromLeft (sw));
}

} // namespace app
