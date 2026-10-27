// Music library: tracks with their DJ prep (beatgrid, key, cue point, 8 hot cues, 8 loop slots, colour),
// crates (nested with "Parent/Child" names), smart crates, the Prepare list and play history.
// Stored as one JSON document, written temp-file-then-rename. Message thread only.
#pragma once
#include "Tags.h"
#include "../Json.h"
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dj::library
{

struct HotCue
{
    double pos = -1.0;          // track frames (at the track's sample rate)
    std::uint32_t color = 0;    // 0xRRGGBB, 0 = default palette colour
    std::string label;
};

struct LoopSlotData
{
    double in = -1.0, out = -1.0;
    bool locked = false;
    std::string label;
};

struct Track
{
    std::uint64_t id = 0;
    std::string path;
    std::string title, artist, album, genre, comment, year, grouping, label, remixer;
    double bpm = 0.0, gridAnchor = 0.0;   // grid anchor in frames
    bool gridLocked = false;
    int key = -1;
    double lengthSeconds = 0.0, sampleRate = 0.0;
    double loudnessLufs = -70.0, autoGainDb = 0.0;
    int rating = 0;                        // 0..5
    std::uint32_t color = 0;
    int playCount = 0;
    std::int64_t added = 0, lastPlayed = 0, fileSize = 0, fileModified = 0;
    bool analysed = false, missing = false;
    double cuePoint = 0.0;
    std::array<HotCue, 8> hotCues {};
    std::array<LoopSlotData, 8> loops {};
};

struct Crate
{
    std::string name;                      // "House/Deep" for a sub-crate
    std::vector<std::uint64_t> tracks;
};

enum class Field { Title, Artist, Album, Genre, Comment, Bpm, Key, Year, Added, Rating, PlayCount, Length, Label, Grouping };
enum class Op { Contains, NotContains, Is, IsNot, Greater, Less, Between, InLastDays };

struct SmartRule
{
    Field field = Field::Genre;
    Op op = Op::Contains;
    std::string text;
    double a = 0.0, b = 0.0;
};

struct SmartCrate
{
    std::string name;
    bool matchAll = true;
    std::vector<SmartRule> rules;
};

struct HistoryEntry { std::uint64_t track = 0; std::int64_t time = 0; int deck = 0; };
struct HistorySession { std::int64_t start = 0, end = 0; std::vector<HistoryEntry> entries; };

enum class SortColumn { Title, Artist, Album, Genre, Bpm, Key, Length, Year, Added, Rating, PlayCount, Comment };

class Library
{
public:
    // persistence
    bool load (const std::string& path, std::string* error = nullptr);
    bool save (const std::string& path, std::string* error = nullptr) const;
    json::Value toJson() const;
    bool fromJson (const json::Value& v);

    // tracks
    Track& addOrUpdate (const std::string& path, const TagInfo& tags, std::int64_t now);
    Track* find (std::uint64_t id);
    const Track* find (std::uint64_t id) const;
    Track* findByPath (const std::string& path);
    void remove (std::uint64_t id);
    const std::map<std::uint64_t, Track>& tracks() const noexcept { return tracks_; }
    std::vector<std::uint64_t> allIds() const;

    // Imports a Serato prep (cues/loops/grid/colour) read from tags. Positions are converted with `sampleRate`.
    static void applySeratoPrep (Track& t, const TagInfo& tags, double sampleRate);

    // browsing
    std::vector<std::uint64_t> search (const std::vector<std::uint64_t>& scope, const std::string& query) const;
    void sort (std::vector<std::uint64_t>& ids, SortColumn column, bool ascending) const;

    // crates
    Crate& crate (const std::string& name);          // creates if missing
    bool hasCrate (const std::string& name) const;
    void deleteCrate (const std::string& name);      // also deletes its sub-crates
    void renameCrate (const std::string& from, const std::string& to);
    void addToCrate (const std::string& name, std::uint64_t id);
    void removeFromCrate (const std::string& name, std::uint64_t id);
    const std::vector<Crate>& crates() const noexcept { return crates_; }
    // A crate's tracks including all its sub-crates (Serato "include sub-crate tracks").
    std::vector<std::uint64_t> crateTracks (const std::string& name, bool includeSubcrates) const;

    // smart crates
    std::vector<SmartCrate>& smartCrates() noexcept { return smart_; }
    std::vector<std::uint64_t> evaluate (const SmartCrate& sc, std::int64_t now) const;

    // prepare list and history
    std::vector<std::uint64_t>& prepare() noexcept { return prepare_; }
    void beginSession (std::int64_t now);
    void logPlay (std::uint64_t id, int deck, std::int64_t now);
    const std::vector<HistorySession>& history() const noexcept { return history_; }

    // Serato crate files (~/Music/_Serato_/Subcrates/*.crate): returns the track paths in order.
    static std::vector<std::string> readSeratoCrate (const std::string& crateFile);

private:
    bool matches (const Track& t, const SmartRule& r, std::int64_t now) const;

    std::map<std::uint64_t, Track> tracks_;
    std::map<std::string, std::uint64_t> byPath_;
    std::uint64_t nextId_ = 1;
    std::vector<Crate> crates_;
    std::vector<SmartCrate> smart_;
    std::vector<std::uint64_t> prepare_;
    std::vector<HistorySession> history_;
};

} // namespace dj::library
