/*
 * assemble.c — Two-pass SM83 assembler driver
 *
 * Pipeline:
 *   asm_lex()    -> token array
 *   asm_parse()  -> AsmStmt array
 *   pass 1       -> assign addresses, build symbol table
 *   pass 2       -> encode bytes, fill ROM + build database
 *
 * Section model (Task 4, single-section):
 *   ROM0  — bank 0, linear offset == cpu_addr  (0x0000..0x3FFF)
 *   Default (no SECTION) — ROM0 starting at 0x0150 (above header region)
 *
 * Task 5 will add multi-bank ROMX, header generation, and INCBIN/INCLUDE.
 */

#include "asm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Forward declarations */
static int  estimate_instr_len(const char *, const AsmToken *,
                                const AsmOperand *, int, uint16_t,
                                const AsmSymbolTable *);
static bool tok_eq_str(const AsmToken *t, const char *s);

/* -------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define ROM_MIN_SIZE   0x8000u   /* 32 KiB minimum */
#define DEFAULT_ORG    0x0150u   /* default start (above cartridge header) */

/* -------------------------------------------------------------------------
 * Internal diagnostic helper
 * ----------------------------------------------------------------------- */

static void push_diag(AsmResult *r, int line, const char *msg)
{
    AsmDiag *d = realloc(r->diags, (size_t)(r->ndiags + 1) * sizeof(AsmDiag));
    if (!d) return;
    r->diags = d;
    AsmDiag *e = &d[r->ndiags++];
    e->filename = NULL;
    e->line     = line;
    e->col      = 0;
    snprintf(e->msg, sizeof(e->msg), "%s", msg);
    r->ok = false;
}

/* -------------------------------------------------------------------------
 * Symbol table helpers
 * ----------------------------------------------------------------------- */

/* Add or update a symbol.  Returns pointer to the slot. */
static AsmSymbol *sym_define(AsmSymbolTable *st,
                              const char *name,
                              int bank, uint16_t addr, uint32_t off,
                              long value, bool is_const)
{
    /* Search existing */
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->syms[i].name, name) == 0) {
            AsmSymbol *s = &st->syms[i];
            s->bank    = bank;
            s->addr    = addr;
            s->off     = off;
            s->value   = is_const ? value : (long)addr;
            s->defined = true;
            return s;
        }
    }
    /* New entry */
    if (st->count >= st->cap) {
        int nc = st->cap ? st->cap * 2 : 32;
        AsmSymbol *ns = realloc(st->syms, (size_t)nc * sizeof(AsmSymbol));
        if (!ns) return NULL;
        st->syms = ns;
        st->cap  = nc;
    }
    AsmSymbol *s = &st->syms[st->count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->bank    = bank;
    s->addr    = addr;
    s->off     = off;
    s->value   = is_const ? value : (long)addr;
    s->defined = true;
    return s;
}

/* -------------------------------------------------------------------------
 * Section state
 * ----------------------------------------------------------------------- */

typedef struct {
    int      bank;       /* 0 for ROM0 */
    uint16_t cur_addr;   /* CPU address */
    uint32_t cur_off;    /* linear ROM offset */
} Section;

static void section_advance(Section *sec, int nbytes)
{
    sec->cur_addr = (uint16_t)(sec->cur_addr + (uint16_t)nbytes);
    sec->cur_off += (uint32_t)nbytes;
}

/* -------------------------------------------------------------------------
 * DB/DW/DS size computation (pass 1)
 *
 * We need to count bytes without fully evaluating expressions (forward refs
 * are fine for DB/DW since each element is one token-span).
 * ----------------------------------------------------------------------- */

/*
 * Count bytes that a DB directive will emit.
 * Each element is either: a string literal (byte per char), a number
 * expression (1 byte), or a comma separator.
 */
static int db_size(const AsmToken *toks, int start, int count)
{
    int total = 0;
    int end   = start + count;
    int i     = start;
    while (i < end) {
        const AsmToken *t = &toks[i];
        if (t->kind == TOK_STRING) {
            total += (int)t->len;
            i++;
        } else if (t->kind == TOK_PUNCT && t->len == 1 && t->text[0] == ',') {
            i++;
        } else {
            /* consume one expression element (skip until ',' or end) */
            total += 1;
            while (i < end) {
                const AsmToken *ti = &toks[i];
                if (ti->kind == TOK_PUNCT && ti->len == 1 && ti->text[0] == ',')
                    break;
                i++;
            }
        }
    }
    return total;
}

