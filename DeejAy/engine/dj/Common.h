// DEEJ-AY by MP Audio — shared constants, real-time annotations and small math helpers.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// With clang >= 20 (Xcode 16.3+) and -fsanitize=realtime, any allocation, lock or syscall reached
// from a [[clang::nonblocking]] function is reported at runtime.
#if defined(__has_cpp_attribute)
 #if __has_cpp_attribute(clang::nonblocking)
  #define DJ_NONBLOCKING [[clang::nonblocking]]
 #else
  #define DJ_NONBLOCKING
 #endif
#else
 #define DJ_NONBLOCKING
#endif

namespace dj
{

constexpr int kNumDecks = 8;          // the DDJ-FLX10 drives four at a time (deck layer 1-4 / 5-8)
constexpr int kNumHotCues = 8;
constexpr int kNumSavedLoops = 8;
constexpr int kNumStems = 4;          // Vocals, Melody, Bass, Drums (Serato order)
constexpr int kNumFxUnits = 2;
constexpr int kFxSlotsPerUnit = 3;
constexpr int kSamplerBanks = 4;
constexpr int kSamplerSlotsPerBank = 8;
constexpr int kSamplerSlots = kSamplerBanks * kSamplerSlotsPerBank;
constexpr int kNumPadFx = 8;
constexpr double kPi = 3.14159265358979323846;

enum class Stem : int { Vocals = 0, Melody = 1, Bass = 2, Drums = 3 };

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }

// Wraps x into [-0.5, 0.5).
inline double wrapHalf (double x) noexcept { return x - std::floor (x + 0.5); }

// Positive modulo for doubles.
inline double fmodPos (double x, double m) noexcept
{
    const double r = std::fmod (x, m);
    return r < 0.0 ? r + m : r;
}

// One-pole parameter smoother: y += k (target - y), k from a time constant.
struct Smoothed
{
    float value = 0.0f, target = 0.0f, coeff = 1.0f;
    void prepare (double sampleRate, float ms) noexcept
    {
        coeff = ms <= 0.0f ? 1.0f : 1.0f - std::exp (-1.0f / (float) (sampleRate * ms * 0.001));
    }
    void snap (float v) noexcept { value = target = v; }
    float next() noexcept { value += coeff * (target - value); return value; }
};

// Catmull-Rom / 4-point 3rd-order Hermite interpolation of y[-1], y[0], y[1], y[2] at t in [0, 1).
inline float hermite (float ym1, float y0, float y1, float y2, float t) noexcept
{
    const float c0 = y0;
    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

// Musical key index 0..23: 0..11 = C..B major, 12..23 = C..B minor.
inline const char* keyName (int key) noexcept
{
    static const char* names[24] { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
                                   "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "Bbm", "Bm" };
    return key >= 0 && key < 24 ? names[key] : "";
}

// Camelot wheel code for a key index ("8A" = A minor, "8B" = C major).
inline int camelotNumber (int key) noexcept
{
    if (key < 0 || key >= 24) return 0;
    const int pc = key % 12;
    const bool minor = key >= 12;
    // major: C=8B, G=9B ... (each fifth +1); minor: Am=8A
    const int ref = minor ? 9 : 0; // A for minor, C for major
    const int fifths = ((pc - ref) * 7 % 12 + 12) % 12;
    return (fifths + 7) % 12 + 1;
}
inline bool camelotIsMinor (int key) noexcept { return key >= 12; }

// Key after transposing by `semitones`.
inline int transposeKey (int key, int semitones) noexcept
{
    if (key < 0) return key;
    const int pc = ((key % 12) + semitones % 12 + 12) % 12;
    return (key >= 12 ? 12 : 0) + pc;
}

} // namespace dj
