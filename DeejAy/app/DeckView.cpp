#include "DeckView.h"

namespace app
{

using dj::midi::PadMode;

namespace
{
    juce::String beatsLabel (double b)
    {
        if (b >= 1.0) return juce::String ((int) std::lround (b));
        return "1/" + juce::String ((int) std::lround (1.0 / b));
    }

    const char* kPadFxNames[8] { "ECHO 1/2", "ECHO 3/4", "ROLL 1/4", "ROLL 1/8", "TRANS 1/4", "HPF", "BRAKE", "REVERB" };
    const char* kStemNames[4] { "VOCAL", "MELODY", "BASS", "DRUMS" };
    const PadMode kModes[] { PadMode::HotCue, PadMode::SavedLoop, PadMode::Roll, PadMode::AutoLoop, PadMode::Slicer, PadMode::SlicerLoop,
                             PadMode::Sampler, PadMode::PitchPlay, PadMode::KeyShift, PadMode::BeatJump, PadMode::PadFx, PadMode::Stems };
}

// ------------------------------------------------------------------------------ platter
float Platter::angleAt (juce::Point<float> p) const
{
    const auto c = getLocalBounds().toFloat().getCentre();
    return std::atan2 (p.y - c.y, p.x - c.x);
}

void Platter::paint (juce::Graphics& g)
{
    const auto& v = model_.engine.deck (deck_).view;
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    const float size = juce::jmin (r.getWidth(), r.getHeight());
    r = r.withSizeKeepingCentre (size, size);
    g.setColour (Theme::panelHi);
    g.fillEllipse (r);
    // track progress ring
    const double len = (double) v.length.load();
    const float progress = len > 0 ? (float) (v.position.load() / len) : 0.0f;
    juce::Path ring;
    ring.addCentredArc (r.getCentreX(), r.getCentreY(), size * 0.46f, size * 0.46f, 0, 0, juce::MathConstants<float>::twoPi * progress, true);
    g.setColour (Theme::deck (deck_));
    g.strokePath (ring, juce::PathStrokeType (3.0f));
    // rotating marker: 33 1/3 RPM
    const double sr = v.trackSampleRate.load();
    const double sec = sr > 0 ? v.position.load() / sr : 0.0;
    const float a = (float) std::fmod (sec / 1.8, 1.0) * juce::MathConstants<float>::twoPi - juce::MathConstants<float>::halfPi;
    const auto c = r.getCentre();
    g.setColour (v.scratching.load() ? Theme::hot : Theme::text);
    g.drawLine (juce::Line<float> (c.getPointOnCircumference (size * 0.12f, a + juce::MathConstants<float>::halfPi),
                                   c.getPointOnCircumference (size * 0.40f, a + juce::MathConstants<float>::halfPi)), 3.0f);
    g.setColour (Theme::bg);
    g.fillEllipse (r.withSizeKeepingCentre (size * 0.18f, size * 0.18f));
    g.setColour (v.vinyl.load() ? Theme::accent : Theme::dim);
    g.setFont (juce::FontOptions (juce::jmax (8.0f, size * 0.09f), juce::Font::bold));
    g.drawText (juce::String (deck_ + 1), r.withSizeKeepingCentre (size * 0.18f, size * 0.18f), juce::Justification::centred);
}

void Platter::mouseDown (const juce::MouseEvent& e)
{
    lastAngle_ = angleAt (e.position);
    model_.engine.deckCommand (dj::Cmd::JogTouch, deck_, 0, 0.0, true);
}

void Platter::mouseDrag (const juce::MouseEvent& e)
{
    const float a = angleAt (e.position);
    float d = a - lastAngle_;
    if (d > juce::MathConstants<float>::pi) d -= juce::MathConstants<float>::twoPi;
    if (d < -juce::MathConstants<float>::pi) d += juce::MathConstants<float>::twoPi;
    lastAngle_ = a;
    auto& in = model_.engine.deck (deck_).inputs;
    const int ticks = (int) std::lround (d / juce::MathConstants<float>::twoPi * (float) in.ticksPerRevolution.load());
    if (model_.engine.deck (deck_).view.vinyl.load()) in.scratchTicks.fetch_add (ticks);
    else in.bendTicks.fetch_add (ticks);
}

void Platter::mouseUp (const juce::MouseEvent&)
{
    model_.engine.deckCommand (dj::Cmd::JogTouch, deck_, 0, 0.0, false);
}

// ------------------------------------------------------------------------------ deck view
DeckView::DeckView (AppModel& model, int deck) : model_ (model), deck_ (deck), overview_ (model, deck), platter_ (model, deck)
{
    addAndMakeVisible (overview_);
    addAndMakeVisible (platter_);
    for (auto* p : { &cue_, &play_, &censor_ }) addAndMakeVisible (p);
    cue_.setColour (Theme::hot);
    play_.setColour (Theme::good);
    censor_.setColour (Theme::warn);
    cue_.onPress = [this] (bool shift) { cmd (shift ? dj::Cmd::JumpToStart : dj::Cmd::CueDown); };
    cue_.onRelease = [this] { cmd (dj::Cmd::CueUp); };
    play_.onPress = [this] (bool shift) { cmd (shift ? dj::Cmd::Stutter : dj::Cmd::PlayPause); };
    censor_.onPress = [this] (bool shift) { cmd (shift ? dj::Cmd::ReverseToggle : dj::Cmd::CensorDown); };
    censor_.onRelease = [this] { cmd (dj::Cmd::CensorUp); };

    const auto dc = Theme::deck (deck);
    addToggle (sync_, "SYNC", dc, [this] { cmd (dj::Cmd::SyncToggle); });
    addToggle (keylock_, "KEY LOCK", Theme::accent, [this] { cmd (dj::Cmd::KeylockToggle); });
    addToggle (slip_, "SLIP", Theme::accent, [this] { cmd (dj::Cmd::SlipToggle); });
    addToggle (vinyl_, "VINYL", Theme::accent, [this] { cmd (dj::Cmd::VinylToggle); });
    addToggle (reverse_, "REV", Theme::warn, [this] { cmd (dj::Cmd::ReverseToggle); });
    addToggle (quantize_, "Q", Theme::accent, [this] { cmd (dj::Cmd::QuantizeToggle); });
    addToggle (tempoRange_, "8%", Theme::panelHi, [this] { cmd (dj::Cmd::TempoRangeCycle); });
    addToggle (tempoReset_, "0", Theme::good, [this] { cmd (dj::Cmd::TempoReset); });
    addToggle (loopIn_, "IN", Theme::good, [this] { cmd (dj::Cmd::LoopIn); });
    addToggle (loopOut_, "OUT", Theme::good, [this] { cmd (dj::Cmd::LoopOut); });
    addToggle (reloop_, "RELOOP", Theme::good, [this] { cmd (dj::Cmd::Reloop); });
    addToggle (halve_, "1/2", Theme::good, [this] { cmd (dj::Cmd::LoopHalve); });
    addToggle (dbl_, "x2", Theme::good, [this] { cmd (dj::Cmd::LoopDouble); });
    addToggle (autoLess_, "<", Theme::good, [this] { autoLoopBeats_ = juce::jmax (1.0 / 32.0, autoLoopBeats_ * 0.5); });
    addToggle (autoMore_, ">", Theme::good, [this] { autoLoopBeats_ = juce::jmin (32.0, autoLoopBeats_ * 2.0); });
    addToggle (autoLoop_, "LOOP 4", Theme::good, [this] { cmd (dj::Cmd::AutoLoop, 0, autoLoopBeats_); });
    addToggle (jumpBack_, "<<", Theme::accent, [this] { cmd (dj::Cmd::BeatJump, 0, -jumpBeats_); });
    addToggle (jumpFwd_, ">>", Theme::accent, [this] { cmd (dj::Cmd::BeatJump, 0, jumpBeats_); });
    addToggle (jumpLess_, "-", Theme::accent, [this] { jumpBeats_ = juce::jmax (1.0 / 32.0, jumpBeats_ * 0.5); });
    addToggle (jumpMore_, "+", Theme::accent, [this] { jumpBeats_ = juce::jmin (256.0, jumpBeats_ * 2.0); });
    addToggle (keyDown_, "KEY-", Theme::accent, [this] { cmd (dj::Cmd::KeyShiftDelta, -1); });
    addToggle (keyUp_, "KEY+", Theme::accent, [this] { cmd (dj::Cmd::KeyShiftDelta, 1); });
    addToggle (keySync_, "KEY SYNC", Theme::accent, [this] { if (juce::ModifierKeys::currentModifiers.isShiftDown()) cmd (dj::Cmd::KeyReset); else cmd (dj::Cmd::KeySync); });
    for (int s = 0; s < dj::kNumStems; ++s)
        addToggle (stems_[s], juce::String (kStemNames[s]).substring (0, 1), Theme::good, [this, s]
        {
            if (juce::ModifierKeys::currentModifiers.isShiftDown()) cmd (dj::Cmd::StemEcho, 1 << s);
            else cmd (dj::Cmd::StemMuteToggle, 1 << s);
        });
    addToggle (stemsAnalyse_, "STEMS", Theme::warn, [this] { model_.separateStems (deck_); });
    stemsAnalyse_.setTooltip ("Separate vocals / melody / bass / drums for this track");

    tempo_.setSliderStyle (juce::Slider::LinearVertical);
    tempo_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    tempo_.setRange (0.0, 1.0);
    tempo_.setValue (0.5, juce::dontSendNotification);
    tempo_.setDoubleClickReturnValue (true, 0.5);
    // Pioneer layout: '-' at the top, '+' at the bottom
    tempo_.setColour (juce::Slider::trackColourId, Theme::panelHi);
    tempo_.setColour (juce::Slider::thumbColourId, dc);
    tempo_.onValueChange = [this]
    {
        auto& in = model_.engine.deck (deck_).inputs;
        in.tempoFader.store ((float) (1.0 - tempo_.getValue()));
        in.tempoFaderMoved.store (true);
    };
    addAndMakeVisible (tempo_);

    for (auto m : kModes) padMode_.addItem (dj::midi::padModeName (m), (int) m + 1);
    padMode_.onChange = [this] { setMode ((PadMode) (padMode_.getSelectedId() - 1)); };
    addAndMakeVisible (padMode_);
    for (int i = 0; i < 8; ++i)
    {
        auto& p = pads_[(std::size_t) i];
        p.onPress = [this, i] (bool shift) { padDown (i, shift); };
        p.onRelease = [this, i] { padUp (i); };
        addAndMakeVisible (p);
    }
    padModeChanged();
    trackChanged();
}

void DeckView::addToggle (juce::TextButton& b, const juce::String& text, juce::Colour on, std::function<void()> click)
{
    b.setButtonText (text);
    styleToggle (b, on);
    b.onClick = std::move (click);
    addAndMakeVisible (b);
}

PadMode DeckView::mode() const
{
    return (PadMode) model_.midi->flx10().pads (deck_).mode.load();
}

void DeckView::setMode (PadMode m)
{
    const auto old = mode();
    if (old == m) return;
    // the slicer is a deck state: entering / leaving the pad mode switches it
    if (m == PadMode::Slicer || m == PadMode::SlicerLoop)
        cmd (dj::Cmd::SlicerSet, m == PadMode::Slicer ? (int) dj::SlicerMode::Slicer : (int) dj::SlicerMode::SlicerLoop, 8.0);
    else if (old == PadMode::Slicer || old == PadMode::SlicerLoop)
        cmd (dj::Cmd::SlicerSet, (int) dj::SlicerMode::Off, 8.0);
    model_.midi->flx10().setPadMode (deck_, m);
    padModeChanged();
}

void DeckView::padModeChanged()
{
    padMode_.setSelectedId ((int) mode() + 1, juce::dontSendNotification);
}

void DeckView::padDown (int pad, bool shift)
{
    auto& ps = model_.midi->flx10().pads (deck_);
    switch (mode())
    {
        case PadMode::HotCue: cmd (dj::Cmd::HotCueDown, pad, 0.0, shift); break;
        case PadMode::SavedLoop: cmd (dj::Cmd::LoopSlotPad, pad, 0.0, shift); break;
        case PadMode::PitchPlay:
            if (shift) cmd (dj::Cmd::PitchPlayCue, pad);
            else cmd (dj::Cmd::PitchPlay, dj::pitchPlaySemitones ((dj::PitchPlayRange) ps.pitchRange.load(), pad, ps.pitchOffset.load()));
            break;
        case PadMode::KeyShift:
            cmd (dj::Cmd::KeyShiftSet, shift ? 0 : dj::pitchPlaySemitones ((dj::PitchPlayRange) ps.pitchRange.load(), pad, ps.pitchOffset.load()));
            break;
        case PadMode::PadFx: cmd (dj::Cmd::PadFxDown, pad); break;
        case PadMode::Roll: cmd (dj::Cmd::RollDown, 0, dj::midi::Flx10::rollBeats (pad, ps.rollShift.load())); break;
        case PadMode::AutoLoop: cmd (dj::Cmd::AutoLoop, 0, dj::midi::Flx10::autoLoopBeats (pad, ps.autoLoopShift.load())); break;
        case PadMode::BeatJump:
        {
            const double size = ps.beatJumpBeats.load();
            switch (pad)
            {
                case 0: cmd (dj::Cmd::BeatJump, 0, -size); break;
                case 1: ps.beatJumpBeats.store (juce::jmax (1.0 / 32.0, size * 0.5)); break;
                case 2: ps.beatJumpBeats.store (juce::jmin (256.0, size * 2.0)); break;
                case 3: cmd (dj::Cmd::BeatJump, 0, size); break;
                case 4: model_.loadPreviousTrack (deck_); break;
                case 5: cmd (dj::Cmd::SearchDown, -1); break;
                case 6: cmd (dj::Cmd::SearchDown, 1); break;
                default: cmd (dj::Cmd::CensorDown); break;
            }
            break;
        }
        case PadMode::Sampler:
        {
            dj::Command c;
            c.deck = model_.engine.samplerBank.load() * dj::kSamplerSlotsPerBank + pad;
            c.type = shift ? dj::Cmd::SamplerStop : dj::Cmd::SamplerPadDown;
            model_.engine.post (c);
            break;
        }
        case PadMode::Slicer: case PadMode::SlicerLoop: cmd (dj::Cmd::SliceDown, pad); break;
        case PadMode::Stems:
            if (pad < 4) cmd (dj::Cmd::StemMuteToggle, 1 << pad);
            else cmd (dj::Cmd::StemEcho, 1 << (pad - 4));
            break;
    }
}

void DeckView::padUp (int pad)
{
    switch (mode())
    {
        case PadMode::HotCue: cmd (dj::Cmd::HotCueUp, pad); break;
        case PadMode::PadFx: cmd (dj::Cmd::PadFxUp, pad); break;
        case PadMode::Roll: cmd (dj::Cmd::RollUp); break;
        case PadMode::BeatJump:
            if (pad == 5 || pad == 6) cmd (dj::Cmd::SearchUp);
            else if (pad == 7) cmd (dj::Cmd::CensorUp);
            break;
        case PadMode::Sampler:
        {
            dj::Command c;
            c.deck = model_.engine.samplerBank.load() * dj::kSamplerSlotsPerBank + pad;
            c.type = dj::Cmd::SamplerPadUp;
            model_.engine.post (c);
            break;
        }
        case PadMode::Slicer: case PadMode::SlicerLoop: cmd (dj::Cmd::SliceUp, pad); break;
        case PadMode::SavedLoop: case PadMode::PitchPlay: case PadMode::KeyShift: case PadMode::AutoLoop: case PadMode::Stems: break;
    }
}

void DeckView::trackChanged()
{
    repaint();
}

void DeckView::refresh()
{
    const auto& v = model_.engine.deck (deck_).view;
    const auto& dt = model_.decks[(std::size_t) deck_];
    sync_.setToggleState (v.sync.load(), juce::dontSendNotification);
    sync_.setButtonText (v.leader.load() ? "MST" : "SYNC");
    keylock_.setToggleState (v.keylock.load(), juce::dontSendNotification);
    slip_.setToggleState (v.slip.load(), juce::dontSendNotification);
    vinyl_.setToggleState (v.vinyl.load(), juce::dontSendNotification);
    reverse_.setToggleState (v.reverse.load(), juce::dontSendNotification);
    quantize_.setToggleState (model_.engine.view.quantize.load(), juce::dontSendNotification);
    tempoRange_.setButtonText (juce::String ((int) std::lround (dj::kTempoRanges[v.tempoRangeIndex.load()] * 100.0)) + "%");
    const bool loop = v.loopActive.load();
    for (auto* b : { &loopIn_, &loopOut_, &reloop_ }) b->setToggleState (loop, juce::dontSendNotification);
    autoLoop_.setButtonText ("LOOP " + beatsLabel (autoLoopBeats_));
    jumpBack_.setButtonText ("<< " + beatsLabel (jumpBeats_));
    jumpFwd_.setButtonText (beatsLabel (jumpBeats_) + " >>");
    play_.setLit (v.playing.load());
    cue_.setLit (v.loaded.load() && ! v.playing.load() && std::abs (v.position.load() - v.cuePoint.load()) < 2.0);
    censor_.setLit (v.censor.load());
    keySync_.setToggleState (v.keyShift.load() != 0, juce::dontSendNotification);
    const bool stems = v.stemsAvailable.load();
    for (int s = 0; s < dj::kNumStems; ++s)
    {
        stems_[s].setEnabled (stems);
        stems_[s].setToggleState (stems && ! v.stemMuted[s].load(), juce::dontSendNotification);
    }
    stemsAnalyse_.setButtonText (dt.stemsBusy ? "STEMS..." : stems ? "STEMS OK" : "STEMS");
    stemsAnalyse_.setToggleState (dt.stemsBusy, juce::dontSendNotification);
    stemsAnalyse_.setEnabled (! dt.stemsBusy && ! stems && v.loaded.load());

    // keep the tempo fader in step when the engine changed tempo (sync, reset) and the user is not on it
    if (! tempo_.isMouseButtonDown())
    {
        const double range = dj::kTempoRanges[v.tempoRangeIndex.load()];
        tempo_.setValue (1.0 - juce::jlimit (0.0, 1.0, v.tempo.load() / (2.0 * range) + 0.5), juce::dontSendNotification);
    }

    // pads
    auto& ps = model_.midi->flx10().pads (deck_);
    const auto* track = model_.library.find (dt.trackId);
    for (int i = 0; i < 8; ++i)
    {
        auto& p = pads_[(std::size_t) i];
        juce::String label;
        juce::Colour colour = Theme::deck (deck_);
        bool lit = true;
        switch (mode())
        {
            case PadMode::HotCue:
                lit = v.hotCue[i].load() >= 0.0;
                colour = Theme::hotCue (i, track != nullptr ? track->hotCues[(std::size_t) i].color : 0);
                label = track != nullptr && track->hotCues[(std::size_t) i].label.size() > 0 ? juce::String (track->hotCues[(std::size_t) i].label) : juce::String (i + 1);
                break;
            case PadMode::SavedLoop:
                lit = v.loopSlotOut[i].load() > 0.0;
                colour = Theme::good;
                label = "LOOP " + juce::String (i + 1) + (v.loopSlotLocked[i].load() ? " *" : "");
                break;
            case PadMode::PitchPlay: case PadMode::KeyShift:
            {
                const int st = dj::pitchPlaySemitones ((dj::PitchPlayRange) ps.pitchRange.load(), i, ps.pitchOffset.load());
                label = (st > 0 ? "+" : "") + juce::String (st);
                colour = Theme::accent;
                lit = mode() == PadMode::PitchPlay || st == v.keyShift.load();
                break;
            }
            case PadMode::PadFx: label = kPadFxNames[i]; colour = v.padFxActive[i].load() ? Theme::text : Theme::hot; break;
            case PadMode::Roll: label = beatsLabel (dj::midi::Flx10::rollBeats (i, ps.rollShift.load())); colour = Theme::warn; break;
            case PadMode::AutoLoop:
            {
                const double b = dj::midi::Flx10::autoLoopBeats (i, ps.autoLoopShift.load());
                label = beatsLabel (b);
                colour = Theme::good;
                const double fpb = v.trackBpm.load() > 0 ? 60.0 * v.trackSampleRate.load() / v.trackBpm.load() : 0.0;
                lit = loop && fpb > 0 && std::abs (v.loopOut.load() - v.loopIn.load() - b * fpb) < 1.0;
                break;
            }
            case PadMode::BeatJump:
            {
                static const char* names[8] { "<", "1/2", "x2", ">", "PREV", "<<", ">>", "CENSOR" };
                label = names[i];
                if (i == 0 || i == 3) label = (i == 0 ? "< " : "") + beatsLabel (ps.beatJumpBeats.load()) + (i == 3 ? " >" : "");
                colour = Theme::accent;
                break;
            }
            case PadMode::Sampler:
            {
                const int slot = model_.engine.samplerBank.load() * dj::kSamplerSlotsPerBank + i;
                label = model_.samplerName (slot).isNotEmpty() ? model_.samplerName (slot) : juce::String::charToString ((juce::juce_wchar) ('A' + model_.engine.samplerBank.load())) + juce::String (i + 1);
                lit = model_.engine.sampler().view[(std::size_t) slot].loaded.load();
                colour = model_.engine.sampler().view[(std::size_t) slot].playing.load() ? Theme::good : Theme::accent;
                break;
            }
            case PadMode::Slicer: case PadMode::SlicerLoop:
                label = "S" + juce::String (i + 1);
                colour = Theme::warn;
                lit = v.activeSlice.load() == i;
                break;
            case PadMode::Stems:
                label = i < 4 ? juce::String (kStemNames[i]) : juce::String (kStemNames[i - 4]).substring (0, 1) + " ECHO";
                colour = Theme::good;
                lit = stems && (i < 4 ? ! v.stemMuted[i].load() : v.stemEcho[i - 4].load());
                break;
        }
        p.setLabel (label);
        p.setColour (colour);
        p.setLit (lit);
    }

    // header text
    const double sr = v.trackSampleRate.load();
    const double pos = sr > 0 ? v.position.load() / sr : 0.0, len = sr > 0 ? (double) v.length.load() / sr : 0.0;
    const auto bpm = v.bpm.load();
    const auto newBpm = bpm > 0 ? juce::String (bpm, 1) : juce::String ("---.-");
    const int key = dt.key >= 0 ? dj::transposeKey (dt.key, v.keyShift.load()) : -1;
    const auto newKey = Theme::keyText (key, model_.settings.camelotKeys) + (v.keyShift.load() != 0 ? " (" + juce::String (v.keyShift.load() > 0 ? "+" : "") + juce::String (v.keyShift.load()) + ")" : "");
    const auto newTime = Theme::formatTime (pos) + "  -" + Theme::formatTime (len - pos);
    const double tp = v.tempo.load() * 100.0;
    const auto newTempo = (tp >= 0.0 ? "+" : "") + juce::String (tp, 2) + "%";
    if (newBpm != bpmText_ || newKey != keyText_ || newTime != timeText_ || newTempo != tempoText_)
    {
        bpmText_ = newBpm; keyText_ = newKey; timeText_ = newTime; tempoText_ = newTempo;
        repaint (0, 0, getWidth(), 40);
    }
    overview_.refresh();
    platter_.refresh();
}

void DeckView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
    const auto& dt = model_.decks[(std::size_t) deck_];
    g.setColour (Theme::deck (deck_));
    g.fillRect (getLocalBounds().removeFromTop (3));
    auto r = getLocalBounds().removeFromTop (40).reduced (6, 2);
    auto line1 = r.removeFromTop (20);
    // line 1: title ............ BPM  tempo
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (Theme::dim);
    g.drawText (tempoText_, line1.removeFromRight (52), juce::Justification::centredRight);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (19.0f, juce::Font::bold));
    g.drawText (bpmText_, line1.removeFromRight (64), juce::Justification::centredRight);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText (dt.title.isNotEmpty() ? dt.title : juce::String ("Deck " + juce::String (deck_ + 1)), line1, juce::Justification::centredLeft, true);
    // line 2: artist ............ key  elapsed  -remaining
    auto line2 = r;
    g.setFont (juce::FontOptions (11.5f));
    g.setColour (Theme::dim);
    g.drawText (timeText_, line2.removeFromRight (150), juce::Justification::centredRight);
    g.setColour (Theme::key (dt.key >= 0 ? dj::transposeKey (dt.key, model_.engine.deck (deck_).view.keyShift.load()) : -1));
    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    g.drawText (keyText_, line2.removeFromRight (64), juce::Justification::centredRight);
    g.setColour (Theme::dim);
    g.setFont (juce::FontOptions (11.5f));
    g.drawText (dt.artist, line2, juce::Justification::centredLeft, true);
}

