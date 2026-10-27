#include "AppModel.h"
#include "AudioHost.h"
#include "MidiHost.h"

namespace app
{

namespace
{
    const char* kAudioWildcard = "*.mp3;*.m4a;*.aac;*.mp4;*.aif;*.aiff;*.wav;*.flac;*.ogg;*.alac";

    std::int64_t nowSeconds() { return (std::int64_t) (juce::Time::currentTimeMillis() / 1000); }

    // Decodes a file at its native sample rate into interleaved stereo float.
    std::shared_ptr<dj::TrackAudio> decode (const juce::File& file)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
        if (r == nullptr || r->lengthInSamples <= 0 || r->sampleRate <= 0) return nullptr;
        const auto frames = (std::int64_t) r->lengthInSamples;
        std::vector<float> mix ((std::size_t) frames * 2);
        const int chunk = 1 << 16;
        juce::AudioBuffer<float> buf ((int) juce::jmax (2u, r->numChannels), chunk);
        for (std::int64_t pos = 0; pos < frames; pos += chunk)
        {
            const int n = (int) juce::jmin<std::int64_t> (chunk, frames - pos);
            buf.clear();
            r->read (&buf, 0, n, pos, true, true);
            const float* l = buf.getReadPointer (0);
            const float* rr = r->numChannels > 1 ? buf.getReadPointer (1) : l;
            for (int i = 0; i < n; ++i)
            {
                mix[(std::size_t) (pos + i) * 2] = l[i];
                mix[(std::size_t) (pos + i) * 2 + 1] = rr[i];
            }
        }
        auto t = std::make_shared<dj::TrackAudio>();
        t->setMix (std::move (mix), r->sampleRate);
        return t;
    }

    struct Scanned
    {
        std::vector<std::pair<juce::String, dj::library::TagInfo>> items;
        std::vector<double> rates, lengths;
    };

    Scanned scan (const std::vector<juce::File>& files)
    {
        Scanned s;
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        for (const auto& f : files)
        {
            dj::library::TagInfo tags;
            dj::library::readTags (f.getFullPathName().toStdString(), tags);
            double sr = 0.0, len = 0.0;
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (f) })
            {
                sr = r->sampleRate;
                len = sr > 0 ? (double) r->lengthInSamples / sr : 0.0;
            }
            if (sr <= 0.0) continue; // not decodable
            s.items.emplace_back (f.getFullPathName(), std::move (tags));
            s.rates.push_back (sr);
            s.lengths.push_back (len);
        }
        return s;
    }
}

// ------------------------------------------------------------------------------ settings
juce::var AppSettings::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("minBpm", minBpm); o->setProperty ("maxBpm", maxBpm); o->setProperty ("targetLufs", targetLufs);
    o->setProperty ("autoGain", autoGain); o->setProperty ("autoStems", autoStems); o->setProperty ("camelotKeys", camelotKeys);
    o->setProperty ("hotCueWhenPaused", hotCueWhenPaused); o->setProperty ("brakeSeconds", brakeSeconds); o->setProperty ("startSeconds", startSeconds);
    o->setProperty ("slipReverseAutoCancel", slipReverseAutoCancel); o->setProperty ("jogTicksPerRev", jogTicksPerRev);
    o->setProperty ("quantizeBeats", quantizeBeats); o->setProperty ("phonesMixInHardware", phonesMixInHardware);
    o->setProperty ("faderStart", faderStart); o->setProperty ("fourDecks", fourDecks); o->setProperty ("eqMode", eqMode);
    o->setProperty ("xfCurve", xfCurve); o->setProperty ("xfCutLag", xfCutLag); o->setProperty ("chFaderCurve", chFaderCurve);
    return juce::var (o);
}

