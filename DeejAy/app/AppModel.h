// Application state shared by every view: the engine, the library, what is loaded on each deck, the
// background loader (decode -> analyse -> stems), settings and persistence. Message thread unless noted.
#pragma once
#include <JuceHeader.h>
#include "dj/Engine.h"
#include "dj/analysis/Analysis.h"
#include "dj/analysis/StemSeparator.h"
#include "dj/library/Library.h"
#include <array>
#include <memory>

namespace app
{

class AudioHost;
class MidiHost;

struct AppSettings
{
    double minBpm = 68.0, maxBpm = 135.0;     // Serato's default analysis range
    double targetLufs = -14.0;
    bool autoGain = true;
    bool autoStems = true;                    // separate stems in the background after loading
    bool camelotKeys = true;
    int hotCueWhenPaused = (int) dj::HotCueWhenPaused::Play;
    float brakeSeconds = 0.0f, startSeconds = 0.0f;
    bool slipReverseAutoCancel = true;
    int jogTicksPerRev = 720;
    double quantizeBeats = 1.0;
    bool phonesMixInHardware = true;
    bool faderStart = true;
    bool fourDecks = true;
    int eqMode = (int) dj::EqMode::Classic;
    float xfCurve = 0.75f, xfCutLag = 0.02f, chFaderCurve = 0.5f;

    juce::var toVar() const;
    void fromVar (const juce::var& v);
};

struct DeckTrack
{
    std::uint64_t trackId = 0;
    juce::String title, artist;
    int key = -1;
    double bpm = 0.0;
    double lengthSeconds = 0.0;
    std::shared_ptr<const dj::analysis::Waveform> detail, overview;
    std::vector<std::uint64_t> audioIds;      // engine ids that belong to this load (mix, then mix + stems)
    bool loading = false, stemsBusy = false, stemsReady = false;
    float progress = 0.0f;
    int generation = 0;
    bool loggedPlay = false;
};

class AppModel : private juce::Timer
{
public:
    AppModel();
    ~AppModel() override;

    static juce::File dataDir();               // ~/Library/Application Support/MP Audio/DEEJ-AY
    static juce::File recordingsDir();         // ~/Music/DEEJ-AY Recordings

    dj::Engine engine;
    dj::library::Library library;
    AppSettings settings;
    std::array<DeckTrack, dj::kNumDecks> decks;
    std::unique_ptr<AudioHost> audio;
    std::unique_ptr<MidiHost> midi;

    // ---- library
    void importPaths (const juce::StringArray& paths);    // files and folders (recursively), in the background
    void importSeratoCrates();                            // ~/Music/_Serato_/Subcrates
    void markLibraryDirty() { libraryDirty_ = true; }
    void saveLibrary();
    std::uint64_t selectedTrack = 0;                      // the library selection (controller loads this)
    std::vector<std::uint64_t> visibleTracks;             // current list in the browser
    std::function<void()> onLibraryChanged;
    std::function<void (int delta, bool shift)> onBrowse; // controller rotary
    std::function<void (bool shift)> onBack, onView;
    std::function<void (int deck)> onDeckChanged;
    std::function<void (int deck)> onPadModeChanged;
    std::function<void (const juce::String&)> onStatus;

    // ---- decks
    void loadTrack (int deck, std::uint64_t trackId);
    void loadPreviousTrack (int deck);
    void loadToSampler (int slot, std::uint64_t trackId);
    void instantDoubles (int toDeck, int partMask = 0);
    void tapTempo (int deck);
    void separateStems (int deck);
    juce::String samplerName (int slot) const { return samplerNames_[(std::size_t) slot]; }

    // ---- settings / recording
    void applySettings();
    void saveSettings();
    bool startRecording();
    void stopRecording();
    juce::File lastRecording() const { return lastRecording_; }

private:
    void timerCallback() override;
    void handleEvent (const dj::DeckEvent& e);
    void finishLoad (int deck, int generation, std::uint64_t trackId, std::shared_ptr<dj::TrackAudio> audio,
                     dj::analysis::TrackAnalysis analysis, bool analysed);
    void addImported (std::vector<std::pair<juce::String, dj::library::TagInfo>> items, std::vector<double> rates, std::vector<double> lengths);
    static juce::File waveformCacheFile (std::uint64_t trackId);
    static bool saveWaveforms (std::uint64_t id, const dj::analysis::Waveform& overview, const dj::analysis::Waveform& detail);
    static bool loadWaveforms (std::uint64_t id, dj::analysis::Waveform& overview, dj::analysis::Waveform& detail);
    int deckForAudioId (std::uint64_t id) const;

    juce::ThreadPool pool_ { 3 };
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>> (true);
    bool libraryDirty_ = false;
    int ticks_ = 0;
    std::array<juce::String, dj::kSamplerSlots> samplerNames_;
    std::array<std::vector<double>, dj::kNumDecks> taps_;
    juce::File lastRecording_;
};

} // namespace app
