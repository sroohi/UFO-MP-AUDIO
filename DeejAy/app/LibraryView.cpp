#include "LibraryView.h"

namespace app
{

namespace
{
    enum Col { ColNum = 1, ColTitle, ColArtist, ColBpm, ColKey, ColLength, ColAlbum, ColGenre, ColComment, ColAdded, ColPlays };
    dj::library::SortColumn sortFor (int col)
    {
        using S = dj::library::SortColumn;
        switch (col)
        {
            case ColArtist: return S::Artist;
            case ColBpm: return S::Bpm;
            case ColKey: return S::Key;
            case ColLength: return S::Length;
            case ColAlbum: return S::Album;
            case ColGenre: return S::Genre;
            case ColComment: return S::Comment;
            case ColAdded: return S::Added;
            case ColPlays: return S::PlayCount;
            default: return S::Title;
        }
    }
}

LibraryView::LibraryView (AppModel& model) : model_ (model)
{
    auto& h = table_.getHeader();
    h.addColumn ("#", ColNum, 34, 30, 50, juce::TableHeaderComponent::notSortable);
    h.addColumn ("Title", ColTitle, 260);
    h.addColumn ("Artist", ColArtist, 180);
    h.addColumn ("BPM", ColBpm, 56);
    h.addColumn ("Key", ColKey, 48);
    h.addColumn ("Length", ColLength, 60);
    h.addColumn ("Album", ColAlbum, 150);
    h.addColumn ("Genre", ColGenre, 100);
    h.addColumn ("Comment", ColComment, 150);
    h.addColumn ("Added", ColAdded, 86);
    h.addColumn ("Plays", ColPlays, 44);
    table_.setMultipleSelectionEnabled (true);
    table_.setRowHeight (20);
    table_.setColour (juce::ListBox::backgroundColourId, Theme::bg);
    h.setColour (juce::TableHeaderComponent::backgroundColourId, Theme::panelHi);
    h.setColour (juce::TableHeaderComponent::textColourId, Theme::dim);
    addAndMakeVisible (table_);

    sourceList_.setRowHeight (20);
    sourceList_.setColour (juce::ListBox::backgroundColourId, Theme::panel);
    addAndMakeVisible (sourceList_);

    search_.setTextToShowWhenEmpty ("Search  (title, artist, BPM, key, genre...)", Theme::dim);
    search_.setColour (juce::TextEditor::backgroundColourId, Theme::panelHi);
    search_.setColour (juce::TextEditor::textColourId, Theme::text);
    search_.onTextChange = [this] { refreshTracks(); };
    search_.onEscapeKey = [this] { search_.clear(); refreshTracks(); table_.grabKeyboardFocus(); };
    addAndMakeVisible (search_);

    for (auto* b : { &addFiles_, &addFolder_, &seratoImport_, &newCrate_, &newSmart_ }) { styleToggle (*b, Theme::accent); addAndMakeVisible (b); }
    addFiles_.onClick = [this]
    {
        chooser_ = std::make_unique<juce::FileChooser> ("Add tracks", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.mp3;*.m4a;*.aac;*.aif;*.aiff;*.wav;*.flac;*.ogg");
        chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                               [this] (const juce::FileChooser& fc)
                               {
                                   juce::StringArray paths;
                                   for (const auto& f : fc.getResults()) paths.add (f.getFullPathName());
                                   model_.importPaths (paths);
                               });
    };
    addFolder_.onClick = [this]
    {
        chooser_ = std::make_unique<juce::FileChooser> ("Add a folder", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                               [this] (const juce::FileChooser& fc) { if (fc.getResult().isDirectory()) model_.importPaths ({ fc.getResult().getFullPathName() }); });
    };
    seratoImport_.setTooltip ("Import crates, cue points, loops and beatgrids from your Serato library (~/Music/_Serato_)");
    seratoImport_.onClick = [this] { model_.importSeratoCrates(); };
    newCrate_.onClick = [this] { newCrate(); };
    newSmart_.onClick = [this] { newSmartCrate(); };

    for (auto* b : { &analyse_, &spliceOpen_, &spliceFolder_, &spliceRescan_ }) { styleToggle (*b, Theme::accent); addChildComponent (b); }
    analyse_.setTooltip ("Detect BPM, key and loudness for every track that has not been analysed yet (in the background)");
    analyse_.onClick = [this] { model_.analyseUnanalysed(); };
    spliceOpen_.setTooltip ("Sign in and browse in the Splice app: the sounds you download there appear in this list");
    spliceOpen_.onClick = []
    {
        const juce::File app ("/Applications/Splice.app");
        if (app.exists()) app.startAsProcess();
        else juce::URL ("https://splice.com/sounds").launchInDefaultBrowser();
    };
    spliceFolder_.setTooltip ("The folder the Splice app downloads into (Splice > Preferences > Splice folder)");
    spliceFolder_.onClick = [this]
    {
        chooser_ = std::make_unique<juce::FileChooser> ("Splice sounds folder", model_.spliceFolder().isDirectory() ? model_.spliceFolder() : juce::File::getSpecialLocation (juce::File::userHomeDirectory));
        chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
        {
            if (! fc.getResult().isDirectory()) return;
            model_.settings.spliceFolder = fc.getResult().getFullPathName();
            model_.saveSettings();
            model_.rescanSplice();
        });
    };
    spliceRescan_.onClick = [this] { model_.rescanSplice(); };
    model_.onSpliceChanged = [this] { if (splice()) refreshTracks(); repaint(); };
    rebuild();
    updateToolbar();
}

