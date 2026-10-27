#include "MidiHost.h"
#include "AppModel.h"

namespace app
{

MidiHost::MidiHost (AppModel& app) : app_ (app), flx_ (app.engine, *this)
{
    refreshDevices();
    startTimerHz (30);
}

MidiHost::~MidiHost()
{
    alive_->store (false);
    stopTimer();
    for (auto& in : inputs_) in->stop();
}

void MidiHost::refreshDevices()
{
    for (auto& in : inputs_) in->stop();
    inputs_.clear();
    output_.reset();
    juce::StringArray names;
    for (const auto& d : juce::MidiInput::getAvailableDevices())
    {
        if (! d.name.containsIgnoreCase ("DDJ-FLX10")) continue;
        if (auto in = juce::MidiInput::openDevice (d.identifier, this))
        {
            in->start();
            inputs_.push_back (std::move (in));
            names.add (d.name);
        }
    }
    for (const auto& d : juce::MidiOutput::getAvailableDevices())
        if (d.name.containsIgnoreCase ("DDJ-FLX10"))
        {
            output_ = juce::MidiOutput::openDevice (d.identifier);
            break;
        }
    flx_.resetOutputCache();
    status_ = names.isEmpty() ? juce::String ("No DDJ-FLX10 connected") : "DDJ-FLX10: " + names.joinIntoString (", ");
}

void MidiHost::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    const auto* raw = m.getRawData();
    if (m.getRawDataSize() < 3 || m.isSysEx()) return;
    flx_.handle ({ raw[0], raw[1], raw[2] });
}

void MidiHost::timerCallback()
{
    // hot-plug: rescan every few seconds while nothing is connected
    if (inputs_.empty() && ++rescanCountdown_ >= 90) { rescanCountdown_ = 0; refreshDevices(); }
    if (output_ == nullptr) return;
    outBuf_.clear();
    flx_.collectOutput (outBuf_);
    for (const auto& m : outBuf_)
        output_->sendMessageNow (juce::MidiMessage (m.status, m.data1, m.data2));
}

// ----------------------------------------------------------------------- ControllerHost
void MidiHost::browse (int delta, bool shift) { onMessageThread ([this, delta, shift] { if (app_.onBrowse) app_.onBrowse (delta, shift); }); }
void MidiHost::loadSelected (int deck) { onMessageThread ([this, deck] { app_.loadTrack (deck, app_.selectedTrack); }); }
void MidiHost::instantDoubles (int toDeck) { onMessageThread ([this, toDeck] { app_.instantDoubles (toDeck); }); }
void MidiHost::back (bool shift) { onMessageThread ([this, shift] { if (app_.onBack) app_.onBack (shift); }); }
void MidiHost::view (bool shift) { onMessageThread ([this, shift] { if (app_.onView) app_.onView (shift); }); }
void MidiHost::previousTrack (int deck) { onMessageThread ([this, deck] { app_.loadPreviousTrack (deck); }); }
void MidiHost::tapTempo (int deck) { onMessageThread ([this, deck] { app_.tapTempo (deck); }); }
void MidiHost::partInstantDoubles (int toDeck, int mask) { onMessageThread ([this, toDeck, mask] { app_.instantDoubles (toDeck, mask); }); }
void MidiHost::padModeChanged (int deck, dj::midi::PadMode) { onMessageThread ([this, deck] { if (app_.onPadModeChanged) app_.onPadModeChanged (deck); }); }
void MidiHost::beatFxSelect (dj::FxType type) { onMessageThread ([this, type] { app_.engine.setFxType (dj::kBeatFxUnit, 0, type); }); }

} // namespace app
