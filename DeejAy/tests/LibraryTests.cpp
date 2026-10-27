// Library, tag and Serato-prep import tests (files are synthesised byte by byte).
#include "TestHarness.h"
#include "dj/library/Library.h"
#include "dj/library/Samples.h"
#include "dj/library/StreamLinks.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

using namespace dj;
using namespace dj::library;

namespace
{
using Bytes = std::vector<std::uint8_t>;

void be32 (Bytes& b, std::uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back ((std::uint8_t) (v >> s)); }
void le32 (Bytes& b, std::uint32_t v) { for (int s = 0; s <= 24; s += 8) b.push_back ((std::uint8_t) (v >> s)); }
void str (Bytes& b, const std::string& s, bool nul = false) { b.insert (b.end(), s.begin(), s.end()); if (nul) b.push_back (0); }

std::string base64 (const Bytes& in)
{
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::uint32_t acc = 0; int bits = 0;
    for (auto c : in) { acc = (acc << 8) | c; bits += 8; while (bits >= 6) { bits -= 6; out += a[(acc >> bits) & 63]; } }
    if (bits > 0) out += a[(acc << (6 - bits)) & 63];
    return out; // Serato omits padding
}

Bytes seratoMarkers2()
{
    Bytes inner { 0x01, 0x01 };
    auto entry = [&] (const std::string& type, const Bytes& data) { str (inner, type, true); be32 (inner, (std::uint32_t) data.size()); inner.insert (inner.end(), data.begin(), data.end()); };
    entry ("COLOR", { 0x00, 0xFF, 0x99, 0xFF });
    Bytes cue { 0x00, 0x02 };                       // hot cue 3
    be32 (cue, 12500);                              // 12.5 s
    cue.insert (cue.end(), { 0x00, 0xCC, 0x00, 0x00, 0x00, 0x00 });
    str (cue, "Drop", true);
    entry ("CUE", cue);
    Bytes loop { 0x00, 0x01 };                      // loop slot 2
    be32 (loop, 30000); be32 (loop, 34000);
    loop.insert (loop.end(), { 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x27, 0xAA, 0xE1, 0x01 });
    str (loop, "Outro", true);
    entry ("LOOP", loop);
    entry ("BPMLOCK", { 0x01 });
    inner.push_back (0x00);
    Bytes payload { 0x01, 0x01 };
    str (payload, base64 (inner), true);
    return payload;
}

Bytes seratoBeatGrid (float pos, float bpm)
{
    Bytes p { 0x01, 0x00 };
    be32 (p, 1);
    std::uint32_t u; std::memcpy (&u, &pos, 4); be32 (p, u);
    std::memcpy (&u, &bpm, 4); be32 (p, u);
    p.push_back (0x00);
    return p;
}

void id3Frame (Bytes& b, const std::string& id, const Bytes& data) { str (b, id); be32 (b, (std::uint32_t) data.size()); b.push_back (0); b.push_back (0); b.insert (b.end(), data.begin(), data.end()); }
Bytes textFrame (const std::string& s) { Bytes d { 0x03 }; str (d, s); return d; }
Bytes geob (const std::string& desc, const Bytes& data) { Bytes d { 0x00 }; str (d, "application/octet-stream", true); d.push_back (0); str (d, desc, true); d.insert (d.end(), data.begin(), data.end()); return d; }

void write (const std::string& path, const Bytes& b) { std::ofstream f (path, std::ios::binary); f.write (reinterpret_cast<const char*> (b.data()), (std::streamsize) b.size()); }
}

MP_TEST (parse_key_spellings)
{
    CHECK (parseKey ("Am") == 21);
    CHECK (parseKey ("A minor") == 21);
    CHECK (parseKey ("8A") == 21);
    CHECK (parseKey ("1m") == 21);                  // Open Key 1m = A minor
    CHECK (parseKey ("8B") == 0);
    CHECK (parseKey ("1d") == 0);
    CHECK (parseKey ("F#m") == 18);
    CHECK (parseKey ("Gbm") == 18);
    CHECK (parseKey ("Bb") == 10);
    CHECK (parseKey ("E\xE2\x99\xAD") == 3);        // E-flat with the Unicode flat sign
    CHECK (parseKey ("12A") == 13);                 // C#m / Dbm
    CHECK (parseKey ("") == -1);
    CHECK (parseKey ("H") == -1);
}

