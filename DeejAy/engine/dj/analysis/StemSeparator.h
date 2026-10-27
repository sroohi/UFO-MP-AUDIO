// Stem separation (Vocals / Melody / Bass / Drums). Backends implement StemSeparator; the deck only
// needs vocals, bass and drums because melody is stored as the remainder (exact reconstruction).
//
// DspStemSeparator is the built-in, dependency-free backend: harmonic/percussive separation by median
// filtering (D. Fitzgerald, "Harmonic/Percussive Separation using Median Filtering", DAFx 2010) for
// drums, a low-frequency split of the harmonic part for bass, and stereo centre extraction
// (C. Avendano & J.-M. Jot, "Frequency-domain upmix", 2002/2004) of the harmonic midrange for vocals.
// It is a preview-quality separator; a neural backend (e.g. HTDemucs converted to Core ML) is the
// planned upgrade and plugs in behind the same interface.
#pragma once
#include "Analysis.h"
#include <memory>
#include <string>

namespace dj::analysis
{

struct StemSeparation
{
    std::vector<float> vocals, bass, drums; // interleaved stereo, same length as the mix
};

class StemSeparator
{
public:
    virtual ~StemSeparator() = default;
    virtual std::string name() const = 0;
    virtual bool separate (const TrackAudio& audio, StemSeparation& out, const Progress& progress) = 0;
};

class DspStemSeparator final : public StemSeparator
{
public:
    std::string name() const override { return "DSP (HPSS + centre extraction)"; }
    bool separate (const TrackAudio& audio, StemSeparation& out, const Progress& progress) override;
};

std::unique_ptr<StemSeparator> makeDefaultStemSeparator();

} // namespace dj::analysis
