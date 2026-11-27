# The language (name TBD: Csus / Cdim / …)

## Runtime music types (`include/music.h`, `src/music.c`)

| Type | Size | Notes |
|---|---|---|
| `note` | 1 byte | `uint8_t` MIDI pitch, C4 = 60. `n++` = semitone up, `n += 7` = fifth up. No loudness: that belongs to the event that plays it. |
| `scale` | 4 bytes | `root`, `mode`, `kind`, `edo`. Notes are computed: `scale_degree(&s, d)` walks forever across octaves; `mode_next()` wraps (Locrian → Ionian). |
| `chord` | 10 bytes | `notes[8]` + `count` + `edo`. Root and name are **computed** from the notes, so `c.notes[1]--` turns C into Cm. Any note set gets a name (`Cmaj7sus4(b5)`, `C7(b9)`, `C/E`). |

`chord_from_scale(&s, degree, 3|4)` stacks thirds, so a progression is a list of degrees: I–vi–IV–V = `{0, 5, 3, 4}`.

## Tunings (EDO)

`edo` = equal steps per octave; `0` means 12, so zero-initialised structs are standard tuning. In a tuning the note byte counts its steps (`note++` = one step). Middle C is `edo_c4(edo)`: 60 in 12-EDO (MIDI-compatible), 124 in 31-EDO, about 256/edo octaves of range.

Scales are spelled on the chain of fifths, so one definition works everywhere: C major is 0 2 4 5 7 9 11 in 12-EDO and 0 5 10 13 18 23 28 in 31-EDO. `SCALE_EDO_ALL` gives every step. Names are spelled too (31-EDO: C, Dbb, C#, Db, …; 24-EDO uses ups: `C^4`).

```c
scale s = { .root = edo_c4(31), .kind = SCALE_MAJOR, .edo = 31 };
chord c = chord_from_scale(&s, 0, 4);   /* Cmaj7 with 31-EDO's near-pure third */
double hz = note_freq_edo(c.notes[1], 31);
```

Still to flesh out: chord names in other tunings use the nearest 12-tone reading (31-EDO's harmonic seventh C-E-G-A# reads as `C7`), and C4 is anchored at the same 261.63 Hz in every tuning.

Tests: `ctest` from the build dir, or `./build/lang/test_music`.

## Program structure

A song is a C-style program. `main()` takes no arguments (nothing to pass to an mp3); it plays sections in order. Sections, instruments and effects are ordinary functions, and they compose:

```c
sound chorus(void) {
    return mix(drums(true),
               pump(reverb(synth(&supersaw, chords()), 0.6, 2.2, 0.5, 0.25), BEATS(1), 0.65, 0.28));
}

void main() {
    play(intro());
    play(verse());
    play(chorus());
}
```

Two kinds of value flow through it:
- **`part`**: notes over time (melody, chords, drum pattern). Plain data.
- **`sound`**: rendered stereo audio. Synths turn parts into sounds; samples load as sounds; every effect takes a sound and returns one.

A sound has a musical `length` and may ring longer (`frames`, for reverb/release tails), so `play()` steps by the length and tails overlap the next section.

Until the interpreter exists, `demo/demo_song.c` is a full song written in exactly this shape in C (`./build/lang/demo_song` writes `demo_song.wav` + `.mp3`).

## Sound engine (`include/audio.h`)

| Piece | What it is |
|---|---|
| Synth | 2 oscillators (saw/square/triangle/sine/noise, up to 7-voice unison with stereo spread), sub, noise, FM, drift, glide. **2× oversampled** with PolyBLEP/BLAMP oscillators and a half-band decimator. Filters: zero-delay-feedback 24 dB **ladder** with saturating feedback (self-oscillates cleanly) and a 12 dB SVF (LP/BP/HP). Analog-style envelopes, LFO to pitch/filter/PWM. |
| Presets | supersaw, warm pad, acid bass, sub bass, reese, pluck, lead, FM keys, brass. |
| Drum synth | kick, snare, clap, closed/open hat, toms, rim, crash; kits 808 / punchy / lo-fi. `part_pattern(&p, "x...x...", KICK, ...)`. |
| Sampler | WAV loading built in (8–32-bit, float, any rate); mp3/flac/ogg via ffmpeg. Windowed-sinc (Kaiser, 32-tap) pitching with anti-alias filtering when pitched up. Loops, envelopes. |
| Chopper | `sound_chop` (equal slices) and `sound_chop_onsets` (at the hits); `sampler_slices` plays slice *n* from note base+*n*, MPC-style. |
| Effects | FDN reverb (8 lines, Hadamard, modulated), ping-pong delay (tempo-synced), chorus, ADAA tanh drive, RBJ EQ, compressor (soft knee), sidechain pump, true-peak look-ahead limiter. |
| Export | `.wav` 24-bit dithered natively; `.mp3` (320k) `.flac` `.ogg` `.opus` `.m4a` via ffmpeg. Songs are mastered to −1 dBTP. |

Measured (tests): saw/square aliasing −56 dB at A7 and −61 dB at A5 (total inharmonic energy below 18 kHz), triangle −84/−101 dB; all filters stable at full resonance and drive.

## Planned
- interpreter: parses the language into these calls; runs code live in C-omposer
- compiler / CLI: `<name> song.<ext> -o song.mp3`, renders to an audio file
- live playback (needs an audio output library: ALSA/PipeWire dev headers aren't installed yet)
- higher-order (4-point) BLEP for even lower aliasing; wavetable oscillators
- microtonal chord naming; per-note detune (`mnote`) if wanted

`sketch.c` is the original data-type sketch. The editor's syntax tables are in `editor/src/language.c` for now.