MP_TEST (id3_tags_and_serato_prep)
{
    Bytes frames;
    id3Frame (frames, "TIT2", textFrame ("Midnight Drive"));
    id3Frame (frames, "TPE1", textFrame ("MP Audio"));
    id3Frame (frames, "TCON", textFrame ("House"));
    id3Frame (frames, "TBPM", textFrame ("124"));
    id3Frame (frames, "TKEY", textFrame ("Fm"));
    id3Frame (frames, "GEOB", geob ("Serato Markers2", seratoMarkers2()));
    id3Frame (frames, "GEOB", geob ("Serato BeatGrid", seratoBeatGrid (0.25f, 124.0f)));
    Bytes file { 'I', 'D', '3', 3, 0, 0 };
    const auto n = (std::uint32_t) frames.size();
    for (int s = 21; s >= 0; s -= 7) file.push_back ((std::uint8_t) ((n >> s) & 0x7F));
    file.insert (file.end(), frames.begin(), frames.end());
    file.insert (file.end(), 400, 0xAA); // "audio"
    const std::string path = "/tmp/deejay_tag_test.mp3";
    write (path, file);

    TagInfo t;
    REQUIRE (readTags (path, t));
    CHECK (t.title == "Midnight Drive");
    CHECK (t.artist == "MP Audio");
    CHECK (t.genre == "House");
    CHECK_NEAR (t.bpm, 124.0, 1e-9);
    CHECK (t.key == 17);
    REQUIRE (t.cues.size() == 1);
    CHECK (t.cues[0].index == 2);
    CHECK_NEAR (t.cues[0].ms, 12500.0, 1e-9);
    CHECK (t.cues[0].color == 0xCC0000u);
    CHECK (t.cues[0].name == "Drop");
    REQUIRE (t.loops.size() == 1);
    CHECK (t.loops[0].index == 1 && t.loops[0].locked && t.loops[0].name == "Outro");
    CHECK (t.hasTrackColor && t.trackColor == 0xFF99FFu);
    CHECK (t.bpmLocked);
    CHECK_NEAR (t.gridAnchorSeconds, 0.25, 1e-6);
    CHECK_NEAR (t.gridBpm, 124.0, 1e-4);

    Library lib;
    auto& tr = lib.addOrUpdate (path, t, 1000);
    Library::applySeratoPrep (tr, t, 44100.0);
    CHECK_NEAR (tr.hotCues[2].pos, 12.5 * 44100.0, 1e-6);
    CHECK_NEAR (tr.loops[1].in, 30.0 * 44100.0, 1e-6);
    CHECK_NEAR (tr.gridAnchor, 0.25 * 44100.0, 1e-3);
    CHECK (tr.gridLocked);
    std::remove (path.c_str());
}

MP_TEST (flac_vorbis_comments_and_filename_fallback)
{
    // FLAC stores the GEOB body without ID3's text-encoding byte: mime \0 \0 description \0 data
    auto flacGeob = [] { auto g = geob ("Serato Markers2", seratoMarkers2()); g.erase (g.begin()); return g; };
    Bytes vc;
    le32 (vc, 4); str (vc, "test");
    const std::vector<std::string> entries { "TITLE=Sunrise", "ARTIST=DJ Test", "BPM=128", "INITIALKEY=8A",
                                             "SERATO_MARKERS_V2=" + base64 (flacGeob()) };
    le32 (vc, (std::uint32_t) entries.size());
    for (const auto& e : entries) { le32 (vc, (std::uint32_t) e.size()); str (vc, e); }
    Bytes file { 'f', 'L', 'a', 'C' };
    file.push_back (0x80 | 4);
    file.push_back ((std::uint8_t) (vc.size() >> 16)); file.push_back ((std::uint8_t) (vc.size() >> 8)); file.push_back ((std::uint8_t) vc.size());
    file.insert (file.end(), vc.begin(), vc.end());
    const std::string path = "/tmp/deejay_tag_test.flac";
    write (path, file);
    TagInfo t;
    REQUIRE (readTags (path, t));
    CHECK (t.title == "Sunrise" && t.artist == "DJ Test");
    CHECK_NEAR (t.bpm, 128.0, 1e-9);
    CHECK (t.key == 21);
    // the FLAC base64 field wraps a GEOB body (mime / name / description / data)
    CHECK (t.cues.size() == 1 && t.loops.size() == 1);
    std::remove (path.c_str());

    write ("/tmp/Artist Name - Track Title.wav", Bytes { 'n', 'o', 'p', 'e' });
    TagInfo f;
    readTags ("/tmp/Artist Name - Track Title.wav", f);
    CHECK (f.artist == "Artist Name" && f.title == "Track Title");
    std::remove ("/tmp/Artist Name - Track Title.wav");
}

