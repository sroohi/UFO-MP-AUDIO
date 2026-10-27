#include "Tags.h"
#include "../Common.h"
#include <cctype>
#include <cstring>
#include <fstream>

namespace dj::library
{

namespace
{
    using Bytes = std::vector<std::uint8_t>;

    std::uint32_t be32 (const std::uint8_t* p) { return ((std::uint32_t) p[0] << 24) | ((std::uint32_t) p[1] << 16) | ((std::uint32_t) p[2] << 8) | p[3]; }
    std::uint32_t le32 (const std::uint8_t* p) { return ((std::uint32_t) p[3] << 24) | ((std::uint32_t) p[2] << 16) | ((std::uint32_t) p[1] << 8) | p[0]; }
    std::uint32_t be24 (const std::uint8_t* p) { return ((std::uint32_t) p[0] << 16) | ((std::uint32_t) p[1] << 8) | p[2]; }
    std::uint32_t syncsafe (const std::uint8_t* p) { return ((std::uint32_t) (p[0] & 0x7F) << 21) | ((std::uint32_t) (p[1] & 0x7F) << 14) | ((std::uint32_t) (p[2] & 0x7F) << 7) | (p[3] & 0x7F); }
    float beFloat (const std::uint8_t* p) { const std::uint32_t u = be32 (p); float f; std::memcpy (&f, &u, 4); return f; }

    bool readFile (const std::string& path, Bytes& out, std::size_t limit = 64u << 20)
    {
        std::ifstream f (path, std::ios::binary);
        if (! f) return false;
        f.seekg (0, std::ios::end);
        const auto size = (std::size_t) std::max<std::streamoff> (0, f.tellg());
        f.seekg (0);
        out.resize (std::min (size, limit));
        f.read (reinterpret_cast<char*> (out.data()), (std::streamsize) out.size());
        return true;
    }

