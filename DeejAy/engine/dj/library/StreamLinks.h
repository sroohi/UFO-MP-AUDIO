// Links pasted into a deck's URL box.
//
// YouTube and SoundCloud: DEEJ-AY runs yt-dlp (https://github.com/yt-dlp/yt-dlp), which the user installs
// themselves; it is not bundled. Downloading is subject to the service's terms and to copyright: use it for
// material you have the rights to (your own uploads, Creative Commons, promos you were sent).
//
// Spotify: audio is DRM-protected and Spotify's terms forbid DJ use, so no app can legitimately play a
// Spotify stream into a mixer. DEEJ-AY reads the track title from Spotify's public link preview (oEmbed)
// and loads the matching track from the user's own library.
#pragma once
#include "Library.h"
#include <optional>
#include <string>
#include <vector>

namespace dj::library
{

enum class LinkService { Unknown, YouTube, SoundCloud, Spotify };

struct LinkInfo
{
    LinkService service = LinkService::Unknown;
    std::string url;          // cleaned (trimmed, https, tracking parameters kept)
    std::string id;           // YouTube video id, Spotify track id; empty for SoundCloud
    bool isTrack = true;      // false for playlists, albums, artists, sets (not loadable onto a deck)
};

LinkInfo classifyLink (const std::string& text);
const char* serviceName (LinkService s) noexcept;

// yt-dlp command line (without the program) that downloads one item's best audio into `outputTemplate`
// and prints the final file path. With ffmpeg available the audio is converted to M4A (AAC), which Core
// Audio decodes; without it yt-dlp is asked for an M4A or MP3 stream directly.
std::vector<std::string> ytDlpArguments (const std::string& url, const std::string& outputTemplate, bool haveFfmpeg);

// Download percentage from a yt-dlp "--newline" progress line ("[download]  42.3% of ..."), if it is one.
std::optional<double> parseYtDlpProgress (const std::string& line);

// "Uploader - Title [id].m4a" (DEEJ-AY's output template) -> artist, title
void splitDownloadName (const std::string& fileName, std::string& artist, std::string& title);

// Title and author from an oEmbed JSON response (author is often absent for Spotify tracks).
bool parseOEmbed (const std::string& json, std::string& title, std::string& author);
std::string spotifyOEmbedUrl (const std::string& trackUrl);

// Best library match for a title (and optional artist): normalised comparison that ignores case,
// punctuation, "(feat. ...)" and "- Original Mix". 0 when nothing matches well enough.
std::uint64_t findTrackByTitle (const Library& lib, const std::string& title, const std::string& artist);

} // namespace dj::library