void LibraryView::updateToolbar()
{
    const bool s = splice();
    for (auto* b : { &addFiles_, &addFolder_, &seratoImport_, &newCrate_, &newSmart_, &analyse_ }) b->setVisible (! s);
    for (auto* b : { &spliceOpen_, &spliceFolder_, &spliceRescan_ }) b->setVisible (s);
    auto& h = table_.getHeader();
    h.setColumnName (ColArtist, s ? "Pack" : "Artist");
    h.setColumnName (ColAlbum, s ? "Type" : "Album");
    h.setColumnName (ColAdded, s ? "Downloaded" : "Added");
    resized();
}

void LibraryView::showSplice()
{
    for (int i = 0; i < (int) sources_.size(); ++i)
        if (sources_[(std::size_t) i].kind == Source::Kind::Splice)
        {
            currentSource_ = i;
            sourceList_.selectRow (i);
            sortColumn_ = 0;
            model_.rescanSplice();
            startTimer (30000);
            refreshTracks();
            updateToolbar();
        }
}

void LibraryView::timerCallback()
{
    if (splice() && isShowing()) model_.rescanSplice();
    else stopTimer();
}

const SpliceSample* LibraryView::spliceAt (int row) const
{
    if (row < 0 || row >= (int) spliceRows_.size()) return nullptr;
    const auto& all = model_.spliceSamples();
    const int i = spliceRows_[(std::size_t) row];
    return i >= 0 && i < (int) all.size() ? &all[(std::size_t) i] : nullptr;
}

int LibraryView::freeSamplerSlot() const
{
    // first empty slot of the bank on screen, then of any bank, else the bank's first slot
    const int bank = model_.engine.samplerBank.load();
    for (int k = 0; k < dj::kSamplerSlots; ++k)
    {
        const int s = (bank * dj::kSamplerSlotsPerBank + k) % dj::kSamplerSlots;
        if (! model_.engine.sampler().view[(std::size_t) s].loaded.load() && model_.samplerName (s).isEmpty()) return s;
    }
    return bank * dj::kSamplerSlotsPerBank;
}

