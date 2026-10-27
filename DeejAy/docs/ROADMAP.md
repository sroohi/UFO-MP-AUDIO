# DEEJ-AY roadmap

Ordered by how much a working DJ will notice the gap.

## Before 1.0
1. **Hardware pass on a DDJ-FLX10.** These were written from the documentation and need checking on a
   unit:
   - pad RGB palette;
   - jog-display key codes and playing-speed encoding;
   - jog-ring status;
   - jog resolution.

   Details are in `CONTROLLER-FLX10.md`.
2. **Neural stems.** The built-in DSP separator is preview quality: drums are good, vocals bleed. The plan
   is HTDemucs (MIT weights) converted to Core ML behind the existing `StemSeparator` interface, with the
   DSP backend kept as the fallback, and stems cached on disk next to the waveform cache.
3. **Saved Flip** (Serato Flip): record and replay cue-jump sequences. Hold HOT CUE currently opens Saved
   Loop instead.
4. **Pad colours from hot-cue colours.** This depends on the palette from item 1.
5. **Dynamic beatgrids** (multiple grid markers) for live-drummer tracks. Today's grids are constant tempo.
6. **Real-machine QA on macOS.** CI builds and runs the tests on macOS. The UI has been exercised on Linux
   only.
   - Check Core Audio device switching mid-set.
   - Check sleep/wake with the controller plugged in.
   - Check Retina rendering of the waveforms.

## After 1.0
- MIDI learn and mappings for controllers other than the FLX10.
- Ableton Link (needs the proprietary licence for a closed-source build).
- A higher-quality key-lock option (Signalsmith Stretch, MIT) next to WSOLA.
- Serato database V2 import (the full library, not only crates and in-file prep).
- rekordbox XML import and export.
- Track preparation view with the waveform zoomed for grid editing, and beatgrid tap.
- Streaming services (needs partner agreements; not a code problem).
- Video, DVS and timecode vinyl.

## Deliberately out of scope
- Copying Serato's or AlphaTheta's branding, artwork or trade dress. DEEJ-AY matches the workflow and
  features, under its own name and look.
