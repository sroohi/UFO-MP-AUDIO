# UFO 0.3 validation

The connected workspace is Linux. A configured GitHub Actions workflow is not evidence of a completed Mac run: inspect the result and artifacts for the exact commit being installed. Third-party DAW compatibility and human listening require observations on the user's Mac.

## Reproduce the automated checks

Run `scripts/test-cloud.sh` after cloud bootstrap. It executes six CTest suites: `audio_engine`, `sample_bank`, `streaming_cache`, `stream_asset_integration`, `plugin_integration` and `gpu_visualization`, followed by the actual Linux VST3 wrapper host.

- DSP checks exercise silence, pitch, release/sustain, MIDI channel isolation, voice stealing, sample rates, band limiting/oversampling, smoothing, polyphonic modulation, PPQ seeks, stereo endpoints and finite/bounded shapers/effects.
- Streaming checks exercise bounded cache memory, paging/read-ahead, file errors and underruns. Asset integration imports a real 31-second stereo file, builds/reuses/recovers its disk cache, and checks the large-preset format against checksum tampering and truncation. These checks do not measure two-hour import time or long-session disk performance on a Mac.
- Plugin checks exercise sample-offset MIDI, audio, factory patches, host state, imported media/preset integrity, modulation editing, panic and native editor pixels.
- Shader checks create an OpenGL context, compile/link the shader, read back its framebuffer, verify changed waveform/cursors, exercise CPU fallback, recreate the context and resize. `gpu/GPU-waveform.png` and `gpu/GPU-diagnostics.json` preserve pixels and driver identity. The current Linux renderer is Mesa llvmpipe, a software renderer; this validates the graphics path without proving physical GPU acceleration or performance.
- The independent wrapper host scans/loads the actual VST3, negotiates stereo, renders MIDI, recalls parameters, clears panic, captures a visible editor, and exercises 44.1/48/96 kHz at 64/127/256/1024 frames. It renders an eight-second 48 kHz stereo 24-bit audition WAV. Timing results are average diagnostics, not audio-device deadline guarantees.

Raw CTest evidence is in `build-native/Testing/Temporary/LastTest.log`; host evidence, native screenshots and shader captures are under `build-native/validation`. Current standalone sample-bank (168 checks) and streaming-cache (71 checks) runs pass ASan/UBSan and TSan. Equivalent sanitizer coverage for the JUCE import/archive helpers remains pending.

Installer transaction checks use mocked native tools: `python3 Tests/MacInstallerTests.py`. They verify source/packaged installation, refusal to overwrite by default, retained backups with `--replace`, and rollback after an injected validation failure. They do not establish that a bundle loads on macOS.

## GitHub Actions Mac evidence

**Build UFO** runs on Apple Silicon (`macos-15`) and Intel (`macos-15-intel`). Each job builds universal arm64/x86_64 bundles, runs the six suites natively, checks bundle metadata/architectures/signatures/linkage, hosts the actual VST3 and checks standalone process liveness. Each architecture executes natively on its matching runner; a universal binary's second slice is not treated as tested by the first runner.

Download these artifacts from a successful run:

- `UFO-macOS-universal-development-arm64` or `UFO-macOS-universal-development-x86_64`: the nested `UFO-macOS-universal-unsigned.zip`, containing the app, VST3 and transactional `Install.command`.
- `UFO-macOS-native-validation-arm64` and `UFO-macOS-native-validation-x86_64`: build/CTest/bundle logs, native editor pixels, GPU framebuffer/driver evidence, audition WAV, audio-device probe and pending human assessment template. Logs are uploaded even when validation fails; a failed job does not produce a validated development archive.

The CI device probe may report no usable output. Offline host tests still run, but an unavailable CI output is not a listening pass. Driver identity must be inspected before describing graphics execution as hardware accelerated.

## Install and assess on the user's Mac

Extract both ZIP layers of a successful development artifact and run `./Install.command` inside the extracted `UFO` folder. To update an existing installation with backups, use `./Install.command --replace`. The installer clears download quarantine only on its own staged, signature-verified bundles. The destinations are `~/Applications/UFO.app` and `~/Library/Audio/Plug-Ins/VST3/UFO.vst3`; no Apple Developer account is needed for these ad-hoc signed local builds. If macOS blocks the downloaded installer, review it and use **Open Anyway** in Privacy & Security.

For a source build and physical output check:

```sh
scripts/install-from-source-macos.sh
scripts/listen-macos.sh
```

The listening script requires a usable stereo output and plays the host-rendered audition through that device. A completed callback verifies delivery to the audio backend. Record audibility and quality separately in `dist/validation/listening/LISTENING-NOTES.md`.

In the actual VST3 DAW, record the DAW/macOS/Mac/device versions, scan/load result, both-channel audio, MIDI pitch/sustain, patch and project save/reopen, GPU rendering, long-file playback/Color seeks, underruns, offline bounce and CPU behavior at the intended buffer size. Listen for clicks, dropouts, stereo errors and unwanted aliasing. Logic requires an AU version, which is not included.

Long imports accept mono/stereo sources up to two hours at 8–192 kHz. Files over 30 seconds use a 16 MiB page cache per source; disk storage includes the canonical stereo WAV and raw filtered levels, approximately 24 bytes per source frame. Read-ahead can miss during abrupt seeks or slow storage; realtime playback fades missing pages rather than blocking. Host-designated non-realtime rendering waits for worker pages with a five-second failure limit. Human listening and DAW tests should exercise both modes and confirm that the DAW supplies the offline flag. Preset hashing/transfer/extraction runs in the background; include a large-preset editor responsiveness check.

Public signing/notarization, Apple distribution validation and commercial release readiness are not established by the local-development workflow.