void LibraryView::showSpliceMenu (int row)
{
    const auto* s = spliceAt (row);
    if (s == nullptr) return;
    const auto sample = *s;
    juce::PopupMenu m, slots;
    m.addItem ("Load to the next free sampler slot", [this, sample]
    {
        model_.loadSampleFile (freeSamplerSlot(), sample.file, sample.info.bpm, sample.info.loop, juce::String (sample.info.name));
    });
    for (int k = 0; k < dj::kSamplerSlots; ++k)
        slots.addItem (juce::String::charToString ((juce::juce_wchar) ('A' + k / 8)) + juce::String (k % 8 + 1) + (model_.samplerName (k).isNotEmpty() ? "  (" + model_.samplerName (k) + ")" : juce::String()),
                       [this, k, sample] { model_.loadSampleFile (k, sample.file, sample.info.bpm, sample.info.loop, juce::String (sample.info.name)); });
    m.addSubMenu ("Load to Sampler", slots);
    for (int d = 0; d < model_.settings.deckCount; ++d)
        m.addItem ("Load to Deck " + juce::String (d + 1), [this, d, f = sample.file] { model_.importAndLoad (f, d, "Splice"); });
    m.addSeparator();
    m.addItem ("Show in Finder", [f = sample.file] { f.revealToUser(); });
    m.showMenuAsync (juce::PopupMenu::Options());
}

LibraryView::~LibraryView() = default;

// --------------------------------------------------------------------------------- sources
void LibraryView::rebuildSources()
{
    sources_.clear();
    sources_.push_back ({ Source::Kind::All, "All Tracks" });
    sources_.push_back ({ Source::Kind::Prepare, "Prepare" });
    sources_.push_back ({ Source::Kind::Splice, "Splice" });
    auto& lib = model_.library;
    for (const auto& c : lib.crates())
    {
        const int depth = (int) std::count (c.name.begin(), c.name.end(), '/');
        const auto slash = c.name.rfind ('/');
        Source s { Source::Kind::Crate, juce::String (slash == std::string::npos ? c.name : c.name.substr (slash + 1)) };
        s.name = c.name;
        s.depth = depth + 1;
        sources_.push_back (s);
    }
    for (int i = 0; i < (int) lib.smartCrates().size(); ++i)
    {
        Source s { Source::Kind::Smart, juce::String::fromUTF8 ("\xE2\x9A\xA1 ") + juce::String (lib.smartCrates()[(std::size_t) i].name) };
        s.index = i;
        s.depth = 1;
        sources_.push_back (s);
    }
    const auto& hist = lib.history();
    for (int i = (int) hist.size() - 1; i >= 0 && i >= (int) hist.size() - 20; --i)
    {
        if (hist[(std::size_t) i].entries.empty()) continue;
        Source s { Source::Kind::History, "History " + juce::Time ((juce::int64) hist[(std::size_t) i].start * 1000).formatted ("%d %b %H:%M") };
        s.index = i;
        sources_.push_back (s);
    }
    currentSource_ = juce::jlimit (0, (int) sources_.size() - 1, currentSource_);
    sourceList_.updateContent();
    sourceList_.selectRow (currentSource_, true, true);
    sourceList_.repaint();
}

std::vector<std::uint64_t> LibraryView::sourceTracks() const
{
    auto& lib = model_.library;
    const auto& s = sources_[(std::size_t) currentSource_];
    switch (s.kind)
    {
        case Source::Kind::All: return lib.allIds();
        case Source::Kind::Prepare: return const_cast<dj::library::Library&> (lib).prepare();
        case Source::Kind::Splice: return {};
        case Source::Kind::Crate: return lib.crateTracks (s.name, true);
        case Source::Kind::Smart:
            return lib.evaluate (const_cast<dj::library::Library&> (lib).smartCrates()[(std::size_t) s.index], juce::Time::currentTimeMillis() / 1000);
        case Source::Kind::History:
        {
            std::vector<std::uint64_t> ids;
            for (const auto& e : lib.history()[(std::size_t) s.index].entries) ids.push_back (e.track);
            return ids;
        }
    }
    return {};
}

