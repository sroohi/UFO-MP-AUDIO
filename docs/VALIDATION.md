# UFO 0.3 validation

The connected workspace is Linux, with native Mac validation in GitHub Actions. Inspect the result and artifacts for the exact commit being installed. The Logic version requires the AUv2 bundle `UFO.component`; actual Logic Pro X sessions and human listening require observations on the user's Mac.

## Reproduce the automated checks

Run `scripts/test-cloud.sh` after cloud bootstrap. It executes six CTest suites: `audio_engine`, `sample_bank`, `streaming_cache`, `stream_asset_integration`, `plugin_integration` and `gpu_visualization`, followed by the actual Linux VST3 wrapper host.

- DSP checks exercise silence, pitch, release/sustain, MIDI channel isolation, voice stealing, sample rates, band limiting/oversampling, smoothing, polyphonic modulation, PPQ seeks, stereo endpoints and finite/bounded shapers/effects.
- Streaming checks exercise bounded cache memory, paging/read-ahead, file errors and underruns. Asset integration imports a real 31-second stereo file, builds/reuses/recovers its disk cache, and checks the large-preset format against checksum tampering and truncation. These checks do not measure two-hour import time or long-session disk performance on a Mac.
- Plugin checks exercise sample-offset MIDI, audio, factory patches, host state, imported media/preset integrity, modulation editing, panic and native editor pixels.
- Shader checks create an OpenGL context, compile/link the shader, read back its framebuffer, verify changed waveform/cursors, exercise CPU fallback, recreate the context and resize. `gpu/GPU-waveform.png` and `gpu/GPU-diagnostics.json` preserve pixels and driver identity. Observed drivers are Mesa llvmpipe on Linux and Apple Software Renderer/OpenGL 4.1 on native macOS CI. These software renderers validate the graphics path without proving physical GPU acceleration or performance.
- The independent wrapper host scans/loads the actual VST3, negotiates stereo, renders MIDI, recalls parameters, clears panic, captures a visible editor, and exercises 44.1/48/96 kHz at 64/127/256/1024 frames. It renders an eight-second 48 kHz stereo 24-bit audition WAV. Timing results are average diagnostics, not audio-device deadline guarantees.

Raw CTest evidence is in `build-native/Testing/Temporary/LastTest.log`; host evidence, native screenshots and shader captures are under `build-native/validation`. Standalone sample-bank checks pass ASan/UBSan; streaming-cache checks also pass TSan. A targeted ASan/UBSan run passes 277 import/archive helper checks with the inline helpers and test code instrumented. Its linked JUCE module objects are uninstrumented, so this is partial sanitizer coverage.

Installer transaction checks use mocked native tools: `python3 Tests/MacInstallerTests.py`. They verify source/packaged installation, refusal to overwrite by default, retained backups with `--replace`, and rollback after an injected validation failure. They do not establish that a bundle loads on macOS.

## GitHub Actions Mac evidence

**Build UFO** targets Apple Silicon (`macos-15`) and Intel (`macos-15-intel`). Each job verifies its native architecture, builds universal app/AU/VST3 bundles, runs the six suites natively, checks bundle metadata/architectures/signatures/linkage, and checks standalone process liveness. Each architecture executes natively on its matching runner; a universal binary's second slice is not treated as tested by the first runner.

The Logic validation step temporarily registers the staged AUv2 instrument, runs `/usr/bin/auval -v aumu Ufo1 MpAu`, and uses an independent host for real AU MIDI/audio/state/editor and sample-rate/block-size checks. It retains AU registration/discovery, validator and host evidence under `dist/validation/au`, and restores the previous component afterward. If ordinary filesystem discovery fails, it logs a refresh of only the current user's AudioComponentRegistrar metadata service and retries. Actual VST3 wrapper hosting remains a separate check. A configured step is not an `auval` pass: read the completed validator log. CI does not run the Logic Pro X application or assess a human listening session.

The Intel runner demonstrated a stale registrar that survived SIGTERM. Refresh now captures its current-user UID, PID, start time and Apple framework executable, requests termination, and permits SIGKILL only if the same verified process survives the grace period. It confirms exit before retrying discovery. Identity changes and real signal errors fail safely; it never restarts the audio-device service or deletes plug-in caches.