MP_TEST (library_search_sort_crates_smart_history_roundtrip)
{
    Library lib;
    auto add = [&] (const std::string& path, const std::string& title, const std::string& artist, const std::string& genre, double bpm, int key, std::int64_t added)
    {
        TagInfo t; t.title = title; t.artist = artist; t.genre = genre; t.bpm = bpm; t.key = key;
        return lib.addOrUpdate (path, t, added).id;
    };
    const auto a = add ("/m/a.mp3", "Deep Space", "Alpha", "Deep House", 122, 21, 100);
    const auto b = add ("/m/b.mp3", "Night Bus", "Bravo", "Techno", 132, 9, 200);
    const auto c = add ("/m/c.mp3", "Space Jam", "Charlie", "House", 124, 0, 86400 * 30);
    CHECK (lib.addOrUpdate ("/m/a.mp3", TagInfo{}, 999).id == a);    // re-import keeps the id

    const auto all = lib.allIds();
    CHECK (lib.search (all, "space").size() == 2);
    CHECK (lib.search (all, "space house").size() == 2);
    CHECK (lib.search (all, "space deep").size() == 1);
    CHECK (lib.search (all, "132").size() == 1);
    CHECK (lib.search (all, "8a").size() == 1);                     // Camelot in search

    auto ids = all;
    lib.sort (ids, SortColumn::Bpm, true);
    CHECK (ids.front() == a && ids.back() == b);
    lib.sort (ids, SortColumn::Key, true);                            // 8A (Am) < 8B (C) < 11B (A)
    CHECK (ids[0] == a && ids[1] == c && ids[2] == b);

    lib.addToCrate ("House/Deep", a);
    lib.addToCrate ("House", c);
    CHECK (lib.hasCrate ("House"));
    CHECK (lib.crateTracks ("House", true).size() == 2);
    CHECK (lib.crateTracks ("House", false).size() == 1);
    lib.renameCrate ("House", "Club");
    CHECK (lib.hasCrate ("Club/Deep"));

    SmartCrate sc;
    sc.name = "Fresh house";
    sc.rules.push_back ({ Field::Genre, Op::Contains, "house", 0, 0 });
    sc.rules.push_back ({ Field::Bpm, Op::Between, "", 120, 125 });
    CHECK (lib.evaluate (sc, 86400 * 31).size() == 2);
    sc.rules.push_back ({ Field::Added, Op::InLastDays, "", 7, 0 });
    CHECK (lib.evaluate (sc, 86400 * 31).size() == 1);
    lib.smartCrates().push_back (sc);

    lib.prepare().push_back (b);
    lib.beginSession (1000);
    lib.logPlay (a, 0, 1010);
    lib.logPlay (b, 1, 1300);
    CHECK (lib.find (a)->playCount == 1);
    lib.find (a)->hotCues[0] = { 4410.0, 0xCC0000u, "Intro" };
    lib.find (a)->loops[3] = { 1000.0, 2000.0, true, "Loop" };

    const std::string path = "/tmp/deejay_library_test.json";
    REQUIRE (lib.save (path));
    Library back;
    REQUIRE (back.load (path));
    CHECK (back.tracks().size() == 3);
    CHECK (back.find (a)->hotCues[0].label == "Intro" && back.find (a)->hotCues[0].color == 0xCC0000u);
    CHECK (back.find (a)->loops[3].locked);
    CHECK (back.crateTracks ("Club", true).size() == 2);
    CHECK (back.smartCrates().size() == 1 && back.evaluate (back.smartCrates()[0], 86400 * 31).size() == 1);
    CHECK (back.prepare().size() == 1 && back.history().size() == 1 && back.history()[0].entries.size() == 2);
    CHECK (back.findByPath ("/m/c.mp3") != nullptr);
    const auto d = back.addOrUpdate ("/m/d.mp3", TagInfo{}, 5).id;
    CHECK (d > c);                                                   // ids keep increasing after a reload
    back.remove (a);
    CHECK (back.crateTracks ("Club", true).size() == 1);
    std::remove (path.c_str());
}