void LibraryView::refreshTracks()
{
    if (splice())
    {
        // Splice samples: every search word must appear in the name, pack, BPM or key
        const auto& all = model_.spliceSamples();
        const auto words = juce::StringArray::fromTokens (search_.getText().toLowerCase(), " ", {});
        spliceRows_.clear();
        for (int i = 0; i < (int) all.size(); ++i)
        {
            const auto& s = all[(std::size_t) i];
            const auto hay = (juce::String (s.info.name) + " " + juce::String (s.info.pack) + " " + (s.info.bpm > 0 ? juce::String ((int) s.info.bpm) : juce::String())
                              + " " + Theme::keyText (s.info.key, model_.settings.camelotKeys) + " " + (s.info.loop ? "loop" : "one shot")).toLowerCase();
            bool ok = true;
            for (const auto& w : words) ok = ok && hay.contains (w);
            if (ok) spliceRows_.push_back (i);
        }
        if (sortColumn_ != 0)
            std::stable_sort (spliceRows_.begin(), spliceRows_.end(), [&] (int a, int b)
            {
                const auto& x = all[(std::size_t) a];
                const auto& y = all[(std::size_t) b];
                auto three = [] (auto p, auto q) { return p < q ? -1 : q < p ? 1 : 0; };
                int c;
                switch (sortColumn_)
                {
                    case ColArtist: c = three (x.info.pack, y.info.pack); break;
                    case ColBpm: c = three (x.info.bpm, y.info.bpm); break;
                    case ColKey: c = three (x.info.key, y.info.key); break;
                    case ColAlbum: c = three ((int) x.info.loop, (int) y.info.loop); break;
                    case ColAdded: c = three (x.modified, y.modified); break;
                    default: c = juce::String (x.info.name).compareNatural (juce::String (y.info.name)); break;
                }
                return sortForwards_ ? c < 0 : c > 0;
            });
        table_.updateContent();
        table_.repaint();
        return;
    }
    auto ids = model_.library.search (sourceTracks(), search_.getText().toStdString());
    const auto kind = sources_[(std::size_t) currentSource_].kind;
    // crates, Prepare and History keep their own order until a column is clicked
    if (sortColumn_ != 0 || kind == Source::Kind::All || kind == Source::Kind::Smart)
        model_.library.sort (ids, sortFor (sortColumn_ == 0 ? ColArtist : sortColumn_), sortForwards_);
    model_.visibleTracks = std::move (ids);
    table_.updateContent();
    table_.repaint();
    // keep the selected track selected if it is still visible
    const auto it = std::find (model_.visibleTracks.begin(), model_.visibleTracks.end(), model_.selectedTrack);
    if (it != model_.visibleTracks.end()) table_.selectRow ((int) (it - model_.visibleTracks.begin()), true, true);
}

void LibraryView::rebuild()
{
    rebuildSources();
    refreshTracks();
}

void LibraryView::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= (int) sources_.size()) return;
    const auto& s = sources_[(std::size_t) row];
    if (selected) { g.setColour (Theme::accent.withAlpha (focusTracks_ ? 0.25f : 0.5f)); g.fillRect (0, 0, w, h); }
    g.setColour (s.kind == Source::Kind::History ? Theme::dim : Theme::text);
    g.setFont (juce::FontOptions (12.5f, s.kind == Source::Kind::All || s.kind == Source::Kind::Prepare ? juce::Font::bold : juce::Font::plain));
    g.drawText (s.label, 8 + s.depth * 12, 0, w - 10, h, juce::Justification::centredLeft);
}

void LibraryView::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (row < 0 || row >= (int) sources_.size()) return;
    currentSource_ = row;
    sortColumn_ = 0;
    if (splice()) { model_.rescanSplice(); startTimer (30000); }
    else stopTimer();
    refreshTracks();
    updateToolbar();
    if (e.mods.isPopupMenu()) showSourceMenu (row);
}