After packaging, each disposable Mac runner extracts the actual ZIP, executes its `Install.command`, verifies all three installed bundles and download-quarantine removal, then exercises `--replace` and retained backups. It requires the installer's bounded Apple validation to pass twice and removes only the test copies it owns. Read `dist/validation/installation/result.txt` and the copied validator logs for this separate installation result. Test executables contain only the runner's native architecture; the production shared code and all three bundles remain universal, with architecture reports retained in the validation artifact.

For installation-only fixes, **Validate UFO macOS package** can reuse the universal bundles compiled at `7eeaf5f` in source run `39827001446`. It verifies the source run and artifacts, rejects any C++ or CMake change, and compares every production bundle file, permission and symlink before and after packaging. It compiles the native wrapper host and repeats AU/VST3 hosting, Apple validation, standalone launch and real installation/update on both architectures. Its six CTest and shader results are explicitly inherited from the matching native source-build evidence under `upstream-*`; they are not presented as newly executed. `package-provenance.json` records the separate source build and current installation-helper commits. A C++ change requires the complete build workflow.

Download these artifacts from a successful run:

- `UFO-macOS-universal-development-arm64` or `UFO-macOS-universal-development-x86_64`: the nested `UFO-macOS-universal-unsigned.zip`, containing the app, AU component, VST3 and transactional `Install.command`.
- `UFO-macOS-native-validation-arm64` and `UFO-macOS-native-validation-x86_64`: build/CTest/bundle logs, native editor pixels, GPU framebuffer/driver evidence, audition WAV, audio-device probe and pending human assessment template. Logs are uploaded even when validation fails; a failed job does not produce a validated development archive.

The CI device probe may report no usable output. Offline host tests still run, but an unavailable CI output is not a listening pass. Driver identity must be inspected before describing graphics execution as hardware accelerated.

## Install and assess on the user's Mac

Quit Logic Pro X and standalone UFO. Extract both ZIP layers of a successful development artifact and run `./Install.command` inside the extracted `UFO` folder. To update an existing installation with backups, use `./Install.command --replace`. The installer clears download quarantine only on its own staged, signature-verified bundles. It installs `~/Applications/UFO.app`, `~/Library/Audio/Plug-Ins/Components/UFO.component` and `~/Library/Audio/Plug-Ins/VST3/UFO.vst3`, and requires Apple AU validation before committing all three. A stale registry triggers a logged refresh of only the current user's AudioComponentRegistrar; validation failure rolls the bundles back together. No Apple Developer account is needed for these ad-hoc signed local builds. If macOS blocks the downloaded installer, review it and use **Open Anyway** in Privacy & Security.

Open Logic Pro X, create a Software Instrument track, and select **AU Instruments → MP Audio → UFO → Stereo**. If needed, use **Preferences → Plug-in Manager → UFO → Reset & Rescan Selection** (**Settings** on newer Logic). The component identifies as AUv2 instrument type `aumu`, subtype `Ufo1`, manufacturer `MpAu`.

For a source build and physical output check:

```sh
scripts/install-from-source-macos.sh
scripts/listen-macos.sh --au
```

The listening script requires a usable stereo output. With `--au`, it loads the installed AU and writes `UFO-au-audition.wav` under `dist/validation/listening/au` before playback. A completed callback verifies delivery to the audio backend. Record audibility and quality separately in that directory's `LISTENING-NOTES.md`. The default mode auditions the staged VST3.

In Logic Pro X, record the Logic/macOS/Mac/device versions, AU scan/load result, both-channel audio, MIDI pitch/sustain, patch and project save/reopen, GPU rendering, long-file playback/Color seeks, underruns, offline bounce and CPU behavior at the intended buffer size. Listen for clicks, dropouts, stereo errors and unwanted aliasing. Repeat relevant checks separately in any VST3 DAW used for production.

Long imports accept mono/stereo sources up to two hours at 8–192 kHz. Files over 30 seconds use a 16 MiB page cache per source; disk storage includes the canonical stereo WAV and raw filtered levels, approximately 24 bytes per source frame. Read-ahead can miss during abrupt seeks or slow storage; realtime playback fades missing pages rather than blocking. Host-designated non-realtime rendering waits for worker pages with a five-second failure limit. Human listening and DAW tests should exercise both modes and confirm that the DAW supplies the offline flag. Preset hashing/transfer/extraction runs in the background; include a large-preset editor responsiveness check.

Public signing/notarization, Apple distribution validation and commercial release readiness are not established by the local-development workflow.
