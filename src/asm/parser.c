/*
 * parser.c — SM83 assembler statement parser
 *
 * Converts a flat token stream (from asm_lex) into a flat array of AsmStmt.
 * One statement per logical line; TOK_NEWLINE terminates each statement.
 *
 * Grammar overview (per line):
 *
 *   line ::= label_def
 *          | directive
 *          | instruction
 *          | (blank / just a newline)
 *
 *   label_def  ::= IDENT ':' (optional directive or instruction follows on same line)
 *   directive  ::= ('SECTION'|'EQU'|'DEF'|'DB'|'DW'|'DS'|'INCBIN'|'INCLUDE') args...
 *                | IDENT 'EQU' expr        (EQU after a bare name)
 *   instruction ::= IDENT operand? (',' operand)*
 *
 * Operand classification:
 *   (expr)     -> OPND_MEM
 *   bare ident that is a register/condition keyword -> OPND_REG
 *   everything else -> OPND_IMM
 */

#include "asm.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Internal parser state
 * --------------------------------------------------------------------- */

typedef struct {
    const AsmToken *toks;
    int             count;   /* total including EOF */
    int             pos;     /* current position    */

    AsmStmt        *stmts;
    int             nstmts;
    int             scap;

    AsmDiag       **diags;
    int            *ndiags;
} Parser;

/* -----------------------------------------------------------------------
 * Diagnostic helper
 * --------------------------------------------------------------------- */

static void parse_diag(Parser *p, int line, int col, const char *msg)
{
    int n = *p->ndiags;
    AsmDiag *d = realloc(*p->diags, (size_t)(n + 1) * sizeof(AsmDiag));
    if (!d) return;
    *p->diags  = d;
    d[n].filename = NULL;   /* no filename in this context */
    d[n].line     = line;
    d[n].col      = col;
    snprintf(d[n].msg, sizeof(d[n].msg), "%s", msg);
    *p->ndiags = n + 1;
}

/* -----------------------------------------------------------------------
 * Token access helpers
 * --------------------------------------------------------------------- */

static const AsmToken *ptok(const Parser *p, int offset)
{
    int idx = p->pos + offset;
    if (idx < 0 || idx >= p->count) return &p->toks[p->count - 1]; /* EOF */
    return &p->toks[idx];
}

static const AsmToken *pcur(const Parser *p) { return ptok(p, 0); }
static void peat(Parser *p) { if (pcur(p)->kind != TOK_EOF) p->pos++; }

static bool punct_is(const AsmToken *t, const char *s)
{
    if (!t || t->kind != TOK_PUNCT) return false;
    size_t sl = strlen(s);
    return t->len == sl && memcmp(t->text, s, sl) == 0;
}

static bool tok_icase_kw(const AsmToken *t, const char *kw)
{
    if (!t || t->kind != TOK_IDENT) return false;
    size_t kl = strlen(kw);
    if (t->len != kl) return false;
    for (size_t i = 0; i < kl; i++) {
        if (tolower((unsigned char)t->text[i]) !=
            tolower((unsigned char)kw[i])) return false;
    }
    return true;
}

/* Copy at most (max-1) chars of token text into buf, NUL-terminate. */
static void tok_copy(char *buf, size_t max, const AsmToken *t)
{
    size_t n = t->len < max - 1 ? t->len : max - 1;
    memcpy(buf, t->text, n);
    buf[n] = '\0';
}

/* -----------------------------------------------------------------------
 * Known register and condition names (used for operand classification)
 * --------------------------------------------------------------------- */

static const char *s_reg_names[] = {
    "a","b","c","d","e","h","l",
    "af","bc","de","hl","sp",
    "hl+","hl-","hli","hld",
    NULL
};

static const char *s_cond_names[] = {
    "z","nz","nc",   /* note: "c" is omitted here — ambiguous with register c */
    NULL
};

/*
 * Classify a bare identifier as register or condition.
 * Returns OPND_REG for register and condition names (including the
 * ambiguous "c"), OPND_IMM for anything else.
 */
