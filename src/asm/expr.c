/*
 * expr.c — SM83 assembler expression evaluator
 *
 * Evaluates a sub-range of an AsmToken array as an arithmetic expression.
 * Grammar (simplified, with standard precedence):
 *
 *   expr   ::= or_expr
 *   or_expr  ::= and_expr ( '|' and_expr )*
 *   and_expr ::= shift_expr ( '&' shift_expr )*
 *   shift_expr ::= add_expr ( ('<<' | '>>') add_expr )*
 *   add_expr ::= mul_expr ( ('+' | '-') mul_expr )*
 *   mul_expr ::= unary ( ('*' | '/') unary )*
 *   unary    ::= '-' unary | '~' unary | primary
 *   primary  ::= NUMBER | CHAR | IDENT | '$' | '@'
 *              | '(' expr ')'
 *              | 'LOW' '(' expr ')'
 *              | 'HIGH' '(' expr ')'
 *
 * '$' and '@' resolve to cur_addr.
 * Identifiers are looked up in syms; an undefined symbol is an error.
 */

#include "asm.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * Internal parser state
 * --------------------------------------------------------------------- */

typedef struct {
    const AsmToken     *toks;
    int                 start;   /* first token index */
    int                 end;     /* one past last token index */
    int                 pos;     /* current token index */
    const AsmSymbolTable *syms;
    long                cur_addr;
    AsmDiag            *err;     /* may be NULL */
    bool                failed;
} EvalCtx;

/* -----------------------------------------------------------------------
 * Helper: does a token text (case-insensitive) match a keyword?
 * --------------------------------------------------------------------- */

static bool tok_icase(const AsmToken *t, const char *kw)
{
    size_t klen = strlen(kw);
    if (t->len != klen) return false;
    for (size_t i = 0; i < klen; i++) {
        if (tolower((unsigned char)t->text[i]) != tolower((unsigned char)kw[i]))
            return false;
    }
    return true;
}

/* -----------------------------------------------------------------------
 * Error helpers
 * --------------------------------------------------------------------- */

static void eval_err(EvalCtx *ctx, const char *msg)
{
    ctx->failed = true;
    if (ctx->err) {
        ctx->err->line = (ctx->pos < ctx->end)
                         ? ctx->toks[ctx->pos].line
                         : (ctx->end > ctx->start ? ctx->toks[ctx->end-1].line : 0);
        ctx->err->col  = (ctx->pos < ctx->end)
                         ? ctx->toks[ctx->pos].col
                         : 0;
        snprintf(ctx->err->msg, sizeof(ctx->err->msg), "%s", msg);
    }
}

/* -----------------------------------------------------------------------
 * Token access
 * --------------------------------------------------------------------- */

static const AsmToken *cur_tok(const EvalCtx *ctx)
{
    if (ctx->pos < ctx->end) return &ctx->toks[ctx->pos];
    return NULL; /* past end */
}

static void eat(EvalCtx *ctx)
{
    if (ctx->pos < ctx->end) ctx->pos++;
}

static bool tok_punct_is(const AsmToken *t, const char *p)
{
    if (!t || t->kind != TOK_PUNCT) return false;
    size_t plen = strlen(p);
    return t->len == plen && memcmp(t->text, p, plen) == 0;
}

/* -----------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------- */

static long parse_expr(EvalCtx *ctx);

/* -----------------------------------------------------------------------
 * primary
 * --------------------------------------------------------------------- */