/* -------------------------------------------------------------------------
 * Pass-1 symbol lookup: returns 0 for undefined (safe for length calc)
 * ----------------------------------------------------------------------- */

typedef struct {
    AsmSymbolTable *real;
} LenSymTable;

/*
 * Wrap the real symbol table for pass-1 length calculation.
 * We need a permissive lookup that returns 0 for undefined symbols so that
 * asm_encode can produce a byte count even for forward references.
 */
/*
 * Build a fake symbol table that returns 0 for everything, for pass-1 length.
 * We do this by temporarily patching the lookup — but since asm_encode uses
 * the real asm_sym_lookup signature, we need a different approach.
 *
 * Strategy: for pass 1, we pass the real symbol table (already-defined
 * labels have real addresses) and rely on the fact that SM83 instruction
 * length is FIXED per opcode form regardless of operand value.  The only
 * exception would be range-checking jr, but length is always 2 bytes for jr.
 * So we just try to encode; if it fails (undefined fwd ref), we estimate
 * length from the mnemonic.
 */
static int estimate_instr_len(const char *mnemonic,
                               const AsmToken *toks,
                               const AsmOperand *ops, int nops,
                               uint16_t cur_addr,
                               const AsmSymbolTable *syms)
{
    uint8_t buf[4];
    AsmDiag dummy;
    memset(&dummy, 0, sizeof(dummy));

    int n = asm_encode(mnemonic, toks, ops, nops, cur_addr, syms, buf, &dummy);
    if (n > 0) return n;

    /*
     * Encode failed (likely undefined forward reference).
     * Estimate based on mnemonic + operand forms — all SM83 lengths are
     * deterministic from the form alone:
     *   - 1 byte:  no operands, or operands that are all registers
     *   - 2 bytes: one immediate/mem operand (jr, ld r,d8, etc.)
     *   - 3 bytes: 16-bit immediate (ld rr,d16, jp, call, ld (a16),a, etc.)
     *
     * We encode with a dummy symbol table that returns 0 for everything.
     */

    /* Build a one-entry symbol table with a zero value for any symbol */
    AsmSymbolTable fake_st;
    AsmSymbol fake_sym;
    memset(&fake_sym, 0, sizeof(fake_sym));
    fake_sym.defined = true;
    /* We'll use the permissive lookup by pre-defining all referenced symbols
     * to 0.  Scan ops for any OPND_IMM tokens that are identifiers. */
    AsmSymbol *syms_arr = NULL;
    int nsyms_arr = 0;
    int cap_arr = 0;

    for (int i = 0; i < nops; i++) {
        const AsmOperand *op = &ops[i];
        int end = op->tok_start + op->tok_count;
        for (int j = op->tok_start; j < end; j++) {
            if (toks[j].kind == TOK_IDENT) {
                /* Check if already in fake table */
                bool found = false;
                for (int k = 0; k < nsyms_arr; k++) {
                    if (strncmp(syms_arr[k].name, toks[j].text, toks[j].len) == 0 &&
                        syms_arr[k].name[toks[j].len] == '\0') {
                        found = true; break;
                    }
                }
                if (!found) {
                    if (nsyms_arr >= cap_arr) {
                        int nc = cap_arr ? cap_arr * 2 : 8;
                        AsmSymbol *ns = realloc(syms_arr, (size_t)nc * sizeof(AsmSymbol));
                        if (!ns) {
                            free(syms_arr);
                            syms_arr = NULL;
                            goto fallback;
                        }
                        syms_arr = ns;
                        cap_arr  = nc;
                    }
                    memset(&syms_arr[nsyms_arr], 0, sizeof(AsmSymbol));
                    size_t nl = toks[j].len < 127 ? toks[j].len : 127;
                    memcpy(syms_arr[nsyms_arr].name, toks[j].text, nl);
                    syms_arr[nsyms_arr].name[nl] = '\0';
                    syms_arr[nsyms_arr].defined = true;
                    nsyms_arr++;
                }
            }
        }
    }
    fake_st.syms  = syms_arr;
    fake_st.count = nsyms_arr;
    fake_st.cap   = cap_arr;

    n = asm_encode(mnemonic, toks, ops, nops, cur_addr, &fake_st, buf, &dummy);
    free(syms_arr);
    syms_arr = NULL;
    if (n > 0) return n;

fallback:
    free(syms_arr);  /* NULL-safe */

    /*
     * Last resort: hard-coded length by mnemonic.
     * These are the fixed SM83 instruction sizes.
     */
    {
        char mn[16];
        size_t mi = 0;
        while (mnemonic[mi] && mi < sizeof(mn)-1) {
            mn[mi] = (char)tolower((unsigned char)mnemonic[mi]);
            mi++;
        }
        mn[mi] = '\0';

        /* 3-byte instructions */
        if (strcmp(mn, "jp") == 0 && nops >= 1) {
            /* jp (hl) is 1 byte */
            if (ops[0].form == OPND_MEM) return 1;
            return 3;
        }
        if (strcmp(mn, "call") == 0)  return 3;
        if (strcmp(mn, "ld") == 0 && nops == 2) {
            /* ld rr, d16 -> 3; ld r, d8 -> 2; ld r, r -> 1 */
            if (ops[1].form == OPND_IMM) {
                /* Check if dst is 16-bit reg */
                if (ops[0].form == OPND_REG) {
                    const AsmToken *rt = &toks[ops[0].tok_start];
                    if (tok_eq_str(rt, "bc") || tok_eq_str(rt, "de") ||
                        tok_eq_str(rt, "hl") || tok_eq_str(rt, "sp"))
                        return 3;
                }
                return 2;
            }
            if (ops[0].form == OPND_MEM || ops[1].form == OPND_MEM) return 3;
            return 2;
        }
        /* jr is always 2 */
        if (strcmp(mn, "jr") == 0) return 2;
        /* push/pop: 1 */
        if (strcmp(mn, "push") == 0 || strcmp(mn, "pop") == 0) return 1;
        /* All others with no or register-only operands: 1 byte typically */
        return 1;
    }
}