// ---------------------------------------------------------------------------------- table
int LibraryView::getNumRows() { return splice() ? (int) spliceRows_.size() : (int) model_.visibleTracks.size(); }

void LibraryView::paintRowBackground (juce::Graphics& g, int row, int w, int h, bool selected)
{
    if (selected) g.fillAll (Theme::accent.withAlpha (focusTracks_ ? 0.35f : 0.2f));
    else if (row % 2) g.fillAll (Theme::panel);
    if (splice()) return;
    // tracks loaded on a deck get that deck's colour bar
    if (row >= 0 && row < (int) model_.visibleTracks.size())
        for (int d = 0; d < dj::kNumDecks; ++d)
            if (model_.decks[(std::size_t) d].trackId == model_.visibleTracks[(std::size_t) row])
            {
                g.setColour (Theme::deck (d));
                g.fillRect (0, 0, 3, h);
            }
    juce::ignoreUnused (w);
}

void LibraryView::paintCell (juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (splice())
    {
        const auto* s = spliceAt (row);
        if (s == nullptr) return;
        juce::String text;
        auto colour = Theme::text;
        switch (column)
        {
            case ColNum: text = juce::String (row + 1); colour = Theme::dim; break;
            case ColTitle: text = juce::String::fromUTF8 (s->info.name.c_str()); break;
            case ColArtist: text = juce::String::fromUTF8 (s->info.pack.c_str()); colour = Theme::dim; break;
            case ColBpm: text = s->info.bpm > 0 ? juce::String (s->info.bpm, 0) : juce::String(); break;
            case ColKey: text = s->info.key >= 0 ? Theme::keyText (s->info.key, model_.settings.camelotKeys) : juce::String(); colour = Theme::key (s->info.key); break;
            case ColAlbum: text = s->info.loop ? "Loop" : "One-shot"; colour = s->info.loop ? Theme::good : Theme::dim; break;
            case ColAdded: text = s->modified.formatted ("%Y-%m-%d"); colour = Theme::dim; break;
            default: break;
        }
        g.setColour (colour);
        g.setFont (juce::FontOptions (12.5f));
        g.drawText (text, 4, 0, w - 6, h, juce::Justification::centredLeft, true);
        return;
    }
    if (row < 0 || row >= (int) model_.visibleTracks.size()) return;
    const auto* t = model_.library.find (model_.visibleTracks[(std::size_t) row]);
    if (t == nullptr) return;
    juce::String text;
    auto colour = t->missing ? Theme::bad : Theme::text;
    switch (column)
    {
        case ColNum: text = juce::String (row + 1); colour = Theme::dim; break;
        case ColTitle: text = juce::String::fromUTF8 (t->title.c_str()); break;
        case ColArtist: text = juce::String::fromUTF8 (t->artist.c_str()); break;
        case ColBpm: text = t->bpm > 0 ? juce::String (t->bpm, std::abs (t->bpm - std::round (t->bpm)) < 1.0e-6 ? 0 : 1) : juce::String(); break;
        case ColKey: text = Theme::keyText (t->key, model_.settings.camelotKeys); colour = Theme::key (t->key); break;
        case ColLength: text = t->lengthSeconds > 0 ? Theme::formatTime (t->lengthSeconds).upToFirstOccurrenceOf (".", false, false) : juce::String(); break;
        case ColAlbum: text = juce::String::fromUTF8 (t->album.c_str()); break;
        case ColGenre: text = juce::String::fromUTF8 (t->genre.c_str()); break;
        case ColComment: text = juce::String::fromUTF8 (t->comment.c_str()); break;
        case ColAdded: text = t->added > 0 ? juce::Time ((juce::int64) t->added * 1000).formatted ("%Y-%m-%d") : juce::String(); colour = Theme::dim; break;
        case ColPlays: text = t->playCount > 0 ? juce::String (t->playCount) : juce::String(); colour = Theme::dim; break;
        default: break;
    }
    g.setColour (colour);
    g.setFont (juce::FontOptions (12.5f));
    g.drawText (text, 4, 0, w - 6, h, juce::Justification::centredLeft, true);
}