    void appendUtf8 (std::string& s, std::uint32_t cp)
    {
        if (cp < 0x80) s += (char) cp;
        else if (cp < 0x800) { s += (char) (0xC0 | (cp >> 6)); s += (char) (0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += (char) (0xE0 | (cp >> 12)); s += (char) (0x80 | ((cp >> 6) & 0x3F)); s += (char) (0x80 | (cp & 0x3F)); }
        else { s += (char) (0xF0 | (cp >> 18)); s += (char) (0x80 | ((cp >> 12) & 0x3F)); s += (char) (0x80 | ((cp >> 6) & 0x3F)); s += (char) (0x80 | (cp & 0x3F)); }
    }

    // Decodes ID3 text in `enc` (0 latin-1, 1 UTF-16 BOM, 2 UTF-16BE, 3 UTF-8) up to the terminator.
    // Advances `p` past the terminator.
    std::string decodeText (const std::uint8_t*& p, const std::uint8_t* end, int enc)
    {
        std::string s;
        if (enc == 0 || enc == 3)
        {
            while (p < end && *p != 0)
            {
                if (enc == 0) appendUtf8 (s, *p); else s += (char) *p;
                ++p;
            }
            if (p < end) ++p;
            return s;
        }
        bool bigEndian = enc == 2;
        if (enc == 1 && p + 1 < end)
        {
            if (p[0] == 0xFF && p[1] == 0xFE) { bigEndian = false; p += 2; }
            else if (p[0] == 0xFE && p[1] == 0xFF) { bigEndian = true; p += 2; }
        }
        while (p + 1 < end)
        {
            std::uint32_t u = bigEndian ? ((std::uint32_t) p[0] << 8) | p[1] : ((std::uint32_t) p[1] << 8) | p[0];
            p += 2;
            if (u == 0) return s;
            if (u >= 0xD800 && u < 0xDC00 && p + 1 < end)
            {
                const std::uint32_t lo = bigEndian ? ((std::uint32_t) p[0] << 8) | p[1] : ((std::uint32_t) p[1] << 8) | p[0];
                p += 2;
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            }
            appendUtf8 (s, u);
        }
        p = end;
        return s;
    }

    std::string trim (std::string s)
    {
        while (! s.empty() && (std::isspace ((unsigned char) s.back()) || s.back() == '\0')) s.pop_back();
        std::size_t i = 0;
        while (i < s.size() && std::isspace ((unsigned char) s[i])) ++i;
        return s.substr (i);
    }

    std::string lower (std::string s)
    {
        for (auto& c : s) c = (char) std::tolower ((unsigned char) c);
        return s;
    }

    void assignField (TagInfo& t, const std::string& field, const std::string& value)
    {
        const std::string v = trim (value);
        if (v.empty()) return;
        if (field == "title") t.title = v;
        else if (field == "artist") t.artist = v;
        else if (field == "album") t.album = v;
        else if (field == "genre")
        {
            // ID3v1 style "(17)Rock" / "(17)"
            std::string g = v;
            if (g.size() > 2 && g[0] == '(' && g.find (')') != std::string::npos) { const auto c = g.find (')'); g = c + 1 < g.size() ? g.substr (c + 1) : g; }
            t.genre = g;
        }
        else if (field == "comment") t.comment = v;
        else if (field == "year") t.year = v.substr (0, 4);
        else if (field == "grouping") t.grouping = v;
        else if (field == "label") t.label = v;
        else if (field == "remixer") t.remixer = v;
        else if (field == "bpm") t.bpm = std::atof (v.c_str());
        else if (field == "key") { t.keyText = v; t.key = parseKey (v); }
    }

    // A GEOB body (also used inside FLAC/MP4 base64): mime \0 filename \0 description \0 data
    void handleGeob (const std::uint8_t* p, const std::uint8_t* end, int enc, TagInfo& t)
    {
        decodeText (p, end, 0);                 // mime type is latin-1
        decodeText (p, end, enc);               // file name
        const std::string desc = decodeText (p, end, enc);
        const Bytes data (p, end);
        if (desc == "Serato Markers2") parseSeratoMarkers2 (data, t);
        else if (desc == "Serato BeatGrid") parseSeratoBeatGrid (data, t);
    }

    void handleSeratoBase64Field (const std::string& name, const std::string& b64, TagInfo& t)
    {
        const Bytes raw = base64Decode (b64);
        if (raw.size() < 4) return;
        const std::uint8_t* p = raw.data();
        const std::uint8_t* end = p + raw.size();
        static const char mime[] = "application/octet-stream";
        if (raw.size() > sizeof (mime) && std::memcmp (p, mime, sizeof (mime) - 1) == 0)
        {
            handleGeob (p, end, 0, t);
            return;
        }
        if (name.find ("markers") != std::string::npos) parseSeratoMarkers2 (raw, t);
        else if (name.find ("beatgrid") != std::string::npos) parseSeratoBeatGrid (raw, t);
    }

    // ------------------------------------------------------------------------------------- ID3v2
    bool parseId3 (const std::uint8_t* d, std::size_t size, TagInfo& t)
    {
        if (size < 10 || std::memcmp (d, "ID3", 3) != 0) return false;
        const int ver = d[3];
        const int flags = d[5];
        std::size_t tagSize = syncsafe (d + 6);
        if (tagSize + 10 > size) tagSize = size - 10;
        Bytes body (d + 10, d + 10 + tagSize);
        if ((flags & 0x80) && ver < 4)
        {
            Bytes un;
            for (std::size_t i = 0; i < body.size(); ++i) { un.push_back (body[i]); if (body[i] == 0xFF && i + 1 < body.size() && body[i + 1] == 0x00) ++i; }
            body.swap (un);
        }
        std::size_t pos = 0;
        if (flags & 0x40)
        {
            if (body.size() < 4) return true;
            pos = ver == 4 ? syncsafe (body.data()) : be32 (body.data()) + 4;
        }
        const int idLen = ver == 2 ? 3 : 4, hdrLen = ver == 2 ? 6 : 10;
        while (pos + (std::size_t) hdrLen <= body.size())
        {
            const std::uint8_t* h = body.data() + pos;
            if (h[0] == 0) break; // padding
            const std::string id (reinterpret_cast<const char*> (h), (std::size_t) idLen);
            std::size_t fsize = ver == 2 ? be24 (h + 3) : ver == 4 ? syncsafe (h + 4) : be32 (h + 4);
            const int fflags = ver == 2 ? 0 : ((int) h[8] << 8) | h[9];
            pos += (std::size_t) hdrLen;
            if (pos + fsize > body.size()) break;
            Bytes frame (body.begin() + (std::ptrdiff_t) pos, body.begin() + (std::ptrdiff_t) (pos + fsize));
            pos += fsize;
            if (ver == 4)
            {
                if ((fflags & 0x0001) && frame.size() >= 4) frame.erase (frame.begin(), frame.begin() + 4); // data length indicator
                if (fflags & 0x0002)
                {
                    Bytes un;
                    for (std::size_t i = 0; i < frame.size(); ++i) { un.push_back (frame[i]); if (frame[i] == 0xFF && i + 1 < frame.size() && frame[i + 1] == 0x00) ++i; }
                    frame.swap (un);
                }
            }
            if (frame.empty()) continue;
            const std::uint8_t* p = frame.data();
            const std::uint8_t* end = p + frame.size();
            auto text = [&] { const int enc = *p++; return decodeText (p, end, enc); };
            const std::string mid = ver == 2 ? id : id;
            if (mid == "TIT2" || mid == "TT2") assignField (t, "title", text());
            else if (mid == "TPE1" || mid == "TP1") assignField (t, "artist", text());
            else if (mid == "TALB" || mid == "TAL") assignField (t, "album", text());
            else if (mid == "TCON" || mid == "TCO") assignField (t, "genre", text());
            else if (mid == "TBPM" || mid == "TBP") assignField (t, "bpm", text());
            else if (mid == "TKEY" || mid == "TKE") assignField (t, "key", text());
            else if (mid == "TYER" || mid == "TDRC" || mid == "TYE") assignField (t, "year", text());
            else if (mid == "TIT1" || mid == "TT1" || mid == "GRP1") assignField (t, "grouping", text());
            else if (mid == "TPUB" || mid == "TPB") assignField (t, "label", text());
            else if (mid == "TPE4" || mid == "TP4") assignField (t, "remixer", text());
            else if (mid == "COMM" || mid == "COM")
            {
                const int enc = *p++;
                if (end - p < 3) continue;
                p += 3; // language
                decodeText (p, end, enc);
                assignField (t, "comment", decodeText (p, end, enc));
            }
            else if (mid == "GEOB" || mid == "GEO")
            {
                const int enc = *p++;
                handleGeob (p, end, enc, t);
            }
        }
        return true;
    }

    // ------------------------------------------------------------------------------- FLAC / Vorbis
    bool parseFlac (const std::uint8_t* d, std::size_t size, TagInfo& t)
    {
        if (size < 4 || std::memcmp (d, "fLaC", 4) != 0) return false;
        std::size_t pos = 4;
        while (pos + 4 <= size)
        {
            const bool last = (d[pos] & 0x80) != 0;
            const int type = d[pos] & 0x7F;
            const std::size_t len = be24 (d + pos + 1);
            pos += 4;
            if (pos + len > size) break;
            if (type == 4)
            {
                const std::uint8_t* p = d + pos;
                const std::uint8_t* end = p + len;
                if (end - p < 8) break;
                const std::uint32_t vendor = le32 (p); p += 4 + vendor;
                if (p + 4 > end) break;
                const std::uint32_t count = le32 (p); p += 4;
                for (std::uint32_t i = 0; i < count && p + 4 <= end; ++i)
                {
                    const std::uint32_t l = le32 (p); p += 4;
                    if (p + l > end) break;
                    const std::string entry (reinterpret_cast<const char*> (p), l);
                    p += l;
                    const auto eq = entry.find ('=');
                    if (eq == std::string::npos) continue;
                    const std::string k = lower (entry.substr (0, eq)), v = entry.substr (eq + 1);
                    if (k == "title") assignField (t, "title", v);
                    else if (k == "artist") assignField (t, "artist", v);
                    else if (k == "album") assignField (t, "album", v);
                    else if (k == "genre") assignField (t, "genre", v);
                    else if (k == "comment" || k == "description") assignField (t, "comment", v);
                    else if (k == "date" || k == "year") assignField (t, "year", v);
                    else if (k == "bpm" || k == "tempo") assignField (t, "bpm", v);
                    else if (k == "initialkey" || k == "key") assignField (t, "key", v);
                    else if (k == "grouping") assignField (t, "grouping", v);
                    else if (k == "label" || k == "organization" || k == "publisher") assignField (t, "label", v);
                    else if (k == "remixer" || k == "mixartist") assignField (t, "remixer", v);
                    else if (k == "serato_markers_v2") handleSeratoBase64Field ("markers", v, t);
                    else if (k == "serato_beatgrid") handleSeratoBase64Field ("beatgrid", v, t);
                }
            }
            pos += len;
            if (last) break;
        }
        return true;
    }

    // ---------------------------------------------------------------------------------------- MP4
    void walkMp4 (const std::uint8_t* d, std::size_t size, TagInfo& t, int depth)
    {
        std::size_t pos = 0;
        while (pos + 8 <= size && depth < 8)
        {
            std::uint64_t len = be32 (d + pos);
            const std::string type (reinterpret_cast<const char*> (d + pos + 4), 4);
            std::size_t hdr = 8;
            if (len == 1 && pos + 16 <= size) { len = ((std::uint64_t) be32 (d + pos + 8) << 32) | be32 (d + pos + 12); hdr = 16; }
            if (len == 0) len = size - pos;
            if (len < hdr || pos + len > size) break;
            const std::uint8_t* body = d + pos + hdr;
            const std::size_t blen = (std::size_t) len - hdr;
            if (type == "moov" || type == "udta" || type == "ilst") walkMp4 (body, blen, t, depth + 1);
            else if (type == "meta" && blen > 4) walkMp4 (body + 4, blen - 4, t, depth + 1);
            else if (depth >= 2)
            {
                // ilst item: children 'mean', 'name', 'data'
                std::string mean, name, value;
                Bytes raw;
                int dataType = 0;
                std::size_t q = 0;
                while (q + 8 <= blen)
                {
                    const std::uint32_t cl = be32 (body + q);
                    if (cl < 8 || q + cl > blen) break;
                    const std::string ct (reinterpret_cast<const char*> (body + q + 4), 4);
                    const std::uint8_t* cb = body + q + 8;
                    const std::size_t cbl = cl - 8;
                    if (ct == "mean" && cbl > 4) mean.assign (reinterpret_cast<const char*> (cb + 4), cbl - 4);
                    else if (ct == "name" && cbl > 4) name.assign (reinterpret_cast<const char*> (cb + 4), cbl - 4);
                    else if (ct == "data" && cbl >= 8)
                    {
                        dataType = (int) (be32 (cb) & 0xFFFFFF);
                        raw.assign (cb + 8, cb + cbl);
                        value.assign (reinterpret_cast<const char*> (cb + 8), cbl - 8);
                    }
                    q += cl;
                }
                const auto u8 = [&] (const char* s) { return type == std::string ("\xA9") + s; };
                if (u8 ("nam")) assignField (t, "title", value);
                else if (u8 ("ART")) assignField (t, "artist", value);
                else if (u8 ("alb")) assignField (t, "album", value);
                else if (u8 ("gen")) assignField (t, "genre", value);
                else if (u8 ("cmt")) assignField (t, "comment", value);
                else if (u8 ("day")) assignField (t, "year", value);
                else if (u8 ("grp")) assignField (t, "grouping", value);
                else if (type == "tmpo" && raw.size() >= 2) t.bpm = (double) (((int) raw[0] << 8) | raw[1]);
                else if (type == "----")
                {
                    const std::string n = lower (name);
                    if (n == "initialkey" || n == "key") assignField (t, "key", value);
                    else if (lower (mean).find ("serato") != std::string::npos && (n == "markersv2" || n == "beatgrid"))
                        handleSeratoBase64Field (n == "markersv2" ? "markers" : "beatgrid", value, t);
                    (void) dataType;
                }
            }
            pos += (std::size_t) len;
        }
    }

    bool parseMp4 (const std::uint8_t* d, std::size_t size, TagInfo& t)
    {
        if (size < 12 || std::memcmp (d + 4, "ftyp", 4) != 0) return false;
        walkMp4 (d, size, t, 0);
        return true;
    }

    // ------------------------------------------------------------------------------- AIFF / WAV
    bool parseIff (const std::uint8_t* d, std::size_t size, TagInfo& t)
    {
        const bool aiff = size >= 12 && std::memcmp (d, "FORM", 4) == 0;
        const bool wav = size >= 12 && std::memcmp (d, "RIFF", 4) == 0 && std::memcmp (d + 8, "WAVE", 4) == 0;
        if (! aiff && ! wav) return false;
        std::size_t pos = 12;
        while (pos + 8 <= size)
        {
            const std::string id (reinterpret_cast<const char*> (d + pos), 4);
            const std::size_t len = aiff ? be32 (d + pos + 4) : le32 (d + pos + 4);
            const std::uint8_t* body = d + pos + 8;
            if (pos + 8 + len > size) break;
            if (id == "ID3 " || id == "id3 ") parseId3 (body, len, t);
            else if (wav && id == "LIST" && len > 4 && std::memcmp (body, "INFO", 4) == 0)
            {
                std::size_t q = 4;
                while (q + 8 <= len)
                {
                    const std::string sid (reinterpret_cast<const char*> (body + q), 4);
                    const std::size_t sl = le32 (body + q + 4);
                    if (q + 8 + sl > len) break;
                    const std::string v (reinterpret_cast<const char*> (body + q + 8), sl);
                    if (sid == "INAM" && t.title.empty()) assignField (t, "title", v);
                    else if (sid == "IART" && t.artist.empty()) assignField (t, "artist", v);
                    else if (sid == "IPRD" && t.album.empty()) assignField (t, "album", v);
                    else if (sid == "IGNR" && t.genre.empty()) assignField (t, "genre", v);
                    else if (sid == "ICMT" && t.comment.empty()) assignField (t, "comment", v);
                    q += 8 + sl + (sl & 1);
                }
            }
            pos += 8 + len + (len & 1);
        }
        return true;
    }
}

std::vector<std::uint8_t> base64Decode (const std::string& text)
{
    static int table[256];
    static bool init = false;
    if (! init)
    {
        for (int& v : table) v = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[(unsigned char) a[i]] = i;
        init = true;
    }
    std::vector<std::uint8_t> out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : text)
    {
        const int v = table[(unsigned char) c];
        if (v < 0) continue; // whitespace, '=', newlines (Serato wraps lines and may omit padding)
        acc = (acc << 6) | (std::uint32_t) v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back ((std::uint8_t) ((acc >> bits) & 0xFF)); }
    }
    return out;
}

