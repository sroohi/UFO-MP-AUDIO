# DEEJ-AY by MP Audio

Native macOS DJ software built around the Pioneer DJ DDJ-FLX10 and the Serato DJ Pro workflow: the same
decks, pads, loops, FX and sampler, with its own engine, analysis and look.

![DEEJ-AY](docs/screenshots/main.png)

## What it does
- **Four decks.** Each deck has:
  - CDJ cue, 8 hot cues and 8 saved loop slots;
  - auto and manual loops, rolls, slicer, slip, censor, reverse, beat jump, vinyl scratch and pitch bend;
  - tempo ranges ±8 / 16 / 50 %;
  - key lock, key shift, key sync and Pitch Play;
  - Smart Sync with leader selection and half/double-tempo matching.
- **Twelve pad modes.** Hot Cue, Saved Loop, Pitch Play, Pad FX, Roll, Beat Jump, Auto Loop, Sampler, Key
  Shift, Slicer, Slicer Loop and Stems.
- **Stems.** Vocals, melody, bass and drums are separated in the background after loading. ACTIVE PART
  mutes stems, Part ISO turns the EQ into stem faders, and Part Instant Doubles copies a stem to another
  deck.
- **Mixer.** Four channels with:
  - trim, DJM-style EQ or isolator, and filter;
  - six Sound Color FX (SPACE, DUB ECHO, CRUSH, PITCH, NOISE, FILTER);
  - adjustable fader and crossfader curves, headphone cue with split cue, booth output, and recording.
- **FX.**
  - Two Serato-style FX units (MULTI / SINGLE) with tempo-synced echo, reverb, filters, flanger, phaser,
    roll, gate, braker, pitch and more.
  - The 14 FLX10 BEAT FX, assignable to any channel, the master or the sampler.
- **Sampler.** 4 banks × 8 slots, with trigger / hold / on-off modes, sync and repeat.
- **Analysis.** BPM and beatgrid, musical key (Camelot), loudness for auto gain, and a three-band colour
  waveform. Serato's own cues, loops and grids are imported from tags.
- **Library.**
  - Crates and nested crates, smart crates, Prepare, and history.
  - Import of Serato crates.
  - Search and sortable columns, and drag and drop.
- **DDJ-FLX10 support.** Full mapping with LEDs and the On Jog Displays (`docs/CONTROLLER-FLX10.md`). The
  computer keyboard works too (Settings > Keyboard).

## Building
Requirements: macOS 13+, Xcode 15+ (or the Command Line Tools), CMake 3.22+ and Ninja. JUCE 9 is fetched
at configure time.

    cmake -S DeejAy -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target DeejAy -j
    open build/DeejAy_artefacts/Release/DEEJ-AY.app

The engine and its tests build anywhere, without JUCE:

    cmake -S DeejAy -B build-engine -DDEEJAY_BUILD_APP=OFF
    cmake --build build-engine -j && ./build-engine/deejay_tests

Signing, notarization and the DMG are covered in `docs/RELEASE.md`. CI does all of it on every push.

## Layout
| Path | What |
|---|---|
| `engine/dj/` | Real-time engine (no JUCE): decks, mixer, FX, sampler, analysis, library, FLX10 mapping |
| `app/` | JUCE application: audio and MIDI hosting, UI, settings |
| `tests/` | Engine, analysis, library and controller tests |
| `packaging/` | Entitlements, signing / notarization and DMG scripts |
| `docs/` | Design notes, controller mapping, roadmap, release process |

## Status
Version 0.1.
- The engine is covered by tests that run on Linux and macOS in CI.
- The app has been exercised end to end on Linux: library, analysis, playback, loops, sync, stems, FX and
  settings. CI builds and signs it for macOS.
- It has not yet been tested on a DDJ-FLX10 or on a Mac with real audio hardware.

`docs/ROADMAP.md` lists what is missing, most importantly neural stems and the hardware pass.

DEEJ-AY is not affiliated with Serato or AlphaTheta; see `THIRD-PARTY-NOTICES.md`.
