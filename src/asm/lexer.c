#include "asm.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

typedef struct {
    const char  *src;
    const char  *filename;
    size_t       pos;    /* current byte offset */
    size_t       len;    /* total source length  */
    int          line;
    int          col;

    AsmToken    *toks;
    int          ntoks;
    int          tcap;

    AsmDiag    **diags;
    int         *ndiags;
} Lexer;

static void push_diag(Lexer *lx, int line, int col, const char *msg)
{
    int n = *lx->ndiags;
    AsmDiag *d = realloc(*lx->diags, (size_t)(n + 1) * sizeof(AsmDiag));
    if (!d) return;
    *lx->diags = d;
    d[n].filename = lx->filename;
    d[n].line     = line;
    d[n].col      = col;
    snprintf(d[n].msg, sizeof(d[n].msg), "%s", msg);
    *lx->ndiags = n + 1;
}

static void push_tok(Lexer *lx, AsmTokenKind kind,
                     const char *text, size_t tlen,
                     long value, int line, int col)
{
    if (lx->ntoks >= lx->tcap) {
        int nc = lx->tcap ? lx->tcap * 2 : 64;
        AsmToken *t = realloc(lx->toks, (size_t)nc * sizeof(AsmToken));
        if (!t) return;
        lx->toks = t;
        lx->tcap = nc;
    }
    AsmToken *t = &lx->toks[lx->ntoks++];
    t->kind  = kind;
    t->text  = text;
    t->len   = tlen;
    t->value = value;
    t->line  = line;
    t->col   = col;
}

static inline int cur(const Lexer *lx)
{
    return (lx->pos < lx->len) ? (unsigned char)lx->src[lx->pos] : -1;
}

static inline int peek(const Lexer *lx, size_t offset)
{
    size_t p = lx->pos + offset;
    return (p < lx->len) ? (unsigned char)lx->src[p] : -1;
}

static inline void advance(Lexer *lx)
{
    if (lx->pos < lx->len) {
        if (lx->src[lx->pos] == '\n') {
            lx->line++;
            lx->col = 1;
        } else {
            lx->col++;
        }
        lx->pos++;
    }
}

/* -----------------------------------------------------------------------
 * Number parsing helpers
 * --------------------------------------------------------------------- */

