#include "MainComponent.h"
#include "AudioHost.h"
#include "MidiHost.h"
#include "SettingsView.h"

namespace app
{

namespace
{
    // Performer keys, in effect order: STUTTER, NOTE REPEAT, TAPE STOP, PITCH, FILTER, TALKBOX, BITCRUSH, DELAY, REVERB
    constexpr int kPerformerKeys[dj::kNumPerformerFx] { '5', '6', 'T', 'Y', 'G', 'H', 'V', 'B', '/' };
    constexpr juce::uint32 kDoubleTapMs = 350;
}

MainComponent::MainComponent()
{
    for (int d = 0; d < dj::kNumDecks; ++d)
    {
        waves_[(std::size_t) d] = std::make_unique<WaveformView> (model_, d);
        decks_[(std::size_t) d] = std::make_unique<DeckView> (model_, d);
        addChildComponent (*waves_[(std::size_t) d]);
        addChildComponent (*decks_[(std::size_t) d]);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &fx1_, &fx2_, &beatFx_, &mixer_, &performer_, &sampler_, &library_ }) addAndMakeVisible (c);

    auto& s = model_.settings;
    for (auto* b : { &decksBtn_, &bankBtn_, &keysBtn_, &settings_, &samplerBtn_, &performerBtn_, &spliceBtn_ })
    {
        styleToggle (*b, b == &keysBtn_ ? Theme::good : b == &performerBtn_ ? Theme::hot : Theme::accent);
        addAndMakeVisible (b);
    }
    settings_.onClick = [this] { SettingsView::show (model_); };
    decksBtn_.setTooltip ("Decks on screen: 2, 4 or 8 (VIEW on the FLX10)");
    decksBtn_.onClick = [this] { cycleDeckCount(); };
    bankBtn_.setTooltip ("Show decks 1-4 or 5-8. The DDJ-FLX10 follows: its four decks control the bank on screen (SHIFT + VIEW).");
    bankBtn_.onClick = [this, &s] { model_.setDeckBank (1 - s.deckBank); };
    keysBtn_.setTooltip ("Computer keyboard as a controller: decks, hot cues, loops, Performer and sampler pads (Settings > Keyboard). "
                         "Turn it off to type without playing anything.");
    keysBtn_.onClick = [this, &s] { s.keyboardControl = ! s.keyboardControl; model_.saveSettings(); updateButtons(); };
    samplerBtn_.onClick = [this, &s] { s.showSampler = ! s.showSampler; model_.saveSettings(); resized(); };
    performerBtn_.setTooltip ("Performer: stutter, note repeat, tape stop, pitch, filter, talkbox, bitcrusher, delay and reverb on the whole mix");
    performerBtn_.onClick = [this, &s] { s.showPerformer = ! s.showPerformer; model_.saveSettings(); resized(); };
    spliceBtn_.setTooltip ("Browse the sounds you downloaded with the Splice app and drop loops onto the sampler");
    spliceBtn_.onClick = [this] { library_.showSplice(); };

    mixer_.onRecordToggled = [this] { toggleRecording(); };
    model_.onLibraryChanged = [this] { library_.rebuild(); };
    model_.onTracksUpdated = [this] { library_.tracksUpdated(); };
    model_.onDeckChanged = [this] (int d) { decks_[(std::size_t) d]->trackChanged(); library_.repaint(); };
    model_.onPadModeChanged = [this] (int d) { decks_[(std::size_t) d]->padModeChanged(); };
    model_.onStatus = [this] (const juce::String& text) { setStatus (text); };
    model_.onLayoutChanged = [this] { resized(); };
    model_.onBrowse = [this] (int delta, bool shift)
    {
        if (shift) WaveformView::zoomSeconds() = juce::jlimit (1.0, 40.0, WaveformView::zoomSeconds() * (delta > 0 ? 0.85 : 1.0 / 0.85));
        else library_.browse (delta, false);
    };
    model_.onBack = [this] (bool shift)
    {
        if (shift && model_.selectedTrack != 0) { model_.library.prepare().push_back (model_.selectedTrack); model_.markLibraryDirty(); library_.rebuild(); }
        else library_.back();
    };
    // FLX10 VIEW: 2 / 4 / 8 decks; SHIFT + VIEW: the other bank of four
    model_.onView = [this, &s] (bool shift)
    {
        if (shift) { if (s.deckCount == 8) model_.setDeckBank (1 - s.deckBank); }
        else cycleDeckCount();
    };