/* helper: case-insensitive token text comparison */
static bool tok_eq_str(const AsmToken *t, const char *s)
{
    size_t sl = strlen(s);
    if (t->len != sl) return false;
    for (size_t i = 0; i < sl; i++)
        if (tolower((unsigned char)t->text[i]) != tolower((unsigned char)s[i]))
            return false;
    return true;
}

/* -------------------------------------------------------------------------
 * DS argument parsing: ds count [, fill]
 * ----------------------------------------------------------------------- */

static int ds_count(const AsmToken *toks, int start, int count,
                    const AsmSymbolTable *syms, uint16_t cur_addr,
                    int *out_fill)
{
    *out_fill = 0;
    if (count == 0) return 0;

    /* Find comma, if any */
    int comma_pos = -1;
    for (int i = start; i < start + count; i++) {
        if (toks[i].kind == TOK_PUNCT &&
            toks[i].len == 1 && toks[i].text[0] == ',') {
            comma_pos = i;
            break;
        }
    }

    int cnt_end = (comma_pos >= 0) ? comma_pos : start + count;
    long cnt_val = 0;
    AsmDiag dummy; memset(&dummy, 0, sizeof(dummy));
    asm_eval_expr(toks, start, cnt_end - start, syms, cur_addr, &cnt_val, &dummy);

    if (comma_pos >= 0) {
        long fill_val = 0;
        asm_eval_expr(toks, comma_pos + 1,
                      (start + count) - (comma_pos + 1),
                      syms, cur_addr, &fill_val, &dummy);
        *out_fill = (int)(fill_val & 0xFF);
    }

    return (int)cnt_val;
}

/* -------------------------------------------------------------------------
 * Linemap helper
 * ----------------------------------------------------------------------- */

static void linemap_push(AsmResult *r, int line, uint32_t off)
{
    int n = r->nlines;
    void *p = realloc(r->linemap, (size_t)(n + 1) * sizeof(*r->linemap));
    if (!p) return;
    r->linemap = p;
    r->linemap[n].line = line;
    r->linemap[n].off  = off;
    r->nlines = n + 1;
}

