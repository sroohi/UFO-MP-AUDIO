# Pioneer DJ DDJ-FLX10 support

DEEJ-AY drives the DDJ-FLX10 with the behaviour the manual describes for Serato DJ Pro. Message numbers
come from AlphaTheta's published "List of MIDI Messages". This file lists what DEEJ-AY does with each
control. It is not a copy of the manual. The code is `engine/dj/midi/Flx10.cpp`, and the tests in
`tests/ControllerTests.cpp` pin the behaviour.

## Deck layer (8 decks)
DEEJ-AY has eight decks; the FLX10 has four deck channels.
- **Layer 0** maps the hardware decks to decks 1-4. **Layer 1** maps them to decks 5-8.
- The layer follows the bank on screen. Switch it with SHIFT + VIEW or the DECKS 1-4 / 5-8 button.
- Everything per deck moves with the layer: transport, jogs, pads, faders, EQ, COLOR knobs, LOAD buttons,
  Part Instant Doubles and BEAT FX CH SELECT 1-4.
- LEDs and the On Jog Displays show the decks of the active layer, on the same MIDI channels.

## Conventions
- **Deck controls** are on MIDI channels 1–4, one per deck.
- **The effects section** is on channel 5.
- **Browser and global mixer** controls are on channel 7.
- **Pads** use one channel per deck: deck *n* is on channel 6 + 2*n* (8, 10, 12, 14), and the next channel
  up carries the same pads with SHIFT held.
  - Pad note = `hardware mode base + (page − 1) × 8 + (pad − 1)`.
  - Page 2 behaves like page 1, as in Serato.
- **Channel 16** is output only and drives the On Jog Displays.
- **Faders and knobs** are 14-bit: the MSB is on CC *x* and the LSB on CC *x* + 0x20.
- **Jogs** are relative around 0x40: 0x41 and up turn clockwise, 0x3F and down turn counter-clockwise.
- **The browse encoder** sends 0x01–0x1E clockwise and 0x7F–0x62 counter-clockwise.
- **Threading:**
  - MIDI input is handled on the MIDI thread. It writes engine atomics and posts discrete commands into
    a lock-free FIFO.
  - Library and UI actions are marshalled to the message thread.
  - LED and display output is diffed and sent at 30 Hz.

## Pad modes
| FLX10 button | DEEJ-AY / Serato mode | Hardware pad base |
|---|---|---|
| HOT CUE | Hot Cue (8 cues, SHIFT + pad deletes) | 0x00 |
| hold HOT CUE | Saved Loop (Serato loop slots; SHIFT + pad clears) | 0x00 |
| SHIFT + HOT CUE | Pitch Play (keyboard; SHIFT + pad picks the cue) | 0x40 |
| PAD FX | Pad FX A–H (hold to apply) | 0x10 |
| SHIFT + PAD FX | Roll | 0x50 |
| BEAT JUMP | Beat Jump (pads 1/4 jump, 2/3 halve/double the size, 5 = previous track, 6/7 = search, 8 = censor) | 0x20 |
| SHIFT + BEAT JUMP | Auto Loop (same pad again exits) | 0x60 |
| SAMPLER | Sampler (current bank; SHIFT + pad stops the slot) | 0x30 |
| SHIFT + SAMPLER | Key Shift (SHIFT + pad resets) | 0x70 |

Slicer, Slicer Loop and Stems have no button on the unit. They are picked on screen with the deck's
pad-mode menu, which the controller shares.

PAGE < / > (notes 0x24–0x33, and SHIFT variants) work per mode:
- **Pitch Play / Key Shift:** PAGE moves the range (Up / Middle / Down); SHIFT + PAGE offsets the table
  by ±1 semitone.
- **Roll:** PAGE halves or doubles the roll sizes.
- **Auto Loop:** PAGE halves or doubles the loop sizes; SHIFT + PAGE shifts the active loop by one beat.
- **Beat Jump:** PAGE halves or doubles the jump size, from 1/32 to 256 beats.
- **Sampler:** PAGE changes the bank (A–D).

## Deck section (channels 1–4)
| Control | Note / CC | Action |
|---|---|---|
| PLAY/PAUSE | note 0x0B | play / pause (SHIFT 0x47: stutter from the cue) |
| CUE | note 0x0C | CDJ cue (hold to preview, release returns); SHIFT 0x48: jump to start. The on-screen double-click restart is deliberately not on the hardware CUE, so cue tapping keeps working |
| Jog touch | note 0x36 | scratch while touched with VINYL on |
| Jog top / side | CC 0x22 / 0x23 / 0x21 | scratch (Vinyl, touched) or pitch bend |
| SHIFT + jog | CC 0x1F | fast search |
| SHIFT + jog side | CC 0x26 | slide the beatgrid |
| SYNC | note 0x58 | sync on (SHIFT 0x5C: sync off) |
| TEMPO slider | CC 0x00 (+0x20) | tempo, soft takeover after a range change |
| TEMPO RESET | note 0x41 | tempo to 0 % (SHIFT 0x60: range ±8 → 16 → 50 %) |
| KEY SYNC / RESET | note 0x65 / 0x64 | match key to the other deck / reset key shift |
| MIX POINT SELECT < / > | note 0x59 / 0x5A | key down / up one semitone (Serato use) |
| MIX POINT LINK | note 0x4A | key lock |
| ACTIVE PART | notes 0x0D drums, 0x0E vocal, 0x0F instrumental | mute / unmute stems |
| LOOP IN / OUT | note 0x10 / 0x11 | manual loop; SHIFT + IN/OUT then the jog moves the point |
| 4 BEAT / EXIT | note 0x14 | 4-beat loop or exit; SHIFT 0x50 reloop |
| Loop slot < / > | note 0x51 / 0x53 | select a saved loop slot; 0x3D locks, 0x3E deletes |
| CENSOR | note 0x15 | censor while held (slip reverse) |
| REVERSE | note 0x38 | reverse toggle |
| QUANTIZE | note 0x35 | quantize (SHIFT 0x68: tap tempo) |
| SLIP | note 0x40 | slip mode |
| VINYL | note 0x17 | vinyl (scratch) mode |
| BEAT JUMP < / > | note 0x5E / 0x5F | ±4 beats (SHIFT 0x61 / 0x62: ±16) |
| SEARCH < / > | note 0x70 / 0x71 | search while held |
| CH CUE | note 0x54 | headphone cue (SHIFT 0x39: EQ ↔ Part ISO) |
| Crossfader assign | note 0x16 A, 0x1D THRU, 0x18 B | |
| Fader start | notes 0x66 / 0x5D / 0x52 | play / sync+play / back to cue, when fader start is on |
| TRIM, HI, MID, LOW, fader | CC 0x04, 0x07, 0x0B, 0x0F, 0x13 | channel strip |

