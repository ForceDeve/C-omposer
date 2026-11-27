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

## Planned
- interpreter: runs code live; embedded in C-omposer
- compiler / CLI: `<name> song.<ext> -o song.wav`, renders to an audio file
- microtonal chord naming; per-note detune (`mnote`) if wanted

`sketch.c` is the original data-type sketch. The editor's syntax tables are in `editor/src/language.c` for now.