    // keyboard map (see Settings > Keyboard)
    const int deckKeys[2][15] {
        { 'Q', 'W', 'E', 'R', '1', '2', '3', '4', 'A', 'S', 'Z', 'X', 'C', 'D', 'F' },
        { 'U', 'I', 'O', 'P', '7', '8', '9', '0', 'J', 'K', 'N', 'M', ',', 'L', ';' } };
    for (int side = 0; side < 2; ++side)
        for (int a = 0; a < 15; ++a) keys_.push_back ({ deckKeys[side][a], side, (KeyAction) a });
    for (int i = 0; i < dj::kNumPerformerFx; ++i) keys_.push_back ({ kPerformerKeys[i], 0, KeyAction::Performer, i });
    for (int i = 0; i < dj::kSamplerSlotsPerBank; ++i) keys_.push_back ({ '1' + i, 0, KeyAction::SamplerPad, i, true });

    setWantsKeyboardFocus (true);
    library_.rebuild();
    updateButtons();
    setSize (1600, 1000);
    startTimerHz (60);
    if (! model_.midi->controllerConnected()) setStatus ("Ready. " + model_.midi->status() + ". Drop music onto the library to start.");
}

MainComponent::~MainComponent()
{
    stopTimer();
}

int MainComponent::bankBase() const
{
    return model_.settings.deckCount == 8 ? model_.settings.deckBank * 4 : 0;
}

int MainComponent::sideDeck (int side) const { return bankBase() + side; }

std::vector<int> MainComponent::shownDecks() const
{
    const int base = bankBase();
    if (model_.settings.deckCount == 2) return { 0, 1 };
    return { base, base + 1, base + 2, base + 3 };
}

void MainComponent::cycleDeckCount()
{
    auto& s = model_.settings;
    s.deckCount = s.deckCount == 2 ? 4 : s.deckCount == 4 ? 8 : 2;
    model_.setDeckBank (s.deckCount == 8 ? s.deckBank : 0);   // saves, updates the FLX10 layer and the layout
}

void MainComponent::updateButtons()
{
    const auto& s = model_.settings;
    decksBtn_.setButtonText (juce::String (s.deckCount) + " DECKS");
    bankBtn_.setVisible (s.deckCount == 8);
    bankBtn_.setButtonText (s.deckBank == 0 ? "DECKS 1-4" : "DECKS 5-8");
    bankBtn_.setToggleState (s.deckBank == 1, juce::dontSendNotification);
    keysBtn_.setToggleState (s.keyboardControl, juce::dontSendNotification);
    samplerBtn_.setToggleState (s.showSampler, juce::dontSendNotification);
    performerBtn_.setToggleState (s.showPerformer, juce::dontSendNotification);
    std::array<juce::String, dj::kNumPerformerFx> hints;
    if (s.keyboardControl)
        for (int i = 0; i < dj::kNumPerformerFx; ++i) hints[(std::size_t) i] = juce::String::charToString ((juce::juce_wchar) kPerformerKeys[i]);
    performer_.setKeyHints (hints);
}

void MainComponent::setStatus (const juce::String& s)
{
    status_ = s;
    statusTime_ = juce::Time::getMillisecondCounter();
    repaint (0, getHeight() - 18, getWidth(), 18);
}

void MainComponent::toggleRecording()
{
    if (model_.engine.recorder.recording())
    {
        model_.stopRecording();
        setStatus ("Recording saved: " + model_.lastRecording().getFullPathName());
    }
    else if (model_.startRecording())
        setStatus ("Recording to " + model_.lastRecording().getFileName());
    else
        setStatus ("Could not start recording");
}