## Browser and mixer (channel 7)
| Control | Note / CC | Action |
|---|---|---|
| Rotary | CC 0x40 (SHIFT 0x64) | browse (SHIFT: waveform zoom) |
| LOAD 1–4 | note 0x46–0x49 | load the selected track; press twice within 0.45 s = Instant Doubles |
| BACK | note 0x65 (SHIFT 0x66) | toggle crates / tracks (SHIFT: add to Prepare) |
| VIEW | note 0x7A (SHIFT 0x68) | 2 / 4 / 8 decks (SHIFT: decks 1-4 / 5-8 and the deck layer) |
| MASTER CUE | note 0x63 | |
| SAMPLER CUE | note 0x69 | |
| SOUND COLOR FX | notes 0x00–0x05 | SPACE, DUB ECHO, CRUSH, PITCH, NOISE, FILTER; pressing the lit one turns it off |
| COLOR knobs | CC 0x17–0x1A | channels 1–4 |
| FX PART SELECT | notes 0x1B vocal, 0x1C drums, 0x1D instrumental | which stems the Beat FX processes |
| SHIFT + ACTIVE PART twice | notes 0x24–0x2F | Part Instant Doubles to deck 1–4 |
| Crossfader | CC 0x1F | |
| MASTER / BOOTH | CC 0x08 / 0x09 | only when the unit does not apply them in hardware (default: hardware) |
| HEADPHONES MIX / LEVEL | CC 0x0C / 0x0D | see the "Headphone mix done by the unit" setting |
| SAMPLER VOLUME | CC 0x03 | |

## BEAT FX (channel 5)
| Control | Note / CC | Action |
|---|---|---|
| BEAT FX SELECT | notes 0x20–0x2D | LOW CUT ECHO, ECHO, MULTI TAP DELAY, SPIRAL, REVERB, TRANS, ENIGMA JET, FLANGER, PHASER, STRETCH, SLIP ROLL, ROLL, MOBIUS (SAW), MOBIUS (TRIANGLE) |
| BEAT FX CH SELECT | notes 0x10–0x16 | CH1–CH4 of the deck layer (CH5–CH8 on layer 1), MASTER, (0x15 unused), SAMPLER |
| BEAT < / > | notes 0x4A / 0x4B | step through the effect's beat range (1/16–16, 1/16–64, ±1/16–±64, or reverb size) |
| SHIFT + BEAT < | note 0x66 | BPM AUTO |
| SHIFT + BEAT > | note 0x6B | tap BPM (3+ taps) |
| BEAT FX ON/OFF | note 0x47 | |
| LEVEL/DEPTH | CC 0x02 (+0x20) | |

The FLX10's TRANS is DEEJ-AY's Gate effect.

## Output
- **LEDs** go on the same note as the button, with velocity 0x7F for on and 0x00 for off. That covers
  PLAY, CUE, SYNC, TEMPO RESET, KEY SYNC, key lock, SLIP, QUANTIZE, CENSOR, the loop buttons, ACTIVE PART,
  CH CUE, the pad-mode buttons, pads, FX PART SELECT, Color FX, BEAT FX ON and SAMPLER CUE.
- **On Jog Display** (channel 16), with *d* = deck 0–3:
  - BPM × 10, 14-bit: CC 0x14+*d* / 0x34+*d*.
  - Playing speed: CC 0x18+*d* / 0x38+*d*.
  - Elapsed minutes / seconds: CC 0x42+2*d* / 0x43+2*d*.
  - Marker angle (one turn = 1.8 s): CC 0x10+*d* / 0x30+*d*.
  - Notes 0x20+*d* (key lock), 0x18+*d* (sync leader) and 0x1C+*d* (sync).
- **Key shift display:** CC 0x4A on the deck channel.
- **Channel level meter:** CC 0x02 on the deck channel.

## Needs checking on hardware
These were written from the documentation without a unit on the desk:
- **Pad colours.** DEEJ-AY sends on/off velocities only. The FLX10's RGB pad palette (velocity → colour)
  is not mapped yet, so hot-cue colours do not reach the pads.
- **Key code table** for the jog display's key readout.
- **Playing-speed encoding.** DEEJ-AY sends −100 … +100 % over 0 … 9999.
- **Jog-ring status byte.**
- **Jog resolution.** The default is 720 ticks per turn and can be changed in Settings > Controller. If
  scratching runs fast or slow, this is the knob.
- **Hold HOT CUE.** In Serato this opens Saved Flip. DEEJ-AY has no Flip yet, so it opens Saved Loop.
