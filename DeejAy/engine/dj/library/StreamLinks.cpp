#include "StreamLinks.h"
#include "../Json.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace dj::library
{

namespace
{
    std::string lower (std::string s)
    {
        for (auto& c : s) c = (char) std::tolower ((unsigned char) c);
        return s;
    }

    std::string trim (const std::string& s)
    {
        std::size_t a = 0, b = s.size();
        while (a < b && std::isspace ((unsigned char) s[a])) ++a;
        while (b > a && std::isspace ((unsigned char) s[b - 1])) --b;
        return s.substr (a, b - a);
    }

    // value of a query parameter ("v" in "...watch?v=abc&t=1")
    std::string queryParam (const std::string& query, const std::string& name)
    {
        std::size_t p = 0;
        while (p < query.size())
        {
            const auto amp = query.find ('&', p);
            const auto part = query.substr (p, amp == std::string::npos ? std::string::npos : amp - p);
            const auto eq = part.find ('=');
            if (eq != std::string::npos && part.substr (0, eq) == name) return part.substr (eq + 1);
            if (amp == std::string::npos) break;
            p = amp + 1;
        }
        return {};
    }

    std::vector<std::string> segments (const std::string& path)
    {
        std::vector<std::string> out;
        std::size_t p = 0;
        while (p <= path.size())
        {
            const auto s = path.find ('/', p);
            const auto seg = path.substr (p, s == std::string::npos ? std::string::npos : s - p);
            if (! seg.empty()) out.push_back (seg);
            if (s == std::string::npos) break;
            p = s + 1;
        }
        return out;
    }

    bool validYouTubeId (const std::string& id)
    {
        return id.size() == 11 && std::all_of (id.begin(), id.end(), [] (char c) { return std::isalnum ((unsigned char) c) || c == '-' || c == '_'; });
    }

    // Normalised title for matching: lower case, letters and digits only, without "(feat. x)",
    // "[...]", " - original mix" / "extended mix" / "radio edit" decorations.
    std::string normaliseTitle (const std::string& in)
    {
        std::string s = lower (in);
        for (const char* open : { "(", "[" })
            for (std::size_t p; (p = s.find (open)) != std::string::npos;)
            {
                const auto close = s.find (open[0] == '(' ? ')' : ']', p);
                s.erase (p, close == std::string::npos ? std::string::npos : close - p + 1);
            }
        for (const char* tail : { " - original mix", " - extended mix", " - radio edit", " - original", " feat. ", " ft. ", " feat ", " ft " })
        {
            const auto p = s.find (tail);
            if (p != std::string::npos) s.erase (p);
        }
        std::string out;
        for (char c : s) if (std::isalnum ((unsigned char) c)) out += c;
        return out;
    }
}

LinkInfo classifyLink (const std::string& text)
{
    LinkInfo info;
    std::string url = trim (text);
    if (url.empty()) return info;

    // spotify:track:ID
    if (lower (url).rfind ("spotify:", 0) == 0)
    {
        const auto parts = segments ([&] { std::string u = url; std::replace (u.begin(), u.end(), ':', '/'); return u; }());
        info.service = LinkService::Spotify;
        info.isTrack = parts.size() >= 3 && parts[1] == "track";
        info.id = parts.size() >= 3 ? parts[2] : std::string();
        info.url = info.isTrack ? "https://open.spotify.com/track/" + info.id : url;
        return info;
    }

    std::string rest = url;
    const auto scheme = rest.find ("://");
    if (scheme != std::string::npos) rest = rest.substr (scheme + 3);
    const auto slash = rest.find ('/');
    std::string host = lower (rest.substr (0, slash));
    std::string pathAndQuery = slash == std::string::npos ? std::string() : rest.substr (slash);
    if (host.rfind ("www.", 0) == 0) host = host.substr (4);
    const auto q = pathAndQuery.find ('?');
    const std::string path = pathAndQuery.substr (0, q);
    const std::string query = q == std::string::npos ? std::string() : pathAndQuery.substr (q + 1);
    const auto seg = segments (path);
    info.url = "https://" + rest;

    if (host == "youtube.com" || host == "m.youtube.com" || host == "music.youtube.com")
    {
        info.service = LinkService::YouTube;
        if (! seg.empty() && seg[0] == "watch") info.id = queryParam (query, "v");
        else if (seg.size() >= 2 && (seg[0] == "shorts" || seg[0] == "live" || seg[0] == "embed")) info.id = seg[1];
        info.isTrack = validYouTubeId (info.id);
        if (info.isTrack) info.url = "https://www.youtube.com/watch?v=" + info.id;
        return info;
    }
    if (host == "youtu.be")
    {
        info.service = LinkService::YouTube;
        info.id = seg.empty() ? std::string() : seg[0];
        info.isTrack = validYouTubeId (info.id);
        if (info.isTrack) info.url = "https://www.youtube.com/watch?v=" + info.id;
        return info;
    }
    if (host == "soundcloud.com" || host == "m.soundcloud.com" || host == "on.soundcloud.com")
    {
        info.service = LinkService::SoundCloud;
        if (host == "on.soundcloud.com") info.isTrack = seg.size() == 1;    // short link; yt-dlp follows it
        else
        {
            // soundcloud.com/<artist>/<track>; "sets", "likes", "tracks", ... are not single tracks
            static const char* notTracks[] { "sets", "likes", "tracks", "albums", "reposts", "followers", "following", "popular-tracks" };
            info.isTrack = seg.size() == 2 && std::none_of (std::begin (notTracks), std::end (notTracks), [&] (const char* w) { return seg[1] == w; });
        }
        if (host == "m.soundcloud.com") info.url = "https://soundcloud.com" + path;
        return info;
    }
    if (host == "open.spotify.com" || host == "play.spotify.com")
    {
        info.service = LinkService::Spotify;
        std::size_t i = 0;
        if (! seg.empty() && seg[0].rfind ("intl-", 0) == 0) i = 1;           // open.spotify.com/intl-de/track/ID
        info.isTrack = seg.size() >= i + 2 && seg[i] == "track";
        info.id = seg.size() >= i + 2 ? seg[i + 1] : std::string();
        info.url = info.isTrack ? "https://open.spotify.com/track/" + info.id : info.url;
        return info;
    }
    return info;
}

const char* serviceName (LinkService s) noexcept
{
    switch (s)
    {
        case LinkService::YouTube: return "YouTube";
        case LinkService::SoundCloud: return "SoundCloud";
        case LinkService::Spotify: return "Spotify";
        case LinkService::Unknown: break;
    }
    return "link";
}

std::vector<std::string> ytDlpArguments (const std::string& url, const std::string& outputTemplate, bool haveFfmpeg)
{
    // --print implies --quiet; --progress brings the progress lines back (one per line with --newline)
    std::vector<std::string> a { "--no-playlist", "--newline", "--progress", "--no-warnings", "--restrict-filenames" };
    if (haveFfmpeg)
        a.insert (a.end(), { "-f", "bestaudio/best", "-x", "--audio-format", "m4a", "--audio-quality", "0" });
    else
        a.insert (a.end(), { "-f", "bestaudio[ext=m4a]/bestaudio[ext=mp3]/bestaudio[acodec=mp3]" });
    a.insert (a.end(), { "--print", "after_move:filepath", "-o", outputTemplate, url });
    return a;
}

std::optional<double> parseYtDlpProgress (const std::string& line)
{
    if (line.rfind ("[download]", 0) != 0) return std::nullopt;
    const auto pct = line.find ('%');
    if (pct == std::string::npos) return std::nullopt;
    std::size_t b = pct;
    while (b > 0 && (std::isdigit ((unsigned char) line[b - 1]) || line[b - 1] == '.')) --b;
    if (b == pct) return std::nullopt;
    return std::clamp (std::atof (line.substr (b, pct - b).c_str()), 0.0, 100.0);
}

void splitDownloadName (const std::string& fileName, std::string& artist, std::string& title)
{
    std::string stem = fileName;
    const auto slash = stem.find_last_of ("/\\");
    if (slash != std::string::npos) stem = stem.substr (slash + 1);
    const auto dot = stem.find_last_of ('.');
    if (dot != std::string::npos) stem = stem.substr (0, dot);
    const auto br = stem.rfind ('[');
    if (br != std::string::npos && stem.back() == ']') stem = trim (stem.substr (0, br));
    std::replace (stem.begin(), stem.end(), '_', ' ');   // --restrict-filenames uses '_' for spaces
    const auto sep = stem.find (" - ");
    if (sep != std::string::npos) { artist = trim (stem.substr (0, sep)); title = trim (stem.substr (sep + 3)); }
    else { artist.clear(); title = trim (stem); }
}

bool parseOEmbed (const std::string& text, std::string& title, std::string& author)
{
    json::Value v;
    if (! json::parse (text, v)) return false;
    title = v["title"].string();
    author = v["author_name"].string();
    return ! title.empty();
}

std::string spotifyOEmbedUrl (const std::string& trackUrl)
{
    std::string enc;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : trackUrl)
    {
        if (std::isalnum (c) || c == '-' || c == '_' || c == '.' || c == '~') enc += (char) c;
        else { enc += '%'; enc += hex[c >> 4]; enc += hex[c & 15]; }
    }
    return "https://open.spotify.com/oembed?url=" + enc;
}

std::uint64_t findTrackByTitle (const Library& lib, const std::string& title, const std::string& artist)
{
    const auto t = normaliseTitle (title);
    const auto a = normaliseTitle (artist);
    if (t.empty()) return 0;
    std::uint64_t best = 0;
    int bestScore = 0;
    for (const auto& [id, track] : lib.tracks())
    {
        const auto tt = normaliseTitle (track.title);
        if (tt.empty()) continue;
        int score = 0;
        if (tt == t) score = 10;
        else if (tt.size() >= 4 && t.size() >= 4 && (tt.find (t) != std::string::npos || t.find (tt) != std::string::npos)) score = 5;
        if (score == 0) continue;
        if (! a.empty())
        {
            const auto ta = normaliseTitle (track.artist);
            if (! ta.empty() && (ta == a || ta.find (a) != std::string::npos || a.find (ta) != std::string::npos)) score += 5;
        }
        if (track.missing) score -= 3;
        if (score > bestScore) { bestScore = score; best = id; }
    }
    return bestScore >= 5 ? best : 0;
}

} // namespace dj::library