static long parse_primary(EvalCtx *ctx)
{
    if (ctx->failed) return 0;

    const AsmToken *t = cur_tok(ctx);
    if (!t) {
        eval_err(ctx, "unexpected end of expression");
        return 0;
    }

    /* NUMBER */
    if (t->kind == TOK_NUMBER) {
        long v = t->value;
        eat(ctx);
        return v;
    }

    /* CHAR */
    if (t->kind == TOK_CHAR) {
        long v = t->value;
        eat(ctx);
        return v;
    }

    /* '$' or '@' — current address */
    if (t->kind == TOK_PUNCT &&
        ((t->len == 1 && t->text[0] == '$') ||
         (t->len == 1 && t->text[0] == '@'))) {
        eat(ctx);
        return ctx->cur_addr;
    }

    /* '(' expr ')' */
    if (tok_punct_is(t, "(")) {
        eat(ctx); /* consume '(' */
        long v = parse_expr(ctx);
        const AsmToken *rp = cur_tok(ctx);
        if (!tok_punct_is(rp, ")")) {
            eval_err(ctx, "expected ')' in expression");
            return 0;
        }
        eat(ctx); /* consume ')' */
        return v;
    }

    /* Identifier: LOW(x), HIGH(x), or symbol lookup */
    if (t->kind == TOK_IDENT) {
        /* LOW(expr) */
        if (tok_icase(t, "LOW")) {
            eat(ctx);
            const AsmToken *lp = cur_tok(ctx);
            if (!tok_punct_is(lp, "(")) {
                eval_err(ctx, "expected '(' after LOW");
                return 0;
            }
            eat(ctx);
            long v = parse_expr(ctx);
            const AsmToken *rp = cur_tok(ctx);
            if (!tok_punct_is(rp, ")")) {
                eval_err(ctx, "expected ')' after LOW(expr");
                return 0;
            }
            eat(ctx);
            return v & 0xFF;
        }

        /* HIGH(expr) */
        if (tok_icase(t, "HIGH")) {
            eat(ctx);
            const AsmToken *lp = cur_tok(ctx);
            if (!tok_punct_is(lp, "(")) {
                eval_err(ctx, "expected '(' after HIGH");
                return 0;
            }
            eat(ctx);
            long v = parse_expr(ctx);
            const AsmToken *rp = cur_tok(ctx);
            if (!tok_punct_is(rp, ")")) {
                eval_err(ctx, "expected ')' after HIGH(expr");
                return 0;
            }
            eat(ctx);
            return (v >> 8) & 0xFF;
        }

        /* Symbol lookup */
        char name[128];
        size_t nlen = t->len < sizeof(name)-1 ? t->len : sizeof(name)-1;
        memcpy(name, t->text, nlen);
        name[nlen] = '\0';
        eat(ctx);

        if (!ctx->syms) {
            char msg[200];
            snprintf(msg, sizeof(msg), "undefined symbol '%s'", name);
            eval_err(ctx, msg);
            return 0;
        }
        const AsmSymbol *sym = asm_sym_lookup(ctx->syms, name);
        if (!sym || !sym->defined) {
            char msg[200];
            snprintf(msg, sizeof(msg), "undefined symbol '%s'", name);
            eval_err(ctx, msg);
            return 0;
        }
        return sym->value;
    }

    {
        char msg[200];
        snprintf(msg, sizeof(msg), "unexpected token in expression");
        eval_err(ctx, msg);
        return 0;
    }
}

/* -----------------------------------------------------------------------
 * unary: '-' | '~' | primary
 * --------------------------------------------------------------------- */

static long parse_unary(EvalCtx *ctx)
{
    if (ctx->failed) return 0;
    const AsmToken *t = cur_tok(ctx);
    if (tok_punct_is(t, "-")) {
        eat(ctx);
        return -parse_unary(ctx);
    }
    if (tok_punct_is(t, "~")) {
        eat(ctx);
        return ~parse_unary(ctx);
    }
    return parse_primary(ctx);
}

/* -----------------------------------------------------------------------
 * mul_expr: unary ( ('*' | '/') unary )*
 * --------------------------------------------------------------------- */

static long parse_mul(EvalCtx *ctx)
{
    long v = parse_unary(ctx);
    for (;;) {
        if (ctx->failed) break;
        const AsmToken *t = cur_tok(ctx);
        if (tok_punct_is(t, "*")) {
            eat(ctx);
            v *= parse_unary(ctx);
        } else if (tok_punct_is(t, "/")) {
            eat(ctx);
            long rhs = parse_unary(ctx);
            if (rhs == 0) { eval_err(ctx, "division by zero"); return 0; }
            v /= rhs;
        } else {
            break;
        }
    }
    return v;
}

