// Application state shared by every view: the engine, the library, what is loaded on each deck, the
// background loader (decode -> analyse -> stems), settings and persistence. Message thread unless noted.
#pragma once
#include <JuceHeader.h>
#include "dj/Engine.h"
#include "dj/analysis/Analysis.h"
#include "dj/analysis/StemSeparator.h"
#include "dj/library/Library.h"
#include "dj/library/Samples.h"
#include <array>
#include <memory>
#include <mutex>

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
    int deckCount = 4;                        // decks on screen: 2, 4 or 8
    int deckBank = 0;                         // with 8 decks: 0 = decks 1-4, 1 = decks 5-8 (screen and FLX10)
    bool keyboardControl = true;              // the computer keyboard plays decks, pads, sampler and Performer
    bool doubleClickCue = true;               // double-click CUE plays the track from the top
    bool analyseOnImport = true;              // BPM / key / waveform for new tracks in the background
    bool showPerformer = true, showSampler = true;
    juce::String spliceFolder;                // empty: the Splice app's default sounds folder
    juce::String ytDlpPath;                   // empty: look in Homebrew / PATH
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
    juce::String linkStatus;                  // URL box: "YouTube 42%", errors
    int linkGeneration = 0;
};

// A sample in the Splice sounds folder.
struct SpliceSample
{
    juce::File file;
    dj::library::SampleInfo info;
    juce::Time modified;
};

class AppModel : private juce::Timer
{
public:
    AppModel();
    ~AppModel() override;

    static juce::File dataDir();               // ~/Library/Application Support/MP Audio/DEEJ-AY
    static juce::File recordingsDir();         // ~/Music/DEEJ-AY Recordings
    static juce::File downloadsDir();          // ~/Music/DEEJ-AY Downloads (URL box)

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
    void analyseInBackground (const std::vector<std::uint64_t>& ids);  // BPM, key, gain, waveforms; one at a time
    void analyseUnanalysed();
    int analysisPending() const noexcept { return analysisPending_; }
    std::function<void()> onTracksUpdated;                // analysis results arrived (repaint, do not re-sort)
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
    // A file into a sampler slot. Loops (bpm from the name or the length) are set up synced, repeating
    // and key-locked in On/Off mode; one-shots as plain triggers.
    void loadSampleFile (int slot, const juce::File& file, double bpm, bool loop, const juce::String& name);
    // URL box: YouTube / SoundCloud through yt-dlp, Spotify through the user's library (see StreamLinks.h)
    void loadFromLink (int deck, const juce::String& text);
    void importAndLoad (const juce::File& file, int deck, const juce::String& crate);
    juce::File findTool (const juce::String& name, const juce::String& configured) const;

    // ---- Splice (the Splice desktop app's sounds folder)
    juce::File spliceFolder() const;
    void rescanSplice();
    const std::vector<SpliceSample>& spliceSamples() const noexcept { return splice_; }
    bool spliceScanning() const noexcept { return spliceBusy_; }
    std::function<void()> onSpliceChanged;

    // ---- deck banks (8 decks: decks 1-4 or 5-8 on screen and on the controller)
    void setDeckBank (int bank);
    std::function<void()> onLayoutChanged;
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
    void addImported (std::vector<std::pair<juce::String, dj::library::TagInfo>> items, std::vector<double> rates, std::vector<double> lengths,
                      bool analyse = true);
    void applyAnalysis (dj::library::Track& t, const dj::analysis::TrackAnalysis& an, double sampleRate, double lengthSeconds);
    void setLinkStatus (int deck, int generation, const juce::String& text);
    static juce::File waveformCacheFile (std::uint64_t trackId);
    static bool saveWaveforms (std::uint64_t id, const dj::analysis::Waveform& overview, const dj::analysis::Waveform& detail);
    static bool loadWaveforms (std::uint64_t id, dj::analysis::Waveform& overview, dj::analysis::Waveform& detail);
    int deckForAudioId (std::uint64_t id) const;

    juce::ThreadPool pool_ { 3 };
    juce::ThreadPool analysePool_ { 1 };      // background analysis never starves deck loads
    juce::ThreadPool linkPool_ { 2 };         // downloads and link lookups (slow, network)
    int analysisPending_ = 0;
    std::vector<SpliceSample> splice_;
    bool spliceBusy_ = false;
    std::mutex procMutex_;                    // running yt-dlp processes, killed on shutdown
    std::vector<juce::ChildProcess*> procs_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>> (true);
    bool libraryDirty_ = false;
    int ticks_ = 0;
    std::array<juce::String, dj::kSamplerSlots> samplerNames_;
    std::array<std::vector<double>, dj::kNumDecks> taps_;
    juce::File lastRecording_;
};

} // namespace app
