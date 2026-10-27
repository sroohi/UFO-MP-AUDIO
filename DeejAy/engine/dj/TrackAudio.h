// Decoded track audio shared (read-only) between the loader, the UI and the audio thread.
//
// The mix is stored as interleaved stereo float. Stems, when present, are stored as three int16
// stereo streams (vocals, bass, drums); melody is derived as mix - (vocals + bass + drums), so
// "all stems at unity" is bit-identical to the original mix and memory is ~2.5x the mix, not 5x.
#pragma once
#include "Common.h"
#include <cstdint>
#include <vector>

namespace dj
{

struct TrackAudio
{
    double sampleRate = 44100.0;
    std::int64_t frames = 0;
    std::vector<float> mix;                 // frames * 2
    bool hasStems = false;
    std::vector<std::int16_t> stems[3];     // vocals, bass, drums; frames * 2 each
    float stemScale = 1.0f / 32767.0f;      // int16 -> float

    static constexpr int kStoredVocals = 0, kStoredBass = 1, kStoredDrums = 2;

    void setMix (std::vector<float> interleaved, double sr)
    {
        mix = std::move (interleaved);
        frames = (std::int64_t) (mix.size() / 2);
        sampleRate = sr;
    }

    // Stores separated stems. `vocals`, `bass` and `drums` are interleaved stereo float, frames * 2.
    void setStems (const std::vector<float>& vocals, const std::vector<float>& bass, const std::vector<float>& drums)
    {
        float peak = 1.0f;
        for (const auto* v : { &vocals, &bass, &drums })
            for (float x : *v) peak = std::max (peak, std::abs (x));
        stemScale = peak / 32767.0f;
        const float inv = 1.0f / stemScale;
        const std::vector<float>* src[3] { &vocals, &bass, &drums };
        for (int s = 0; s < 3; ++s)
        {
            stems[s].resize ((std::size_t) frames * 2);
            for (std::size_t i = 0; i < stems[s].size(); ++i)
                stems[s][i] = (std::int16_t) std::lround (std::clamp (i < src[s]->size() ? (*src[s])[i] * inv : 0.0f, -32767.0f, 32767.0f));
        }
        hasStems = true;
    }

    // Mix frame at an integer index; outside the track is silence.
    void mixFrame (std::int64_t i, float& l, float& r) const noexcept
    {
        if (i < 0 || i >= frames) { l = r = 0.0f; return; }
        l = mix[(std::size_t) i * 2];
        r = mix[(std::size_t) i * 2 + 1];
    }

    // Frame with stem gains applied: out = g_m * mix + sum_s (g_s - g_m) * stem_s for s in {V, B, D}.
    // `coeff` = { gVocals - gMelody, gBass - gMelody, gDrums - gMelody }, gMix = gMelody.
    void stemFrame (std::int64_t i, float gMix, const float* coeff, float& l, float& r) const noexcept
    {
        if (i < 0 || i >= frames) { l = r = 0.0f; return; }
        const auto k = (std::size_t) i * 2;
        l = mix[k] * gMix;
        r = mix[k + 1] * gMix;
        if (! hasStems) return;
        for (int s = 0; s < 3; ++s)
        {
            if (std::abs (coeff[s]) < 1.0e-9f) continue;
            const float c = coeff[s] * stemScale;
            l += c * (float) stems[s][k];
            r += c * (float) stems[s][k + 1];
        }
    }

    double lengthSeconds() const noexcept { return sampleRate > 0 ? (double) frames / sampleRate : 0.0; }
};

// Constant-tempo beatgrid in track frames (the grid most DJ tracks need; dynamic grids are a roadmap item).
struct Beatgrid
{
    double bpm = 0.0;            // track BPM at 0% pitch
    double anchor = 0.0;         // frame of a downbeat (beat 1 of a bar)
    int beatsPerBar = 4;

    bool valid() const noexcept { return bpm > 1.0; }
    double framesPerBeat (double sr) const noexcept { return valid() ? 60.0 * sr / bpm : 0.0; }
    // Fractional beat index at `pos` (0 at the anchor).
    double beatAt (double pos, double sr) const noexcept { return valid() ? (pos - anchor) / framesPerBeat (sr) : 0.0; }
    double posOfBeat (double beat, double sr) const noexcept { return anchor + beat * framesPerBeat (sr); }
    // Start of the grid cell of `beats` length that contains pos.
    double floorTo (double pos, double beats, double sr) const noexcept
    {
        if (! valid() || beats <= 0) return pos;
        const double cell = beats * framesPerBeat (sr);
        return anchor + std::floor ((pos - anchor) / cell + 1.0e-9) * cell;
    }
    // Nearest grid line at `beats` resolution.
    double roundTo (double pos, double beats, double sr) const noexcept
    {
        if (! valid() || beats <= 0) return pos;
        const double cell = beats * framesPerBeat (sr);
        return anchor + std::round ((pos - anchor) / cell) * cell;
    }
};

} // namespace dj