/* -------------------------------------------------------------------------
 * SECTION directive argument parser
 *
 * Syntax: "name", ROM0  or  "name", ROMX, BANK[n]
 * For Task 4 we only support ROM0 (bank 0); ROMX is stubbed at bank 1+.
 * Returns true and sets *bank, *org if the section header was parsed.
 * ----------------------------------------------------------------------- */

static bool parse_section_args(const AsmToken *toks, int start, int count,
                                int *bank, uint16_t *org)
{
    *bank = 0;
    *org  = 0x0000;

    int end = start + count;
    int i   = start;

    /* Skip string name */
    if (i >= end) return false;
    if (toks[i].kind == TOK_STRING) i++;
    /* Consume comma after name */
    if (i < end && toks[i].kind == TOK_PUNCT &&
        toks[i].text[0] == ',') i++;

    /* Section type */
    if (i >= end || toks[i].kind != TOK_IDENT) return true; /* bare ROM0 default */

    /* case-insensitive check for ROM0 / ROMX */
    const AsmToken *type_tok = &toks[i];
    bool is_romx = false;
    if (type_tok->len == 4 &&
        tolower((unsigned char)type_tok->text[0]) == 'r' &&
        tolower((unsigned char)type_tok->text[1]) == 'o' &&
        tolower((unsigned char)type_tok->text[2]) == 'm' &&
        tolower((unsigned char)type_tok->text[3]) == 'x') {
        is_romx = true;
    }
    i++;

    /* Optional BANK[n] */
    if (is_romx && i < end) {
        /* skip comma */
        if (toks[i].kind == TOK_PUNCT && toks[i].text[0] == ',') i++;
        /* BANK[n] — look for BANK ident followed by '[' num ']' */
        if (i < end && toks[i].kind == TOK_IDENT) {
            i++; /* skip BANK */
            if (i < end && toks[i].kind == TOK_PUNCT && toks[i].text[0] == '[') {
                i++;
                if (i < end && toks[i].kind == TOK_NUMBER) {
                    *bank = (int)toks[i].value;
                    i++;
                }
                /* skip ']' */
                if (i < end && toks[i].kind == TOK_PUNCT && toks[i].text[0] == ']')
                    i++;
            }
        }
        if (*bank == 0) *bank = 1; /* default ROMX bank 1 */
        /* ROMX starts at cpu addr 0x4000 */
        *org = 0x4000;
    }

    return true;
}

/* Linear ROM offset from bank + cpu_addr */
static uint32_t linear_off(int bank, uint16_t addr)
{
    if (bank == 0) return (uint32_t)addr;
    return (uint32_t)bank * 0x4000u + ((uint32_t)addr - 0x4000u);
}

/* -------------------------------------------------------------------------
 * EQU directive: evaluate and define constant symbol
 * ----------------------------------------------------------------------- */

static void handle_equ(const AsmToken *toks, const AsmStmt *s,
                       AsmSymbolTable *st, AsmResult *r)
{
    /* Name token */
    if (s->dir.name_count < 1) {
        push_diag(r, s->line, "EQU: missing symbol name");
        return;
    }
    const AsmToken *name_tok = &toks[s->dir.name_start];
    char name[128];
    size_t nl = name_tok->len < 127 ? name_tok->len : 127;
    memcpy(name, name_tok->text, nl);
    name[nl] = '\0';

    long val = 0;
    AsmDiag dummy; memset(&dummy, 0, sizeof(dummy));
    if (!asm_eval_expr(toks, s->dir.args_start, s->dir.args_count,
                       st, 0, &val, &dummy)) {
        char msg[200];
        snprintf(msg, sizeof(msg), "EQU '%s': %s", name, dummy.msg);
        push_diag(r, s->line, msg);
        return;
    }

    sym_define(st, name, -1, 0, 0, val, true);
}

/* -------------------------------------------------------------------------
 * DB emit (pass 2)
 * ----------------------------------------------------------------------- */

