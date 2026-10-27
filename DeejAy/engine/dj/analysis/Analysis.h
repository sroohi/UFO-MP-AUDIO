// Offline track analysis: BPM + beatgrid, musical key, loudness (auto gain), colour waveforms.
// Runs on a background thread after decoding; never on the audio thread.
#pragma once
#include "../TrackAudio.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace dj::analysis
{

// Serato-style analysis BPM ranges ("58-115" .. "98-195"); results are folded into [min, max).
struct AnalysisSettings
{
    double minBpm = 68.0;
    double maxBpm = 135.0;
    double targetLufs = -14.0;        // auto-gain reference
    bool snapToInteger = true;        // prefer integer / half BPMs when they fit as well
    int detailBinsPerSecond = 150;    // scrolling waveform resolution
    int overviewBins = 1600;          // whole-track overview strip
};

// Serato-style colour waveform: per bin, the peak and the energy of low / mid / high bands (0..255).
struct Waveform
{
    double binsPerSecond = 0.0;
    std::vector<std::uint8_t> peak, low, mid, high;
    std::size_t size() const noexcept { return peak.size(); }
};

struct BpmResult
{
    double bpm = 0.0;
    double anchor = 0.0;     // frame of the first downbeat at or after 0
    double confidence = 0.0; // 0..1
};

struct TrackAnalysis
{
    BpmResult beat;
    int key = -1;              // 0..11 major C..B, 12..23 minor
    double keyConfidence = 0.0;
    double loudnessLufs = -70.0;
    double autoGainDb = 0.0;
    double peak = 0.0;
    Waveform overview, detail;
};

using Progress = std::function<bool (double)>; // return false to cancel

BpmResult detectBpm (const std::vector<float>& mono, double sampleRate, const AnalysisSettings& s);
int detectKey (const std::vector<float>& mono, double sampleRate, double* confidence = nullptr);
double integratedLoudness (const float* interleavedStereo, std::int64_t frames, double sampleRate);
Waveform buildWaveform (const float* interleavedStereo, std::int64_t frames, double sampleRate, double binsPerSecond);

std::vector<float> toMono (const TrackAudio& audio);
TrackAnalysis analyseTrack (const TrackAudio& audio, const AnalysisSettings& s, const Progress& progress = {});

} // namespace dj::analysis