void AppSettings::fromVar (const juce::var& v)
{
    if (! v.isObject()) return;
    auto get = [&] (const char* k, auto def) { return v.hasProperty (k) ? (decltype (def)) v[k] : def; };
    minBpm = get ("minBpm", minBpm); maxBpm = get ("maxBpm", maxBpm); targetLufs = get ("targetLufs", targetLufs);
    autoGain = get ("autoGain", autoGain); autoStems = get ("autoStems", autoStems); camelotKeys = get ("camelotKeys", camelotKeys);
    hotCueWhenPaused = get ("hotCueWhenPaused", hotCueWhenPaused);
    brakeSeconds = (float) get ("brakeSeconds", (double) brakeSeconds); startSeconds = (float) get ("startSeconds", (double) startSeconds);
    slipReverseAutoCancel = get ("slipReverseAutoCancel", slipReverseAutoCancel); jogTicksPerRev = get ("jogTicksPerRev", jogTicksPerRev);
    quantizeBeats = get ("quantizeBeats", quantizeBeats); phonesMixInHardware = get ("phonesMixInHardware", phonesMixInHardware);
    faderStart = get ("faderStart", faderStart); fourDecks = get ("fourDecks", fourDecks); eqMode = get ("eqMode", eqMode);
    xfCurve = (float) get ("xfCurve", (double) xfCurve); xfCutLag = (float) get ("xfCutLag", (double) xfCutLag);
    chFaderCurve = (float) get ("chFaderCurve", (double) chFaderCurve);
}

// ----------------------------------------------------------------------------- lifecycle
juce::File AppModel::dataDir()
{
#if JUCE_MAC
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support/MP Audio/DEEJ-AY");
#else
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("MP Audio/DEEJ-AY");
#endif
    d.createDirectory();
    return d;
}

juce::File AppModel::recordingsDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("DEEJ-AY Recordings");
    d.createDirectory();
    return d;
}

AppModel::AppModel()
{
    settings.fromVar (juce::JSON::parse (dataDir().getChildFile ("settings.json")));
    std::string err;
    const auto libFile = dataDir().getChildFile ("library.json");
    if (libFile.existsAsFile() && ! library.load (libFile.getFullPathName().toStdString(), &err))
        libFile.copyFileTo (libFile.withFileExtension ("broken.json")); // keep the unreadable one
    library.beginSession (nowSeconds());

    audio = std::make_unique<AudioHost> (engine);
    const auto audioErr = audio->initialise (dataDir().getChildFile ("audio-device.xml"));
    midi = std::make_unique<MidiHost> (*this);
    applySettings();
    if (audioErr.isNotEmpty() && onStatus) onStatus (audioErr);
    startTimerHz (30);
}

AppModel::~AppModel()
{
    stopTimer();
    alive_->store (false);
    pool_.removeAllJobs (true, 8000);
    stopRecording();
    midi.reset();
    if (audio != nullptr) audio->saveState (dataDir().getChildFile ("audio-device.xml"));
    audio.reset();
    saveLibrary();
    saveSettings();
}

void AppModel::saveSettings()
{
    dataDir().getChildFile ("settings.json").replaceWithText (juce::JSON::toString (settings.toVar()));
}

void AppModel::saveLibrary()
{
    std::string err;
    if (library.save (dataDir().getChildFile ("library.json").getFullPathName().toStdString(), &err))
        libraryDirty_ = false;
    else if (onStatus) onStatus ("Library save failed: " + juce::String (err));
}