static OperandForm classify_ident(const AsmToken *t)
{
    /* Check registers */
    for (int i = 0; s_reg_names[i]; i++) {
        const char *rn = s_reg_names[i];
        size_t rl = strlen(rn);
        if (t->len == rl) {
            bool match = true;
            for (size_t j = 0; j < rl && match; j++)
                if (tolower((unsigned char)t->text[j]) !=
                    (unsigned char)rn[j]) match = false;
            if (match) return OPND_REG;
        }
    }
    /* Check conditions (not "c" — handled as register) */
    for (int i = 0; s_cond_names[i]; i++) {
        const char *cn = s_cond_names[i];
        size_t cl = strlen(cn);
        if (t->len == cl) {
            bool match = true;
            for (size_t j = 0; j < cl && match; j++)
                if (tolower((unsigned char)t->text[j]) !=
                    (unsigned char)cn[j]) match = false;
            if (match) return OPND_REG;
        }
    }
    return OPND_IMM;
}

/* -----------------------------------------------------------------------
 * Operand parser
 *
 * Consumes tokens up to the next ',' or NEWLINE/EOF and classifies the
 * operand.  Returns false if nothing was found.
 * --------------------------------------------------------------------- */

/*
 * Parse a single operand starting at the current position.
 * Returns false if at NEWLINE/EOF with nothing to parse.
 *
 * Operand ends at ',' or NEWLINE/EOF (at depth 0).
 */
static bool parse_operand(Parser *p, AsmOperand *op)
{
    const AsmToken *t = pcur(p);
    if (t->kind == TOK_NEWLINE || t->kind == TOK_EOF) return false;
    if (punct_is(t, ",")) return false;

    int tok_start = p->pos;

    /* (expr) -> OPND_MEM */
    if (punct_is(t, "(")) {
        /* consume tokens including matching close paren */
        int depth = 0;
        int end_pos = p->pos;
        for (int i = p->pos; i < p->count; i++) {
            const AsmToken *ti = &p->toks[i];
            if (ti->kind == TOK_NEWLINE || ti->kind == TOK_EOF) break;
            if (punct_is(ti, "(")) depth++;
            else if (punct_is(ti, ")")) {
                depth--;
                if (depth == 0) { end_pos = i; break; }
            }
        }
        /* advance past all these tokens */
        p->pos = end_pos + 1;
        op->form      = OPND_MEM;
        op->tok_start = tok_start;
        op->tok_count = p->pos - tok_start;
        return true;
    }

    /* Bare register/condition/immediate: collect until ',' or end-of-line */
    /* Peek at first token to decide form */
    OperandForm form = OPND_IMM;
    if (t->kind == TOK_IDENT) {
        /* Could be a register, or sp+disp, or a symbol expression */
        form = classify_ident(t);
        /* Check for sp+e8 form */
        if (form == OPND_REG &&
            (t->len == 2 && tolower((unsigned char)t->text[0]) == 's' &&
             tolower((unsigned char)t->text[1]) == 'p')) {
            /* peek for '+' or '-' to detect sp+e8 */
            const AsmToken *nxt = ptok(p, 1);
            if (punct_is(nxt, "+") || punct_is(nxt, "-")) {
                form = OPND_SP_DISP;
            }
        }
    }

    /* Consume tokens until ',' or NEWLINE/EOF */
    while (pcur(p)->kind != TOK_NEWLINE && pcur(p)->kind != TOK_EOF &&
           !punct_is(pcur(p), ",")) {
        peat(p);
    }

    op->form      = form;
    op->tok_start = tok_start;
    op->tok_count = p->pos - tok_start;
    return true;
}

/* -----------------------------------------------------------------------
 * Statement emit helpers
 * --------------------------------------------------------------------- */