void MainComponent::timerCallback()
{
    ++tick_;
    const int count = model_.settings.deckCount;
    for (int d = 0; d < count; ++d) waves_[(std::size_t) d]->refresh();
    if (tick_ % 2 == 0)
    {
        for (int d : shownDecks()) decks_[(std::size_t) d]->refresh();
        mixer_.refresh();
        fx1_.refresh();
        fx2_.refresh();
        beatFx_.refresh();
        if (model_.settings.showSampler) sampler_.refresh();
        if (model_.settings.showPerformer) performer_.refresh();
    }
    // keyboard pitch bend while held
    for (const auto& k : keys_)
        if (k.held && k.deck >= 0 && (k.action == KeyAction::BendDown || k.action == KeyAction::BendUp))
            model_.engine.deck (k.deck).inputs.bendTicks.fetch_add (k.action == KeyAction::BendUp ? 4 : -4);
    if (tick_ % 30 == 0)
    {
        midiStatus_ = model_.midi->status();
        repaint (0, getHeight() - 18, getWidth(), 18);
    }
    if (status_.isNotEmpty() && juce::Time::getMillisecondCounter() - statusTime_ > 8000) { status_.clear(); repaint (0, getHeight() - 18, getWidth(), 18); }
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Theme::bg);
    auto top = getLocalBounds().removeFromTop (76).reduced (8, 6);
    auto logo = top.removeFromLeft (150);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    g.drawText ("DEEJ-AY", logo.removeFromTop (34), juce::Justification::centredLeft);
    g.setColour (Theme::hot);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("by MP AUDIO", logo.removeFromTop (14), juce::Justification::centredLeft);
    g.setColour (Theme::dim);
    g.setFont (juce::FontOptions (10.0f));
    const double cpu = model_.audio->cpuLoad() * 100.0;
    g.drawText ("CPU " + juce::String ((int) std::lround (cpu)) + "%  " + juce::String (model_.audio->sampleRate() / 1000.0, 1) + " kHz / " + juce::String (model_.audio->bufferSize()),
                logo, juce::Justification::centredLeft);

    // status bar: messages left; keyboard, controller and clock right
    auto bar = getLocalBounds().removeFromBottom (18);
    g.setColour (Theme::panelHi);
    g.fillRect (bar);
    bar = bar.reduced (8, 0);
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (Theme::dim);
    g.drawText (juce::Time::getCurrentTime().formatted ("%H:%M"), bar.removeFromRight (44), juce::Justification::centredRight);
    g.setColour (model_.midi->controllerConnected() ? Theme::good : Theme::dim);
    g.drawText (midiStatus_, bar.removeFromRight (220), juce::Justification::centredRight);
    g.setColour (model_.settings.keyboardControl ? Theme::good : Theme::dim);
    g.drawText (model_.settings.keyboardControl ? "KEYS ON" : "KEYS OFF", bar.removeFromRight (70), juce::Justification::centredRight);
    if (model_.analysisPending() > 0)
    {
        g.setColour (Theme::warn);
        g.drawText ("Analysing " + juce::String (model_.analysisPending()), bar.removeFromRight (110), juce::Justification::centredRight);
    }
    g.setColour (Theme::text);
    g.drawText (status_, bar, juce::Justification::centredLeft, true);
}