void AppModel::applySettings()
{
    for (int d = 0; d < dj::kNumDecks; ++d)
    {
        auto& in = engine.deck (d).inputs;
        in.hotCueWhenPaused.store (settings.hotCueWhenPaused);
        in.brakeSeconds.store (settings.brakeSeconds);
        in.startSeconds.store (settings.startSeconds);
        in.slipReverseAutoCancel.store (settings.slipReverseAutoCancel);
        in.ticksPerRevolution.store (settings.jogTicksPerRev);
    }
    engine.mixer.phonesMixInHardware.store (settings.phonesMixInHardware);
    engine.mixer.eqMode.store (settings.eqMode);
    engine.mixer.xfCurve.store (settings.xfCurve);
    engine.mixer.xfCutLag.store (settings.xfCutLag);
    engine.mixer.chFaderCurve.store (settings.chFaderCurve);
    dj::Command q;
    q.type = dj::Cmd::QuantizeBeats;
    q.value = settings.quantizeBeats;
    engine.post (q);
    if (midi != nullptr) midi->flx10().faderStart.store (settings.faderStart);
    for (int d = 0; d < dj::kNumDecks; ++d)
        engine.mixer.ch[(std::size_t) d].autoGainDb.store (settings.autoGain && decks[(std::size_t) d].trackId != 0 && library.find (decks[(std::size_t) d].trackId) != nullptr
                                                               ? (float) library.find (decks[(std::size_t) d].trackId)->autoGainDb : 0.0f);
}

// ------------------------------------------------------------------------------- library
void AppModel::importPaths (const juce::StringArray& paths)
{
    std::vector<juce::File> files;
    for (const auto& p : paths)
    {
        const juce::File f (p);
        if (f.isDirectory())
            for (const auto& c : f.findChildFiles (juce::File::findFiles, true, kAudioWildcard))
                files.push_back (c);
        else if (f.existsAsFile() && f.hasFileExtension (juce::String (kAudioWildcard).removeCharacters ("*")))
            files.push_back (f);
    }
    if (files.empty()) return;
    if (onStatus) onStatus ("Importing " + juce::String ((int) files.size()) + " files...");
    pool_.addJob ([this, alive = alive_, files]
    {
        for (std::size_t off = 0; off < files.size() && alive->load(); off += 200)
        {
            const std::vector<juce::File> chunk (files.begin() + (std::ptrdiff_t) off, files.begin() + (std::ptrdiff_t) std::min (files.size(), off + 200));
            auto s = std::make_shared<Scanned> (scan (chunk));
            juce::MessageManager::callAsync ([this, alive, s]
            {
                if (alive->load()) addImported (std::move (s->items), std::move (s->rates), std::move (s->lengths));
            });
        }
    });
}

void AppModel::addImported (std::vector<std::pair<juce::String, dj::library::TagInfo>> items, std::vector<double> rates, std::vector<double> lengths)
{
    const auto now = nowSeconds();
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const auto& [path, tags] = items[i];
        auto& t = library.addOrUpdate (path.toStdString(), tags, now);
        t.sampleRate = rates[i];
        t.lengthSeconds = lengths[i];
        bool hasCues = false;
        for (const auto& c : t.hotCues) hasCues = hasCues || c.pos >= 0.0;
        if (! hasCues) dj::library::Library::applySeratoPrep (t, tags, rates[i]);
    }
    markLibraryDirty();
    if (onLibraryChanged) onLibraryChanged();
    if (onStatus) onStatus ("Imported " + juce::String ((int) items.size()) + " tracks");
}

void AppModel::importSeratoCrates()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("_Serato_/Subcrates");
    if (! dir.isDirectory()) { if (onStatus) onStatus ("No Serato library found in ~/Music/_Serato_"); return; }
    std::vector<std::pair<std::string, std::vector<juce::File>>> crates;
    std::vector<juce::File> all;
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.crate"))
    {
        std::vector<juce::File> files;
        for (const auto& p : dj::library::Library::readSeratoCrate (f.getFullPathName().toStdString()))
        {
            const juce::String path = juce::String (p).startsWith ("/") ? juce::String (p) : "/" + juce::String (p);
            const juce::File tf (path);
            if (tf.existsAsFile()) { files.push_back (tf); all.push_back (tf); }
        }
        crates.emplace_back (f.getFileNameWithoutExtension().replace ("%%", "/").toStdString(), std::move (files));
    }
    if (onStatus) onStatus ("Importing " + juce::String ((int) crates.size()) + " Serato crates...");
    pool_.addJob ([this, alive = alive_, crates, all]
    {
        auto s = std::make_shared<Scanned> (scan (all));
        juce::MessageManager::callAsync ([this, alive, s, crates]
        {
            if (! alive->load()) return;
            addImported (std::move (s->items), std::move (s->rates), std::move (s->lengths));
            for (const auto& [name, files] : crates)
            {
                library.crate (name);
                for (const auto& f : files)
                    if (auto* t = library.findByPath (f.getFullPathName().toStdString()))
                        library.addToCrate (name, t->id);
            }
            markLibraryDirty();
            if (onLibraryChanged) onLibraryChanged();
        });
    });
}

