/*
 * lexer.h — turns source text into a flat list of Tokens.
 *
 * The lexer itself has no hard-coded syntax: every decision (what a comment
 * looks like, which words are keywords, what a note literal is, ...) is read
 * from the Language passed in (see language.c).
 */
#pragma once

#include <glib.h>
#include "language.h"
#include "tokens.h"

/* Returns a GArray of Token.  Tokens point into `text` by byte offset, so
 * keep `text` alive as long as you use them.  Whitespace produces no token. */
GArray *lexer_tokenize(const Language *lang, const char *text, int length);

/* Convenience: does the token's text equal `s`? */
gboolean token_text_is(const char *text, const Token *tok, const char *s);
