# DEEJ-AY design notes

DEEJ-AY targets the DDJ-FLX10's Serato DJ Pro feature set and workflow, then goes further. It has:
- eight decks and an 8-channel mixer (four on screen per bank);
- two Serato FX units plus the FLX10 BEAT FX unit;
- the Performer, master-bus performance FX;
- a 32-slot BPM-matching sampler fed from the library or Splice;
- stems and twelve pad modes.

These notes cover how each piece works and why.

## Layout
```
engine/dj/            real-time core (C++20, no JUCE, builds and tests on Linux)
  Engine              owns decks, mixer, FX units, sampler, recorder; command FIFO in, event FIFO out
  Deck                transport, cues, loops, rolls, slicer, slip, censor, scratch, sync, key, stems, Pad FX
  TimeStretch         WSOLA key lock / key shift / Pitch Play
  Mixer, ColorFx      channel strips, EQ / isolator / Part ISO, Sound Color FX, crossfader, cue, limiter
  Effects, FxUnit     the effect library and the Serato-style FX units
  Sampler, Recorder   4 banks × 8 slots (key-locked sync, beat-quantized launch); master recording tap
  Performer           master-bus performance FX (stutter ... reverb)
  dsp/                biquads (RBJ, LR4), SVF, Freeverb, compressor
  analysis/           FFT, BPM + beatgrid, key, loudness, colour waveform, DSP stem separation
  library/            JSON database, crates, smart crates, history, tag reader with Serato prep import,
                      sample-pack names (Splice), link handling (YouTube / SoundCloud / Spotify)
  midi/               DDJ-FLX10 mapping (input, LEDs, jog displays)
app/                  JUCE 9 application (audio/MIDI hosting, UI, settings, file decoding)
tests/                engine, analysis, library and controller tests (no framework, `deejay_tests`)
```

## Threads
- **Audio thread.** `Engine::process` only. No locks, no allocation and no I/O.
  - Commands arrive through SPSC FIFOs: one for the UI and one for the MIDI thread, so each FIFO has a
    single producer.
  - Continuous controls (faders, knobs, jog ticks) are atomics read once per block.
  - Results go back through atomics (`DeckView`, `MixerView`, FX unit views) and an event FIFO for
    things the library must persist: cue and loop edits, grid edits, play counts.
- **Memory release.** Track audio and effects are reference-counted and released off the audio thread.
  A replaced `TrackAudio` is handed back through an `AudioReleased` event. A replaced effect goes into a
  retired-effects FIFO that the message thread drains.
- **MIDI thread.** Runs `Flx10::handle`: it writes the atomics and posts commands. Library actions are
  marshalled to the message thread with an alive token, so a late callback after shutdown is a no-op.
- **Workers.** A three-thread pool handles decoding, analysis and stem separation. Results are applied on
  the message thread, never directly to a deck.
- **Clang RealtimeSanitizer** can be enabled with `-DDEEJAY_RTSAN=ON` to catch allocations or locks on the
  audio thread.

## Deck
- **Cues.** Cues follow CDJ / Serato rules.
  - CUE on a paused deck sets the cue; held, it previews and returns on release.
  - CUE while playing returns to the cue and pauses.
  - Eight hot cues; quantize makes the jump keep the current beat phase instead of landing early or late.
  - A hot cue on a paused deck plays from the cue (Serato), plays while held (CDJ gate) or only jumps,
    depending on a setting.
- **Loops.**
  - Manual IN/OUT, auto loops (1/8–16 beats on the pads, with PAGE moving the range), halve/double,
    shift, and reloop.
  - Eight Serato loop slots with lock.
  - Loops are clamped to the track end, so a 4-beat loop placed near the end never runs into silence.
  - Rolls run a slip-style ghost position and return to it on release.
- **Slicer.** Divides a domain (8 beats by default, 2–64 allowed) into eight slices, with quantize from
  1/8 to 1 slice. In Slicer the domain follows the playhead; Slicer Loop loops the domain.
- **Slip.** A ghost playhead keeps running during scratches, loops, rolls, censor and reverse. Releasing
  returns to where the track would have been. Censor (slip reverse) cancels itself after 8 beats if that
  setting is on.