void LibraryView::sortOrderChanged (int columnId, bool forwards)
{
    sortColumn_ = columnId;
    sortForwards_ = forwards;
    refreshTracks();
}

void LibraryView::selectedRowsChanged (int lastRow)
{
    if (splice()) return;
    if (lastRow >= 0 && lastRow < (int) model_.visibleTracks.size()) model_.selectedTrack = model_.visibleTracks[(std::size_t) lastRow];
}

int LibraryView::idleDeck() const
{
    // first empty deck on screen, else the first one on screen that is not playing
    const int count = model_.settings.deckCount;
    const int first = count == 8 ? model_.settings.deckBank * 4 : 0;
    const int shown = count == 8 ? 4 : count;
    for (int d = first; d < first + shown; ++d)
        if (model_.decks[(std::size_t) d].trackId == 0) return d;
    for (int d = first; d < first + shown; ++d)
        if (! model_.engine.deck (d).view.playing.load()) return d;
    return -1;
}

void LibraryView::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    if (splice())
    {
        if (const auto* s = spliceAt (row))
            model_.loadSampleFile (freeSamplerSlot(), s->file, s->info.bpm, s->info.loop, juce::String (s->info.name));
        return;
    }
    if (row < 0 || row >= (int) model_.visibleTracks.size()) return;
    const int d = idleDeck();
    if (d >= 0) model_.loadTrack (d, model_.visibleTracks[(std::size_t) row]);
}

void LibraryView::cellClicked (int row, int, const juce::MouseEvent& e)
{
    focusTracks_ = true;
    if (e.mods.isPopupMenu()) { if (splice()) showSpliceMenu (row); else showTrackMenu (row); }
}

juce::var LibraryView::getDragSourceDescription (const juce::SparseSet<int>& rows)
{
    if (rows.isEmpty()) return {};
    const int row = rows[0];
    if (splice())
    {
        const auto* s = spliceAt (row);
        if (s == nullptr) return {};
        return "sample:\n" + s->file.getFullPathName() + "\n" + juce::String (s->info.bpm) + "\n" + (s->info.loop ? "1" : "0") + "\n" + juce::String (s->info.name);
    }
    if (row < 0 || row >= (int) model_.visibleTracks.size()) return {};
    return "track:" + juce::String ((juce::int64) model_.visibleTracks[(std::size_t) row]);
}

// ---------------------------------------------------------------------------------- menus
void LibraryView::showTrackMenu (int row)
{
    if (row < 0 || row >= (int) model_.visibleTracks.size()) return;
    const auto id = model_.visibleTracks[(std::size_t) row];
    juce::PopupMenu m, crates, sampler;
    for (int d = 0; d < model_.settings.deckCount; ++d) m.addItem ("Load to Deck " + juce::String (d + 1), [this, d, id] { model_.loadTrack (d, id); });
    m.addItem ("Add to Prepare", [this, id] { model_.library.prepare().push_back (id); model_.markLibraryDirty(); rebuild(); });
    for (const auto& c : model_.library.crates())
    {
        const auto name = c.name;
        crates.addItem (juce::String (name), [this, name, id] { model_.library.addToCrate (name, id); model_.markLibraryDirty(); });
    }
    m.addSubMenu ("Add to Crate", crates);
    for (int s = 0; s < dj::kSamplerSlots; ++s)
        sampler.addItem (juce::String::charToString ((juce::juce_wchar) ('A' + s / 8)) + juce::String (s % 8 + 1), [this, s, id] { model_.loadToSampler (s, id); });
    m.addSubMenu ("Load to Sampler", sampler);
    m.addSeparator();
    m.addItem ("Re-analyse (BPM, key, gain)", [this, id]
    {
        if (auto* t = model_.library.find (id)) { t->analysed = false; t->gridLocked = false; t->key = -1; model_.markLibraryDirty(); }
        model_.analyseInBackground ({ id });
    });
    const auto& src = sources_[(std::size_t) currentSource_];
    if (src.kind == Source::Kind::Crate)
        m.addItem ("Remove from Crate", [this, id, name = src.name] { model_.library.removeFromCrate (name, id); model_.markLibraryDirty(); refreshTracks(); });
    if (src.kind == Source::Kind::Prepare)
        m.addItem ("Remove from Prepare", [this, id]
        {
            auto& p = model_.library.prepare();
            p.erase (std::remove (p.begin(), p.end(), id), p.end());
            model_.markLibraryDirty();
            refreshTracks();
        });
    if (auto* t = model_.library.find (id))
        m.addItem ("Show in Finder", [path = t->path] { juce::File (path).revealToUser(); });
    m.addItem ("Remove from Library", [this, id] { model_.library.remove (id); model_.markLibraryDirty(); rebuild(); });
    m.showMenuAsync (juce::PopupMenu::Options());
}

