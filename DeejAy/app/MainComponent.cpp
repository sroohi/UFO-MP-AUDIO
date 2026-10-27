#include "MainComponent.h"
#include "AudioHost.h"
#include "MidiHost.h"
#include "SettingsView.h"

namespace app
{

MainComponent::MainComponent()
{
    for (int d = 0; d < dj::kNumDecks; ++d)
    {
        waves_[(std::size_t) d] = std::make_unique<WaveformView> (model_, d);
        decks_[(std::size_t) d] = std::make_unique<DeckView> (model_, d);
        addAndMakeVisible (*waves_[(std::size_t) d]);
        addAndMakeVisible (*decks_[(std::size_t) d]);
    }
    for (auto* c : std::initializer_list<juce::Component*> { &fx1_, &fx2_, &beatFx_, &mixer_, &sampler_, &library_ }) addAndMakeVisible (c);
    styleToggle (settings_, Theme::accent);
    settings_.onClick = [this] { SettingsView::show (model_); };
    styleToggle (layout_, Theme::accent);
    layout_.onClick = [this]
    {
        model_.settings.fourDecks = ! model_.settings.fourDecks;
        model_.saveSettings();
        resized();
    };
    styleToggle (stemsAll_, Theme::good);
    stemsAll_.setTooltip ("Show / hide the sampler");
    stemsAll_.setButtonText ("SAMPLER");
    stemsAll_.onClick = [this] { showSampler_ = ! showSampler_; resized(); };
    for (auto* b : { &settings_, &layout_, &stemsAll_ }) addAndMakeVisible (b);

    mixer_.onRecordToggled = [this] { toggleRecording(); };
    model_.onLibraryChanged = [this] { library_.rebuild(); };
    model_.onDeckChanged = [this] (int d) { decks_[(std::size_t) d]->trackChanged(); library_.repaint(); };
    model_.onPadModeChanged = [this] (int d) { decks_[(std::size_t) d]->padModeChanged(); };
    model_.onStatus = [this] (const juce::String& s) { setStatus (s); };
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
    model_.onView = [this] (bool) { model_.settings.fourDecks = ! model_.settings.fourDecks; resized(); };

    // keyboard map (see Settings > Keyboard)
    auto add = [this] (int key, int deck, KeyAction a) { keys_.push_back ({ key, deck, a }); };
    const int deckKeys[2][15] {
        { 'Q', 'W', 'E', 'R', '1', '2', '3', '4', 'A', 'S', 'Z', 'X', 'C', 'D', 'F' },
        { 'U', 'I', 'O', 'P', '7', '8', '9', '0', 'J', 'K', 'N', 'M', ',', 'L', ';' } };
    for (int d = 0; d < 2; ++d)
        for (int a = 0; a < 15; ++a) add (deckKeys[d][a], d, (KeyAction) a);

    setWantsKeyboardFocus (true);
    library_.rebuild();
    setSize (1600, 1000);
    startTimerHz (60);
    if (! model_.midi->controllerConnected()) setStatus ("Ready. " + model_.midi->status() + ". Drop music onto the library to start.");
}

MainComponent::~MainComponent()
{
    stopTimer();
}

void MainComponent::setStatus (const juce::String& s)
{
    status_ = s;
    statusTime_ = juce::Time::getMillisecondCounter();
    repaint (0, getHeight() - 20, getWidth(), 20);
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
    const bool four = model_.settings.fourDecks;
    for (int d = 0; d < dj::kNumDecks; ++d)
        if (four || d < 2) waves_[(std::size_t) d]->refresh();
    if (tick_ % 2 == 0)
    {
        for (int d = 0; d < dj::kNumDecks; ++d)
            if (four || d < 2) decks_[(std::size_t) d]->refresh();
        mixer_.refresh();
        fx1_.refresh();
        fx2_.refresh();
        beatFx_.refresh();
        if (showSampler_) sampler_.refresh();
    }
    // keyboard pitch bend while held
    for (const auto& k : keys_)
        if (k.held && (k.action == KeyAction::BendDown || k.action == KeyAction::BendUp))
            model_.engine.deck (k.deck).inputs.bendTicks.fetch_add (k.action == KeyAction::BendUp ? 4 : -4);
    if (tick_ % 30 == 0)
    {
        const auto ms = model_.midi->status();
        if (ms != midiStatus_) { midiStatus_ = ms; }
        repaint (0, 0, getWidth(), 76);
    }
    if (status_.isNotEmpty() && juce::Time::getMillisecondCounter() - statusTime_ > 8000) { status_.clear(); repaint(); }
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
    auto right = getLocalBounds().removeFromTop (76).reduced (8, 6).removeFromRight (240);
    g.setColour (model_.midi->controllerConnected() ? Theme::good : Theme::dim);
    g.drawText (midiStatus_, right.removeFromBottom (14), juce::Justification::centredRight);
    g.setColour (Theme::dim);
    g.drawText (juce::Time::getCurrentTime().formatted ("%H:%M"), right.removeFromBottom (14), juce::Justification::centredRight);

    if (status_.isNotEmpty())
    {
        auto bar = getLocalBounds().removeFromBottom (20);
        g.setColour (Theme::panelHi);
        g.fillRect (bar);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (status_, bar.reduced (8, 0), juce::Justification::centredLeft);
    }
}

void MainComponent::resized()
{
    const bool four = model_.settings.fourDecks;
    layout_.setButtonText (four ? "4 DECKS" : "2 DECKS");
    auto r = getLocalBounds();
    auto top = r.removeFromTop (76).reduced (8, 6);
    top.removeFromLeft (156);
    auto buttons = top.removeFromRight (240).removeFromTop (34);
    settings_.setBounds (buttons.removeFromRight (80).reduced (2, 4));
    layout_.setBounds (buttons.removeFromRight (80).reduced (2, 4));
    stemsAll_.setBounds (buttons.removeFromRight (80).reduced (2, 4));
    const int fw = top.getWidth() / 3;
    fx1_.setBounds (top.removeFromLeft (fw).reduced (3, 0));
    fx2_.setBounds (top.removeFromLeft (fw).reduced (3, 0));
    beatFx_.setBounds (top.reduced (3, 0));

    if (status_.isNotEmpty()) r.removeFromBottom (20);
    const int n = four ? 4 : 2;
    const int waveH = juce::jlimit (56, 110, getHeight() / (four ? 13 : 8));
    for (int d = 0; d < dj::kNumDecks; ++d)
    {
        waves_[(std::size_t) d]->setVisible (d < n);
        decks_[(std::size_t) d]->setVisible (d < n);
    }
    // waveforms: Serato stacks them in deck order
    for (int d = 0; d < n; ++d) waves_[(std::size_t) d]->setBounds (r.removeFromTop (waveH).reduced (0, 1));

    auto deckRow = r.removeFromTop (juce::jlimit (300, 420, r.getHeight() * 52 / 100));
    const int mixerW = four ? juce::jmin (420, deckRow.getWidth() / 4) : juce::jmin (300, deckRow.getWidth() / 4);
    auto mixerArea = deckRow.withSizeKeepingCentre (mixerW, deckRow.getHeight());
    mixer_.setFourDecks (four);
    mixer_.setBounds (mixerArea);
    auto left = deckRow.withRight (mixerArea.getX());
    auto right = deckRow.withLeft (mixerArea.getRight());
    if (four)
    {
        // FLX10 sides: decks 1 and 3 on the left, 2 and 4 on the right
        decks_[0]->setBounds (left.removeFromLeft (left.getWidth() / 2).reduced (2));
        decks_[2]->setBounds (left.reduced (2));
        decks_[3]->setBounds (right.removeFromRight (right.getWidth() / 2).reduced (2));
        decks_[1]->setBounds (right.reduced (2));
    }
    else
    {
        decks_[0]->setBounds (left.reduced (2));
        decks_[1]->setBounds (right.reduced (2));
    }
    sampler_.setVisible (showSampler_);
    if (showSampler_) sampler_.setBounds (r.removeFromTop (72));
    library_.setBounds (r);
}

bool MainComponent::textEditing() const
{
    return dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr;
}

void MainComponent::keyAction (const KeyBind& k, bool down)
{
    auto cmd = [&] (dj::Cmd t, int index = 0, double value = 0.0, bool flag = false) { model_.engine.deckCommand (t, k.deck, index, value, flag); };
    const bool shift = juce::ModifierKeys::currentModifiers.isShiftDown();
    switch (k.action)
    {
        case KeyAction::Cue: cmd (down ? dj::Cmd::CueDown : dj::Cmd::CueUp); break;
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
    }
}

bool MainComponent::keyStateChanged (bool)
{
    if (textEditing()) return false;
    const auto mods = juce::ModifierKeys::currentModifiers;
    if (mods.isCommandDown() || mods.isAltDown()) return false;
    bool used = false;
    for (auto& k : keys_)
    {
        const bool down = juce::KeyPress::isKeyCurrentlyDown (k.key);
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
    if (textEditing()) return false;
    if (key.getKeyCode() == juce::KeyPress::leftKey && mods.isShiftDown()) { model_.loadTrack (mods.isCommandDown() ? 2 : 0, model_.selectedTrack); return true; }
    if (key.getKeyCode() == juce::KeyPress::rightKey && mods.isShiftDown()) { model_.loadTrack (mods.isCommandDown() ? 3 : 1, model_.selectedTrack); return true; }
    if (key.getKeyCode() == juce::KeyPress::upKey) { library_.browse (-1, false); return true; }
    if (key.getKeyCode() == juce::KeyPress::downKey) { library_.browse (1, false); return true; }
    if (key.getKeyCode() == juce::KeyPress::tabKey) { library_.back(); return true; }
    // deck keys are handled in keyStateChanged (they need key-up)
    for (const auto& k : keys_) if (k.key == key.getKeyCode() || k.key == juce::CharacterFunctions::toUpperCase (key.getTextCharacter())) return true;
    return false;
}

} // namespace app