bool parseSeratoMarkers2 (const std::vector<std::uint8_t>& payload, TagInfo& t)
{
    if (payload.size() < 3 || payload[0] != 0x01 || payload[1] != 0x01) return false;
    // the GEOB body is "\x01\x01" + base64 text (terminated by \0)
    std::string b64;
    for (std::size_t i = 2; i < payload.size() && payload[i] != 0; ++i) b64 += (char) payload[i];
    const Bytes data = base64Decode (b64);
    if (data.size() < 2 || data[0] != 0x01 || data[1] != 0x01) return false;
    std::size_t pos = 2;
    while (pos < data.size())
    {
        std::string type;
        while (pos < data.size() && data[pos] != 0) type += (char) data[pos++];
        if (pos >= data.size() || type.empty()) break;
        ++pos;
        if (pos + 4 > data.size()) break;
        const std::size_t len = be32 (data.data() + pos);
        pos += 4;
        if (pos + len > data.size()) break;
        const std::uint8_t* e = data.data() + pos;
        if (type == "COLOR" && len >= 4)
        {
            t.trackColor = ((std::uint32_t) e[1] << 16) | ((std::uint32_t) e[2] << 8) | e[3];
            t.hasTrackColor = true;
        }
        else if (type == "CUE" && len >= 13)
        {
            // 00 | index | position ms | 00 | RR GG BB | 00 00 | name\0
            SeratoCue c;
            c.index = e[1];
            c.ms = (double) be32 (e + 2);
            c.color = ((std::uint32_t) e[7] << 16) | ((std::uint32_t) e[8] << 8) | e[9];
            for (std::size_t k = 12; k < len && e[k] != 0; ++k) c.name += (char) e[k];
            t.cues.push_back (c);
        }
        else if (type == "LOOP" && len >= 20)
        {
            // 00 | index | start ms | end ms | ff ff ff ff | 00 RR GG BB | locked | name\0
            SeratoLoop l;
            l.index = e[1];
            l.startMs = (double) be32 (e + 2);
            l.endMs = (double) be32 (e + 6);
            l.locked = e[18] != 0;
            for (std::size_t k = 19; k < len && e[k] != 0; ++k) l.name += (char) e[k];
            t.loops.push_back (l);
        }
        else if (type == "BPMLOCK" && len >= 1)
            t.bpmLocked = e[0] != 0;
        pos += len;
    }
    return true;
}