// --------------------------------------------------------------------------------- decks
void AppModel::loadTrack (int deck, std::uint64_t trackId)
{
    const auto* t = library.find (trackId);
    if (t == nullptr || deck < 0 || deck >= dj::kNumDecks) return;
    // Serato will not load onto a playing deck from the controller; neither do we
    if (engine.deck (deck).view.playing.load()) { if (onStatus) onStatus ("Deck " + juce::String (deck + 1) + " is playing"); return; }
    auto& dt = decks[(std::size_t) deck];
    const int generation = ++dt.generation;
    dt.trackId = trackId;
    dt.title = t->title; dt.artist = t->artist; dt.key = t->key; dt.bpm = t->bpm; dt.lengthSeconds = t->lengthSeconds;
    dt.detail.reset(); dt.overview.reset(); dt.audioIds.clear();
    dt.loading = true; dt.stemsBusy = dt.stemsReady = false; dt.loggedPlay = false; dt.progress = 0.0f;
    if (onDeckChanged) onDeckChanged (deck);

    const juce::File file (t->path);
    const bool needAnalysis = ! t->analysed;
    dj::analysis::AnalysisSettings as;
    as.minBpm = settings.minBpm; as.maxBpm = settings.maxBpm; as.targetLufs = settings.targetLufs;
    pool_.addJob ([this, alive = alive_, deck, generation, trackId, file, needAnalysis, as]
    {
        auto audioData = decode (file);
        dj::analysis::TrackAnalysis an;
        bool analysed = false;
        if (audioData != nullptr)
        {
            if (needAnalysis || ! loadWaveforms (trackId, an.overview, an.detail))
            {
                an = dj::analysis::analyseTrack (*audioData, as);
                analysed = true;
                saveWaveforms (trackId, an.overview, an.detail);
            }
        }
        juce::MessageManager::callAsync ([this, alive, deck, generation, trackId, audioData, result = std::move (an), analysed]() mutable
        {
            if (alive->load()) finishLoad (deck, generation, trackId, std::move (audioData), std::move (result), analysed);
        });
    });
}