static AsmStmt *push_stmt(Parser *p)
{
    if (p->nstmts >= p->scap) {
        int nc = p->scap ? p->scap * 2 : 32;
        AsmStmt *s = realloc(p->stmts, (size_t)nc * sizeof(AsmStmt));
        if (!s) return NULL;
        p->stmts = s;
        p->scap  = nc;
    }
    AsmStmt *s = &p->stmts[p->nstmts++];
    memset(s, 0, sizeof(*s));
    return s;
}

/* -----------------------------------------------------------------------
 * Skip to end of current line (past NEWLINE or EOF)
 * --------------------------------------------------------------------- */

static void skip_to_eol(Parser *p)
{
    while (pcur(p)->kind != TOK_NEWLINE && pcur(p)->kind != TOK_EOF)
        peat(p);
    if (pcur(p)->kind == TOK_NEWLINE) peat(p);
}

/* -----------------------------------------------------------------------
 * Parse directive arguments as a token span
 *
 * Fills args_start / args_count in *dir to span from current position
 * up to but not including the terminating NEWLINE/EOF.
 * Does NOT advance past the NEWLINE.
 * --------------------------------------------------------------------- */

static void collect_args(Parser *p, int *args_start, int *args_count)
{
    *args_start = p->pos;
    int start = p->pos;
    while (pcur(p)->kind != TOK_NEWLINE && pcur(p)->kind != TOK_EOF)
        peat(p);
    *args_count = p->pos - start;
}

/* -----------------------------------------------------------------------
 * Main line parser
 *
 * Parses one logical line and emits 1 or 2 statements (a label can be
 * followed by a directive or instruction on the same line).
 * --------------------------------------------------------------------- */