MP_TEST (serato_crate_file)
{
    auto utf16 = [] (const std::string& s) { Bytes b; for (char ch : s) { b.push_back (0); b.push_back ((std::uint8_t) ch); } return b; };
    Bytes f;
    const auto ver = utf16 ("1.0/Serato ScratchLive Crate");
    str (f, "vrsn"); be32 (f, (std::uint32_t) ver.size()); f.insert (f.end(), ver.begin(), ver.end());
    for (const std::string p : { "Users/dj/Music/one.mp3", "Users/dj/Music/two.flac" })
    {
        const auto path = utf16 (p);
        Bytes ptrk; str (ptrk, "ptrk"); be32 (ptrk, (std::uint32_t) path.size()); ptrk.insert (ptrk.end(), path.begin(), path.end());
        str (f, "otrk"); be32 (f, (std::uint32_t) ptrk.size()); f.insert (f.end(), ptrk.begin(), ptrk.end());
    }
    write ("/tmp/deejay_test.crate", f);
    const auto paths = Library::readSeratoCrate ("/tmp/deejay_test.crate");
    REQUIRE (paths.size() == 2);
    CHECK (paths[0] == "Users/dj/Music/one.mp3" && paths[1] == "Users/dj/Music/two.flac");
    std::remove ("/tmp/deejay_test.crate");
}

// ============================================================================ sample names (Splice)

MP_TEST (sample_names_give_bpm_key_and_loop)
{
    using dj::library::parseSampleName;
    auto a = parseSampleName ("packs/KSHMR Sounds/Loops/KSHMR_Lead_Loop_128_Am.wav");
    CHECK_NEAR (a.bpm, 128.0, 1e-9);
    CHECK (a.key == 21);                               // A minor
    CHECK (a.loop);
    CHECK (a.pack == "KSHMR Sounds");
    CHECK (a.name == "KSHMR Lead Loop 128 Am");
    auto b = parseSampleName ("Cymatics - Vocal Loop 1 - 95 BPM C Min.wav");
    CHECK_NEAR (b.bpm, 95.0, 1e-9);
    CHECK (b.key == 12);                               // C minor
    auto c = parseSampleName ("PL_KRM_100_Fmaj_keys.wav");
    CHECK_NEAR (c.bpm, 100.0, 1e-9);
    CHECK (c.key == 5);                                // F major
    CHECK (c.loop);                                    // has a tempo, no one-shot words
    auto d = parseSampleName ("packs/Drums/One Shots/ESM_House_Kick_One_Shot_808.wav");
    CHECK (d.bpm == 0.0);                              // 808 is not a tempo
    CHECK (! d.loop);
    auto e = parseSampleName ("trap_hats_140bpm_F#m.wav");
    CHECK_NEAR (e.bpm, 140.0, 1e-9);
    CHECK (e.key == 18);                               // F# minor
    auto f = parseSampleName ("OS_DMY_120_G_guitar_loop.wav");
    CHECK_NEAR (f.bpm, 120.0, 1e-9);
    CHECK (f.key == 7);                                // bare "G" next to the tempo
    auto g = parseSampleName ("i_am_a_vocal_chop_A_take2.wav");
    CHECK (g.key == -1);                               // "am" is a word, a lone "A" without context is not a key
    auto h = parseSampleName ("FM_Bass_Loop_126.wav");
    CHECK (h.key == -1);                               // FM synthesis, not F major
    CHECK_NEAR (h.bpm, 126.0, 1e-9);
}

MP_TEST (loop_bpm_from_length)
{
    using dj::library::guessLoopBpm;
    CHECK_NEAR (guessLoopBpm (8.0), 120.0, 1e-9);      // 16 beats
    CHECK_NEAR (guessLoopBpm (7.5), 128.0, 1e-9);      // 16 beats
    CHECK_NEAR (guessLoopBpm (16.0 * 60.0 / 140.0), 140.0, 1e-9);   // not 70 (8 beats)
    CHECK_NEAR (guessLoopBpm (0.05), 0.0, 1e-9);
}