/* -----------------------------------------------------------------------
 * add_expr: mul_expr ( ('+' | '-') mul_expr )*
 * --------------------------------------------------------------------- */

static long parse_add(EvalCtx *ctx)
{
    long v = parse_mul(ctx);
    for (;;) {
        if (ctx->failed) break;
        const AsmToken *t = cur_tok(ctx);
        if (tok_punct_is(t, "+")) {
            eat(ctx);
            v += parse_mul(ctx);
        } else if (tok_punct_is(t, "-")) {
            eat(ctx);
            v -= parse_mul(ctx);
        } else {
            break;
        }
    }
    return v;
}

/* -----------------------------------------------------------------------
 * shift_expr: add_expr ( ('<<' | '>>') add_expr )*
 * --------------------------------------------------------------------- */

static long parse_shift(EvalCtx *ctx)
{
    long v = parse_add(ctx);
    for (;;) {
        if (ctx->failed) break;
        const AsmToken *t = cur_tok(ctx);
        if (tok_punct_is(t, "<<")) {
            eat(ctx);
            long rhs = parse_add(ctx);
            v = (rhs >= 0 && rhs < 64) ? (v << rhs) : 0;
        } else if (tok_punct_is(t, ">>")) {
            eat(ctx);
            long rhs = parse_add(ctx);
            v = (rhs >= 0 && rhs < 64) ? (v >> rhs) : 0;
        } else {
            break;
        }
    }
    return v;
}

/* -----------------------------------------------------------------------
 * and_expr: shift_expr ( '&' shift_expr )*
 * --------------------------------------------------------------------- */

static long parse_and(EvalCtx *ctx)
{
    long v = parse_shift(ctx);
    for (;;) {
        if (ctx->failed) break;
        const AsmToken *t = cur_tok(ctx);
        if (tok_punct_is(t, "&")) {
            eat(ctx);
            v &= parse_shift(ctx);
        } else {
            break;
        }
    }
    return v;
}

/* -----------------------------------------------------------------------
 * or_expr: and_expr ( '|' and_expr )*
 * --------------------------------------------------------------------- */

static long parse_or(EvalCtx *ctx)
{
    long v = parse_and(ctx);
    for (;;) {
        if (ctx->failed) break;
        const AsmToken *t = cur_tok(ctx);
        if (tok_punct_is(t, "|")) {
            eat(ctx);
            v |= parse_and(ctx);
        } else {
            break;
        }
    }
    return v;
}

/* -----------------------------------------------------------------------
 * Top-level expression
 * --------------------------------------------------------------------- */

static long parse_expr(EvalCtx *ctx)
{
    return parse_or(ctx);
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/* Symbol table lookup */
const AsmSymbol *asm_sym_lookup(const AsmSymbolTable *st, const char *name)
{
    if (!st || !name) return NULL;
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->syms[i].name, name) == 0)
            return &st->syms[i];
    }
    return NULL;
}

bool asm_eval_expr(const AsmToken      *toks,
                   int                  start,
                   int                  count,
                   const AsmSymbolTable *syms,
                   long                 cur_addr,
                   long                *out,
                   AsmDiag             *err)
{
    EvalCtx ctx;
    ctx.toks     = toks;
    ctx.start    = start;
    ctx.end      = start + count;
    ctx.pos      = start;
    ctx.syms     = syms;
    ctx.cur_addr = cur_addr;
    ctx.err      = err;
    ctx.failed   = false;

    long result = parse_expr(&ctx);
    if (ctx.failed) return false;

    /* Ensure all tokens consumed */
    if (ctx.pos < ctx.end) {
        char msg[200];
        const AsmToken *t = &toks[ctx.pos];
        snprintf(msg, sizeof(msg), "unexpected token after expression");
        if (err) {
            err->line = t->line;
            err->col  = t->col;
            snprintf(err->msg, sizeof(err->msg), "%s", msg);
        }
        return false;
    }

    if (out) *out = result;
    return true;
}
