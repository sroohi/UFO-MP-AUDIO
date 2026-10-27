# Third-party notices — DEEJ-AY

Only MIT / BSD / public-domain / commercially licensed code may be vendored. GPL projects (Mixxx and others)
are design references at most, and none of their code is in this tree.

| Component | Licence | Use |
|---|---|---|
| JUCE 9 (juce.com) | JUCE licence (Starter tier until revenue thresholds; Indie/Pro above) or AGPLv3. MP Audio builds under the commercial tiers. | Audio device I/O, MIDI I/O, audio file decoding, UI. Fetched at build time, not vendored. |
| JUCE MP3 decoder (`JUCE_USE_MP3AUDIOFORMAT`) | Part of JUCE; the MP3 patents have expired worldwide | MP3 decoding on platforms without a system decoder. On macOS, Core Audio decodes MP3 / AAC / ALAC. |
| Freeverb (Jezar at Dreampoint) | Public domain | `engine/dj/dsp/Reverb.h`, an in-house implementation of the published algorithm. |
| Cytomic SVF (Andrew Simper, "Solving the continuous SVF equations", 2013) | Published paper, implementation in-house | `engine/dj/dsp/Svf.h` |
| RBJ "Audio EQ Cookbook" (Robert Bristow-Johnson) | Public text, implementation in-house | `engine/dj/dsp/Biquad.h` |
| Giannoulis, Massberg & Reiss, "Digital Dynamic Range Compressor Design" (JAES 2012) | Published paper, implementation in-house | `engine/dj/dsp/Compressor.h` (master limiter) |
| WSOLA (Verhelst & Roelands, ICASSP 1993) | Published paper, implementation in-house | `engine/dj/TimeStretch.h` (key lock, key shift, Pitch Play) |
| HPSS by median filtering (D. Fitzgerald, DAFx 2010); frequency-domain centre extraction (Avendano & Jot) | Published papers, implementation in-house | `engine/dj/analysis/StemSeparator.cpp` |
| ITU-R BS.1770 loudness | Public standard | `engine/dj/analysis/Analysis.cpp` (auto gain) |
| Serato tag layouts (Markers2, BeatGrid, crate files) | Format facts as documented publicly by the MIT-licensed `serato-tags` project; no code copied | `engine/dj/library/Tags.cpp`, `Library.cpp` (read-only import) |
| DDJ-FLX10 MIDI message numbers | Published by AlphaTheta in the "List of MIDI Messages"; facts, not code | `engine/dj/midi/Flx10.cpp` |

## Trademarks
Serato and Serato DJ Pro are trademarks of Serato Limited. Pioneer DJ, AlphaTheta and DDJ-FLX10 are
trademarks of AlphaTheta Corporation. DEEJ-AY is an independent product by MP Audio. It is not affiliated
with, endorsed by or sponsored by either company. The controller mapping exists so that owners of the
hardware can use it with DEEJ-AY.

Planned additions (not yet in the tree): a Core ML stem model (HTDemucs weights are MIT), Ableton Link
(GPLv2+ or a proprietary licence from link-devs@ableton.com), Signalsmith Stretch (MIT) as a higher-quality
key-lock option.
