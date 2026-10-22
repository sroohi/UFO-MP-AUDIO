# UFO — MP Audio

UFO is a native C++17/JUCE dual-layer instrument for standalone macOS, Logic Pro X through Audio Units, and VST3 hosts. Version 0.3 adds OpenGL waveform rendering and background streaming for long imported files. It remains a development build; native wrapper tests, actual DAW sessions and human listening have separate validation results.

## Instrument

- Two independently tuned layers and fourteen dual-layer voices; velocity, channel-specific sustain and ±2-semitone pitch bend.
- Four generated banks (Solar, Hollow, Prism, Chaos), each with 32 interpolated frames and band-limited harmonic tables. The complete render path runs at 2× rate through a filtered downsampler; the plugin reports 32 samples of latency.
- Color position with attack/decay movement; SVF low/band/high-pass, vactrol-style low-pass gate, or three-band resonator per layer.
- Four modulation routes: shaped LFO, deterministic sample-and-hold, Euclidean pulses, independent per-note ADSR envelopes, and editable 8/16-step patterns. Drag a colored Mod handle onto Color, Frequency, Semitones, Layer Mix or Output to assign it. Open **EDIT** for steps, rotation and ENV ADSR.
- Beat sync follows the host's absolute quarter-note position, including seeks, loops and stop. Rate is cycles per quarter note; without host transport it free-runs at 120 BPM. Generator output and routing depth appear as colored arcs.
- Continuous controls ramp over approximately 10 ms. ADSR, equal-power layer mix, saturation, chorus, flanger, delay, and stereo all-pass/four-line feedback diffusion reverb complete the path.
- Four factory patches, host automation/state recall, a user-preset browser, onscreen MIDI keyboard and panic.

All audio and graphics are original; no Softube code, samples or assets are included.

## Samples and presets

Use **IMPORT AUDIO** on either layer for mono/stereo WAV, AIFF or FLAC at 8–192 kHz, from 64 frames to two hours. Sources up to 30 seconds stay resident; longer files use a bounded 16 MiB page cache per source, plus small metadata. Decoding, filtering, waveform summaries and cache preparation run on a loader thread; a background worker reads playback pages. Realtime rendering performs no disk I/O or sample-buffer allocation. Imported material is pitched relative to C4 and overrides the generated bank until **CLEAR**.

Long imports create a canonical stereo WAV and raw, filtered resolution levels on disk. Allow approximately 24 bytes per source frame for this cache, in addition to the original file and any saved presets. Import preparation can take time and requires sufficient free disk space. Rapid Color jumps or a slow disk can outrun read-ahead; realtime playback fades missing pages instead of blocking the audio thread. Check the layer's underrun status during your own sessions.

When the host marks rendering as non-realtime, missing pages wait for the background reader, with a five-second failure limit. This keeps a faster-than-realtime bounce from treating normal disk latency as a realtime underrun. Hosts must supply that offline flag for this behavior.

Color selects a crossfaded 4096-frame scanning window. One-shot mode plays from the selected region to the real sample endpoint. Filtered source levels and continuous crossfades reduce transposition aliasing, with some high-frequency softening.

Each layer's OpenGL 3.2 shader draws its actual source waveform summary, Color position, playback cursor and modulation. Click **GPU** to switch to the CPU renderer; CPU fallback also handles unavailable graphics contexts. `UFO_DISABLE_GPU=1` starts with CPU rendering. This is a source/playback visualization, not a live output oscilloscope. Observed CI drivers are Mesa software OpenGL on Linux and Apple Software Renderer/OpenGL 4.1 on macOS. Shader/framebuffer tests validate the graphics path; physical GPU acceleration and performance remain unverified.

**SAVE** writes a self-contained `.ufo` archive containing parameters and imported audio with checksums. Long-source presets use a 64-bit archive format; older ZIP-based presets remain readable. Preset hashing, media transfer and extraction run in the background to keep the editor responsive. **LOAD** and the preset menu restore these archives, even if the original import moves. Host state references the owned cache on the same machine. Factory presets clear imported layers. Invalid archives/media are rejected, and asynchronous decoding failures appear in the layer status.

On macOS the default library is `~/Library/MP Audio/UFO`, with `Presets` and `Samples` subdirectories. `UFO_USER_DATA_DIR` can override the library root with an absolute path. Tests isolate their library under the build directory.

## Install the GitHub Actions Mac build

After a successful **Build UFO** workflow run, download `UFO-macOS-universal-development-arm64` or `UFO-macOS-universal-development-x86_64` from that run's **Artifacts**. Both contain universal binaries; the suffix identifies the architecture used for native validation. Download the matching `UFO-macOS-native-validation-*` artifact for logs, screenshots, GPU diagnostics, `UFO-au-audition.wav` and `UFO-vst3-audition.wav`.

With GitHub CLI, set `RUN_ID` to that successful run's numeric ID and download:

```sh
gh run download "$RUN_ID" --repo sroohi/UFO-MP-AUDIO \
  --name UFO-macOS-universal-development-arm64 --dir ufo-download
gh run download "$RUN_ID" --repo sroohi/UFO-MP-AUDIO \
  --name UFO-macOS-native-validation-arm64 --dir ufo-validation
```

For a browser download, extract the outer Actions ZIP; then extract `UFO-macOS-universal-unsigned.zip`. The `UFO` folder contains `UFO.app`, `UFO.component` (the Audio Unit for Logic), `UFO.vst3` and the installer. Run:

```sh
./Install.command
# To update an existing installation while retaining backups:
./Install.command --replace
```

Quit Logic Pro X and the standalone UFO app before installing or updating. The installer checks native architecture and signatures, stages all three bundles, clears download quarantine on those verified copies, and rolls back its own partial installation on failure. It installs to:

- `~/Applications/UFO.app`
- `~/Library/Audio/Plug-Ins/Components/UFO.component`
- `~/Library/Audio/Plug-Ins/VST3/UFO.vst3`

These are ad-hoc signed development builds; local installation needs no Apple Developer account. If macOS blocks the downloaded installer, review `Install.command` and use its **Open Anyway** option in Privacy & Security.

In Logic Pro X, create a **Software Instrument** track and choose **AU Instruments → MP Audio → UFO → Stereo** in its instrument slot. Logic discovers the AUv2 instrument `aumu Ufo1 MpAu`. If it needs rescanning, open **Logic Pro X → Preferences → Plug-in Manager** (**Settings** in newer Logic), select UFO, then **Reset & Rescan Selection**. Reopen Logic if the new entry has not appeared yet.

The standalone app offers audio/MIDI devices in its Options menu. Other hosts can rescan and load the VST3 on a MIDI instrument track. Listen to the validation WAV and record your own device/DAW observations using `LISTENING-NOTES.md` from the validation artifact. Automated audio generation does not establish what a person heard.

## Build and audition on your Mac

Requires macOS, Xcode Command Line Tools, CMake 3.22+, and Git. JUCE 8.0.6 is fetched at a fixed commit; review its AGPL/commercial licensing before closed-source distribution.

```sh
scripts/install-from-source-macos.sh
# Existing installation:
# scripts/install-from-source-macos.sh --replace
scripts/listen-macos.sh --au
```

The build targets macOS 11+ and universal arm64/x86_64 app/AU/VST3 binaries. It runs engine, sample, streaming, archive, plugin and shader tests, checks bundle architectures/metadata/signatures/library paths, runs Apple's Audio Unit validator, and loads the actual AU and VST3 in a native test host. The host captures their editors, exercises 44.1/48/96 kHz and four block sizes, profiles 14 voices, renders stereo auditions and probes the default audio device. The standalone smoke check verifies process liveness. Packaging succeeds only after required automated checks pass. Logs and captures are in `dist/validation`, with AU evidence under `dist/validation/au`.

`scripts/listen-macos.sh --au` loads the installed Audio Unit and plays its host-rendered audition through the Mac's selected default stereo output. Callback completion is checked automatically; add what you heard and the Logic checks to `dist/validation/listening/au/LISTENING-NOTES.md`. Without `--au`, the script auditions the staged VST3 and writes under `dist/validation/listening`. To repeat Apple's validator after installation:

```sh
/usr/bin/auval -v aumu Ufo1 MpAu
```

A successful `auval` result establishes Apple validator compatibility for that run. Actual Logic scan/load, project recall, offline bounce and listening still need a Logic session.

To build without installing, use `scripts/build-macos.sh`; then run `scripts/install-local-macos.sh` when ready. The optional `scripts/sign-and-notarize.sh` supports a separate public distribution workflow.

## Cloud development and verification

This environment is Debian Linux. Use the existing isolated checkout at `/workspace/UFO-MP-AUDIO`; no worktree is needed.

```sh
scripts/bootstrap-cloud.sh
source /workspace/.ufo-system/activate.sh
cmake --build build-native --parallel 3
scripts/test-cloud.sh
build-native/ufo_engine_tests build-native/UFO-demo.wav
```

Bootstrap preserves repository signatures/TLS and installs verified development packages under `/workspace/.ufo-system`, with pinned CMake/Ninja under `/workspace/.ufo-tools`. The test script manages its own virtual display and runs six CTest suites plus a host test loading the actual Linux VST3 wrapper. It retains UI captures, shader readbacks, driver identity and host results under `build-native/validation`.

Independent DSP tests require no JUCE or GUI:

```sh
cmake -S . -B build-engine -DUFO_BUILD_PLUGIN=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-engine --parallel 2
ctest --test-dir build-engine --output-on-failure
```

The regressions cover pitch, silence/release/sustain/channels/stealing; alias reduction; mip continuity and automation; polyphonic modulation and PPQ alignment; stereo endpoints; bounded streaming memory and page handling; long-import cache/archive integrity; stable reverb; preset/media corruption; modulation drops and step editing. Shader tests verify real context creation, framebuffer changes, fallback, resize and context recreation. The VST3 host checks scanning/loading, stereo negotiation, MIDI sample offsets, parameter-state recall, rendered editor pixels, panic and sustained audio.

## Remaining release work

Consult the actual Actions run and [validation record](docs/VALIDATION.md) for native build evidence. Linux checks cannot establish Mac/DAW compatibility, physical-device listening or performance on real Macs. Host timing is a diagnostic average, not a realtime deadline guarantee. Human listening, third-party DAW sessions and Apple public-distribution validation remain separate work.

A larger curated factory sample library, AAX and public release packaging are not implemented. Discrete source/shaper changes and hard voice stealing are not crossfaded. Smoothing and oversampling do not make every automated transition click-free or the complete nonlinear instrument alias-free. This is not a finished commercial clone or a signed public release.