static int emit_db(const AsmToken *toks, int start, int count,
                   const AsmSymbolTable *syms, uint16_t cur_addr,
                   uint8_t *rom, uint32_t rom_off,
                   int32_t *prov_line, int32_t src_line,
                   AsmResult *r)
{
    int end    = start + count;
    int i      = start;
    int emitted = 0;

    while (i < end) {
        const AsmToken *t = &toks[i];

        if (t->kind == TOK_STRING) {
            for (size_t ci = 0; ci < t->len; ci++) {
                rom[rom_off + (uint32_t)emitted] = (uint8_t)t->text[ci];
                if (prov_line) prov_line[rom_off + (uint32_t)emitted] = src_line;
                emitted++;
            }
            i++;
        } else if (t->kind == TOK_PUNCT && t->len == 1 && t->text[0] == ',') {
            i++;
        } else {
            /* expression: consume until next comma */
            int expr_start = i;
            while (i < end) {
                if (toks[i].kind == TOK_PUNCT &&
                    toks[i].len == 1 && toks[i].text[0] == ',')
                    break;
                i++;
            }
            long val = 0;
            AsmDiag err; memset(&err, 0, sizeof(err));
            if (!asm_eval_expr(toks, expr_start, i - expr_start,
                               syms, cur_addr, &val, &err)) {
                char msg[200];
                snprintf(msg, sizeof(msg), "DB: %s", err.msg);
                push_diag(r, src_line, msg);
                rom[rom_off + (uint32_t)emitted] = 0;
            } else {
                rom[rom_off + (uint32_t)emitted] = (uint8_t)(val & 0xFF);
            }
            if (prov_line) prov_line[rom_off + (uint32_t)emitted] = src_line;
            emitted++;
        }
    }
    return emitted;
}

/* -------------------------------------------------------------------------
 * DW emit (pass 2)
 * ----------------------------------------------------------------------- */

static int emit_dw(const AsmToken *toks, int start, int count,
                   const AsmSymbolTable *syms, uint16_t cur_addr,
                   uint8_t *rom, uint32_t rom_off,
                   int32_t *prov_line, int32_t src_line,
                   AsmResult *r)
{
    int end     = start + count;
    int i       = start;
    int emitted = 0;

    while (i < end) {
        const AsmToken *t = &toks[i];
        if (t->kind == TOK_PUNCT && t->len == 1 && t->text[0] == ',') {
            i++; continue;
        }
        /* consume expression */
        int expr_start = i;
        while (i < end) {
            if (toks[i].kind == TOK_PUNCT &&
                toks[i].len == 1 && toks[i].text[0] == ',')
                break;
            i++;
        }
        long val = 0;
        AsmDiag err; memset(&err, 0, sizeof(err));
        if (!asm_eval_expr(toks, expr_start, i - expr_start,
                           syms, cur_addr, &val, &err)) {
            char msg[200];
            snprintf(msg, sizeof(msg), "DW: %s", err.msg);
            push_diag(r, src_line, msg);
            val = 0;
        }
        rom[rom_off + (uint32_t)emitted]     = (uint8_t)(val & 0xFF);
        rom[rom_off + (uint32_t)emitted + 1] = (uint8_t)((val >> 8) & 0xFF);
        if (prov_line) {
            prov_line[rom_off + (uint32_t)emitted]     = src_line;
            prov_line[rom_off + (uint32_t)emitted + 1] = src_line;
        }
        emitted += 2;
    }
    return emitted;
}

/* -------------------------------------------------------------------------
 * Local-label name resolution
 *
 * Local labels (.foo) are scoped to the last global label.  We expand them
 * to "GlobalLabel.foo" during pass 1 & 2.
 * ----------------------------------------------------------------------- */

static void expand_local(char *out, size_t outsz,
                          const char *local_name, const char *global_ctx)
{
    if (global_ctx && global_ctx[0]) {
        snprintf(out, outsz, "%s%s", global_ctx, local_name);
    } else {
        snprintf(out, outsz, "%s", local_name);
    }
}

/* -------------------------------------------------------------------------
 * Public: asm_assemble
 * ----------------------------------------------------------------------- */

