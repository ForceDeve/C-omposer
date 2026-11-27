/*
 * symbols.h — finds what the user declared in the document:
 *   typedef'd / struct / union / enum types (with their fields),
 *   functions, variables, enum constants and #defines.
 *
 * This is a forgiving, best-effort scan (no real parser): it only needs to be
 * good enough to colour user types and feed the autocompletion.
 */
#pragma once

#include <glib.h>
#include "language.h"
#include "tokens.h"

typedef enum {
    SYM_TYPE,
    SYM_FUNCTION,
    SYM_VARIABLE,
    SYM_CONSTANT,
    SYM_MACRO,
} SymbolKind;

typedef struct Symbol Symbol;
struct Symbol {
    char       *name;
    SymbolKind  kind;
    char       *type_name;  /* variables/fields: their type, functions: return type,
                               macros: their value, constants: their enum         */
    GPtrArray  *members;    /* SYM_TYPE: fields (Symbol*), may be NULL           */
};

typedef struct {
    GHashTable *by_name;    /* char* -> Symbol* (owned) */
} SymbolTable;

/* Scans the tokens and re-categorises identifiers that turn out to be user
 * types (TOK_USER_TYPE) or user constants (TOK_CONSTANT). */
SymbolTable  *symbols_scan(const Language *lang, const char *text, GArray *tokens);
void          symbols_free(SymbolTable *table);
const Symbol *symbols_lookup(const SymbolTable *table, const char *name);