void MainComponent::resized()
{
    updateButtons();
    const auto& s = model_.settings;
    auto r = getLocalBounds();
    auto top = r.removeFromTop (76).reduced (8, 6);
    top.removeFromLeft (156);
    auto buttons = top.removeFromRight (316);
    auto row1 = buttons.removeFromTop (buttons.getHeight() / 2);
    auto row2 = buttons;
    const int bw = row1.getWidth() / 4;
    for (auto* b : { &settings_, &keysBtn_, &bankBtn_, &decksBtn_ })     // right to left; the bank button only with 8 decks
        if (b->isVisible()) b->setBounds (row1.removeFromRight (bw).reduced (2, 3));
    for (auto* b : { &spliceBtn_, &performerBtn_, &samplerBtn_ })
        b->setBounds (row2.removeFromRight (bw).reduced (2, 3));
    const int fw = top.getWidth() / 3;
    fx1_.setBounds (top.removeFromLeft (fw).reduced (3, 0));
    fx2_.setBounds (top.removeFromLeft (fw).reduced (3, 0));
    beatFx_.setBounds (top.reduced (3, 0));
    r.removeFromBottom (18);   // status bar

    // vertical budget: waveforms, decks + mixer, Performer, sampler, library (at least ~110 px)
    const int count = s.deckCount;
    const int laneH = count == 8 ? juce::jlimit (24, 40, getHeight() / 28) : count == 4 ? juce::jlimit (40, 80, getHeight() / 14) : juce::jlimit (56, 110, getHeight() / 9);
    const int perfH = s.showPerformer ? 104 : 0, smpH = s.showSampler ? 72 : 0;
    const int deckH = juce::jlimit (280, 400, r.getHeight() - laneH * count - perfH - smpH - 110);

    for (int d = 0; d < dj::kNumDecks; ++d) waves_[(std::size_t) d]->setVisible (d < count);
    for (int d = 0; d < count; ++d) waves_[(std::size_t) d]->setBounds (r.removeFromTop (laneH).reduced (0, 1));

    const auto shown = shownDecks();
    for (int d = 0; d < dj::kNumDecks; ++d)
        decks_[(std::size_t) d]->setVisible (std::find (shown.begin(), shown.end(), d) != shown.end());
    auto deckRow = r.removeFromTop (deckH);
    const bool four = shown.size() == 4;
    const int mixerW = four ? juce::jmin (420, deckRow.getWidth() / 4) : juce::jmin (300, deckRow.getWidth() / 4);
    auto mixerArea = deckRow.withSizeKeepingCentre (mixerW, deckRow.getHeight());
    mixer_.setStrips (four ? std::vector<int> { shown[2], shown[0], shown[1], shown[3] } : std::vector<int> { shown[0], shown[1] });
    mixer_.setBounds (mixerArea);
    auto left = deckRow.withRight (mixerArea.getX());
    auto right = deckRow.withLeft (mixerArea.getRight());
    if (four)
    {
        // FLX10 sides: decks 1 and 3 on the left, 2 and 4 on the right (5/7 and 6/8 on the second bank)
        decks_[(std::size_t) shown[0]]->setBounds (left.removeFromLeft (left.getWidth() / 2).reduced (2));
        decks_[(std::size_t) shown[2]]->setBounds (left.reduced (2));
        decks_[(std::size_t) shown[3]]->setBounds (right.removeFromRight (right.getWidth() / 2).reduced (2));
        decks_[(std::size_t) shown[1]]->setBounds (right.reduced (2));
    }
    else
    {
        decks_[(std::size_t) shown[0]]->setBounds (left.reduced (2));
        decks_[(std::size_t) shown[1]]->setBounds (right.reduced (2));
    }
    performer_.setVisible (s.showPerformer);
    if (s.showPerformer) performer_.setBounds (r.removeFromTop (perfH));
    sampler_.setVisible (s.showSampler);
    if (s.showSampler) sampler_.setBounds (r.removeFromTop (smpH));
    library_.setBounds (r);
    repaint();
}

bool MainComponent::textEditing() const
{
    return dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr;
}

