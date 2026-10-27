// Tag reading: ID3v2.2/2.3/2.4 (MP3, AIFF, WAV), FLAC Vorbis comments, MP4/M4A ilst atoms. Also reads
// Serato's own prep stored in tags (cue points, loops, colours, beatgrid) so existing Serato crates move
// over with their cues: "Serato Markers2" and "Serato BeatGrid" (ID3 GEOB), SERATO_MARKERS_V2 /
// SERATO_BEATGRID (FLAC), ----:com.serato.dj:markersv2 / beatgrid (MP4). Formats as publicly documented by
// the serato-tags project; read-only.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dj::library
{

struct SeratoCue { int index = -1; double ms = 0.0; std::uint32_t color = 0; std::string name; };
struct SeratoLoop { int index = -1; double startMs = 0.0, endMs = 0.0; bool locked = false; std::string name; };

struct TagInfo
{
    std::string title, artist, album, genre, comment, year, grouping, label, remixer, keyText;
    double bpm = 0.0;
    int key = -1;                       // parsed from keyText
    std::vector<SeratoCue> cues;
    std::vector<SeratoLoop> loops;
    std::uint32_t trackColor = 0;
    bool hasTrackColor = false;
    bool bpmLocked = false;
    double gridAnchorSeconds = -1.0;    // Serato beatgrid (constant-tempo part)
    double gridBpm = 0.0;
};

// Reads tags from a file; returns false when the format is not recognised (title falls back to the name).
bool readTags (const std::string& path, TagInfo& out);

// Parses tag key spellings: "Am", "A minor", "A#m", "Bbm", "8A" (Camelot), "1m"/"1d" (Open Key).
int parseKey (const std::string& text);

// Decoders for the Serato payloads (exposed for tests).
bool parseSeratoMarkers2 (const std::vector<std::uint8_t>& payload, TagInfo& out);
bool parseSeratoBeatGrid (const std::vector<std::uint8_t>& payload, TagInfo& out);
std::vector<std::uint8_t> base64Decode (const std::string& text);

} // namespace dj::library
