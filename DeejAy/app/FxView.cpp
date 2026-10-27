#include "FxView.h"

namespace app
{

namespace
{
    // FX unit effect list (everything except the FLX10-only tone generators)
    const dj::FxType kUnitFx[] {
        dj::FxType::Echo, dj::FxType::PingPong, dj::FxType::TapeEcho, dj::FxType::LowCutEcho, dj::FxType::MultiTapDelay, dj::FxType::Spiral,
        dj::FxType::Reverb, dj::FxType::Flanger, dj::FxType::Phaser, dj::FxType::EnigmaJet, dj::FxType::HighPass, dj::FxType::LowPass,
        dj::FxType::FilterLfo, dj::FxType::Bitcrusher, dj::FxType::Distortion, dj::FxType::Gate, dj::FxType::Roll, dj::FxType::ReverseRoll,
        dj::FxType::SlipRoll, dj::FxType::Braker, dj::FxType::Stretch, dj::FxType::PitchShift, dj::FxType::Noise };

    juce::String beatsText (double b)
    {
        const double a = std::abs (b);
        juce::String s = a >= 1.0 ? juce::String ((int) std::lround (a)) : a > 0.7 ? juce::String ("3/4") : "1/" + juce::String ((int) std::lround (1.0 / a));
        return (b < 0 ? "-" : "") + s;
    }