MP_TEST (stream_links_are_classified)
{
    using namespace dj::library;
    auto yt = classifyLink ("  https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=RD123&t=42 ");
    CHECK (yt.service == LinkService::YouTube && yt.isTrack);
    CHECK (yt.url == "https://www.youtube.com/watch?v=dQw4w9WgXcQ");     // playlist context dropped
    CHECK (classifyLink ("https://youtu.be/dQw4w9WgXcQ?si=abc").id == "dQw4w9WgXcQ");
    CHECK (classifyLink ("youtube.com/shorts/dQw4w9WgXcQ").isTrack);
    CHECK (classifyLink ("https://music.youtube.com/watch?v=dQw4w9WgXcQ").service == LinkService::YouTube);
    CHECK (! classifyLink ("https://www.youtube.com/@somechannel").isTrack);
    auto sc = classifyLink ("https://soundcloud.com/artist-name/track-name");
    CHECK (sc.service == LinkService::SoundCloud && sc.isTrack);
    CHECK (! classifyLink ("https://soundcloud.com/artist-name/sets/an-album").isTrack);
    CHECK (classifyLink ("https://on.soundcloud.com/AbCd1").isTrack);
    auto sp = classifyLink ("https://open.spotify.com/intl-de/track/4cOdK2wGLETKBW3PvgPWqT?si=x");
    CHECK (sp.service == LinkService::Spotify && sp.isTrack && sp.id == "4cOdK2wGLETKBW3PvgPWqT");
    CHECK (classifyLink ("spotify:track:4cOdK2wGLETKBW3PvgPWqT").url == "https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWqT");
    CHECK (! classifyLink ("https://open.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M").isTrack);
    CHECK (classifyLink ("https://example.com/song.mp3").service == LinkService::Unknown);
}

MP_TEST (yt_dlp_arguments_and_progress)
{
    using namespace dj::library;
    const auto a = ytDlpArguments ("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "/tmp/%(title)s.%(ext)s", true);
    CHECK (std::find (a.begin(), a.end(), "--no-playlist") != a.end());
    CHECK (std::find (a.begin(), a.end(), "--progress") != a.end());   // --print would silence progress otherwise
    CHECK (std::find (a.begin(), a.end(), "m4a") != a.end());
    CHECK (a.back() == "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    const auto b = ytDlpArguments ("u", "o", false);
    CHECK (std::find (b.begin(), b.end(), "-x") == b.end());   // no conversion without ffmpeg
    CHECK_NEAR (*parseYtDlpProgress ("[download]  42.3% of    3.45MiB at  1.23MiB/s ETA 00:02"), 42.3, 1e-9);
    CHECK_NEAR (*parseYtDlpProgress ("[download] 100% of 3.45MiB in 00:01"), 100.0, 1e-9);
    CHECK (! parseYtDlpProgress ("[youtube] dQw4w9WgXcQ: Downloading webpage").has_value());
    std::string artist, title;
    splitDownloadName ("/x/Rick_Astley - Never_Gonna_Give_You_Up [dQw4w9WgXcQ].m4a", artist, title);
    CHECK (artist == "Rick Astley");
    CHECK (title == "Never Gonna Give You Up");
}

MP_TEST (spotify_link_finds_the_track_in_the_library)
{
    using namespace dj::library;
    std::string title, author;
    CHECK (parseOEmbed (R"({"html":"<iframe></iframe>","provider_name":"Spotify","title":"Strings of Life - Original Mix","type":"rich"})", title, author));
    CHECK (title == "Strings of Life - Original Mix");
    CHECK (author.empty());
    CHECK (spotifyOEmbedUrl ("https://open.spotify.com/track/abc").find ("url=https%3A%2F%2Fopen.spotify.com%2Ftrack%2Fabc") != std::string::npos);
    Library lib;
    TagInfo t1; t1.title = "Strings Of Life"; t1.artist = "Derrick May";
    TagInfo t2; t2.title = "Strings"; t2.artist = "Someone Else";
    const auto id1 = lib.addOrUpdate ("/m/a.mp3", t1, 0).id;
    lib.addOrUpdate ("/m/b.mp3", t2, 0);
    CHECK (findTrackByTitle (lib, title, "") == id1);
    CHECK (findTrackByTitle (lib, "Strings of Life (feat. Nobody)", "Derrick May") == id1);
    CHECK (findTrackByTitle (lib, "Completely Different", "") == 0);
}
