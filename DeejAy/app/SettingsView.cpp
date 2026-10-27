#include "SettingsView.h"

namespace app
{

namespace
{
    // A simple vertical form: label + control rows.
    class Form : public juce::Component
    {
    public:
        explicit Form (AppModel& m) : model (m) {}
        AppModel& model;

        void addRow (const juce::String& label, std::unique_ptr<juce::Component> control, const juce::String& help = {})
        {
            auto l = std::make_unique<juce::Label> (juce::String(), label);
            l->setColour (juce::Label::textColourId, Theme::text);
            l->setTooltip (help);
            addAndMakeVisible (*l);
            addAndMakeVisible (*control);
            rows.push_back ({ std::move (l), std::move (control) });
        }
        juce::ToggleButton& toggle (const juce::String& label, bool value, std::function<void (bool)> set, const juce::String& help = {})
        {
            auto t = std::make_unique<juce::ToggleButton>();
            t->setToggleState (value, juce::dontSendNotification);
            auto* raw = t.get();
            t->onClick = [raw, set, this] { set (raw->getToggleState()); model.applySettings(); model.saveSettings(); };
            addRow (label, std::move (t), help);
            return *raw;
        }
        juce::ComboBox& combo (const juce::String& label, const juce::StringArray& items, int selected, std::function<void (int)> set, const juce::String& help = {})
        {
            auto c = std::make_unique<juce::ComboBox>();
            c->addItemList (items, 1);
            c->setSelectedItemIndex (selected, juce::dontSendNotification);
            auto* raw = c.get();
            c->onChange = [raw, set, this] { set (raw->getSelectedItemIndex()); model.applySettings(); model.saveSettings(); };
            addRow (label, std::move (c), help);
            return *raw;
        }
        juce::Slider& slider (const juce::String& label, double lo, double hi, double step, double value, std::function<void (double)> set, const juce::String& suffix = {})
        {
            auto s = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            s->setRange (lo, hi, step);
            s->setValue (value, juce::dontSendNotification);
            s->setTextValueSuffix (suffix);
            auto* raw = s.get();
            s->onDragEnd = [raw, set, this] { set (raw->getValue()); model.applySettings(); model.saveSettings(); };
            s->onValueChange = [raw, set] { set (raw->getValue()); };
            addRow (label, std::move (s));
            return *raw;
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (16);
            for (auto& [l, c] : rows)
            {
                auto row = r.removeFromTop (30);
                l->setBounds (row.removeFromLeft (260));
                c->setBounds (row.removeFromLeft (juce::jmin (420, row.getWidth())).reduced (0, 3));
            }
        }
        std::vector<std::pair<std::unique_ptr<juce::Label>, std::unique_ptr<juce::Component>>> rows;
    };

    class AudioPage : public juce::Component
    {
    public:
        explicit AudioPage (AppModel& m)
            : selector (m.audio->deviceManager(), 0, 0, 2, 6, false, false, true, false)
        {
            addAndMakeVisible (selector);
            info.setText ("Outputs 1/2 = MASTER, 3/4 = HEADPHONES (cue), 5/6 = BOOTH when the device has them.\n"
                          "On a DDJ-FLX10 the unit's MASTER / BOOTH / HEADPHONES knobs act in hardware.",
                          juce::dontSendNotification);
            info.setColour (juce::Label::textColourId, Theme::dim);
            addAndMakeVisible (info);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            info.setBounds (r.removeFromBottom (44));
            selector.setBounds (r);
        }
        juce::AudioDeviceSelectorComponent selector;
        juce::Label info;
    };

