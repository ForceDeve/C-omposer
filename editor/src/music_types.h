/*
 * music_types.h — the runtime data types behind the language.
 *
 * Keep the built-in type tables in language.c (note_members, scale_members,
 * ...) in sync with these, so the editor's autocompletion matches reality.
 */
#pragma once

typedef struct {
    char pitch;
    char volume;
} note;

typedef struct {
    note *notes;
    char  mode;
    char  scale;
} scale;
