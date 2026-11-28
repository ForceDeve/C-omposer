# C-omposer

A C-like language for music composition, and the editor built for it.

| Folder | What it is |
|---|---|
| `editor/` | **C-omposer**, a GTK4 editor with music-aware highlighting and autocomplete. It will use the interpreter for live-coding mode. |
| `lang/` | **The language** (name not settled: Csus / Cdim / …). It will hold the interpreter, plus a compiler that renders to audio files (mp3, wav, …) and runs from the terminal, so it works with any editor. |

## Using the editor

- **Header bar**: **▶** runs `main()` live in the interpreter (F5); **🔨** compiles the song to an audio file and plays it (Ctrl+B; asks where once, *☰ → Export audio as…* to change; mp3, wav, flac, ogg, opus or m4a by extension); **■** stops. The lit button is the current mode: in interpreter mode `//viz` panels and background visuals stay live as you type.
- **Templates** live in *☰ → New from template*: *Basic C program* (the default), *Test functions*, and your own. *Save this file as a template…* adds one to `~/.config/c-omposer/templates/`. Unsaved work is never replaced: a template opens in a new window instead.
- **▶ in the margin** plays any function on its own: no arguments, or only `note`/`chord` arguments (played with C4 / a C major chord). Functions returning a `part`, `note` or `chord` are played with a plain beep. Click ■ to stop.
- **Output** panel: compiler errors (pointing at your file and line), `printf` output, render times.

- **Parameter hints**: inside a call's `( )` a popup shows what goes there: types and names, with the argument you're typing highlighted (your own functions and all built-ins).
- **Projects sidebar** (F9): a project is a folder for songs, code snippets and samples. *New project…* creates `samples/`, `snippets/` and `main.cmp`. Double-click opens code or previews a sample; right-click inserts a snippet or `load_sample("samples/…")` at the cursor; samples can be dragged into the code. Songs run inside the project folder, so relative sample paths work.
- **Themes** (Preferences, Ctrl+,): INI files in the same format as FHI's (an FHI theme works as is). Bundled: C-omposer Dark/Light, Catppuccin Mocha, Dracula, Gruvbox Dark, Nord, Solarized Light, Pastel Pink. Your own go in `~/.config/c-omposer/themes/`; every key is optional, including per-token syntax colours. See `editor/themes/c-omposer-dark.ini` for all keys.
- **Interpreter-mode background** (Preferences): a picture behind the code (dimmable) and/or an audio visualizer (spectrum bars, oscilloscope, radial spectrum, particles) that follows whatever is playing. Off by default; while off nothing is loaded or animated.

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
