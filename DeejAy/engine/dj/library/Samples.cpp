#include "Samples.h"
#include "Tags.h"
#include <algorithm>
#include <cctype>
#include <cmath>
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

    std::vector<std::string> tokenize (const std::string& s)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s)
        {
            if (c == '_' || c == '-' || c == ' ' || c == '.' || c == '(' || c == ')' || c == '[' || c == ']' || c == ',' || c == '+')
            {
                if (! cur.empty()) out.push_back (cur);
                cur.clear();
            }
            else cur += c;
        }
        if (! cur.empty()) out.push_back (cur);
        return out;
    }

    bool allDigits (const std::string& s)
    {
        return ! s.empty() && std::all_of (s.begin(), s.end(), [] (char c) { return std::isdigit ((unsigned char) c) != 0; });
    }

    // "128", "128bpm", "bpm128" -> 128; anything else -> 0
    double tokenBpm (const std::string& token, bool& explicitBpm)
    {
        const auto t = lower (token);
        std::string digits = t;
        explicitBpm = false;
        if (t.size() > 3 && t.compare (t.size() - 3, 3, "bpm") == 0) { digits = t.substr (0, t.size() - 3); explicitBpm = true; }
        else if (t.size() > 3 && t.compare (0, 3, "bpm") == 0) { digits = t.substr (3); explicitBpm = true; }
        if (! allDigits (digits) || digits.size() > 3) return 0.0;
        const double v = std::atof (digits.c_str());
        return v >= 50.0 && v <= 220.0 ? v : 0.0;
    }

    // Musical key in one or two tokens: "Am", "A#m", "Ebmin", "Fmaj", "C" + "Min", "F#" + "major".
    // Returns the number of tokens used (0 = none). A bare note ("F") only counts when `bareOk`.
    int tokenKey (const std::vector<std::string>& tk, std::size_t i, bool bareOk, int& key)
    {
        const std::string& t = tk[i];
        if (t.empty() || t.size() > 8) return 0;
        const char letter = (char) std::toupper ((unsigned char) t[0]);
        if (letter < 'A' || letter > 'G') return 0;
        std::size_t p = 1;
        std::string accidental;
        if (p < t.size() && (t[p] == '#' || t[p] == 'b')) { accidental = t.substr (p, 1); ++p; }
        else if (lower (t.substr (p, 5)) == "sharp") { accidental = "#"; p += 5; }
        else if (lower (t.substr (p, 4)) == "flat") { accidental = "b"; p += 4; }
        const std::string rest = t.substr (p);
        const std::string lr = lower (rest);
        const bool lowerLetter = std::islower ((unsigned char) t[0]) != 0;
        bool minor = false, quality = false;
        if ((rest == "m" && ! lowerLetter) || lr == "min" || lr == "minor") { minor = true; quality = true; }   // "am" is a word
        else if (lr == "maj" || lr == "major") { quality = true; }                                                // "FM" is a synth
        else if (! rest.empty()) return 0;
        int used = 1;
        if (! quality && i + 1 < tk.size())
        {
            const auto next = lower (tk[i + 1]);
            if (next == "min" || next == "minor" || next == "m") { minor = true; quality = true; used = 2; }
            else if (next == "maj" || next == "major") { quality = true; used = 2; }
        }
        // a bare note is only a key with context (next to the BPM, after "key"), and never in lower case
        if (! quality && (! bareOk || lowerLetter)) return 0;
        key = parseKey (std::string (1, letter) + accidental + (minor ? "m" : ""));
        return key >= 0 ? used : 0;
    }

    bool isAny (const std::string& token, std::initializer_list<const char*> words)
    {
        const auto t = lower (token);
        for (const char* w : words) if (t == w) return true;
        return false;
    }
}

