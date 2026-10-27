#include "Library.h"
#include "../Common.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace dj::library
{

namespace
{
    std::string lower (std::string s) { for (auto& c : s) c = (char) std::tolower ((unsigned char) c); return s; }

    std::string fieldText (const Track& t, Field f)
    {
        switch (f)
        {
            case Field::Title: return t.title;
            case Field::Artist: return t.artist;
            case Field::Album: return t.album;
            case Field::Genre: return t.genre;
            case Field::Comment: return t.comment;
            case Field::Year: return t.year;
            case Field::Label: return t.label;
            case Field::Grouping: return t.grouping;
            case Field::Key: return t.key >= 0 ? std::string (keyName (t.key)) + " " + std::to_string (camelotNumber (t.key)) + (camelotIsMinor (t.key) ? "A" : "B") : "";
            default: return "";
        }
    }

    double fieldNumber (const Track& t, Field f)
    {
        switch (f)
        {
            case Field::Bpm: return t.bpm;
            case Field::Added: return (double) t.added;
            case Field::Rating: return t.rating;
            case Field::PlayCount: return t.playCount;
            case Field::Length: return t.lengthSeconds;
            case Field::Year: return std::atof (t.year.c_str());
            case Field::Key: return t.key;
            default: return 0.0;
        }
    }

    json::Value cueJson (const HotCue& c)
    {
        json::Value v;
        v.set ("pos", c.pos).set ("color", (double) c.color).set ("label", c.label);
        return v;
    }
}

// --------------------------------------------------------------------------------- tracks
Track& Library::addOrUpdate (const std::string& path, const TagInfo& tags, std::int64_t now)
{
    Track* existing = findByPath (path);
    Track& t = existing != nullptr ? *existing : tracks_[nextId_];
    if (existing == nullptr)
    {
        t.id = nextId_++;
        t.path = path;
        t.added = now;
        byPath_[path] = t.id;
    }
    // a re-import only replaces what the file actually carries
    auto take = [] (std::string& dst, const std::string& src) { if (! src.empty()) dst = src; };
    take (t.title, tags.title); take (t.artist, tags.artist); take (t.album, tags.album); take (t.genre, tags.genre);
    take (t.comment, tags.comment); take (t.year, tags.year); take (t.grouping, tags.grouping); take (t.label, tags.label);
    take (t.remixer, tags.remixer);
    if (! t.analysed)
    {
        if (tags.bpm > 0.0) t.bpm = tags.bpm;
        if (tags.key >= 0) t.key = tags.key;
    }
    if (tags.hasTrackColor && t.color == 0) t.color = tags.trackColor;
    t.missing = false;
    return t;
}

Track* Library::find (std::uint64_t id) { const auto it = tracks_.find (id); return it == tracks_.end() ? nullptr : &it->second; }
const Track* Library::find (std::uint64_t id) const { const auto it = tracks_.find (id); return it == tracks_.end() ? nullptr : &it->second; }
Track* Library::findByPath (const std::string& path) { const auto it = byPath_.find (path); return it == byPath_.end() ? nullptr : find (it->second); }

void Library::remove (std::uint64_t id)
{
    const auto it = tracks_.find (id);
    if (it == tracks_.end()) return;
    byPath_.erase (it->second.path);
    tracks_.erase (it);
    for (auto& c : crates_) c.tracks.erase (std::remove (c.tracks.begin(), c.tracks.end(), id), c.tracks.end());
    prepare_.erase (std::remove (prepare_.begin(), prepare_.end(), id), prepare_.end());
}

std::vector<std::uint64_t> Library::allIds() const
{
    std::vector<std::uint64_t> ids;
    ids.reserve (tracks_.size());
    for (const auto& [id, t] : tracks_) ids.push_back (id);
    return ids;
}

void Library::applySeratoPrep (Track& t, const TagInfo& tags, double sr)
{
    if (sr <= 0.0) return;
    for (const auto& c : tags.cues)
        if (c.index >= 0 && c.index < 8)
        {
            auto& h = t.hotCues[(std::size_t) c.index];
            h.pos = c.ms * 0.001 * sr;
            h.color = c.color;
            h.label = c.name;
        }
    for (const auto& l : tags.loops)
        if (l.index >= 0 && l.index < 8 && l.endMs > l.startMs)
        {
            auto& s = t.loops[(std::size_t) l.index];
            s.in = l.startMs * 0.001 * sr;
            s.out = l.endMs * 0.001 * sr;
            s.locked = l.locked;
            s.label = l.name;
        }
    if (tags.gridBpm > 0.0 && tags.gridAnchorSeconds >= 0.0)
    {
        // a grid prepared in Serato is authoritative: analysis must not overwrite it
        t.bpm = tags.gridBpm;
        t.gridAnchor = tags.gridAnchorSeconds * sr;
        t.gridLocked = true;
    }
    if (tags.hasTrackColor) t.color = tags.trackColor;
}

// ------------------------------------------------------------------------------- browsing
std::vector<std::uint64_t> Library::search (const std::vector<std::uint64_t>& scope, const std::string& query) const
{
    // Serato-style: every whitespace-separated word must appear in one of the text fields
    std::vector<std::string> words;
    std::istringstream in (lower (query));
    for (std::string w; in >> w;) words.push_back (w);
    if (words.empty()) return scope;
    std::vector<std::uint64_t> out;
    for (const auto id : scope)
    {
        const Track* t = find (id);
        if (t == nullptr) continue;
        char bpmText[16];
        std::snprintf (bpmText, sizeof (bpmText), "%.0f", t->bpm);
        const std::string hay = lower (t->title + "\n" + t->artist + "\n" + t->album + "\n" + t->genre + "\n" + t->comment + "\n"
                                       + t->label + "\n" + t->remixer + "\n" + t->grouping + "\n" + bpmText + "\n" + fieldText (*t, Field::Key));
        bool all = true;
        for (const auto& w : words) all = all && hay.find (w) != std::string::npos;
        if (all) out.push_back (id);
    }
    return out;
}

void Library::sort (std::vector<std::uint64_t>& ids, SortColumn col, bool ascending) const
{
    auto key = [&] (std::uint64_t id) -> std::pair<double, std::string>
    {
        const Track* t = find (id);
        if (t == nullptr) return { 0.0, {} };
        switch (col)
        {
            case SortColumn::Title: return { 0.0, lower (t->title) };
            case SortColumn::Artist: return { 0.0, lower (t->artist) };
            case SortColumn::Album: return { 0.0, lower (t->album) };
            case SortColumn::Genre: return { 0.0, lower (t->genre) };
            case SortColumn::Comment: return { 0.0, lower (t->comment) };
            case SortColumn::Bpm: return { t->bpm, {} };
            // Camelot order (1A, 1B, 2A ...) is what DJs sort keys by
            case SortColumn::Key: return { t->key < 0 ? 99.0 : camelotNumber (t->key) * 2.0 + (camelotIsMinor (t->key) ? 0.0 : 1.0), {} };
            case SortColumn::Length: return { t->lengthSeconds, {} };
            case SortColumn::Year: return { std::atof (t->year.c_str()), {} };
            case SortColumn::Added: return { (double) t->added, {} };
            case SortColumn::Rating: return { (double) t->rating, {} };
            case SortColumn::PlayCount: return { (double) t->playCount, {} };
        }
        return { 0.0, {} };
    };
    std::stable_sort (ids.begin(), ids.end(), [&] (std::uint64_t a, std::uint64_t b)
    {
        const auto ka = key (a), kb = key (b);
        return ascending ? ka < kb : kb < ka;
    });
}

// --------------------------------------------------------------------------------- crates
Crate& Library::crate (const std::string& name)
{
    for (auto& c : crates_) if (c.name == name) return c;
    // creating "A/B" also creates "A"
    const auto slash = name.rfind ('/');
    if (slash != std::string::npos && ! hasCrate (name.substr (0, slash))) crate (name.substr (0, slash));
    crates_.push_back ({ name, {} });
    return crates_.back();
}

bool Library::hasCrate (const std::string& name) const
{
    return std::any_of (crates_.begin(), crates_.end(), [&] (const Crate& c) { return c.name == name; });
}

void Library::deleteCrate (const std::string& name)
{
    crates_.erase (std::remove_if (crates_.begin(), crates_.end(), [&] (const Crate& c)
    {
        return c.name == name || c.name.rfind (name + "/", 0) == 0;
    }), crates_.end());
}

void Library::renameCrate (const std::string& from, const std::string& to)
{
    for (auto& c : crates_)
    {
        if (c.name == from) c.name = to;
        else if (c.name.rfind (from + "/", 0) == 0) c.name = to + c.name.substr (from.size());
    }
}

void Library::addToCrate (const std::string& name, std::uint64_t id)
{
    auto& c = crate (name);
    if (std::find (c.tracks.begin(), c.tracks.end(), id) == c.tracks.end()) c.tracks.push_back (id);
}

void Library::removeFromCrate (const std::string& name, std::uint64_t id)
{
    for (auto& c : crates_)
        if (c.name == name) c.tracks.erase (std::remove (c.tracks.begin(), c.tracks.end(), id), c.tracks.end());
}

std::vector<std::uint64_t> Library::crateTracks (const std::string& name, bool includeSubcrates) const
{
    std::vector<std::uint64_t> out;
    for (const auto& c : crates_)
        if (c.name == name || (includeSubcrates && c.name.rfind (name + "/", 0) == 0))
            for (const auto id : c.tracks)
                if (std::find (out.begin(), out.end(), id) == out.end()) out.push_back (id);
    return out;
}

// ---------------------------------------------------------------------------- smart crates
bool Library::matches (const Track& t, const SmartRule& r, std::int64_t now) const
{
    switch (r.op)
    {
        case Op::Contains: return lower (fieldText (t, r.field)).find (lower (r.text)) != std::string::npos;
        case Op::NotContains: return lower (fieldText (t, r.field)).find (lower (r.text)) == std::string::npos;
        case Op::Is:
            if (r.field == Field::Key) return t.key == parseKey (r.text);
            return r.text.empty() ? fieldNumber (t, r.field) == r.a : lower (fieldText (t, r.field)) == lower (r.text);
        case Op::IsNot:
            if (r.field == Field::Key) return t.key != parseKey (r.text);
            return r.text.empty() ? fieldNumber (t, r.field) != r.a : lower (fieldText (t, r.field)) != lower (r.text);
        case Op::Greater: return fieldNumber (t, r.field) > r.a;
        case Op::Less: return fieldNumber (t, r.field) < r.a;
        case Op::Between: { const double v = fieldNumber (t, r.field); return v >= r.a && v <= r.b; }
        case Op::InLastDays: return (double) (now - t.added) <= r.a * 86400.0;
    }
    return false;
}

std::vector<std::uint64_t> Library::evaluate (const SmartCrate& sc, std::int64_t now) const
{
    std::vector<std::uint64_t> out;
    for (const auto& [id, t] : tracks_)
    {
        bool ok = sc.matchAll;
        for (const auto& r : sc.rules)
        {
            const bool m = matches (t, r, now);
            if (sc.matchAll) ok = ok && m; else ok = ok || m;
        }
        if (sc.rules.empty()) ok = false;
        if (ok) out.push_back (id);
    }
    return out;
}

// ---------------------------------------------------------------------------------- history
void Library::beginSession (std::int64_t now)
{
    history_.push_back ({ now, now, {} });
}

void Library::logPlay (std::uint64_t id, int deck, std::int64_t now)
{
    if (history_.empty()) beginSession (now);
    auto& s = history_.back();
    s.entries.push_back ({ id, now, deck });
    s.end = now;
    if (Track* t = find (id)) { ++t->playCount; t->lastPlayed = now; }
}

// ----------------------------------------------------------------------------- Serato crates
std::vector<std::string> Library::readSeratoCrate (const std::string& file)
{
    // Tagged blocks: 4-byte ASCII tag, 4-byte big-endian length, payload. Text is UTF-16BE.
    // "otrk" blocks contain a "ptrk" block holding the track path (relative to the volume root).
    std::vector<std::string> out;
    std::ifstream f (file, std::ios::binary);
    if (! f) return out;
    std::vector<std::uint8_t> d ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    auto be32 = [&] (std::size_t p) { return ((std::uint32_t) d[p] << 24) | ((std::uint32_t) d[p + 1] << 16) | ((std::uint32_t) d[p + 2] << 8) | d[p + 3]; };
    auto utf16 = [&] (std::size_t p, std::size_t len)
    {
        std::string s;
        for (std::size_t i = 0; i + 1 < len; i += 2)
        {
            const std::uint32_t u = ((std::uint32_t) d[p + i] << 8) | d[p + i + 1];
            if (u < 0x80) s += (char) u;
            else if (u < 0x800) { s += (char) (0xC0 | (u >> 6)); s += (char) (0x80 | (u & 0x3F)); }
            else { s += (char) (0xE0 | (u >> 12)); s += (char) (0x80 | ((u >> 6) & 0x3F)); s += (char) (0x80 | (u & 0x3F)); }
        }
        return s;
    };
    std::size_t pos = 0;
    while (pos + 8 <= d.size())
    {
        const std::string tag (reinterpret_cast<const char*> (&d[pos]), 4);
        const std::size_t len = be32 (pos + 4);
        if (pos + 8 + len > d.size()) break;
        if (tag == "otrk")
        {
            std::size_t q = pos + 8;
            while (q + 8 <= pos + 8 + len)
            {
                const std::string sub (reinterpret_cast<const char*> (&d[q]), 4);
                const std::size_t sl = be32 (q + 4);
                if (q + 8 + sl > pos + 8 + len) break;
                if (sub == "ptrk") out.push_back (utf16 (q + 8, sl));
                q += 8 + sl;
            }
        }
        pos += 8 + len;
    }
    return out;
}

// ------------------------------------------------------------------------------ persistence
json::Value Library::toJson() const
{
    json::Array tracks;
    for (const auto& [id, t] : tracks_)
    {
        json::Value v;
        v.set ("id", (double) t.id).set ("path", t.path).set ("title", t.title).set ("artist", t.artist).set ("album", t.album)
         .set ("genre", t.genre).set ("comment", t.comment).set ("year", t.year).set ("grouping", t.grouping).set ("label", t.label)
         .set ("remixer", t.remixer).set ("bpm", t.bpm).set ("gridAnchor", t.gridAnchor).set ("gridLocked", t.gridLocked)
         .set ("key", t.key).set ("length", t.lengthSeconds).set ("sampleRate", t.sampleRate).set ("lufs", t.loudnessLufs)
         .set ("autoGain", t.autoGainDb).set ("rating", t.rating).set ("color", (double) t.color).set ("plays", t.playCount)
         .set ("added", (double) t.added).set ("lastPlayed", (double) t.lastPlayed).set ("fileSize", (double) t.fileSize)
         .set ("fileModified", (double) t.fileModified).set ("analysed", t.analysed).set ("cue", t.cuePoint);
        json::Array cues, loops;
        for (const auto& c : t.hotCues) cues.push_back (cueJson (c));
        for (const auto& l : t.loops)
        {
            json::Value lv;
            lv.set ("in", l.in).set ("out", l.out).set ("locked", l.locked).set ("label", l.label);
            loops.push_back (lv);
        }
        v.set ("hotCues", cues).set ("loops", loops);
        tracks.push_back (v);
    }
    json::Array crates;
    for (const auto& c : crates_)
    {
        json::Array ids;
        for (const auto id : c.tracks) ids.push_back ((double) id);
        json::Value v;
        v.set ("name", c.name).set ("tracks", ids);
        crates.push_back (v);
    }
    json::Array smart;
    for (const auto& sc : smart_)
    {
        json::Array rules;
        for (const auto& r : sc.rules)
        {
            json::Value rv;
            rv.set ("field", (int) r.field).set ("op", (int) r.op).set ("text", r.text).set ("a", r.a).set ("b", r.b);
            rules.push_back (rv);
        }
        json::Value v;
        v.set ("name", sc.name).set ("matchAll", sc.matchAll).set ("rules", rules);
        smart.push_back (v);
    }
    json::Array prep, hist;
    for (const auto id : prepare_) prep.push_back ((double) id);
    for (const auto& s : history_)
    {
        json::Array entries;
        for (const auto& e : s.entries)
        {
            json::Value ev;
            ev.set ("track", (double) e.track).set ("time", (double) e.time).set ("deck", e.deck);
            entries.push_back (ev);
        }
        json::Value v;
        v.set ("start", (double) s.start).set ("end", (double) s.end).set ("entries", entries);
        hist.push_back (v);
    }
    json::Value root;
    root.set ("format", "deejay-library").set ("version", 1).set ("nextId", (double) nextId_)
        .set ("tracks", tracks).set ("crates", crates).set ("smartCrates", smart).set ("prepare", prep).set ("history", hist);
    return root;
}

bool Library::fromJson (const json::Value& root)
{
    if (root["format"].string() != "deejay-library") return false;
    tracks_.clear(); byPath_.clear(); crates_.clear(); smart_.clear(); prepare_.clear(); history_.clear();
    nextId_ = (std::uint64_t) root["nextId"].integer (1);
    for (const auto& v : root["tracks"].array())
    {
        Track t;
        t.id = (std::uint64_t) v["id"].integer();
        t.path = v["path"].string(); t.title = v["title"].string(); t.artist = v["artist"].string(); t.album = v["album"].string();
        t.genre = v["genre"].string(); t.comment = v["comment"].string(); t.year = v["year"].string(); t.grouping = v["grouping"].string();
        t.label = v["label"].string(); t.remixer = v["remixer"].string();
        t.bpm = v["bpm"].number(); t.gridAnchor = v["gridAnchor"].number(); t.gridLocked = v["gridLocked"].boolean();
        t.key = (int) v["key"].integer (-1); t.lengthSeconds = v["length"].number(); t.sampleRate = v["sampleRate"].number();
        t.loudnessLufs = v["lufs"].number (-70.0); t.autoGainDb = v["autoGain"].number(); t.rating = (int) v["rating"].integer();
        t.color = (std::uint32_t) v["color"].integer(); t.playCount = (int) v["plays"].integer();
        t.added = v["added"].integer(); t.lastPlayed = v["lastPlayed"].integer(); t.fileSize = v["fileSize"].integer();
        t.fileModified = v["fileModified"].integer(); t.analysed = v["analysed"].boolean(); t.cuePoint = v["cue"].number();
        const auto& cues = v["hotCues"].array();
        for (std::size_t i = 0; i < cues.size() && i < 8; ++i)
        {
            t.hotCues[i].pos = cues[i]["pos"].number (-1.0);
            t.hotCues[i].color = (std::uint32_t) cues[i]["color"].integer();
            t.hotCues[i].label = cues[i]["label"].string();
        }
        const auto& loops = v["loops"].array();
        for (std::size_t i = 0; i < loops.size() && i < 8; ++i)
        {
            t.loops[i].in = loops[i]["in"].number (-1.0);
            t.loops[i].out = loops[i]["out"].number (-1.0);
            t.loops[i].locked = loops[i]["locked"].boolean();
            t.loops[i].label = loops[i]["label"].string();
        }
        nextId_ = std::max (nextId_, t.id + 1);
        byPath_[t.path] = t.id;
        tracks_[t.id] = std::move (t);
    }
    for (const auto& v : root["crates"].array())
    {
        Crate c;
        c.name = v["name"].string();
        for (const auto& id : v["tracks"].array()) c.tracks.push_back ((std::uint64_t) id.integer());
        crates_.push_back (std::move (c));
    }
    for (const auto& v : root["smartCrates"].array())
    {
        SmartCrate sc;
        sc.name = v["name"].string();
        sc.matchAll = v["matchAll"].boolean (true);
        for (const auto& r : v["rules"].array())
            sc.rules.push_back ({ (Field) r["field"].integer(), (Op) r["op"].integer(), r["text"].string(), r["a"].number(), r["b"].number() });
        smart_.push_back (std::move (sc));
    }
    for (const auto& id : root["prepare"].array()) prepare_.push_back ((std::uint64_t) id.integer());
    for (const auto& v : root["history"].array())
    {
        HistorySession s;
        s.start = v["start"].integer(); s.end = v["end"].integer();
        for (const auto& e : v["entries"].array()) s.entries.push_back ({ (std::uint64_t) e["track"].integer(), e["time"].integer(), (int) e["deck"].integer() });
        history_.push_back (std::move (s));
    }
    return true;
}

bool Library::load (const std::string& path, std::string* error)
{
    std::ifstream f (path, std::ios::binary);
    if (! f) { if (error) *error = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    json::Value v;
    if (! json::parse (ss.str(), v, error)) return false;
    if (! fromJson (v)) { if (error) *error = "not a DEEJ-AY library"; return false; }
    return true;
}

bool Library::save (const std::string& path, std::string* error) const
{
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f (tmp, std::ios::binary | std::ios::trunc);
        if (! f) { if (error) *error = "cannot write " + tmp; return false; }
        f << json::serialise (toJson(), 1);
        if (! f) { if (error) *error = "write failed"; return false; }
    }
    if (std::rename (tmp.c_str(), path.c_str()) != 0) { if (error) *error = "rename failed"; return false; }
    return true;
}

} // namespace dj::library