- **Scratch.** The platter's angle comes from the jog ticks (720 per turn by default). An alpha-beta filter
  (α = 0.25, β = 0.036) smooths it into a playback rate. One turn equals 1.8 s of audio, a 33⅓ rpm record.
  Vinyl off turns the platter top into pitch bend.
- **Brake and start.** Platter stop and start times are settings.
- **Tempo and key.**
  - Tempo range ±8 / 16 / 50 %. After a range change the slider uses soft takeover, so the tempo does not
    jump.
  - Key lock, key shift (±12) and Pitch Play all run through WSOLA. Each grain is read from the track at the
    deck's own timeline position, so jumps cost one crossfade and there is no stretcher FIFO to flush.
- **Sync.**
  - Serato Smart Sync style. When SYNC is pressed and there is no usable leader, another playing deck with a
    grid becomes leader (one already synced is preferred). Otherwise the deck leads itself. A leader that
    stops hands over to a playing synced deck.
  - Tempo matches with octave folding (×½ / ×2), so 70 and 140 BPM tracks lock.
  - Phase locks on sync-on and on play, then drift is corrected with a rate nudge capped at 2 %.
- **Stems.**
  - A deck with stems renders three buses: the stems selected for FX, the rest, and the stem-echo send.
  - Muting a stem (ACTIVE PART) is a gain on the stored stems. The melody stem is the mix minus the other
    three, so all stems at unity give back the original mix exactly.
- **Pad FX.** Eight hold-to-apply presets per deck (A–H): echo 1/2, echo 3/4, roll 1/4, roll 1/8, gate,
  high-pass, braker and reverb. Echo tails ring out after release.

## Mixer
- **Signal path per channel:**
  1. Trim and auto gain.
  2. NOISE Color FX, then FX units placed pre-EQ.
  3. EQ, then FILTER / CRUSH / PITCH Color FX, then FX units placed pre-fader.
  4. Cue tap and meter.
  5. Fader, then DUB ECHO / SPACE Color FX (post-fader, so tails survive a closed fader), then FX units
     placed post-fader (the default).
  6. Crossfader.

  On a deck with stems, Color FX and the FX units process only the stems chosen with FX PART SELECT. The
  other stems still get trim, EQ and the fader, but skip the effects.
- **EQ.** Either DJM-style (−26 / +6 dB shelving and peaking) or an LR4 isolator with full kill.
- **Part ISO** turns HI / MID / LOW into Vocal / Bass + Melody / Drums gains on a deck with stems.
- **FX insert placement** per unit: pre-EQ, pre-fader or post-fader (the default).
- **Faders.** Channel-fader and crossfader curves are adjustable, and the crossfader has a cut-lag setting.
- **Cue and master.**
  - Headphone cue / master mix, split cue, and an option to leave the phones mix to the hardware (the FLX10
    does it on the unit).
  - Master runs through a look-ahead limiter at −0.2 dBFS before the recorder tap.

## FX
- **FX units.** Two Serato-style units, each switchable between MULTI (three effects, one knob each) and
  SINGLE (one effect with three parameters). A third unit is the FLX10 BEAT FX.
- **Targets.** Any unit can target channels 1–4, master or the sampler, with its own effect instance per
  target, so assigning a unit to two decks never shares delay lines.
- **Timing.** BPM comes from the target deck, the leader, or tap. BEAT < / > walks the effect's own range:
  1/16–16, 1/16–64, signed ±1/16–±64, or size for reverb.

## Analysis
Runs off the audio thread after decoding. Results are cached: the library holds BPM, key and gain, and
waveforms go in `Analysis/<id>.djwf`.
- **BPM.**
  1. Log-compressed spectral flux.
  2. Autocorrelation scored by a comb over the analysis range (default 68–135 BPM, as in Serato).
  3. A phase histogram smoothed with a [1 4 6 4 1] kernel.
  4. A snap to the nearest integer BPM when the fit is within 95 % of the best.
- **Beatgrid.**
  - The phase comes from the low band (kick), with an N/4 correction for the onset detector's latency.
    Using the full band locked grids to off-beat hi-hats.
  - The downbeat is the beat with the most low-band energy, measured linearly; log compression hid the
    difference.
- **Key.** A chromagram scored against Temperley-style profiles and harmonic-aware templates (harmonics
  weighted 0.6^(h−1)). This fixed major keys being reported as their dominant. Keys are shown as Camelot
  by default.