void LibraryView::showSourceMenu (int row)
{
    const auto s = sources_[(std::size_t) row];
    if (s.kind != Source::Kind::Crate && s.kind != Source::Kind::Smart) return;
    juce::PopupMenu m;
    if (s.kind == Source::Kind::Crate)
    {
        m.addItem ("New Sub-crate", [this, name = s.name]
        {
            auto* w = new juce::AlertWindow ("New Sub-crate", "Name", juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", "");
            w->addButton ("Create", 1);
            w->addButton ("Cancel", 0);
            w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, name] (int r)
            {
                if (r == 1 && w->getTextEditorContents ("name").trim().isNotEmpty())
                {
                    model_.library.crate (name + "/" + w->getTextEditorContents ("name").trim().replaceCharacter ('/', '-').toStdString());
                    model_.markLibraryDirty();
                    rebuild();
                }
            }), true);
        });
        m.addItem ("Delete Crate", [this, name = s.name] { model_.library.deleteCrate (name); model_.markLibraryDirty(); currentSource_ = 0; rebuild(); });
    }
    else
        m.addItem ("Delete Smart Crate", [this, idx = s.index]
        {
            auto& sc = model_.library.smartCrates();
            if (idx < (int) sc.size()) sc.erase (sc.begin() + idx);
            model_.markLibraryDirty();
            currentSource_ = 0;
            rebuild();
        });
    m.showMenuAsync (juce::PopupMenu::Options());
}

void LibraryView::newCrate()
{
    auto* w = new juce::AlertWindow ("New Crate", "Crate name (use / for a sub-crate)", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", "");
    w->addButton ("Create", 1);
    w->addButton ("Cancel", 0);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int r)
    {
        const auto name = w->getTextEditorContents ("name").trim();
        if (r == 1 && name.isNotEmpty())
        {
            model_.library.crate (name.toStdString());
            model_.markLibraryDirty();
            rebuild();
        }
    }), true);
}

