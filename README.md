# C-omposer

A C-like language for music composition, and the editor built for it.

| Folder | What it is |
|---|---|
| `editor/` | **C-omposer**, a GTK4 editor with music-aware highlighting and autocomplete. It will use the interpreter for live-coding mode. |
| `lang/` | **The language** (name not settled: Csus / Cdim / …). It will hold the interpreter, plus a compiler that renders to audio files (mp3, wav, …) and runs from the terminal, so it works with any editor. |

## Using the editor

- **Templates ▾**: *Basic C program* (the default: `#include`, `main`, hello world) or *Test functions*. Unsaved work is never replaced: a template opens in a new window instead.
- **Interpreter / Compiler** switch in the header bar:
  - *Interpreter*: live. `//viz` after a function's closing brace shows its waveform under it, re-rendered ~1 s after you stop typing. **Run (F5)** plays `main()`.
  - *Compiler*: **Build (F5)** compiles the whole song to an audio file (mp3, wav, flac, ogg, opus, m4a; chosen by the extension).
- **▶ in the margin** plays any function on its own: no arguments, or only `note`/`chord` arguments (played with C4 / a C major chord). Functions returning a `part`, `note` or `chord` are played with a plain beep. Click ■ to stop.
- **Output** panel: compiler errors (pointing at your file and line), `printf` output, render times.

From a terminal, the same compiler: `./build/lang/composerc song.cmp -o song.mp3` (`--call chorus` renders one function, `--list` shows what's playable).

How it runs today: your code is translated (note literals `C4`, units `1b` `2bars` `120bpm` `250ms`) and compiled with gcc against the engine in the background, and each function runs in its own process, so a crashing song can't take the editor down. A real interpreter can replace that backend later without changing the editor.

## Build

Needs GTK 4.8+, CMake 3.20+ and gcc (songs are compiled with it). ffmpeg is optional (mp3/flac/ogg); playback uses `pw-play`, `paplay` or `aplay`.

```sh
cmake -B build -G Ninja && cmake --build build
./build/editor/C_omposer
```

## Branches

`main` is always buildable. Work happens on short branches named `editor/<feature>` or `lang/<feature>`, which merge back into `main`.