static int is_hex_digit(int c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* -----------------------------------------------------------------------
 * Ident chars: letters, digits, '_', '.'
 * A leading '.' makes a local label like ".loop" — treat as one IDENT.
 * --------------------------------------------------------------------- */

static int is_ident_start(int c)
{
    return isalpha(c) || c == '_' || c == '.';
}

static int is_ident_cont(int c)
{
    return isalnum(c) || c == '_' || c == '.';
}

/* -----------------------------------------------------------------------
 * Main tokenisation pass
 * --------------------------------------------------------------------- */

static void lex_all(Lexer *lx)
{
    /* Track whether we've already emitted a NEWLINE for this run of blank
     * lines so we collapse multiple consecutive blank lines into one. */
    int last_was_newline = 1; /* suppress leading newline at BOF */

    while (lx->pos < lx->len) {
        int c = cur(lx);

        /* ---- skip horizontal whitespace ---- */
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lx);
            continue;
        }

        /* ---- newline ---- */
        if (c == '\n') {
            int ln = lx->line, co = lx->col;
            advance(lx); /* increments line */
            if (!last_was_newline) {
                push_tok(lx, TOK_NEWLINE, lx->src + lx->pos - 1, 1, 0, ln, co);
                last_was_newline = 1;
            }
            continue;
        }

        /* ---- comment: ; to end of line ---- */
        if (c == ';') {
            while (lx->pos < lx->len && cur(lx) != '\n')
                advance(lx);
            continue;
        }

        last_was_newline = 0;

        /* ---- string literal ---- */
        if (c == '"') {
            int ln = lx->line, co = lx->col;
            advance(lx); /* skip opening quote */
            const char *start = lx->src + lx->pos;
            while (lx->pos < lx->len && cur(lx) != '"' && cur(lx) != '\n')
                advance(lx);
            size_t slen = (size_t)(lx->src + lx->pos - start);
            if (cur(lx) != '"') {
                char msg[200];
                snprintf(msg, sizeof(msg),
                         "unterminated string literal");
                push_diag(lx, ln, co, msg);
            } else {
                advance(lx); /* skip closing quote */
            }
            push_tok(lx, TOK_STRING, start, slen, 0, ln, co);
            continue;
        }

        /* ---- char literal 'c' ---- */
        if (c == '\'') {
            int ln = lx->line, co = lx->col;
            advance(lx); /* skip opening quote */
            long val = 0;
            const char *start = lx->src + lx->pos;
            if (cur(lx) != '\'' && cur(lx) != '\n' && cur(lx) != -1) {
                val = (unsigned char)cur(lx);
                advance(lx);
            }
            size_t slen = (size_t)(lx->src + lx->pos - start);
            if (cur(lx) != '\'') {
                char msg[200];
                snprintf(msg, sizeof(msg), "unterminated char literal");
                push_diag(lx, ln, co, msg);
            } else {
                advance(lx); /* skip closing quote */
            }
            push_tok(lx, TOK_CHAR, start, slen, val, ln, co);
            continue;
        }

        /* ---- hex number: $hhhh  OR bare $ (current-address symbol) ---- */
        if (c == '$') {
            int ln = lx->line, co = lx->col;
            const char *start = lx->src + lx->pos;
            advance(lx); /* skip '$' */
            if (!is_hex_digit(cur(lx))) {
                /* Bare '$' — current address; emit as TOK_PUNCT so the
                 * expression evaluator can distinguish it from a number. */
                push_tok(lx, TOK_PUNCT, start, 1, 0, ln, co);
                continue;
            }
            long val = 0;
            while (is_hex_digit(cur(lx))) {
                val = val * 16 + hex_val(cur(lx));
                advance(lx);
            }
            push_tok(lx, TOK_NUMBER, start,
                     (size_t)(lx->src + lx->pos - start), val, ln, co);
            continue;
        }

        /* ---- binary number: %bbbb ---- */
        if (c == '%') {
            int ln = lx->line, co = lx->col;
            const char *start = lx->src + lx->pos;
            advance(lx); /* skip '%' */
            long val = 0;
            while (cur(lx) == '0' || cur(lx) == '1') {
                val = val * 2 + (cur(lx) - '0');
                advance(lx);
            }
            push_tok(lx, TOK_NUMBER, start,
                     (size_t)(lx->src + lx->pos - start), val, ln, co);
            continue;
        }

        /* ---- decimal / 0x hex ---- */
        if (isdigit(c)) {
            int ln = lx->line, co = lx->col;
            const char *start = lx->src + lx->pos;
            /* check 0x prefix */
            if (c == '0' && (peek(lx, 1) == 'x' || peek(lx, 1) == 'X')) {
                advance(lx); advance(lx); /* skip '0x' */
                long val = 0;
                while (is_hex_digit(cur(lx))) {
                    val = val * 16 + hex_val(cur(lx));
                    advance(lx);
                }
                push_tok(lx, TOK_NUMBER, start,
                         (size_t)(lx->src + lx->pos - start), val, ln, co);
            } else {
                long val = 0;
                while (isdigit(cur(lx))) {
                    val = val * 10 + (cur(lx) - '0');
                    advance(lx);
                }
                push_tok(lx, TOK_NUMBER, start,
                         (size_t)(lx->src + lx->pos - start), val, ln, co);
            }
            continue;
        }

        /* ---- identifier (includes .local) ---- */
        if (is_ident_start(c)) {
            int ln = lx->line, co = lx->col;
            const char *start = lx->src + lx->pos;
            while (is_ident_cont(cur(lx)))
                advance(lx);
            size_t ilen = (size_t)(lx->src + lx->pos - start);
            push_tok(lx, TOK_IDENT, start, ilen, 0, ln, co);
            continue;
        }

        /* ---- punctuation ---- */
        {
            int ln = lx->line, co = lx->col;
            const char *start = lx->src + lx->pos;

            /* multi-char: << >> */
            if (c == '<' && peek(lx, 1) == '<') {
                advance(lx); advance(lx);
                push_tok(lx, TOK_PUNCT, start, 2, 0, ln, co);
                continue;
            }
            if (c == '>' && peek(lx, 1) == '>') {
                advance(lx); advance(lx);
                push_tok(lx, TOK_PUNCT, start, 2, 0, ln, co);
                continue;
            }

            /* single-char punct */
            switch (c) {
            case ',': case ':': case '(': case ')':
            case '+': case '-': case '*': case '/':
            case '&': case '|': case '~': case '[': case ']':
                advance(lx);
                push_tok(lx, TOK_PUNCT, start, 1, 0, ln, co);
                continue;
            default:
                break;
            }

            /* unknown character — emit a diagnostic and skip */
            {
                char msg[200];
                snprintf(msg, sizeof(msg),
                         "unexpected character '%c' (0x%02x)", c, (unsigned)c);
                push_diag(lx, ln, co, msg);
                advance(lx);
            }
        }
    }

    /* EOF sentinel */
    push_tok(lx, TOK_EOF, lx->src + lx->len, 0, 0, lx->line, lx->col);
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

AsmToken *asm_lex(const char *src,
                  const char *filename,
                  int        *out_count,
                  AsmDiag   **diags,
                  int        *ndiags)
{
    Lexer lx;
    memset(&lx, 0, sizeof(lx));
    lx.src      = src;
    lx.filename = filename ? filename : "<input>";
    lx.len      = strlen(src);
    lx.line     = 1;
    lx.col      = 1;
    lx.diags    = diags;
    lx.ndiags   = ndiags;

    lex_all(&lx);

    if (out_count)
        *out_count = lx.ntoks;

    return lx.toks;
}