    double tapBpm (std::vector<double>& taps)
    {
        const double t = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (! taps.empty() && t - taps.back() > 2.0) taps.clear();
        taps.push_back (t);
        if (taps.size() > 8) taps.erase (taps.begin());
        return taps.size() >= 3 ? 60.0 * (double) (taps.size() - 1) / (taps.back() - taps.front()) : 0.0;
    }
}

// ---------------------------------------------------------------------------- Serato FX unit
FxUnitView::FxUnitView (AppModel& model, int unit) : model_ (model), unit_ (unit)
{
    auto& u = model_.engine.fxUnit (unit_).controls;
    mode_.onClick = [&u] { u.multi.store (! u.multi.load()); };
    styleToggle (mode_, Theme::accent);
    addAndMakeVisible (mode_);
    for (int s = 0; s < dj::kFxSlotsPerUnit; ++s)
    {
        auto& c = type_[(std::size_t) s];
        c.addItem ("-", 1);
        for (auto t : kUnitFx) c.addItem (dj::fxName (t), (int) t + 1);
        c.onChange = [this, s, &c] { model_.engine.setFxType (unit_, s, (dj::FxType) juce::jmax (0, c.getSelectedId() - 1)); };
        addAndMakeVisible (c);
        auto& b = on_[(std::size_t) s];
        b.setButtonText ("FX" + juce::String (s + 1));
        styleToggle (b, Theme::hot);
        b.onClick = [this, s] { dj::Command cmd; cmd.type = dj::Cmd::FxSlotToggle; cmd.deck = unit_; cmd.index = s; model_.engine.post (cmd); };
        addAndMakeVisible (b);
        auto& k = level_[(std::size_t) s];
        k.setValue (0.8, juce::dontSendNotification);
        k.setArcColour (Theme::hot);
        k.onValueChange = [s, &u, &k]
        {
            // Multi: the slot's level; Single: parameter s of the one effect
            if (u.multi.load()) u.slotLevel[s].store ((float) k.getValue());
            else u.params[s].store ((float) k.getValue());
        };
        addAndMakeVisible (k);
    }
    depth_.setValue (u.depth.load(), juce::dontSendNotification);
    depth_.setArcColour (Theme::hot);
    depth_.onValueChange = [this, &u] { u.depth.store ((float) depth_.getValue()); };
    addAndMakeVisible (depth_);
    less_.onClick = [this] { stepBeats (-1); };
    more_.onClick = [this] { stepBeats (1); };
    tap_.onClick = [this, &u] { const double bpm = tapBpm (taps_); if (bpm > 0) { u.tapBpm.store (bpm); u.tapMode.store (true); } };
    tap_.setTooltip ("Tap the FX tempo (click AUTO in the BPM label to return to the deck tempo)");
    for (auto* b : { &less_, &more_, &tap_ }) { styleToggle (*b, Theme::accent); addAndMakeVisible (b); }
    beats_.setJustificationType (juce::Justification::centred);
    beats_.setColour (juce::Label::textColourId, Theme::text);
    addAndMakeVisible (beats_);
    static const char* names[6] { "1", "2", "3", "4", "M", "S" };
    for (int t = 0; t < 6; ++t)
    {
        auto& b = assign_[(std::size_t) t];
        b.setButtonText (names[t]);
        styleToggle (b, Theme::deck (t < 4 ? t : 0).withSaturation (t < 4 ? 0.7f : 0.0f).withBrightness (t < 4 ? 0.95f : 0.85f));
        b.onClick = [&u, t] { u.assign[t].store (! u.assign[t].load()); };
        b.setTooltip (t < 4 ? "Assign to channel " + juce::String (t + 1) : t == 4 ? juce::String ("Assign to master") : juce::String ("Assign to sampler"));
        addAndMakeVisible (b);
    }
}

void FxUnitView::stepBeats (int dir)
{
    static constexpr double steps[] { 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 3.0 / 4, 1, 2, 4, 8, 16 };
    auto& b = model_.engine.fxUnit (unit_).controls.beats;
    int idx = 0;
    for (int i = 0; i < 10; ++i) if (std::abs (steps[i] - b.load()) < std::abs (steps[idx] - b.load())) idx = i;
    b.store ((float) steps[juce::jlimit (0, 9, idx + dir)]);
}

void FxUnitView::refresh()
{
    auto& unit = model_.engine.fxUnit (unit_);
    auto& u = unit.controls;
    const bool multi = u.multi.load();
    mode_.setButtonText (multi ? "MULTI" : "SINGLE");
    for (int s = 0; s < dj::kFxSlotsPerUnit; ++s)
    {
        const int type = unit.view.type[s].load();
        if (type_[(std::size_t) s].getSelectedId() != type + 1) type_[(std::size_t) s].setSelectedId (type + 1, juce::dontSendNotification);
        on_[(std::size_t) s].setToggleState (unit.view.on[s].load(), juce::dontSendNotification);
        // Single FX: slots 2 and 3 are the effect's parameters
        type_[(std::size_t) s].setVisible (multi || s == 0);
        on_[(std::size_t) s].setVisible (multi || s == 0);
        level_[(std::size_t) s].setTooltip (multi ? juce::String ("Level") : juce::String (dj::fxParamName ((dj::FxType) unit.view.type[0].load(), s)));
    }
    const double bpm = u.tapMode.load() ? u.tapBpm.load() : unit.view.bpm.load();
    beats_.setText (beatsText (u.beats.load()) + (u.tapMode.load() ? "  TAP " + juce::String (bpm, 1) : juce::String()), juce::dontSendNotification);
    for (int t = 0; t < 6; ++t) assign_[(std::size_t) t].setToggleState (u.assign[t].load(), juce::dontSendNotification);
}

void FxUnitView::paint (juce::Graphics& g)
{
    g.setColour (Theme::panelHi.withAlpha (0.4f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    g.setColour (Theme::hot);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("FX " + juce::String (unit_ + 1), getLocalBounds().reduced (4, 2).removeFromTop (14), juce::Justification::topLeft);
}

void FxUnitView::resized()
{
    auto r = getLocalBounds().reduced (4);
    auto top = r.removeFromTop (16);
    top.removeFromLeft (34);
    mode_.setBounds (top.removeFromLeft (56));
    top.removeFromLeft (4);
    for (int t = 0; t < 6; ++t) assign_[(std::size_t) t].setBounds (top.removeFromLeft (20).reduced (1, 0));
    top.removeFromLeft (4);
    less_.setBounds (top.removeFromLeft (18));
    beats_.setBounds (top.removeFromLeft (juce::jmax (40, top.getWidth() - 60)));
    more_.setBounds (top.removeFromLeft (18));
    tap_.setBounds (top);
    r.removeFromTop (2);
    depth_.setBounds (r.removeFromRight (44));
    const int sw = r.getWidth() / dj::kFxSlotsPerUnit;
    for (int s = 0; s < dj::kFxSlotsPerUnit; ++s)
    {
        auto c = r.removeFromLeft (sw).reduced (2, 0);
        level_[(std::size_t) s].setBounds (c.removeFromRight (36));
        type_[(std::size_t) s].setBounds (c.removeFromTop (c.getHeight() / 2).reduced (0, 1));
        on_[(std::size_t) s].setBounds (c.reduced (0, 1));
    }
}

// ----------------------------------------------------------------------------- FLX10 BEAT FX
BeatFxView::BeatFxView (AppModel& model) : model_ (model)
{
    auto& u = model_.engine.fxUnit (dj::kBeatFxUnit).controls;
    for (int i = 0; i < dj::kFlx10BeatFxCount; ++i) type_.addItem (juce::String (dj::fxName (dj::flx10BeatFx (i))).toUpperCase(), i + 1);
    type_.setSelectedId (2, juce::dontSendNotification);
    type_.onChange = [this] { model_.engine.setFxType (dj::kBeatFxUnit, 0, dj::flx10BeatFx (type_.getSelectedId() - 1)); };
    addAndMakeVisible (type_);
    static const char* chans[7] { "CH1", "CH2", "CH3", "CH4", "MST", "SP", "-" };
    for (int i = 0; i < 6; ++i) channel_.addItem (chans[i], i + 1);
    channel_.setSelectedId (5, juce::dontSendNotification);
    channel_.onChange = [this, &u]
    {
        static constexpr int target[6] { 0, 1, 2, 3, (int) dj::FxTarget::Master, (int) dj::FxTarget::Sampler };
        for (int t = 0; t < dj::kFxTargets; ++t) u.assign[t].store (t == target[channel_.getSelectedId() - 1]);
    };
    addAndMakeVisible (channel_);
    auto step = [this, &u] (int dir)
    {
        const auto type = (dj::FxType) model_.engine.fxUnit (dj::kBeatFxUnit).view.type[0].load();
        const auto kind = dj::fxBeatKind (type);
        if (kind == dj::BeatKind::Size) { u.params[0].store (juce::jlimit (0.01f, 1.0f, u.params[0].load() + 0.1f * (float) dir)); return; }
        std::vector<double> list { 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 3.0 / 4, 1, 2, 4, 8, 16 };
        if (kind != dj::BeatKind::Time16) { list.push_back (32); list.push_back (64); }
        if (kind == dj::BeatKind::Signed64)
        {
            std::vector<double> s;
            for (auto it = list.rbegin(); it != list.rend(); ++it) s.push_back (-*it);
            s.insert (s.end(), list.begin(), list.end());
            list = s;
        }
        int idx = 0;
        for (int i = 0; i < (int) list.size(); ++i) if (std::abs (list[(std::size_t) i] - u.beats.load()) < std::abs (list[(std::size_t) idx] - u.beats.load())) idx = i;
        u.beats.store ((float) list[(std::size_t) juce::jlimit (0, (int) list.size() - 1, idx + dir)]);
    };
    less_.onClick = [step] { step (-1); };
    more_.onClick = [step] { step (1); };
    on_.onClick = [this] { dj::Command c; c.type = dj::Cmd::FxSlotToggle; c.deck = dj::kBeatFxUnit; c.index = 0; model_.engine.post (c); };
    tap_.onClick = [this, &u] { const double bpm = tapBpm (taps_); if (bpm > 0) { u.tapBpm.store (bpm); u.tapMode.store (true); } };
    auto_.onClick = [&u] { u.tapMode.store (false); };
    for (auto* b : { &less_, &more_, &on_, &tap_, &auto_ }) { styleToggle (*b, b == &on_ ? Theme::hot : Theme::accent); addAndMakeVisible (b); }
    depth_.setValue (u.depth.load(), juce::dontSendNotification);
    depth_.setArcColour (Theme::hot);
    depth_.onValueChange = [this, &u] { u.depth.store ((float) depth_.getValue()); };
    addAndMakeVisible (depth_);
    info_.setJustificationType (juce::Justification::centred);
    info_.setColour (juce::Label::textColourId, Theme::text);
    addAndMakeVisible (info_);
}

void BeatFxView::refresh()
{
    auto& unit = model_.engine.fxUnit (dj::kBeatFxUnit);
    auto& u = unit.controls;
    on_.setToggleState (unit.view.on[0].load(), juce::dontSendNotification);
    tap_.setToggleState (u.tapMode.load(), juce::dontSendNotification);
    auto_.setToggleState (! u.tapMode.load(), juce::dontSendNotification);
    if (! depth_.isMouseButtonDown()) depth_.setValue (u.depth.load(), juce::dontSendNotification);
    const auto type = (dj::FxType) unit.view.type[0].load();
    for (int i = 0; i < dj::kFlx10BeatFxCount; ++i)
        if (dj::flx10BeatFx (i) == type && type_.getSelectedId() != i + 1) type_.setSelectedId (i + 1, juce::dontSendNotification);
    static constexpr int target[6] { 0, 1, 2, 3, (int) dj::FxTarget::Master, (int) dj::FxTarget::Sampler };
    for (int i = 0; i < 6; ++i) if (u.assign[target[i]].load() && channel_.getSelectedId() != i + 1) channel_.setSelectedId (i + 1, juce::dontSendNotification);
    const auto kind = dj::fxBeatKind (type);
    const juce::String amount = kind == dj::BeatKind::Size ? juce::String ((int) std::lround (u.params[0].load() * 100.0f)) + "%" : beatsText (u.beats.load());
    const double bpm = u.tapMode.load() ? u.tapBpm.load() : unit.view.bpm.load();
    info_.setText (amount + "   " + (u.tapMode.load() ? "TAP " : "AUTO ") + juce::String (bpm, 1), juce::dontSendNotification);
}

void BeatFxView::paint (juce::Graphics& g)
{
    g.setColour (Theme::panelHi.withAlpha (0.4f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    g.setColour (Theme::accent);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("BEAT FX", getLocalBounds().reduced (4, 2).removeFromTop (14), juce::Justification::topLeft);
}

void BeatFxView::resized()
{
    auto r = getLocalBounds().reduced (4);
    auto top = r.removeFromTop (16);
    top.removeFromLeft (52);
    channel_.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (4);
    auto_.setBounds (top.removeFromLeft (40));
    tap_.setBounds (top.removeFromLeft (36));
    info_.setBounds (top);
    r.removeFromTop (2);
    depth_.setBounds (r.removeFromRight (56));
    on_.setBounds (r.removeFromRight (40).reduced (2, 2));
    type_.setBounds (r.removeFromTop (r.getHeight() / 2).reduced (2, 1));
    less_.setBounds (r.removeFromLeft (r.getWidth() / 2).reduced (2, 1));
    more_.setBounds (r.reduced (2, 1));
}

} // namespace app