void AppModel::finishLoad (int deck, int generation, std::uint64_t trackId, std::shared_ptr<dj::TrackAudio> audioData,
                           dj::analysis::TrackAnalysis an, bool analysed)
{
    auto& dt = decks[(std::size_t) deck];
    if (dt.generation != generation) return;            // a newer load replaced this one
    dt.loading = false;
    auto* t = library.find (trackId);
    if (audioData == nullptr || t == nullptr)
    {
        dt.trackId = 0;
        if (onStatus) onStatus ("Could not decode " + (t != nullptr ? juce::String (t->path) : juce::String()));
        if (onDeckChanged) onDeckChanged (deck);
        return;
    }
    if (analysed)
    {
        // a grid the DJ edited or imported from Serato wins over the analysis
        if (! t->gridLocked && an.beat.bpm > 0.0) { t->bpm = an.beat.bpm; t->gridAnchor = an.beat.anchor; }
        if (t->key < 0) t->key = an.key;
        t->loudnessLufs = an.loudnessLufs;
        t->autoGainDb = an.autoGainDb;
        t->sampleRate = audioData->sampleRate;
        t->lengthSeconds = audioData->lengthSeconds();
        t->analysed = true;
        markLibraryDirty();
    }
    dt.overview = std::make_shared<dj::analysis::Waveform> (std::move (an.overview));
    dt.detail = std::make_shared<dj::analysis::Waveform> (std::move (an.detail));
    dt.bpm = t->bpm; dt.key = t->key; dt.lengthSeconds = t->lengthSeconds;

    engine.loadDeck (deck, audioData);
    dt.audioIds = { engine.deckAudioId (deck) };
    auto post = [&] (dj::Cmd type, int index, double a, double b = 0.0, bool flag = false)
    {
        dj::Command c; c.type = type; c.deck = deck; c.index = index; c.value = a; c.value2 = b; c.flag = flag;
        engine.post (c);
    };
    if (t->bpm > 0.0) post (dj::Cmd::DeckSetGrid, 0, t->bpm, t->gridAnchor);
    post (dj::Cmd::DeckSetCuePoint, 0, t->cuePoint);
    for (int i = 0; i < 8; ++i)
    {
        if (t->hotCues[(std::size_t) i].pos >= 0.0) post (dj::Cmd::DeckSetHotCue, i, t->hotCues[(std::size_t) i].pos);
        const auto& l = t->loops[(std::size_t) i];
        if (l.out > l.in && l.in >= 0.0) post (dj::Cmd::DeckSetLoopSlot, i, l.in, l.out, l.locked);
    }
    post (dj::Cmd::DeckSetKey, 0, (double) t->key);
    engine.mixer.ch[(std::size_t) deck].autoGainDb.store (settings.autoGain ? (float) t->autoGainDb : 0.0f);
    if (onDeckChanged) onDeckChanged (deck);
    if (settings.autoStems) separateStems (deck);
}

void AppModel::separateStems (int deck)
{
    auto& dt = decks[(std::size_t) deck];
    if (dt.stemsBusy || dt.stemsReady || dt.audioIds.empty()) return;
    auto source = engine.audioFor (dt.audioIds.back());
    if (source == nullptr || source->hasStems) return;
    dt.stemsBusy = true;
    const int generation = dt.generation;
    const auto oldId = dt.audioIds.back();
    if (onDeckChanged) onDeckChanged (deck);
    pool_.addJob ([this, alive = alive_, deck, generation, oldId, source]
    {
        dj::analysis::StemSeparation st;
        auto separator = dj::analysis::makeDefaultStemSeparator();
        std::shared_ptr<dj::TrackAudio> withStems;
        if (separator->separate (*source, st, [alive] (double) { return alive->load(); }))
        {
            withStems = std::make_shared<dj::TrackAudio>();
            withStems->setMix (source->mix, source->sampleRate);
            withStems->setStems (st.vocals, st.bass, st.drums);
        }
        juce::MessageManager::callAsync ([this, alive, deck, generation, oldId, withStems]
        {
            if (! alive->load()) return;
            auto& d = decks[(std::size_t) deck];
            if (d.generation == generation) d.stemsBusy = false;
            if (withStems == nullptr) return;
            engine.replaceDeckAudio (oldId, withStems);
            // every deck that played this audio (Instant Doubles) now has the stems version
            for (int k = 0; k < dj::kNumDecks; ++k)
            {
                auto& o = decks[(std::size_t) k];
                if (std::find (o.audioIds.begin(), o.audioIds.end(), oldId) != o.audioIds.end())
                {
                    o.audioIds.push_back (engine.deckAudioId (k));
                    o.stemsReady = true;
                    o.stemsBusy = false;
                    if (onDeckChanged) onDeckChanged (k);
                }
            }
        });
    });
}

void AppModel::loadPreviousTrack (int deck)
{
    const auto current = decks[(std::size_t) deck].trackId;
    const auto it = std::find (visibleTracks.begin(), visibleTracks.end(), current);
    if (it != visibleTracks.end() && it != visibleTracks.begin()) loadTrack (deck, *(it - 1));
}

