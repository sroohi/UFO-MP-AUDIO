#include "SamplerView.h"

namespace app
{

SamplerSlotView::SamplerSlotView (AppModel& model, int index) : model_ (model), index_ (index)
{
    play_.onPress = [this] (bool shift)
    {
        dj::Command c; c.deck = slot(); c.type = shift ? dj::Cmd::SamplerStop : dj::Cmd::SamplerPadDown;
        model_.engine.post (c);
    };
    play_.onRelease = [this] { dj::Command c; c.deck = slot(); c.type = dj::Cmd::SamplerPadUp; model_.engine.post (c); };
    addAndMakeVisible (play_);
    styleToggle (mode_, Theme::accent);
    mode_.onClick = [this] { auto& m = model_.engine.sampler().controls[(std::size_t) slot()].mode; m.store ((m.load() + 1) % 3); };
    mode_.setTooltip ("Play mode: Trigger (plays through, re-press restarts) / Hold (plays while held) / On-Off (press again to stop)");
    styleToggle (sync_, Theme::accent);
    sync_.onClick = [this]
    {
        auto& c = model_.engine.sampler().controls[(std::size_t) slot()];
        if (juce::ModifierKeys::currentModifiers.isShiftDown()) c.keylock.store (! c.keylock.load());
        else c.sync.store (! c.sync.load());
    };
    sync_.setTooltip ("SYNC: match the master tempo and start on the beat. SHIFT-click: key lock on / off "
                      "(off = the sample speeds up like a record, SYNC~)");
    styleToggle (repeat_, Theme::accent);
    repeat_.onClick = [this] { auto& s = model_.engine.sampler().controls[(std::size_t) slot()].repeat; s.store (! s.load()); };
    for (auto* b : { &mode_, &sync_, &repeat_ }) addAndMakeVisible (b);
    volume_.setSliderStyle (juce::Slider::LinearBar);
    volume_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volume_.setRange (0.0, 1.0);
    volume_.setValue (1.0, juce::dontSendNotification);
    volume_.setColour (juce::Slider::trackColourId, Theme::accent.withAlpha (0.5f));
    volume_.onValueChange = [this] { model_.engine.sampler().controls[(std::size_t) slot()].volume.store ((float) volume_.getValue()); };
    addAndMakeVisible (volume_);
}

void SamplerSlotView::refresh()
{
    const auto& c = model_.engine.sampler().controls[(std::size_t) slot()];
    const auto& v = model_.engine.sampler().view[(std::size_t) slot()];
    static const char* modes[3] { "TRIG", "HOLD", "ON/OFF" };
    mode_.setButtonText (modes[juce::jlimit (0, 2, c.mode.load())]);
    sync_.setToggleState (c.sync.load(), juce::dontSendNotification);
    sync_.setButtonText (c.keylock.load() ? "SYNC" : "SYNC~");
    repeat_.setToggleState (c.repeat.load(), juce::dontSendNotification);
    if (! volume_.isMouseButtonDown()) volume_.setValue (c.volume.load(), juce::dontSendNotification);
    const auto name = model_.samplerName (slot());
    const double bpm = v.bpm.load();
    play_.setLabel ((name.isNotEmpty() ? name : juce::String::charToString ((juce::juce_wchar) ('A' + bank_)) + juce::String (index_ + 1))
                    + (v.loaded.load() && bpm > 0.0 ? "\n" + juce::String (bpm, std::abs (bpm - std::round (bpm)) < 0.05 ? 0 : 1) + " BPM" : juce::String()));
    play_.setLit (v.loaded.load());
    // waiting for the beat: blink
    const bool blink = v.waiting.load() && (juce::Time::getMillisecondCounter() / 120) % 2 == 0;
    play_.setColour (v.playing.load() || blink ? Theme::good : Theme::accent);
    repaint();
}

void SamplerSlotView::paint (juce::Graphics& g)
{
    const auto& v = model_.engine.sampler().view[(std::size_t) slot()];
    if (v.playing.load())
    {
        g.setColour (Theme::good);
        g.fillRect (getLocalBounds().removeFromBottom (2).withWidth ((int) (v.position.load() * getWidth())));
    }
}

void SamplerSlotView::resized()
{
    auto r = getLocalBounds().reduced (2);
    volume_.setBounds (r.removeFromBottom (8));
    auto row = r.removeFromBottom (16);
    const int w = row.getWidth() / 3;
    mode_.setBounds (row.removeFromLeft (w));
    sync_.setBounds (row.removeFromLeft (w));
    repeat_.setBounds (row);
    play_.setBounds (r);
}

void SamplerSlotView::itemDropped (const SourceDetails& d)
{
    const auto s = d.description.toString();
    if (s.startsWith ("sample:"))
    {
        // "sample:\n<path>\n<bpm>\n<loop 0/1>\n<name>" from the library's Splice list
        const auto parts = juce::StringArray::fromLines (s.fromFirstOccurrenceOf ("\n", false, false));
        if (parts.size() >= 4) model_.loadSampleFile (slot(), juce::File (parts[0]), parts[1].getDoubleValue(), parts[2] == "1", parts[3]);
        return;
    }
    model_.loadToSampler (slot(), (std::uint64_t) s.fromFirstOccurrenceOf ("track:", false, false).getLargeIntValue());
}

void SamplerSlotView::filesDropped (const juce::StringArray& files, int, int)
{
    if (files.isEmpty()) return;
    const juce::File f (files[0]);
    const auto info = dj::library::parseSampleName (f.getFileName().toStdString());
    model_.loadSampleFile (slot(), f, info.bpm, info.loop, juce::String (info.name));
}

SamplerView::SamplerView (AppModel& model) : model_ (model)
{
    for (int b = 0; b < dj::kSamplerBanks; ++b)
    {
        auto& btn = banks_[(std::size_t) b];
        btn.setButtonText (juce::String::charToString ((juce::juce_wchar) ('A' + b)));
        styleToggle (btn, Theme::accent);
        btn.onClick = [this, b] { model_.engine.samplerBank.store (b); };
        addAndMakeVisible (btn);
    }
    for (int i = 0; i < dj::kSamplerSlotsPerBank; ++i)
    {
        slots_[(std::size_t) i] = std::make_unique<SamplerSlotView> (model, i);
        addAndMakeVisible (*slots_[(std::size_t) i]);
    }
}

void SamplerView::refresh()
{
    const int bank = model_.engine.samplerBank.load();
    for (int b = 0; b < dj::kSamplerBanks; ++b) banks_[(std::size_t) b].setToggleState (b == bank, juce::dontSendNotification);
    for (auto& s : slots_) { s->setBank (bank); s->refresh(); }
}

void SamplerView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
    g.setColour (Theme::accent);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("SAMPLER", getLocalBounds().reduced (6, 2).removeFromTop (14), juce::Justification::topLeft);
}

void SamplerView::resized()
{
    auto r = getLocalBounds().reduced (4);
    auto left = r.removeFromLeft (64);
    left.removeFromTop (16);
    // banks A-D as a 2 x 2 grid
    const int bw = left.getWidth() / 2, bh = left.getHeight() / 2;
    for (int b = 0; b < dj::kSamplerBanks; ++b)
        banks_[(std::size_t) b].setBounds (left.getX() + (b % 2) * bw, left.getY() + (b / 2) * bh, bw, bh);
    for (auto& b : banks_) b.setBounds (b.getBounds().reduced (2, 1));
    const int w = r.getWidth() / dj::kSamplerSlotsPerBank;
    for (auto& s : slots_) s->setBounds (r.removeFromLeft (w));
}

} // namespace app