    class KeyboardPage : public juce::Component
    {
    public:
        KeyboardPage()
        {
            text.setMultiLine (true);
            text.setReadOnly (true);
            text.setColour (juce::TextEditor::backgroundColourId, Theme::panel);
            text.setColour (juce::TextEditor::textColourId, Theme::text);
            text.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
            text.setText (
                "                       DECK 1 (left)     DECK 2 (right)\n"
                "Cue (hold)             Q                 U\n"
                "Play / pause           W                 I\n"
                "Sync                   E                 O\n"
                "Censor (hold)          R                 P\n"
                "Hot cues 1-4 (hold)    1 2 3 4           7 8 9 0\n"
                "  (SHIFT = delete)\n"
                "Beat jump back / fwd   A / S             J / K\n"
                "Loop in / out          Z / X             N / M\n"
                "4-beat loop / exit     C                 ,\n"
                "Pitch bend - / +       D / F             L / ;\n\n"
                "Load selected track    SHIFT + LEFT / RIGHT (decks 1 / 2)\n"
                "                       CMD + SHIFT + LEFT / RIGHT (decks 3 / 4)\n"
                "Browse                 UP / DOWN,  TAB toggles crates / tracks\n"
                "Search                 CMD + F\n"
                "Record the mix         CMD + R\n"
                "Settings               CMD + ,\n");
            addAndMakeVisible (text);
        }
        void resized() override { text.setBounds (getLocalBounds().reduced (12)); }
        juce::TextEditor text;
    };
}

SettingsView::SettingsView (AppModel& model) : model_ (model)
{
    auto& s = model_.settings;
    tabs_.addTab ("Audio", Theme::panel, new AudioPage (model), true);

    auto* ctrl = new Form (model);
    auto status = std::make_unique<juce::Label> (juce::String(), model.midi->status());
    status->setColour (juce::Label::textColourId, Theme::accent);
    ctrl->addRow ("Controller", std::move (status));
    auto rescan = std::make_unique<juce::TextButton> ("Rescan MIDI devices");
    rescan->onClick = [&model] { model.midi->refreshDevices(); };
    ctrl->addRow ("", std::move (rescan));
    ctrl->toggle ("Fader Start (SHIFT + fader)", s.faderStart, [&s] (bool v) { s.faderStart = v; });
    ctrl->toggle ("Headphone mix done by the unit", s.phonesMixInHardware, [&s] (bool v) { s.phonesMixInHardware = v; },
                  "On for the DDJ-FLX10 (its HEADPHONES MIX / LEVEL knobs act in hardware); off for a plain interface");
    ctrl->slider ("Jog resolution (ticks / turn)", 128, 4096, 1, s.jogTicksPerRev, [&s] (double v) { s.jogTicksPerRev = (int) v; });
    tabs_.addTab ("Controller", Theme::panel, ctrl, true);

    auto* decks = new Form (model);
    decks->combo ("Hot cue on a paused deck", { "Play from the cue (Serato)", "Play while held (CDJ gate)", "Jump only" }, s.hotCueWhenPaused,
                  [&s] (int i) { s.hotCueWhenPaused = i; });
    decks->combo ("Quantize resolution", { "1/8 beat", "1/4 beat", "1/2 beat", "1 beat" },
                  s.quantizeBeats <= 0.125 ? 0 : s.quantizeBeats <= 0.25 ? 1 : s.quantizeBeats <= 0.5 ? 2 : 3,
                  [&s] (int i) { static constexpr double q[4] { 0.125, 0.25, 0.5, 1.0 }; s.quantizeBeats = q[i]; });
    decks->slider ("Platter stop time (brake)", 0.0, 3.0, 0.05, s.brakeSeconds, [&s] (double v) { s.brakeSeconds = (float) v; }, " s");
    decks->slider ("Platter start time", 0.0, 3.0, 0.05, s.startSeconds, [&s] (double v) { s.startSeconds = (float) v; }, " s");
    decks->toggle ("SLIP REVERSE cancels after 8 beats", s.slipReverseAutoCancel, [&s] (bool v) { s.slipReverseAutoCancel = v; });
    decks->toggle ("Four decks", s.fourDecks, [&s] (bool v) { s.fourDecks = v; });
    decks->combo ("EQ type", { "DJM-style EQ (-26 / +6 dB)", "Isolator (full kill)" }, s.eqMode, [&s] (int i) { s.eqMode = i; });
    decks->slider ("Crossfader curve (smooth - sharp)", 0.0, 1.0, 0.01, s.xfCurve, [&s] (double v) { s.xfCurve = (float) v; });
    decks->slider ("Crossfader cut lag", 0.0, 0.1, 0.001, s.xfCutLag, [&s] (double v) { s.xfCutLag = (float) v; });
    decks->slider ("Channel fader curve (slow - fast)", 0.0, 1.0, 0.01, s.chFaderCurve, [&s] (double v) { s.chFaderCurve = (float) v; });
    tabs_.addTab ("Decks & Mixer", Theme::panel, decks, true);

    auto* lib = new Form (model);
    static const double ranges[5][2] { { 58, 115 }, { 68, 135 }, { 78, 155 }, { 88, 175 }, { 98, 195 } };
    int rangeIdx = 1;
    for (int i = 0; i < 5; ++i) if (std::abs (ranges[i][0] - s.minBpm) < 0.5) rangeIdx = i;
    lib->combo ("BPM analysis range", { "58 - 115", "68 - 135", "78 - 155", "88 - 175", "98 - 195" }, rangeIdx,
                [&s] (int i) { s.minBpm = ranges[i][0]; s.maxBpm = ranges[i][1]; },
                "Tempos outside the range are folded in by halving or doubling (as in Serato). Re-analyse tracks after changing it.");
    lib->toggle ("Auto gain", s.autoGain, [&s] (bool v) { s.autoGain = v; });
    lib->slider ("Auto gain reference", -20.0, -6.0, 0.5, s.targetLufs, [&s] (double v) { s.targetLufs = v; }, " LUFS");
    lib->toggle ("Separate stems after loading", s.autoStems, [&s] (bool v) { s.autoStems = v; },
                 "Built-in DSP separation (preview quality). Needed for ACTIVE PART, Part ISO, FX PART SELECT and Stem FX.");
    lib->toggle ("Show keys as Camelot", s.camelotKeys, [&s] (bool v) { s.camelotKeys = v; });
    tabs_.addTab ("Library & Analysis", Theme::panel, lib, true);

    tabs_.addTab ("Keyboard", Theme::panel, new KeyboardPage(), true);
    addAndMakeVisible (tabs_);
    setSize (760, 560);
}

void SettingsView::resized() { tabs_.setBounds (getLocalBounds()); }

void SettingsView::show (AppModel& model)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new SettingsView (model));
    o.dialogTitle = "DEEJ-AY Settings";
    o.dialogBackgroundColour = Theme::bg;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

} // namespace app
