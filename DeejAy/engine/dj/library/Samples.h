// Sample-pack file names (Splice and most pack vendors): BPM, key and loop / one-shot read from names like
// "KSHMR_Lead_Loop_128_Am.wav", "Cymatics - Vocal Loop 1 - 95 BPM C Min.wav" or "PL_KRM_100_Fmaj_keys.wav".
//
// DEEJ-AY's Splice support works with the Splice desktop app: you sign in there, the samples you download
// land in its sounds folder (~/Splice/sounds by default), and DEEJ-AY browses that folder. Splice offers no
// public sign-in API for third-party apps, so DEEJ-AY never sees Splice credentials.
#pragma once
#include <string>
#include <vector>

namespace dj::library
{

struct SampleInfo
{
    std::string name;        // display name: the file name without extension, separators as spaces
    std::string pack;        // pack folder ("Splice/sounds/packs/<pack>/..."), empty when unknown
    double bpm = 0.0;        // 0 = not in the name
    int key = -1;            // dj key index (0-11 major, 12-23 minor), -1 = not in the name
    bool loop = false;       // a loop (true) or a one-shot (false)
};

// `relativePath`: the path below the sounds folder, '/'-separated (the folders help: "Loops", "One Shots").
SampleInfo parseSampleName (const std::string& relativePath);

// A loop's tempo from its length when the name has none: the whole-beat count (2 .. 64) that gives a
// tempo in the usual DJ range, preferring 85-175 BPM and integer tempos. 0 when nothing fits.
double guessLoopBpm (double lengthSeconds);

// Default Splice sounds folders to look in (macOS first).
std::vector<std::string> defaultSpliceFolders (const std::string& home);

} // namespace dj::library