void DeckView::resized()
{
    auto r = getLocalBounds().reduced (4);
    r.removeFromTop (38);
    overview_.setBounds (r.removeFromTop (22));
    r.removeFromTop (4);

    const int padRows = juce::jmax (64, r.getHeight() * 36 / 100);
    auto padArea = r.removeFromBottom (padRows);
    const bool narrow = r.getWidth() < 460;
    auto loopRow = r.removeFromBottom (narrow ? 44 : 22);
    r.removeFromBottom (3);

    // tempo column
    auto tempoCol = r.removeFromRight (34);
    tempoRange_.setBounds (tempoCol.removeFromTop (20));
    tempoReset_.setBounds (tempoCol.removeFromBottom (20));
    tempo_.setBounds (tempoCol);
    r.removeFromRight (4);

    // platter + buttons
    const int platterSize = juce::jmin (r.getHeight(), r.getWidth() / 2);
    platter_.setBounds (r.removeFromLeft (platterSize));
    r.removeFromLeft (4);
    auto grid = r;
    const int rowH = juce::jmax (18, grid.getHeight() / 5);
    auto row = [&] { return grid.removeFromTop (rowH).reduced (0, 1); };
    auto place = [] (juce::Rectangle<int> area, std::initializer_list<juce::Component*> comps)
    {
        const int w = area.getWidth() / (int) comps.size();
        for (auto* c : comps) c->setBounds (area.removeFromLeft (w).reduced (1, 0));
    };
    place (row(), { &sync_, &keylock_, &quantize_ });
    place (row(), { &slip_, &vinyl_, &reverse_ });
    place (row(), { &keyDown_, &keyUp_, &keySync_ });
    place (row(), { &stems_[0], &stems_[1], &stems_[2], &stems_[3], &stemsAnalyse_ });
    place (grid, { &cue_, &play_, &censor_ });

    if (narrow)
    {
        place (loopRow.removeFromTop (22).reduced (0, 1), { &loopIn_, &loopOut_, &reloop_, &halve_, &dbl_ });
        auto second = loopRow.reduced (0, 1);
        place (second.removeFromLeft (second.getWidth() / 2), { &autoLess_, &autoLoop_, &autoMore_ });
        place (second, { &jumpBack_, &jumpLess_, &jumpMore_, &jumpFwd_ });
    }
    else
        place (loopRow, { &loopIn_, &loopOut_, &reloop_, &halve_, &dbl_, &autoLess_, &autoLoop_, &autoMore_, &jumpBack_, &jumpLess_, &jumpMore_, &jumpFwd_ });

    padMode_.setBounds (padArea.removeFromTop (22).reduced (1));
    const int pw = padArea.getWidth() / 4, ph = padArea.getHeight() / 2;
    for (int i = 0; i < 8; ++i)
        pads_[(std::size_t) i].setBounds (padArea.getX() + (i % 4) * pw, padArea.getY() + (i / 4) * ph, pw, ph);
}

void DeckView::itemDropped (const SourceDetails& d)
{
    const auto id = (std::uint64_t) d.description.toString().fromFirstOccurrenceOf ("track:", false, false).getLargeIntValue();
    model_.loadTrack (deck_, id);
}

void DeckView::filesDropped (const juce::StringArray& files, int, int)
{
    // a file dropped from Finder: import it, then load it once the import lands
    model_.importPaths (files);
    loadWhenImported (files[0].toStdString(), 25);
}

void DeckView::loadWhenImported (std::string path, int attempts)
{
    if (auto* t = model_.library.findByPath (path)) { model_.loadTrack (deck_, t->id); return; }
    if (attempts <= 0) return;
    juce::Component::SafePointer<DeckView> self (this);
    juce::Timer::callAfterDelay (200, [self, path, attempts] { if (self != nullptr) self->loadWhenImported (path, attempts - 1); });
}

} // namespace app