AsmResult asm_assemble(const char *src, const char *filename)
{
    AsmResult r;
    memset(&r, 0, sizeof(r));
    r.ok = true;

    /* ------------------------------------------------------------------
     * Lex
     * ------------------------------------------------------------------ */
    AsmDiag *lex_diags  = NULL;
    int      lex_ndiags = 0;
    int      tok_count  = 0;

    AsmToken *toks = asm_lex(src, filename, &tok_count, &lex_diags, &lex_ndiags);
    if (!toks) {
        push_diag(&r, 0, "lex allocation failure");
        return r;
    }
    /* Absorb lex diagnostics */
    for (int i = 0; i < lex_ndiags; i++) {
        push_diag(&r, lex_diags[i].line, lex_diags[i].msg);
    }
    free(lex_diags);

    /* ------------------------------------------------------------------
     * Parse
     * ------------------------------------------------------------------ */
    AsmDiag *parse_diags  = NULL;
    int      parse_ndiags = 0;
    int      nstmts       = 0;

    AsmStmt *stmts = asm_parse(toks, tok_count, &nstmts,
                                &parse_diags, &parse_ndiags);
    for (int i = 0; i < parse_ndiags; i++) {
        push_diag(&r, parse_diags[i].line, parse_diags[i].msg);
    }
    free(parse_diags);

    if (!stmts && nstmts > 0) {
        push_diag(&r, 0, "parse allocation failure");
        free(toks);
        return r;
    }

    /* ------------------------------------------------------------------
     * Symbol table
     * ------------------------------------------------------------------ */
    AsmSymbolTable st;
    memset(&st, 0, sizeof(st));

    /* ------------------------------------------------------------------
     * Pass 1: assign addresses
     * ------------------------------------------------------------------ */

    Section sec;
    sec.bank     = 0;
    sec.cur_addr = DEFAULT_ORG;
    sec.cur_off  = DEFAULT_ORG;

    char global_ctx[128] = "";  /* last global label name */

    /* Per-statement offset table (pass 1 records these for pass 2) */
    uint32_t *stmt_off  = calloc((size_t)(nstmts > 0 ? nstmts : 1), sizeof(uint32_t));
    uint16_t *stmt_addr = calloc((size_t)(nstmts > 0 ? nstmts : 1), sizeof(uint16_t));
    int      *stmt_bank = calloc((size_t)(nstmts > 0 ? nstmts : 1), sizeof(int));
    if (!stmt_off || !stmt_addr || !stmt_bank) {
        push_diag(&r, 0, "allocation failure in pass 1");
        goto done;
    }

    for (int i = 0; i < nstmts; i++) {
        const AsmStmt *s = &stmts[i];

        stmt_off[i]  = sec.cur_off;
        stmt_addr[i] = sec.cur_addr;
        stmt_bank[i] = sec.bank;

        switch (s->kind) {

        case ST_LABEL: {
            char full_name[256];
            if (s->label.is_local) {
                expand_local(full_name, sizeof(full_name),
                             s->label.name, global_ctx);
            } else {
                snprintf(full_name, sizeof(full_name), "%s", s->label.name);
                snprintf(global_ctx, sizeof(global_ctx), "%s", s->label.name);
            }
            sym_define(&st, full_name, sec.bank, sec.cur_addr,
                       sec.cur_off, (long)sec.cur_addr, false);
            break;
        }

        case ST_INSTR: {
            int len = estimate_instr_len(s->instr.mnemonic, toks,
                                         s->instr.ops, s->instr.nops,
                                         sec.cur_addr, &st);
            if (len < 1) len = 1;
            section_advance(&sec, len);
            break;
        }

        case ST_DIRECTIVE:
            switch (s->dir.kind) {

            case DIR_SECTION: {
                int    new_bank = 0;
                uint16_t new_org = 0x0000;
                parse_section_args(toks, s->dir.args_start, s->dir.args_count,
                                   &new_bank, &new_org);
                sec.bank     = new_bank;
                sec.cur_addr = new_org;
                sec.cur_off  = linear_off(new_bank, new_org);
                /* Update recorded offset for this statement itself */
                stmt_off[i]  = sec.cur_off;
                stmt_addr[i] = sec.cur_addr;
                stmt_bank[i] = sec.bank;
                global_ctx[0] = '\0';
                break;
            }

            case DIR_EQU:
                handle_equ(toks, s, &st, &r);
                break;

            case DIR_DB: {
                int sz = db_size(toks, s->dir.args_start, s->dir.args_count);
                section_advance(&sec, sz);
                break;
            }

            case DIR_DW: {
                /* Count commas+1 elements; each is 2 bytes */
                int nelems = 1;
                int end = s->dir.args_start + s->dir.args_count;
                for (int j = s->dir.args_start; j < end; j++) {
                    if (toks[j].kind == TOK_PUNCT &&
                        toks[j].text[0] == ',') nelems++;
                }
                section_advance(&sec, nelems * 2);
                break;
            }

            case DIR_DS: {
                int fill = 0;
                int cnt  = ds_count(toks,
                                    s->dir.args_start, s->dir.args_count,
                                    &st, sec.cur_addr, &fill);
                if (cnt > 0) section_advance(&sec, cnt);
                break;
            }

            case DIR_INCBIN:
            case DIR_INCLUDE:
                /* Task 5: stubs */
                break;
            }
            break;
        }
    }

    /* Compute symbol sizes: distance to next label in same section */
    for (int i = 0; i < st.count; i++) {
        if (st.syms[i].bank < 0) continue; /* constant */
        uint32_t next_off = sec.cur_off;    /* end of section */
        for (int j = 0; j < st.count; j++) {
            if (i == j) continue;
            if (st.syms[j].bank == st.syms[i].bank &&
                st.syms[j].off > st.syms[i].off &&
                st.syms[j].off < next_off) {
                next_off = st.syms[j].off;
            }
        }
        st.syms[i].size = (int)(next_off - st.syms[i].off);
    }

    /* ------------------------------------------------------------------
     * Allocate ROM image
     * ------------------------------------------------------------------ */
    size_t rom_size = sec.cur_off > ROM_MIN_SIZE ? sec.cur_off : ROM_MIN_SIZE;
    /* Round up to power of two >= 0x8000 (32K, 64K, …) */
    {
        size_t sz = ROM_MIN_SIZE;
        while (sz < rom_size) sz <<= 1;
        rom_size = sz;
    }

    r.rom      = calloc(1, rom_size);
    r.prov_line = malloc(rom_size * sizeof(int32_t));
    if (!r.rom || !r.prov_line) {
        push_diag(&r, 0, "ROM allocation failure");
        goto done;
    }
    r.rom_size = rom_size;
    memset(r.prov_line, 0xFF, rom_size * sizeof(int32_t)); /* -1 = unknown */

    /* ------------------------------------------------------------------
     * Pass 2: encode bytes
     * ------------------------------------------------------------------ */

    /* Reset context for second pass */
    sec.bank     = 0;
    sec.cur_addr = DEFAULT_ORG;
    sec.cur_off  = DEFAULT_ORG;
    global_ctx[0] = '\0';

    for (int i = 0; i < nstmts; i++) {
        const AsmStmt *s = &stmts[i];

        /* Restore section state from pass 1 */
        sec.cur_off  = stmt_off[i];
        sec.cur_addr = stmt_addr[i];
        sec.bank     = stmt_bank[i];

        switch (s->kind) {

        case ST_LABEL:
            /* Update global context so local-label resolution works */
            if (!s->label.is_local)
                snprintf(global_ctx, sizeof(global_ctx), "%s", s->label.name);
            break;

        case ST_INSTR: {
            uint8_t buf[4];
            AsmDiag err; memset(&err, 0, sizeof(err));

            /*
             * Inject local-label aliases into the symbol table for this
             * instruction's scope.  For each `.foo` token in the operands,
             * look up `global_ctx.foo` and add `.foo` as an alias.
             * We remove these aliases after encoding.
             */
            int injected_start = st.count;
            if (global_ctx[0]) {
                for (int oi = 0; oi < s->instr.nops; oi++) {
                    const AsmOperand *op = &s->instr.ops[oi];
                    int opend = op->tok_start + op->tok_count;
                    for (int ti = op->tok_start; ti < opend; ti++) {
                        const AsmToken *tt = &toks[ti];
                        if (tt->kind == TOK_IDENT && tt->len > 1 &&
                            tt->text[0] == '.') {
                            /* Get short name (e.g. ".loop") */
                            char short_name[128];
                            size_t slen = tt->len < 127 ? tt->len : 127;
                            memcpy(short_name, tt->text, slen);
                            short_name[slen] = '\0';
                            /* Skip if already defined (could be a label alias) */
                            if (asm_sym_lookup(&st, short_name)) continue;
                            /* Build expanded name */
                            char full_name[256];
                            snprintf(full_name, sizeof(full_name), "%s%s",
                                     global_ctx, short_name);
                            const AsmSymbol *full = asm_sym_lookup(&st, full_name);
                            if (full) {
                                /* Inject alias */
                                sym_define(&st, short_name,
                                           full->bank, full->addr, full->off,
                                           full->value, (full->bank < 0));
                            }
                        }
                    }
                }
            }

            int n = asm_encode(s->instr.mnemonic, toks,
                               s->instr.ops, s->instr.nops,
                               sec.cur_addr, &st, buf, &err);

            /* Remove injected aliases (trim symbol table back) */
            st.count = injected_start;
            if (n < 0) {
                char msg[220];
                snprintf(msg, sizeof(msg), "encode '%s': %s",
                         s->instr.mnemonic, err.msg);
                push_diag(&r, s->line, msg);
                n = 1; buf[0] = 0x00;
            }
            if (sec.cur_off + (uint32_t)n <= r.rom_size) {
                memcpy(&r.rom[sec.cur_off], buf, (size_t)n);
                for (int k = 0; k < n; k++)
                    r.prov_line[sec.cur_off + (uint32_t)k] = s->line;
                linemap_push(&r, s->line, sec.cur_off);
            }
            break;
        }

        case ST_DIRECTIVE:
            switch (s->dir.kind) {

            case DIR_SECTION: {
                int new_bank = 0; uint16_t new_org = 0x0000;
                parse_section_args(toks, s->dir.args_start, s->dir.args_count,
                                   &new_bank, &new_org);
                sec.bank     = new_bank;
                sec.cur_addr = new_org;
                sec.cur_off  = linear_off(new_bank, new_org);
                global_ctx[0] = '\0';
                break;
            }

            case DIR_EQU:
                /* Already handled in pass 1 */
                break;

            case DIR_DB: {
                int n = emit_db(toks,
                                s->dir.args_start, s->dir.args_count,
                                &st, sec.cur_addr,
                                r.rom, sec.cur_off,
                                r.prov_line, (int32_t)s->line,
                                &r);
                if (n > 0) linemap_push(&r, s->line, sec.cur_off);
                break;
            }

            case DIR_DW: {
                int n = emit_dw(toks,
                                s->dir.args_start, s->dir.args_count,
                                &st, sec.cur_addr,
                                r.rom, sec.cur_off,
                                r.prov_line, (int32_t)s->line,
                                &r);
                if (n > 0) linemap_push(&r, s->line, sec.cur_off);
                break;
            }

            case DIR_DS: {
                int fill = 0;
                int cnt  = ds_count(toks,
                                    s->dir.args_start, s->dir.args_count,
                                    &st, sec.cur_addr, &fill);
                if (cnt > 0 && sec.cur_off + (uint32_t)cnt <= r.rom_size) {
                    memset(&r.rom[sec.cur_off], fill, (size_t)cnt);
                    for (int k = 0; k < cnt; k++)
                        r.prov_line[sec.cur_off + (uint32_t)k] = s->line;
                    linemap_push(&r, s->line, sec.cur_off);
                }
                break;
            }

            case DIR_INCBIN:
            case DIR_INCLUDE:
                break;
            }
            break;
        }
    }

    /* ------------------------------------------------------------------
     * Copy symbol table into result
     * ------------------------------------------------------------------ */
    if (st.count > 0) {
        r.syms  = malloc((size_t)st.count * sizeof(AsmSymbol));
        if (r.syms) {
            memcpy(r.syms, st.syms, (size_t)st.count * sizeof(AsmSymbol));
            r.nsyms = st.count;
        }
    }

done:
    free(stmt_off);
    free(stmt_addr);
    free(stmt_bank);
    free(st.syms);
    free(stmts);
    free(toks);

    return r;
}

/* -------------------------------------------------------------------------
 * Public: asm_free
 * ----------------------------------------------------------------------- */

void asm_free(AsmResult *r)
{
    if (!r) return;
    free(r->rom);
    free(r->syms);
    free(r->linemap);
    free(r->prov_line);
    free(r->diags);
    memset(r, 0, sizeof(*r));
}