SampleInfo parseSampleName (const std::string& relativePath)
{
    SampleInfo info;
    std::string path = relativePath;
    std::replace (path.begin(), path.end(), '\\', '/');
    const auto slash = path.find_last_of ('/');
    const std::string file = slash == std::string::npos ? path : path.substr (slash + 1);
    const std::string folders = slash == std::string::npos ? std::string() : path.substr (0, slash);
    const auto dot = file.find_last_of ('.');
    const std::string stem = dot == std::string::npos ? file : file.substr (0, dot);

    // display name
    info.name = stem;
    std::replace (info.name.begin(), info.name.end(), '_', ' ');

    // pack: the folder after "packs/" (Splice layout), else the first folder
    {
        const auto lf = lower (folders);
        const auto at = lf.find ("packs/");
        std::string rest = at != std::string::npos ? folders.substr (at + 6) : folders;
        info.pack = rest.substr (0, rest.find ('/'));
    }

    const auto tk = tokenize (stem);
    // BPM: an explicit "bpm" spelling wins, then a number next to the word "bpm", then a lone 2-3 digit number
    int bpmAt = -1;
    for (std::size_t i = 0; i < tk.size() && info.bpm <= 0.0; ++i)
    {
        bool ex = false;
        const double v = tokenBpm (tk[i], ex);
        if (v > 0.0 && (ex || (i + 1 < tk.size() && lower (tk[i + 1]) == "bpm") || (i > 0 && lower (tk[i - 1]) == "bpm")))
        { info.bpm = v; bpmAt = (int) i; }
    }
    for (std::size_t i = 0; i < tk.size() && info.bpm <= 0.0; ++i)
    {
        bool ex = false;
        const double v = tokenBpm (tk[i], ex);
        if (v > 0.0 && tk[i].size() >= 2) { info.bpm = v; bpmAt = (int) i; }
    }

    // key: qualified spellings anywhere; a bare note only right next to the BPM or after "key"
    for (std::size_t i = 0; i < tk.size() && info.key < 0; ++i)
    {
        const int d = bpmAt >= 0 ? (int) i - bpmAt : 100;
        const bool nearBpm = d == 1 || d == -1 || (d == 2 && lower (tk[(std::size_t) bpmAt + 1]) == "bpm");
        const bool afterKeyWord = i > 0 && lower (tk[i - 1]) == "key";
        int k = -1;
        if (tokenKey (tk, i, nearBpm || afterKeyWord, k) > 0) info.key = k;
    }

    // loop or one-shot: words in the name, then the folders, then "has a tempo"
    bool loopWord = false, shotWord = false;
    for (std::size_t i = 0; i < tk.size(); ++i)
    {
        if (isAny (tk[i], { "loop", "loops", "lp" })) loopWord = true;
        if (isAny (tk[i], { "oneshot", "oneshots", "os", "hit", "shot" })) shotWord = true;
        if (isAny (tk[i], { "one" }) && i + 1 < tk.size() && isAny (tk[i + 1], { "shot", "shots" })) shotWord = true;
    }
    const auto lf = lower (folders);
    const bool loopFolder = lf.find ("loop") != std::string::npos;
    const bool shotFolder = lf.find ("one shot") != std::string::npos || lf.find ("one_shot") != std::string::npos
                            || lf.find ("oneshot") != std::string::npos || lf.find ("one-shot") != std::string::npos;
    if (loopWord) info.loop = true;
    else if (shotWord) info.loop = false;
    else if (loopFolder) info.loop = true;
    else if (shotFolder) info.loop = false;
    else info.loop = info.bpm > 0.0;
    return info;
}

double guessLoopBpm (double lengthSeconds)
{
    if (! (lengthSeconds > 0.2)) return 0.0;
    static constexpr int beatCounts[] { 2, 4, 8, 16, 32, 64 };
    double best = 0.0, bestScore = 1.0e9;
    for (int b : beatCounts)
    {
        const double bpm = b * 60.0 / lengthSeconds;
        if (bpm < 70.0 || bpm > 180.0) continue;
        double score = std::abs (bpm - std::round (bpm)) * 10.0;   // pack loops are cut at integer tempos
        if (bpm < 85.0 || bpm > 175.0) score += 5.0;
        score += std::abs (bpm - 124.0) * 0.001;                     // tie-break towards dance tempos
        if (score < bestScore) { bestScore = score; best = bpm; }
    }
    return best;
}

std::vector<std::string> defaultSpliceFolders (const std::string& home)
{
    return { home + "/Splice/sounds", home + "/Documents/Splice/sounds", home + "/Music/Splice/sounds", home + "/Splice" };
}

} // namespace dj::library
