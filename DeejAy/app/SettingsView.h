// Settings: audio device + routing, DDJ-FLX10 options, deck / mixer behaviour, analysis, keyboard map.
#pragma once
#include "AppModel.h"
#include "AudioHost.h"
#include "MidiHost.h"
#include "Widgets.h"

namespace app
{

class SettingsView : public juce::Component
{
public:
    explicit SettingsView (AppModel& model);
    void resized() override;
    void paint (juce::Graphics& g) override { g.fillAll (Theme::bg); }
    static void show (AppModel& model);
private:
    AppModel& model_;
    juce::TabbedComponent tabs_ { juce::TabbedButtonBar::TabsAtTop };
};

} // namespace app