void AppModel::loadToSampler (int slot, std::uint64_t trackId)
{
    const auto* t = library.find (trackId);
    if (t == nullptr || slot < 0 || slot >= dj::kSamplerSlots) return;
    samplerNames_[(std::size_t) slot] = t->title;
    const juce::File file (t->path);
    const double bpm = t->bpm;
    pool_.addJob ([this, alive = alive_, slot, file, bpm]
    {
        auto audioData = decode (file);
        juce::MessageManager::callAsync ([this, alive, slot, audioData, bpm]
        {
            if (alive->load() && audioData != nullptr) engine.loadSampler (slot, audioData, bpm);
        });
    });
}

void AppModel::instantDoubles (int toDeck, int partMask)
{
    int from = -1;
    const int leader = engine.view.leader.load();
    if (leader >= 0 && leader != toDeck && engine.deck (leader).view.loaded.load()) from = leader;
    for (int d = 0; d < dj::kNumDecks && from < 0; ++d)
        if (d != toDeck && engine.deck (d).view.playing.load()) from = d;
    for (int d = 0; d < dj::kNumDecks && from < 0; ++d)
        if (d != toDeck && engine.deck (d).view.loaded.load()) from = d;
    if (from < 0) return;
    engine.instantDouble (from, toDeck, partMask);
    const int generation = decks[(std::size_t) toDeck].generation + 1;
    decks[(std::size_t) toDeck] = decks[(std::size_t) from];
    decks[(std::size_t) toDeck].generation = generation;
    decks[(std::size_t) toDeck].loggedPlay = true;
    engine.mixer.ch[(std::size_t) toDeck].autoGainDb.store (engine.mixer.ch[(std::size_t) from].autoGainDb.load());
    if (onDeckChanged) onDeckChanged (toDeck);
}

void AppModel::tapTempo (int deck)
{
    auto& taps = taps_[(std::size_t) deck];
    const double t = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (! taps.empty() && t - taps.back() > 2.0) taps.clear();
    taps.push_back (t);
    if (taps.size() > 8) taps.erase (taps.begin());
    if (taps.size() < 4) return;
    const double bpm = 60.0 * (double) (taps.size() - 1) / (taps.back() - taps.front());
    auto& dt = decks[(std::size_t) deck];
    auto* tr = library.find (dt.trackId);
    if (tr == nullptr || bpm < 40.0 || bpm > 250.0) return;
    tr->bpm = std::round (bpm * 100.0) / 100.0;
    tr->gridLocked = true;
    dt.bpm = tr->bpm;
    dj::Command c; c.type = dj::Cmd::DeckSetGrid; c.deck = deck; c.value = tr->bpm; c.value2 = tr->gridAnchor;
    engine.post (c);
    markLibraryDirty();
    if (onDeckChanged) onDeckChanged (deck);
}

// --------------------------------------------------------------------------- recording
bool AppModel::startRecording()
{
    const auto name = "DEEJ-AY " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ".wav";
    lastRecording_ = recordingsDir().getChildFile (name);
    const double sr = audio != nullptr ? audio->sampleRate() : 44100.0;
    return engine.recorder.start (lastRecording_.getFullPathName().toStdString(), sr, 24);
}

void AppModel::stopRecording()
{
    if (engine.recorder.recording()) engine.recorder.stop();
}

// ------------------------------------------------------------------------- engine events
int AppModel::deckForAudioId (std::uint64_t id) const
{
    for (int d = 0; d < dj::kNumDecks; ++d)
        if (std::find (decks[(std::size_t) d].audioIds.begin(), decks[(std::size_t) d].audioIds.end(), id) != decks[(std::size_t) d].audioIds.end())
            return d;
    return -1;
}