static void parse_line(Parser *p)
{
    /* Skip leading NEWLINEs (blank lines) */
    while (pcur(p)->kind == TOK_NEWLINE) peat(p);
    if (pcur(p)->kind == TOK_EOF) return;

    const AsmToken *t0 = pcur(p);

    /* ------------------------------------------------------------------ */
    /* Check for DEF: "DEF name EQU expr" */
    /* ------------------------------------------------------------------ */
    if (tok_icase_kw(t0, "DEF")) {
        peat(p); /* consume DEF */
        const AsmToken *name_tok = pcur(p);
        if (name_tok->kind != TOK_IDENT) {
            parse_diag(p, t0->line, t0->col, "expected name after DEF");
            skip_to_eol(p); return;
        }
        int name_start = p->pos;
        peat(p); /* consume name */
        const AsmToken *eq_tok = pcur(p);
        if (!tok_icase_kw(eq_tok, "EQU")) {
            parse_diag(p, t0->line, t0->col, "expected EQU after DEF name");
            skip_to_eol(p); return;
        }
        peat(p); /* consume EQU */
        AsmStmt *s = push_stmt(p);
        if (!s) { skip_to_eol(p); return; }
        s->kind        = ST_DIRECTIVE;
        s->line        = t0->line;
        s->dir.kind    = DIR_EQU;
        s->dir.name_start = name_start;
        s->dir.name_count = 1;
        collect_args(p, &s->dir.args_start, &s->dir.args_count);
        if (pcur(p)->kind == TOK_NEWLINE) peat(p);
        return;
    }

    /* ------------------------------------------------------------------ */
    /* Check for standalone directives: SECTION, DB, DW, DS, INCBIN, INCLUDE */
    /* ------------------------------------------------------------------ */
    struct { const char *kw; AsmDirectiveKind dk; } dir_table[] = {
        { "SECTION", DIR_SECTION },
        { "DB",      DIR_DB      },
        { "DW",      DIR_DW      },
        { "DS",      DIR_DS      },
        { "INCBIN",  DIR_INCBIN  },
        { "INCLUDE", DIR_INCLUDE },
        { NULL, 0 }
    };
    for (int i = 0; dir_table[i].kw; i++) {
        if (tok_icase_kw(t0, dir_table[i].kw)) {
            peat(p); /* consume directive keyword */
            AsmStmt *s = push_stmt(p);
            if (!s) { skip_to_eol(p); return; }
            s->kind      = ST_DIRECTIVE;
            s->line      = t0->line;
            s->dir.kind  = dir_table[i].dk;
            s->dir.name_start = 0;
            s->dir.name_count = 0;
            collect_args(p, &s->dir.args_start, &s->dir.args_count);
            if (pcur(p)->kind == TOK_NEWLINE) peat(p);
            return;
        }
    }

    /* ------------------------------------------------------------------ */
    /* Identifier: could be label, EQU, or instruction mnemonic */
    /* ------------------------------------------------------------------ */
    if (t0->kind == TOK_IDENT) {
        const AsmToken *t1 = ptok(p, 1);

        /* Label: IDENT ':' */
        if (punct_is(t1, ":")) {
            AsmStmt *s = push_stmt(p);
            if (!s) { skip_to_eol(p); return; }
            s->kind          = ST_LABEL;
            s->line          = t0->line;
            s->label.is_local = (t0->len > 0 && t0->text[0] == '.');
            tok_copy(s->label.name, sizeof(s->label.name), t0);
            peat(p); /* consume ident */
            peat(p); /* consume ':' */

            /* After a label, there may be another statement on the same line */
            if (pcur(p)->kind != TOK_NEWLINE && pcur(p)->kind != TOK_EOF) {
                parse_line(p); /* recurse for same-line instr/directive */
                return;
            }
            if (pcur(p)->kind == TOK_NEWLINE) peat(p);
            return;
        }

        /* NAME EQU expr (bare EQU without DEF) */
        if (tok_icase_kw(t1, "EQU")) {
            int name_start = p->pos;
            peat(p); /* consume name */
            peat(p); /* consume EQU */
            AsmStmt *s = push_stmt(p);
            if (!s) { skip_to_eol(p); return; }
            s->kind        = ST_DIRECTIVE;
            s->line        = t0->line;
            s->dir.kind    = DIR_EQU;
            s->dir.name_start = name_start;
            s->dir.name_count = 1;
            collect_args(p, &s->dir.args_start, &s->dir.args_count);
            if (pcur(p)->kind == TOK_NEWLINE) peat(p);
            return;
        }

        /* Instruction: IDENT [operand (, operand)*] */
        {
            AsmStmt *s = push_stmt(p);
            if (!s) { skip_to_eol(p); return; }
            s->kind = ST_INSTR;
            s->line = t0->line;
            tok_copy(s->instr.mnemonic, sizeof(s->instr.mnemonic), t0);
            peat(p); /* consume mnemonic */

            /* Parse operands */
            int nops = 0;
            if (pcur(p)->kind != TOK_NEWLINE && pcur(p)->kind != TOK_EOF) {
                AsmOperand op;
                if (parse_operand(p, &op) && nops < 4)
                    s->instr.ops[nops++] = op;
                while (punct_is(pcur(p), ",") && nops < 4) {
                    peat(p); /* consume ',' */
                    if (parse_operand(p, &op))
                        s->instr.ops[nops++] = op;
                }
            }
            s->instr.nops = nops;
            if (pcur(p)->kind == TOK_NEWLINE) peat(p);
            return;
        }
    }

    /* Unrecognised line */
    {
        char msg[200];
        snprintf(msg, sizeof(msg), "unexpected token at start of statement");
        parse_diag(p, t0->line, t0->col, msg);
        skip_to_eol(p);
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

AsmStmt *asm_parse(const AsmToken *toks,
                   int             count,
                   int            *out_n,
                   AsmDiag       **diags,
                   int            *ndiags)
{
    Parser p;
    memset(&p, 0, sizeof(p));
    p.toks   = toks;
    p.count  = count;
    p.diags  = diags;
    p.ndiags = ndiags;

    while (pcur(&p)->kind != TOK_EOF) {
        parse_line(&p);
    }

    if (out_n) *out_n = p.nstmts;
    return p.stmts;
}
