# C-omposer

A C-like language for music composition, and the editor built for it.

| Folder | What it is |
|---|---|
| `editor/` | **C-omposer**, a GTK4 editor with music-aware highlighting and autocomplete. It will use the interpreter for live-coding mode. |
| `lang/` | **The language** (name not settled: Csus / Cdim / …). It will hold the interpreter, plus a compiler that renders to audio files (mp3, wav, …) and runs from the terminal, so it works with any editor. |

## Build

Needs GTK 4.8+ and CMake 3.20+.

```sh
cmake -B build -G Ninja && cmake --build build
./build/editor/C_omposer
```

## Branches

`main` is always buildable. Work happens on short branches named `editor/<feature>` or `lang/<feature>`, which merge back into `main`.