void LibraryView::newSmartCrate()
{
    auto* w = new juce::AlertWindow ("New Smart Crate", "Tracks are added automatically when they match every rule you fill in.", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", "", "Name");
    w->addTextEditor ("genre", "", "Genre contains");
    w->addTextEditor ("artist", "", "Artist contains");
    w->addTextEditor ("bpmMin", "", "BPM from");
    w->addTextEditor ("bpmMax", "", "BPM to");
    w->addTextEditor ("key", "", "Key (e.g. 8A or Am)");
    w->addTextEditor ("days", "", "Added in the last N days");
    w->addButton ("Create", 1);
    w->addButton ("Cancel", 0);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int r)
    {
        if (r != 1) return;
        using namespace dj::library;
        SmartCrate sc;
        sc.name = w->getTextEditorContents ("name").trim().toStdString();
        if (sc.name.empty()) sc.name = "Smart Crate";
        auto text = [&] (const char* k) { return w->getTextEditorContents (k).trim(); };
        if (text ("genre").isNotEmpty()) sc.rules.push_back ({ Field::Genre, Op::Contains, text ("genre").toStdString(), 0, 0 });
        if (text ("artist").isNotEmpty()) sc.rules.push_back ({ Field::Artist, Op::Contains, text ("artist").toStdString(), 0, 0 });
        if (text ("bpmMin").isNotEmpty() || text ("bpmMax").isNotEmpty())
            sc.rules.push_back ({ Field::Bpm, Op::Between, "", text ("bpmMin").getDoubleValue(), text ("bpmMax").isNotEmpty() ? text ("bpmMax").getDoubleValue() : 999.0 });
        if (text ("key").isNotEmpty()) sc.rules.push_back ({ Field::Key, Op::Is, text ("key").toStdString(), 0, 0 });
        if (text ("days").isNotEmpty()) sc.rules.push_back ({ Field::Added, Op::InLastDays, "", text ("days").getDoubleValue(), 0 });
        if (sc.rules.empty()) return;
        model_.library.smartCrates().push_back (sc);
        model_.markLibraryDirty();
        rebuild();
    }), true);
}

// --------------------------------------------------------------------------- controller
void LibraryView::browse (int delta, bool shift)
{
    if (shift) return; // SHIFT + rotary zooms the waveforms (handled by MainComponent)
    if (focusTracks_)
    {
        const int n = (int) model_.visibleTracks.size();
        if (n == 0) return;
        const int row = juce::jlimit (0, n - 1, (table_.getSelectedRow() < 0 ? -1 : table_.getSelectedRow()) + delta);
        table_.selectRow (row);
        table_.scrollToEnsureRowIsOnscreen (row);
    }
    else
    {
        currentSource_ = juce::jlimit (0, (int) sources_.size() - 1, currentSource_ + delta);
        sourceList_.selectRow (currentSource_);
        sortColumn_ = 0;
        refreshTracks();
    }
}

void LibraryView::back()
{
    focusTracks_ = ! focusTracks_;
    sourceList_.repaint();
    table_.repaint();
}

void LibraryView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::bg);
}

void LibraryView::paintOverChildren (juce::Graphics& g)
{
    if (! splice() || ! spliceRows_.empty()) return;
    // empty Splice list: say how to fill it
    juce::String msg;
    if (! model_.spliceFolder().isDirectory())
        msg = "Splice is not set up on this Mac.\n\nInstall the Splice desktop app and sign in (Open Splice). The sounds you download there land in its "
              "Splice folder and appear here, ready to drag onto a sampler pad. If you moved the folder, pick it with Splice Folder...";
    else if (model_.spliceScanning())
        msg = "Reading " + model_.spliceFolder().getFullPathName() + " ...";
    else if (search_.getText().isNotEmpty())
        msg = "No sample matches \"" + search_.getText() + "\"";
    else
        msg = "No sounds in " + model_.spliceFolder().getFullPathName() + " yet.\n\nDownload sounds in the Splice app (Open Splice): they appear here.";
    g.setColour (Theme::dim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawFittedText (msg, table_.getBounds().reduced (40, 50), juce::Justification::centredTop, 6);
}

void LibraryView::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (28).reduced (4, 3);
    if (splice())
        for (auto* b : { &spliceRescan_, &spliceFolder_, &spliceOpen_ })
            b->setBounds (top.removeFromRight (b == &spliceRescan_ ? 66 : 110).reduced (2, 0));
    else
        for (auto* b : { &newSmart_, &newCrate_, &seratoImport_, &addFolder_, &addFiles_, &analyse_ })
            b->setBounds (top.removeFromRight (b == &seratoImport_ ? 100 : 66).reduced (2, 0));
    search_.setBounds (top.reduced (2, 0));
    sourceList_.setBounds (r.removeFromLeft (juce::jlimit (150, 240, getWidth() / 6)));
    table_.setBounds (r);
}

} // namespace app