void AppModel::handleEvent (const dj::DeckEvent& e)
{
    if (e.deck < 0 || e.deck >= dj::kNumDecks) return;
    const auto& dt = decks[(std::size_t) e.deck];
    if (std::find (dt.audioIds.begin(), dt.audioIds.end(), e.audioId) == dt.audioIds.end()) return;
    auto* t = library.find (dt.trackId);
    if (t == nullptr) return;
    using K = dj::DeckEvent::Kind;
    switch (e.kind)
    {
        case K::CueSet: t->cuePoint = e.a; break;
        case K::HotCueSet: if (e.index >= 0 && e.index < 8) t->hotCues[(std::size_t) e.index].pos = e.a; break;
        case K::HotCueDeleted: if (e.index >= 0 && e.index < 8) t->hotCues[(std::size_t) e.index] = {}; break;
        case K::LoopSlotSet: if (e.index >= 0 && e.index < 8) { t->loops[(std::size_t) e.index].in = e.a; t->loops[(std::size_t) e.index].out = e.b; } break;
        case K::LoopSlotDeleted: if (e.index >= 0 && e.index < 8) t->loops[(std::size_t) e.index] = {}; break;
        case K::LoopSlotLocked: if (e.index >= 0 && e.index < 8) t->loops[(std::size_t) e.index].locked = e.a > 0.5; break;
        case K::GridChanged:
            t->bpm = e.a; t->gridAnchor = e.b; t->gridLocked = true;
            decks[(std::size_t) e.deck].bpm = e.a;
            break;
        case K::TrackEnded: case K::AudioReleased: return;
    }
    markLibraryDirty();
}

void AppModel::timerCallback()
{
    engine.pollEvents ([this] (const dj::DeckEvent& e) { handleEvent (e); });
    for (int d = 0; d < dj::kNumDecks; ++d)
    {
        auto& dt = decks[(std::size_t) d];
        if (dt.trackId != 0 && ! dt.loggedPlay && engine.deck (d).view.playing.load())
        {
            library.logPlay (dt.trackId, d, nowSeconds());
            dt.loggedPlay = true;
            markLibraryDirty();
        }
    }
    if (++ticks_ % 300 == 0 && libraryDirty_) saveLibrary();   // every 10 s
}

// --------------------------------------------------------------------- waveform cache
juce::File AppModel::waveformCacheFile (std::uint64_t id)
{
    auto d = dataDir().getChildFile ("Analysis");
    d.createDirectory();
    return d.getChildFile (juce::String ((juce::int64) id) + ".djwf");
}

bool AppModel::saveWaveforms (std::uint64_t id, const dj::analysis::Waveform& overview, const dj::analysis::Waveform& detail)
{
    juce::FileOutputStream out (waveformCacheFile (id));
    if (! out.openedOk()) return false;
    out.setPosition (0);
    out.truncate();
    out.write ("DJWF", 4);
    out.writeInt (1);
    for (const auto* w : { &overview, &detail })
    {
        out.writeDouble (w->binsPerSecond);
        out.writeInt ((int) w->size());
        for (const auto* v : { &w->peak, &w->low, &w->mid, &w->high }) out.write (v->data(), v->size());
    }
    return true;
}

bool AppModel::loadWaveforms (std::uint64_t id, dj::analysis::Waveform& overview, dj::analysis::Waveform& detail)
{
    juce::FileInputStream in (waveformCacheFile (id));
    if (! in.openedOk()) return false;
    char magic[4];
    if (in.read (magic, 4) != 4 || std::memcmp (magic, "DJWF", 4) != 0 || in.readInt() != 1) return false;
    for (auto* w : { &overview, &detail })
    {
        w->binsPerSecond = in.readDouble();
        const int n = in.readInt();
        if (n <= 0 || n > 50'000'000) return false;
        for (auto* v : { &w->peak, &w->low, &w->mid, &w->high })
        {
            v->resize ((std::size_t) n);
            if (in.read (v->data(), n) != n) return false;
        }
    }
    return true;
}

} // namespace app