bool parseSeratoBeatGrid (const std::vector<std::uint8_t>& p, TagInfo& t)
{
    if (p.size() < 6 || p[0] != 0x01 || p[1] != 0x00) return false;
    const std::uint32_t count = be32 (p.data() + 2);
    if (count == 0 || p.size() < 6 + (std::size_t) count * 8) return false;
    const std::uint8_t* m = p.data() + 6;
    const double firstPos = beFloat (m);
    const std::uint8_t* last = m + (count - 1) * 8;
    double bpm = beFloat (last + 4);
    if (count > 1)
    {
        // non-terminal marker: beats until the next marker -> tempo of the first segment
        const double beats = (double) be32 (m + 4);
        const double next = beFloat (m + 8);
        if (beats > 0 && next > firstPos) bpm = 60.0 * beats / (next - firstPos);
    }
    if (! (bpm > 1.0 && bpm < 400.0)) return false;
    t.gridAnchorSeconds = firstPos;
    t.gridBpm = bpm;
    return true;
}

int parseKey (const std::string& text)
{
    std::string s = trim (text);
    if (s.empty()) return -1;
    // Unicode sharps/flats
    auto replaceAll = [&] (const std::string& from, const std::string& to) { for (std::size_t p; (p = s.find (from)) != std::string::npos;) s.replace (p, from.size(), to); };
    replaceAll ("\xE2\x99\xAF", "#");
    replaceAll ("\xE2\x99\xAD", "b");
    // Camelot "8A" / Open Key "1m"
    if (std::isdigit ((unsigned char) s[0]))
    {
        const int num = std::atoi (s.c_str());
        const char suffix = (char) std::tolower ((unsigned char) s.back());
        if (num < 1 || num > 12) return -1;
        int camelot = num;
        bool minor;
        if (suffix == 'a' || suffix == 'b') minor = suffix == 'a';
        else if (suffix == 'm' || suffix == 'd') { minor = suffix == 'm'; camelot = (num + 6) % 12 + 1; }
        else return -1;
        for (int k = 0; k < 24; ++k) if (camelotNumber (k) == camelot && camelotIsMinor (k) == minor) return k;
        return -1;
    }
    static const int letterPc[7] { 9, 11, 0, 2, 4, 5, 7 }; // A..G
    const char letter = (char) std::toupper ((unsigned char) s[0]);
    if (letter < 'A' || letter > 'G') return -1;
    int pc = letterPc[letter - 'A'];
    std::size_t i = 1;
    if (i < s.size() && s[i] == '#') { ++pc; ++i; }
    else if (i < s.size() && s[i] == 'b') { --pc; ++i; }
    const std::string rest = lower (trim (s.substr (i)));
    const bool minor = rest == "m" || rest.rfind ("min", 0) == 0 || rest == "-";
    pc = (pc % 12 + 12) % 12;
    return (minor ? 12 : 0) + pc;
}

bool readTags (const std::string& path, TagInfo& t)
{
    Bytes d;
    if (! readFile (path, d)) return false;
    bool ok = parseId3 (d.data(), d.size(), t) || parseFlac (d.data(), d.size(), t) || parseMp4 (d.data(), d.size(), t) || parseIff (d.data(), d.size(), t);
    if (t.title.empty())
    {
        // "Artist - Title.ext" fallback
        std::string name = path.substr (path.find_last_of ("/\\") + 1);
        name = name.substr (0, name.find_last_of ('.'));
        const auto dash = name.find (" - ");
        if (dash != std::string::npos && t.artist.empty()) { t.artist = trim (name.substr (0, dash)); t.title = trim (name.substr (dash + 3)); }
        else t.title = name;
    }
    if (t.bpm <= 0.0 && t.gridBpm > 0.0) t.bpm = t.gridBpm;
    return ok;
}

} // namespace dj::library
