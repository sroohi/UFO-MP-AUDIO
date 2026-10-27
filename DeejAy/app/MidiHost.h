// MIDI devices: a connected DDJ-FLX10 is driven by dj::midi::Flx10 on the MIDI thread; its LEDs and
// On Jog Displays are refreshed from a timer. Library/UI requests from the controller are marshalled
// to the message thread.
#pragma once
#include <JuceHeader.h>
#include "dj/midi/Flx10.h"

namespace app
{

class AppModel;

class MidiHost : private juce::MidiInputCallback, private juce::Timer, private dj::midi::ControllerHost
{
public:
    explicit MidiHost (AppModel& app);
    ~MidiHost() override;

    void refreshDevices();
    juce::String status() const { return status_; }
    bool controllerConnected() const { return output_ != nullptr || ! inputs_.empty(); }
    dj::midi::Flx10& flx10() { return flx_; }

private:
    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message) override;
    void timerCallback() override;

    // ControllerHost (called on the MIDI thread)
    void browse (int delta, bool shift) override;
    void loadSelected (int deck) override;
    void instantDoubles (int toDeck) override;
    void back (bool shift) override;
    void view (bool shift) override;
    void previousTrack (int deck) override;
    void tapTempo (int deck) override;
    void partInstantDoubles (int toDeck, int stemMask) override;
    void padModeChanged (int deck, dj::midi::PadMode mode) override;
    void beatFxSelect (dj::FxType type) override;

    template <typename Fn> void onMessageThread (Fn&& fn)
    {
        juce::MessageManager::callAsync ([alive = alive_, f = std::forward<Fn> (fn)]() mutable { if (alive->load()) f(); });
    }

    AppModel& app_;
    dj::midi::Flx10 flx_;
    std::vector<std::unique_ptr<juce::MidiInput>> inputs_;
    std::unique_ptr<juce::MidiOutput> output_;
    std::vector<dj::midi::Message> outBuf_;
    juce::String status_ { "No controller" };
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>> (true);
    int rescanCountdown_ = 0;
};

} // namespace app
