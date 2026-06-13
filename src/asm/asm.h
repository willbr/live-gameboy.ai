#ifndef ASM_H
#define ASM_H

#include <stddef.h>

/* -----------------------------------------------------------------------
 * Diagnostic
 * --------------------------------------------------------------------- */

typedef struct {
    const char *filename;
    int         line;
    int         col;
    char        msg[160];
} AsmDiag;

/* -----------------------------------------------------------------------
 * Token kinds
 * --------------------------------------------------------------------- */

typedef enum {
    TOK_IDENT,    /* identifier, mnemonic, register name, directive */
    TOK_NUMBER,   /* $hex, 0xhex, %bin, decimal integer             */
    TOK_STRING,   /* "..." — text/len point at contents (no quotes) */
    TOK_CHAR,     /* 'c'  — single character, value holds codepoint */
    TOK_PUNCT,    /* , : ( ) + - * / & | ~ << >>                    */
    TOK_NEWLINE,  /* physical newline; significant for stmt parsing  */
    TOK_EOF       /* sentinel                                        */
} AsmTokenKind;

typedef struct {
    AsmTokenKind kind;
    const char  *text;   /* pointer into original source (NOT NUL-terminated) */
    size_t       len;    /* byte length of lexeme (for strings: excludes quotes) */
    long         value;  /* numeric value for TOK_NUMBER / TOK_CHAR; 0 otherwise */
    int          line;   /* 1-based line where token starts */
    int          col;    /* 1-based column where token starts */
} AsmToken;

/* -----------------------------------------------------------------------
 * Lexer API
 *
 * asm_lex() tokenises `src` (NUL-terminated source text) and returns a
 * malloc'd array of tokens terminated by a TOK_EOF entry.  The caller is
 * responsible for free()ing the returned array.
 *
 * *out_count receives the number of tokens INCLUDING the TOK_EOF sentinel.
 *
 * Lexical errors are appended to *diags (realloc'd); *ndiags is updated.
 * On entry *diags may be NULL and *ndiags may be 0; the function will
 * malloc/realloc as needed.  The caller frees *diags.
 *
 * Returns NULL only on an allocation failure.
 * --------------------------------------------------------------------- */

AsmToken *asm_lex(const char *src,
                  const char *filename,
                  int        *out_count,
                  AsmDiag   **diags,
                  int        *ndiags);

#endif /* ASM_H */
