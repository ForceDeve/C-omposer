# The language (name TBD)

Planned:
- `interpreter`: runs code live, and is embedded in C-omposer
- `compiler` / CLI: `<name> song.<ext> -o song.wav`, renders to an audio file
- runtime music types: notes, chords, scales (see `sketch.c` for the original data-type sketch)

The editor's syntax tables currently live in `editor/src/language.c` and will move here once the interpreter shares them.
