/*
 * tokens.h — the categories every piece of source text can fall into.
 *
 * Each category gets its own colour/style in theme.c and its own GtkTextTag
 * (named by the string in the second column).  To add a category, add a line
 * here, give it a style in theme.c and make the lexer / language.c produce it.
 */
#pragma once

#define TOKEN_CATEGORIES(X)                     \
    X(TOK_PLAIN,         "plain")               \
    X(TOK_IDENTIFIER,    "identifier")          \
    X(TOK_KEYWORD,       "keyword")             \
    X(TOK_MUSIC_KEYWORD, "music-keyword")       \
    X(TOK_TYPE,          "type")                \
    X(TOK_MUSIC_TYPE,    "music-type")          \
    X(TOK_USER_TYPE,     "user-type")           \
    X(TOK_BUILTIN,       "builtin")             \
    X(TOK_FUNCTION,      "function")            \
    X(TOK_CONSTANT,      "constant")            \
    X(TOK_NUMBER,        "number")              \
    X(TOK_UNIT,          "unit")                \
    X(TOK_NOTE,          "note")                \
    X(TOK_STRING,        "string")              \
    X(TOK_CHAR,          "char")                \
    X(TOK_COMMENT,       "comment")             \
    X(TOK_PREPROCESSOR,  "preprocessor")        \
    X(TOK_OPERATOR,      "operator")            \
    X(TOK_PUNCTUATION,   "punctuation")         \
    X(TOK_ERROR,         "error")

typedef enum {
#define X(id, name) id,
    TOKEN_CATEGORIES(X)
#undef X
    TOK_COUNT
} TokenCategory;

/* "keyword", "music-type", ... (also the GtkTextTag / CSS class suffix) */
const char *token_category_name(TokenCategory category);

typedef struct {
    TokenCategory category;
    int start;        /* byte offset into the source text   */
    int length;       /* length in bytes                     */
    int char_start;   /* character offset (for GtkTextIter)  */
    int char_length;  /* length in characters                */
} Token;