- **Loudness.** ITU-R BS.1770 integrated LUFS, used for auto gain against −14 LUFS by default.
- **Waveform.** Three bands (low / mid / high), coloured like Serato's RGB waveform.
- **Stems (built-in DSP backend).**
  - Drums come from HPSS by median filtering.
  - Bass is the low split of the harmonic part.
  - Vocals come from centre extraction of the harmonic midrange.
  - Melody is the remainder.

  It is fast and has no dependencies, but it is preview quality. The `StemSeparator` interface is there
  so a neural backend (HTDemucs in Core ML) can replace it.

## Library
- **Storage.** A JSON database (`library.json`) written to a temporary file and then renamed, so a crash
  never leaves a half-written library.
- **Organisation.**
  - Crates nest with `/` in the name.
  - Smart crates are rule-based: title, artist, BPM, key, year, date added, play count and more, with
    contains / is / greater / less / between / in-last-days operators, matching all or any rule.
  - There is a Prepare list and a history per session.
- **Tags.** ID3v2.2–2.4, FLAC and MP4 tags are read.
  - Serato prep (Markers2 cues and loops with colours, BeatGrid) is imported, and a Serato grid locks the
    track's grid.
  - Serato `.crate` files can be imported.
  - Re-importing a track only fills fields that are present, so it never wipes existing data.

## Eight decks and banks
- The engine always runs eight decks and eight channel strips. A deck that is not playing costs almost
  nothing.
- The UI shows 2, 4 or 8 decks. With 8, the deck panels and mixer strips show one bank of four (1-4 or 5-8)
  in FLX10 order, while all eight waveform lanes stay visible.
- The FLX10's deck layer follows the bank. The keyboard plays the left and right deck of the bank.

## Performer (master-bus performance FX)
- **Position:** after the master FX unit, before master level, limiter, outputs and recorder. Everything the
  decks and the sampler play goes through it, and the recording captures it.
- **Clock:** the clock deck is the sync leader while it plays, otherwise the loudest playing deck with a grid.
  Its tempo and beat drive every rhythmic value. Without one, an internal clock keeps time.
- **Time effects** share a 12-second input ring; one plays at a time (tape stop > stutter > note repeat).
  - Stutter repeats the grid slice that contains the press, starting from where the music is, so the switch
    is seamless. Build-up halves the slice every 1-8 beats.
  - Note repeat re-captures the start of every beat, with a gate.
  - Tape stop reads the ring at a falling rate (linear to hard brake); a stopped tape is silent.
- **Pitch:** a two-tap delay-line shifter with Hann crossfades, 45 ms window.
- **Filter:** SVF low-pass / high-pass around an open centre.
- **Talkbox:** three formant band-passes morphing A-E-I-O-U, fed by a soft-clipped signal for harmonics,
  with an optional beat-locked vowel sweep.
- **Bitcrusher:** bit depth and sample-and-hold.
- **Delay:** ping-pong with a damped feedback path.
- **Reverb:** Freeverb as a send.
- Every switch crossfades over 10 ms. Delay and reverb tails ring out after switching off.

## Sampler sync
- SYNC with key lock (the default) time-stretches with the same WSOLA as the decks, so a loop matches the
  master tempo at its own pitch. Without key lock it varispeeds.
- A synced slot launches on the next master beat. The stretcher is primed one hop ahead, so the first
  transient plays at full level instead of fading in.
- A repeating synced loop wraps at the nearest whole number of beats when the file is within 0.15 beat of
  one, so it cannot drift against the master.
- A loop's BPM comes from its name (Splice and most packs) or, failing that, from its length: the whole-beat
  count that gives an integer tempo in the 85-175 BPM range.

## Links
- **YouTube / SoundCloud:** a child process runs the user's yt-dlp, reading its progress lines (`--newline
  --progress`) and the final path (`--print after_move:filepath`). It asks for M4A through ffmpeg when
  available, else an M4A or MP3 stream.
- **Spotify:** the oEmbed title, then a normalised title / artist match against the library.
- The downloads, the analysis queue and the Splice scan each run on their own worker pool, so none of them
  delays loading a deck.

## Known gaps
See `ROADMAP.md`.