void MainComponent::keyAction (KeyBind& k, bool down)
{
    if (down) k.deck = sideDeck (k.side);
    const int deck = k.deck;
    auto cmd = [&] (dj::Cmd t, int index = 0, double value = 0.0, bool flag = false) { model_.engine.deckCommand (t, deck, index, value, flag); };
    const bool shift = juce::ModifierKeys::currentModifiers.isShiftDown();
    switch (k.action)
    {
        case KeyAction::Cue:
            if (down)
            {
                // double-tap: play from the top, like a double-click on the CUE button
                const auto now = juce::Time::getMillisecondCounter();
                auto& last = lastCueKey_[k.side];
                const bool second = model_.settings.doubleClickCue && now - last < kDoubleTapMs;
                last = second ? 0 : now;
                cmd (second ? dj::Cmd::RestartFromStart : dj::Cmd::CueDown);
            }
            else cmd (dj::Cmd::CueUp);
            break;
        case KeyAction::Play: if (down) cmd (dj::Cmd::PlayPause); break;
        case KeyAction::Sync: if (down) cmd (shift ? dj::Cmd::SyncOff : dj::Cmd::SyncToggle); break;
        case KeyAction::Censor: cmd (down ? dj::Cmd::CensorDown : dj::Cmd::CensorUp); break;
        case KeyAction::Hot1: case KeyAction::Hot2: case KeyAction::Hot3: case KeyAction::Hot4:
        {
            const int i = (int) k.action - (int) KeyAction::Hot1;
            if (down) cmd (dj::Cmd::HotCueDown, i, 0.0, shift); else cmd (dj::Cmd::HotCueUp, i);
            break;
        }
        case KeyAction::JumpBack: if (down) cmd (dj::Cmd::BeatJump, 0, shift ? -16.0 : -4.0); break;
        case KeyAction::JumpFwd: if (down) cmd (dj::Cmd::BeatJump, 0, shift ? 16.0 : 4.0); break;
        case KeyAction::LoopIn: if (down) cmd (dj::Cmd::LoopIn); break;
        case KeyAction::LoopOut: if (down) cmd (shift ? dj::Cmd::Reloop : dj::Cmd::LoopOut); break;
        case KeyAction::FourBeat: if (down) cmd (dj::Cmd::FourBeatExit); break;
        case KeyAction::BendDown: case KeyAction::BendUp: break; // continuous, from the timer
        case KeyAction::Performer:
        {
            // hold: momentary; SHIFT + key: latch on / off
            auto& on = model_.engine.performer.controls.on[k.index];
            if (down) { k.latched = shift; on.store (shift ? ! on.load() : true); }
            else if (! k.latched) on.store (false);
            break;
        }
        case KeyAction::SamplerPad:
        {
            dj::Command c;
            c.deck = model_.engine.samplerBank.load() * dj::kSamplerSlotsPerBank + k.index;
            c.type = down ? (shift ? dj::Cmd::SamplerStop : dj::Cmd::SamplerPadDown) : dj::Cmd::SamplerPadUp;
            model_.engine.post (c);
            break;
        }
    }
}

bool MainComponent::keyStateChanged (bool)
{
    if (textEditing() || ! model_.settings.keyboardControl) return false;
    const auto mods = juce::ModifierKeys::currentModifiers;
    if (mods.isCommandDown()) return false;
    const bool alt = mods.isAltDown();
    bool used = false;
    for (auto& k : keys_)
    {
        const bool physical = juce::KeyPress::isKeyCurrentlyDown (k.key);
        // a press belongs to the binding whose Option state matches; its release always goes back to it
        const bool down = k.held ? physical : physical && k.alt == alt;
        if (down != k.held)
        {
            k.held = down;
            keyAction (k, down);
            used = true;
        }
    }
    return used;
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() && key.getKeyCode() == 'F') { library_.focusSearch(); return true; }
    if (mods.isCommandDown() && key.getKeyCode() == 'R') { toggleRecording(); return true; }
    if (mods.isCommandDown() && key.getKeyCode() == ',') { SettingsView::show (model_); return true; }
    if (mods.isCommandDown() && key.getKeyCode() == 'K') { keysBtn_.triggerClick(); return true; }
    if (mods.isCommandDown() && key.getKeyCode() == 'B' && model_.settings.deckCount == 8) { model_.setDeckBank (1 - model_.settings.deckBank); return true; }
    if (textEditing()) return false;
    // SHIFT + LEFT / RIGHT: load onto the left / right deck of the bank on screen (CMD: its third / fourth deck)
    if (key.getKeyCode() == juce::KeyPress::leftKey && mods.isShiftDown()) { model_.loadTrack (sideDeck (mods.isCommandDown() ? 2 : 0), model_.selectedTrack); return true; }
    if (key.getKeyCode() == juce::KeyPress::rightKey && mods.isShiftDown()) { model_.loadTrack (sideDeck (mods.isCommandDown() ? 3 : 1), model_.selectedTrack); return true; }
    if (key.getKeyCode() == juce::KeyPress::upKey) { library_.browse (-1, false); return true; }
    if (key.getKeyCode() == juce::KeyPress::downKey) { library_.browse (1, false); return true; }
    if (key.getKeyCode() == juce::KeyPress::tabKey) { library_.back(); return true; }
    if (! model_.settings.keyboardControl) return false;
    // control keys are handled in keyStateChanged (they need key-up)
    for (const auto& k : keys_) if (k.key == key.getKeyCode() || k.key == juce::CharacterFunctions::toUpperCase (key.getTextCharacter())) return true;
    return false;
}

} // namespace app
