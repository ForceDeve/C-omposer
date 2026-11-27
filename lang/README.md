# The language (name TBD: Csus / Cdim / …)

## Runtime music types (`include/music.h`, `src/music.c`)

| Type | Size | Notes |
|---|---|---|
| `note` | 1 byte | `uint8_t` MIDI pitch, C4 = 60. `n++` = semitone up, `n += 7` = fifth up. No loudness: that belongs to the event that plays it. |
| `scale` | 3 bytes | `root`, `mode`, `kind`. Notes are computed: `scale_degree(&s, d)` walks forever across octaves; `mode_next()` wraps (Locrian → Ionian). |
| `chord` | 9 bytes | `notes[8]` + `count`. Root and name are **computed** from the notes, so `c.notes[1]--` turns C into Cm. Any note set gets a name (`Cmaj7sus4(b5)`, `C7(b9)`, `C/E`). |

`chord_from_scale(&s, degree, 3|4)` stacks thirds, so a progression is a list of degrees: I–vi–IV–V = `{0, 5, 3, 4}`.

Tests: `ctest` from the build dir, or `./build/lang/test_music`.

## Planned
- interpreter: runs code live; embedded in C-omposer
- compiler / CLI: `<name> song.<ext> -o song.wav`, renders to an audio file
- microtonal pitch (design open)

`sketch.c` is the original data-type sketch. The editor's syntax tables are in `editor/src/language.c` for now.
