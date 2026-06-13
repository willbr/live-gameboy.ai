/*
 * assemble.c — Two-pass SM83 assembler driver
 *
 * Pipeline:
 *   asm_lex()    -> token array
 *   asm_parse()  -> AsmStmt array
 *   pass 1       -> assign addresses, build symbol table
 *   pass 2       -> encode bytes, fill ROM + build database
 *
 * Section model:
 *   ROM0  — bank 0, linear offset == cpu_addr  (0x0000..0x3FFF)
 *   ROMX  — bank n>=1, cpu addr 0x4000-0x7FFF, linear offset = n*0x4000 + (addr-0x4000)
 *   Default (no SECTION) — ROM0 starting at 0x0150 (above header region)
 *
 * Cartridge header (Task 5):
 *   Written after assembly into the ROM image.
 *   0x0104-0x0133: Nintendo logo (48 bytes)
 *   0x0134-0x0143: title (ASCII, zero-padded)
 *   0x0147: cartridge type (0x00 ROM-only, 0x01 MBC1 if any ROMX bank used)
 *   0x0148: ROM size code (log2(rom_size / 32KB))
 *   0x0149: RAM size (0x00)
 *   0x014D: header checksum (x=0; for 0x0134..0x014C: x=x-byte-1; result & 0xFF)
 *   0x014E-0x014F: global checksum (16-bit big-endian sum of all bytes except 0x014E/0x014F)
 *
 * Provenance sentinels (prov_line array):
 *   >= 1   : 1-based source line number
 *   -1     : no provenance (uninitialised or padding)
 *   -2     : INCBIN asset byte (not from source text)
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

/* Provenance sentinels for prov_line[] */
#define PROV_NONE     (-1)   /* uninitialised / padding */
#define PROV_ASSET    (-2)   /* INCBIN asset byte */

/* Cartridge header layout */
#define HDR_LOGO_ADDR  0x0104u
#define HDR_TITLE_ADDR 0x0134u
#define HDR_TYPE_ADDR  0x0147u
#define HDR_ROMSIZE_ADDR 0x0148u
#define HDR_RAMSIZE_ADDR 0x0149u
#define HDR_HCHK_ADDR  0x014Du
#define HDR_GCHK_ADDR  0x014Eu

/* The canonical 48-byte Nintendo logo (verified against Pan Docs) */
static const uint8_t NINTENDO_LOGO[48] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
    0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E
};

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

static void refsite_push(AsmResult *r, uint32_t off, const char *sym, long addend)
{
    int n = r->nrefs;
    void *p = realloc(r->refs, (size_t)(n + 1) * sizeof(*r->refs));
    if (!p) return;
    r->refs = p;
    AsmRefSite *rs = &r->refs[n];
    rs->off    = off;
    rs->addend = addend;
    rs->size   = 2;
    size_t slen = strlen(sym);
    if (slen >= sizeof(rs->sym)) slen = sizeof(rs->sym) - 1;
    memcpy(rs->sym, sym, slen);
    rs->sym[slen] = '\0';
    r->nrefs = n + 1;
}

/*
 * expr_single_sym — inspect a token span [start, start+count) and determine
 * whether it is a single-symbol reference with an optional constant addend.
 *
 * Accepted forms:
 *   SYM            -> sym_out = "SYM", addend_out = 0
 *   SYM + constant -> sym_out = "SYM", addend_out = +constant
 *   SYM - constant -> sym_out = "SYM", addend_out = -constant
 *
 * Returns true only if exactly one IDENT token that names a defined symbol
 * (from `st`) is present, and the expression is of the above form.
 * Returns false for pure constants or multi-symbol expressions.
 */
static bool expr_single_sym(const AsmToken *toks, int start, int count,
                             const AsmSymbolTable *st,
                             char *sym_out, size_t sym_out_sz,
                             long *addend_out)
{
    int end = start + count;

    /* Count identifiers that are defined non-constant symbols */
    int sym_count = 0;
    int sym_idx   = -1;
    for (int i = start; i < end; i++) {
        if (toks[i].kind == TOK_IDENT) {
            /* Check if this ident is a register name or keyword — skip those */
            const char *text = toks[i].text;
            size_t      len  = toks[i].len;
            /* Common register names to skip */
            static const char *regs[] = {
                "a","b","c","d","e","h","l",
                "bc","de","hl","sp","af",
                "nz","z","nc",
                "hli","hld","hl+","hl-",
                "low","high","bank",
                NULL
            };
            bool is_reg = false;
            for (int r = 0; regs[r]; r++) {
                size_t rl = strlen(regs[r]);
                if (len == rl) {
                    bool eq = true;
                    for (size_t k = 0; k < rl && eq; k++)
                        eq = (tolower((unsigned char)text[k]) ==
                              tolower((unsigned char)regs[r][k]));
                    if (eq) { is_reg = true; break; }
                }
            }
            if (is_reg) continue;

            /* Look up in symbol table */
            char name[128];
            size_t nl = len < 127 ? len : 127;
            memcpy(name, text, nl);
            name[nl] = '\0';
            const AsmSymbol *s = asm_sym_lookup(st, name);
            if (!s || !s->defined || s->bank < 0) continue; /* not a code/data sym */

            sym_count++;
            sym_idx = i;
        }
    }

    if (sym_count != 1) return false;

    /* Extract the symbol name */
    const AsmToken *st_tok = &toks[sym_idx];
    size_t nl = st_tok->len < sym_out_sz - 1 ? st_tok->len : sym_out_sz - 1;
    memcpy(sym_out, st_tok->text, nl);
    sym_out[nl] = '\0';

    /* Check expression shape: must be SYM, SYM+N, or SYM-N.
     * Count non-comma, non-whitespace tokens (excluding the sym itself). */
    int non_sym = 0;
    for (int i = start; i < end; i++) {
        if (i == sym_idx) continue;
        if (toks[i].kind == TOK_PUNCT &&
            toks[i].len == 1 &&
            (toks[i].text[0] == '+' || toks[i].text[0] == '-')) {
            non_sym++; continue;
        }
        if (toks[i].kind == TOK_NUMBER) {
            non_sym++; continue;
        }
        /* Any other token: complex expression */
        return false;
    }

    /* Simple: just SYM */
    if (non_sym == 0) { *addend_out = 0; return true; }

    /* SYM +/- N: expect exactly 2 more tokens (sign + number) */
    if (non_sym == 2) {
        /* Find sign and number */
        int sign_idx = -1, num_idx = -1;
        for (int i = start; i < end; i++) {
            if (i == sym_idx) continue;
            if (toks[i].kind == TOK_PUNCT) sign_idx = i;
            if (toks[i].kind == TOK_NUMBER) num_idx = i;
        }
        if (sign_idx < 0 || num_idx < 0) return false;
        long addend = toks[num_idx].value;
        if (toks[sign_idx].text[0] == '-') addend = -addend;
        *addend_out = addend;
        return true;
    }

    return false;
}

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
 * INCLUDE helper: resolve a path relative to the including file's directory.
 * Returns a heap-allocated absolute/relative path string; caller frees.
 * ----------------------------------------------------------------------- */
static char *resolve_include_path(const char *inc_path, const char *parent_file)
{
    /* If inc_path is absolute or parent_file has no directory, use as-is */
    if (inc_path[0] == '/' || !parent_file) {
        return strdup(inc_path);
    }
    const char *slash = strrchr(parent_file, '/');
    if (!slash) {
        return strdup(inc_path);
    }
    size_t dirlen = (size_t)(slash - parent_file + 1);
    size_t inclen = strlen(inc_path);
    char *out = malloc(dirlen + inclen + 1);
    if (!out) return NULL;
    memcpy(out, parent_file, dirlen);
    memcpy(out + dirlen, inc_path, inclen);
    out[dirlen + inclen] = '\0';
    return out;
}

/* -------------------------------------------------------------------------
 * INCLUDE pre-expansion context:
 * We maintain a flat merged token array and statement array that includes
 * both the root source and all included files.  Token indices in statements
 * reference the merged token array.
 *
 * expand_includes() walks the statement list; whenever it finds a DIR_INCLUDE
 * statement it reads the file, lexes + parses it (with token offsets adjusted
 * to point into a new segment of the merged token array), and splices those
 * statements in-place.
 * ----------------------------------------------------------------------- */

/*
 * Merge an included file's tokens into the master token array, adjusting
 * all token indices in the included statements by `tok_base`.
 */
static bool splice_include(
    const char *inc_filename,
    AsmToken  **master_toks, int *master_tok_count,
    AsmStmt   **stmts,       int *nstmts,
    int         insert_pos,                   /* replace statement at this index */
    AsmResult  *r)
{
    /* Read the file */
    FILE *fh = fopen(inc_filename, "rb");
    if (!fh) {
        char msg[400];
        snprintf(msg, sizeof(msg), "INCLUDE: cannot open '%s'", inc_filename);
        push_diag(r, 0, msg);
        return false;
    }
    fseek(fh, 0, SEEK_END);
    long fsz = ftell(fh);
    rewind(fh);
    char *inc_src = NULL;
    if (fsz > 0) {
        inc_src = malloc((size_t)fsz + 1);
        if (!inc_src) { fclose(fh); push_diag(r, 0, "INCLUDE: alloc fail"); return false; }
        if (fread(inc_src, 1, (size_t)fsz, fh) != (size_t)fsz) {
            fclose(fh); free(inc_src);
            push_diag(r, 0, "INCLUDE: read fail"); return false;
        }
        inc_src[fsz] = '\0';
    } else {
        inc_src = strdup("");
    }
    fclose(fh);

    /* Lex the included file */
    AsmDiag *ldiags = NULL; int lndiags = 0; int ltok_count = 0;
    AsmToken *ltoks = asm_lex(inc_src, inc_filename, &ltok_count, &ldiags, &lndiags);
    /* NOTE: do NOT free inc_src here — ltoks' .text pointers point into it.
     * inc_src must remain alive until after asm_parse() is done, because
     * asm_parse reads token .text to identify mnemonics, labels, etc. */
    for (int i = 0; i < lndiags; i++) push_diag(r, ldiags[i].line, ldiags[i].msg);
    free(ldiags);
    if (!ltoks) { free(inc_src); push_diag(r, 0, "INCLUDE: lex fail"); return false; }

    /* Parse included tokens — must happen before freeing inc_src */
    AsmDiag *pdiags = NULL; int pndiags = 0; int inc_nstmts = 0;
    AsmStmt *inc_stmts = asm_parse(ltoks, ltok_count, &inc_nstmts, &pdiags, &pndiags);
    /* Now we can safely free the source buffer (all data we need is in inc_stmts char arrays) */
    free(inc_src);
    for (int i = 0; i < pndiags; i++) push_diag(r, pdiags[i].line, pdiags[i].msg);
    free(pdiags);
    if (!inc_stmts && inc_nstmts > 0) {
        free(ltoks);
        push_diag(r, 0, "INCLUDE: parse fail");
        return false;
    }

    /*
     * The included tokens (ltoks) need to be appended to the master token
     * array.  All token-span indices in inc_stmts must be shifted by
     * the current master_tok_count (before appending ltoks).
     *
     * We skip the final TOK_EOF of ltoks (the master already has one).
     */
    int inc_real_toks = ltok_count - 1; /* exclude TOK_EOF */
    if (inc_real_toks < 0) inc_real_toks = 0;

    int tok_base = *master_tok_count - 1; /* master TOK_EOF will be overwritten */

    /* Grow master token array: replace the trailing TOK_EOF with inc tokens + new EOF */
    int new_tok_count = *master_tok_count + inc_real_toks; /* keeps one EOF at end */
    AsmToken *new_toks = realloc(*master_toks, (size_t)new_tok_count * sizeof(AsmToken));
    if (!new_toks) {
        free(ltoks); free(inc_stmts);
        push_diag(r, 0, "INCLUDE: token merge alloc fail");
        return false;
    }
    *master_toks = new_toks;
    /* Shift old EOF to end; insert inc_real_toks tokens before it */
    /* Overwrite the old EOF at position (master_tok_count-1) with included tokens */
    memcpy(&new_toks[tok_base], ltoks, (size_t)inc_real_toks * sizeof(AsmToken));
    /* Copy the included EOF token as the new master EOF */
    new_toks[tok_base + inc_real_toks] = ltoks[ltok_count - 1]; /* TOK_EOF */
    *master_tok_count = new_tok_count;
    free(ltoks);

    /* Adjust token indices in inc_stmts */
    for (int i = 0; i < inc_nstmts; i++) {
        AsmStmt *s = &inc_stmts[i];
        if (s->kind == ST_INSTR) {
            for (int oi = 0; oi < s->instr.nops; oi++) {
                s->instr.ops[oi].tok_start += tok_base;
            }
        } else if (s->kind == ST_DIRECTIVE) {
            s->dir.name_start  += tok_base;
            s->dir.args_start  += tok_base;
        }
    }

    /*
     * Splice inc_stmts into the master statement array at insert_pos
     * (replacing the DIR_INCLUDE statement).
     */
    int old_nstmts = *nstmts;
    int new_nstmts = old_nstmts - 1 + inc_nstmts; /* remove 1 INCLUDE, add inc_nstmts */
    AsmStmt *new_stmts;
    if (new_nstmts <= 0) {
        new_stmts = calloc(1, sizeof(AsmStmt));
    } else {
        new_stmts = malloc((size_t)new_nstmts * sizeof(AsmStmt));
    }
    if (!new_stmts) {
        free(inc_stmts);
        push_diag(r, 0, "INCLUDE: stmt splice alloc fail");
        return false;
    }

    /* Copy: [0..insert_pos-1], [inc_stmts], [insert_pos+1..old_nstmts-1] */
    if (insert_pos > 0)
        memcpy(new_stmts, *stmts, (size_t)insert_pos * sizeof(AsmStmt));
    if (inc_nstmts > 0)
        memcpy(&new_stmts[insert_pos], inc_stmts, (size_t)inc_nstmts * sizeof(AsmStmt));
    int tail = old_nstmts - insert_pos - 1;
    if (tail > 0)
        memcpy(&new_stmts[insert_pos + inc_nstmts], &(*stmts)[insert_pos + 1],
               (size_t)tail * sizeof(AsmStmt));


    free(*stmts);
    *stmts   = new_stmts;
    *nstmts  = new_nstmts;
    free(inc_stmts);
    return true;
}

/* -------------------------------------------------------------------------
 * Public: asm_assemble_mem
 * ----------------------------------------------------------------------- */

AsmResult asm_assemble_mem(const char *src, const char *filename,
                            AsmPlacementMem *inout_mem)
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
     * Pre-expand INCLUDE directives (splice inline before two passes)
     * ------------------------------------------------------------------ */
    {
        int i = 0;
        while (i < nstmts) {
            AsmStmt *s = &stmts[i];
            if (s->kind == ST_DIRECTIVE && s->dir.kind == DIR_INCLUDE) {
                if (s->dir.args_count < 1) { i++; continue; }
                const AsmToken *ft = &toks[s->dir.args_start];
                if (ft->kind != TOK_STRING) { i++; continue; }
                char inc_rel[512];
                size_t flen = ft->len < sizeof(inc_rel)-1 ? ft->len : sizeof(inc_rel)-1;
                memcpy(inc_rel, ft->text, flen);
                inc_rel[flen] = '\0';
                char *resolved = resolve_include_path(inc_rel, filename);
                if (!resolved) { i++; continue; }
                int inc_count_before = nstmts;
                bool ok = splice_include(resolved, &toks, &tok_count,
                                         &stmts, &nstmts, i, &r);
                free(resolved);
                if (!ok) { i++; continue; }
                int added = nstmts - inc_count_before + 1; /* +1 for removed INCLUDE */
                /* Re-scan from same position (included stmts might have nested INCLUDEs) */
                (void)added;
                /* Don't increment i — re-examine position i which now has new stmts */
            } else {
                i++;
            }
        }
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

            case DIR_INCBIN: {
                /*
                 * Pass 1: measure file size so we can advance the address.
                 * The filename is in the args token span as a TOK_STRING.
                 */
                if (s->dir.args_count < 1) break;
                const AsmToken *ft = &toks[s->dir.args_start];
                if (ft->kind != TOK_STRING) break;
                /* Build NUL-terminated filename (relative to cwd for now) */
                char fname[512];
                size_t flen = ft->len < sizeof(fname)-1 ? ft->len : sizeof(fname)-1;
                memcpy(fname, ft->text, flen);
                fname[flen] = '\0';
                FILE *fh = fopen(fname, "rb");
                if (!fh) {
                    /* Try relative to source file if filename provided */
                    if (filename) {
                        const char *slash = strrchr(filename, '/');
                        if (slash) {
                            size_t dirlen = (size_t)(slash - filename + 1);
                            if (dirlen + flen < sizeof(fname)) {
                                memmove(fname + dirlen, fname, flen + 1);
                                memcpy(fname, filename, dirlen);
                                fh = fopen(fname, "rb");
                            }
                        }
                    }
                }
                if (!fh) {
                    char msg[300];
                    snprintf(msg, sizeof(msg), "INCBIN: cannot open '%.*s'",
                             (int)ft->len, ft->text);
                    push_diag(&r, s->line, msg);
                    break;
                }
                fseek(fh, 0, SEEK_END);
                long fsz = ftell(fh);
                fclose(fh);
                if (fsz > 0) section_advance(&sec, (int)fsz);
                break;
            }

            case DIR_INCLUDE:
                /*
                 * Pass 1: INCLUDE is handled by pre-expanding at assembly time.
                 * The included statements are already spliced into the statement
                 * array by a preprocessing step, so this stub is intentionally
                 * empty in the pass-1 loop.  (See INCLUDE preprocessing below.)
                 */
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
     * Layout pass (between pass 1 and pass 2)
     *
     * layout_plan() assigns slot addresses to all global-label-delimited
     * functions and updates their symbol table entries (addr, off, value).
     * It also adjusts local labels that belong to each function.
     *
     * After layout, we must recompute stmt_off[] / stmt_addr[] to reflect
     * the new function addresses.  We do a "pass 1.5" that replays the
     * address-assignment logic but jumps to the assigned address whenever
     * it encounters a global label.
     * ------------------------------------------------------------------ */
    {
        /* Determine the section base and bank for layout.
         * Walk the statements to find the first SECTION directive; if none,
         * use the default ROM0 @ DEFAULT_ORG. */
        uint16_t layout_base = DEFAULT_ORG;
        int      layout_bank = 0;
        for (int i = 0; i < nstmts; i++) {
            if (stmts[i].kind == ST_DIRECTIVE &&
                stmts[i].dir.kind == DIR_SECTION) {
                parse_section_args(toks, stmts[i].dir.args_start,
                                   stmts[i].dir.args_count,
                                   &layout_bank, &layout_base);
                break;
            }
        }

        int np = 0;
        AsmPlacement *placements = layout_plan(st.syms, st.count,
                                                layout_base, layout_bank,
                                                inout_mem, &np);
        if (placements && np > 0) {
            /* Store placements in result */
            r.placements  = placements;
            r.nplacements = np;

            /* Pass 1.5: recompute stmt_off / stmt_addr using new symbol addresses.
             *
             * Rules:
             *  - Reset section state from the initial SECTION directive (or default).
             *  - When we see a global label, jump sec.cur_addr to sym->addr,
             *    sec.cur_off to sym->off (already updated by layout_plan).
             *  - All other statements advance by their size as before.
             *  - The recorded stmt_off[i]/stmt_addr[i] are set to sec.cur_{off,addr}
             *    at the START of each statement (same semantics as pass 1).
             */
            sec.bank     = 0;
            sec.cur_addr = DEFAULT_ORG;
            sec.cur_off  = DEFAULT_ORG;
            global_ctx[0] = '\0';

            for (int i = 0; i < nstmts; i++) {
                const AsmStmt *s = &stmts[i];

                switch (s->kind) {
                case ST_DIRECTIVE:
                    if (s->dir.kind == DIR_SECTION) {
                        int nb = 0; uint16_t no = 0;
                        parse_section_args(toks, s->dir.args_start,
                                           s->dir.args_count, &nb, &no);
                        sec.bank     = nb;
                        sec.cur_addr = no;
                        sec.cur_off  = linear_off(nb, no);
                        global_ctx[0] = '\0';
                    }
                    break;
                case ST_LABEL:
                    if (!s->label.is_local) {
                        /* Global label: jump to its (possibly new) address */
                        const AsmSymbol *sym = asm_sym_lookup(&st, s->label.name);
                        if (sym && sym->bank >= 0) {
                            sec.bank     = sym->bank;
                            sec.cur_addr = sym->addr;
                            sec.cur_off  = sym->off;
                        }
                        snprintf(global_ctx, sizeof(global_ctx), "%s", s->label.name);
                    } else {
                        /* Local label: look up the expanded name */
                        char full[256];
                        expand_local(full, sizeof(full), s->label.name, global_ctx);
                        const AsmSymbol *sym = asm_sym_lookup(&st, full);
                        if (sym && sym->bank >= 0) {
                            sec.bank     = sym->bank;
                            sec.cur_addr = sym->addr;
                            sec.cur_off  = sym->off;
                        }
                    }
                    break;
                default:
                    break;
                }

                stmt_off[i]  = sec.cur_off;
                stmt_addr[i] = sec.cur_addr;
                stmt_bank[i] = sec.bank;

                /* Advance for non-label statements */
                switch (s->kind) {
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
                    case DIR_DB: {
                        int sz = db_size(toks, s->dir.args_start, s->dir.args_count);
                        section_advance(&sec, sz);
                        break;
                    }
                    case DIR_DW: {
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
                    case DIR_INCBIN: {
                        if (s->dir.args_count < 1) break;
                        const AsmToken *ft = &toks[s->dir.args_start];
                        if (ft->kind != TOK_STRING) break;
                        char fname[512];
                        size_t flen2 = ft->len < sizeof(fname)-1 ? ft->len : sizeof(fname)-1;
                        memcpy(fname, ft->text, flen2);
                        fname[flen2] = '\0';
                        FILE *fh2 = fopen(fname, "rb");
                        if (!fh2 && filename) {
                            const char *slash2 = strrchr(filename, '/');
                            if (slash2) {
                                size_t dlen = (size_t)(slash2 - filename + 1);
                                if (dlen + flen2 < sizeof(fname)) {
                                    memmove(fname + dlen, fname, flen2 + 1);
                                    memcpy(fname, filename, dlen);
                                    fh2 = fopen(fname, "rb");
                                }
                            }
                        }
                        if (fh2) {
                            fseek(fh2, 0, SEEK_END);
                            long fsz2 = ftell(fh2);
                            fclose(fh2);
                            if (fsz2 > 0) section_advance(&sec, (int)fsz2);
                        }
                        break;
                    }
                    default:
                        break;
                    }
                    break;
                default:
                    break;
                }
            }

            /* After pass 1.5, sec.cur_off is the end of the last function slot.
             * We need this for ROM size calculation below. */
        }
        /* else: no functions or allocation failure — keep original stmt_off/addr */
    }

    /* ------------------------------------------------------------------
     * Allocate ROM image
     *
     * After layout, sec.cur_off may have changed.  We need to figure out
     * the maximum ROM offset across all placements.
     * ------------------------------------------------------------------ */

    /* Compute the maximum offset needed: scan all placements */
    {
        uint32_t max_off = sec.cur_off;  /* fallback: end of last statement */
        if (r.placements) {
            for (int pi = 0; pi < r.nplacements; pi++) {
                AsmPlacement *pl = &r.placements[pi];
                uint32_t pl_end = linear_off(pl->bank, pl->addr) + (uint32_t)pl->slot_size;
                if (pl_end > max_off) max_off = pl_end;
            }
        }
        sec.cur_off = max_off;
    }

    size_t rom_size = sec.cur_off > ROM_MIN_SIZE ? sec.cur_off : ROM_MIN_SIZE;
    /* Round up to power of two >= 0x8000 (32K, 64K, …) */
    {
        size_t sz = ROM_MIN_SIZE;
        while (sz < rom_size) sz <<= 1;
        rom_size = sz;
    }

    r.rom            = calloc(1, rom_size);
    r.prov_line      = malloc(rom_size * sizeof(int32_t));
    r.prov_asset     = malloc(rom_size * sizeof(int32_t));
    r.prov_asset_off = malloc(rom_size * sizeof(uint32_t));
    if (!r.rom || !r.prov_line || !r.prov_asset || !r.prov_asset_off) {
        push_diag(&r, 0, "ROM allocation failure");
        goto done;
    }
    r.rom_size = rom_size;
    memset(r.prov_line,  0xFF, rom_size * sizeof(int32_t));  /* -1 = unknown */
    memset(r.prov_asset, 0xFF, rom_size * sizeof(int32_t));  /* -1 = none */
    memset(r.prov_asset_off, 0, rom_size * sizeof(uint32_t));

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

            /*
             * Reference-site tracking: for 3-byte instructions whose last 2
             * bytes are a 16-bit absolute address operand, check whether that
             * operand references exactly one symbol. If so, record a refsite
             * at (sec.cur_off + 1) — the byte offset of the low address byte.
             *
             * Mnemonics that carry an absolute a16 operand:
             *   jp  a16          (nops==1, ops[0] is IMM)
             *   jp  cc, a16      (nops==2, ops[1] is IMM)
             *   call a16         (nops==1, ops[0] is IMM)
             *   call cc, a16     (nops==2, ops[1] is IMM)
             *   ld  rr, d16      (nops==2, ops[0] is REG rp, ops[1] is IMM)
             *
             * jr / jr cc use a RELATIVE displacement — not recorded.
             */
            if (n == 3 && sec.cur_off + 1 < r.rom_size) {
                /* Identify which operand holds the address */
                const AsmOperand *addr_op = NULL;
                char mn_lc[16] = {0};
                for (size_t mi = 0; s->instr.mnemonic[mi] && mi < sizeof(mn_lc)-1; mi++)
                    mn_lc[mi] = (char)tolower((unsigned char)s->instr.mnemonic[mi]);

                bool is_jp   = (strcmp(mn_lc, "jp")   == 0);
                bool is_call = (strcmp(mn_lc, "call") == 0);
                bool is_ld   = (strcmp(mn_lc, "ld")   == 0);

                if (is_jp || is_call) {
                    /* jp (hl) is 1 byte — already filtered by n==3.
                     * jp a16: ops[0] is IMM; jp cc,a16: ops[1] is IMM */
                    if (s->instr.nops == 1 && s->instr.ops[0].form == OPND_IMM)
                        addr_op = &s->instr.ops[0];
                    else if (s->instr.nops == 2 && s->instr.ops[1].form == OPND_IMM)
                        addr_op = &s->instr.ops[1];
                } else if (is_ld && s->instr.nops == 2 &&
                           s->instr.ops[0].form == OPND_REG &&
                           s->instr.ops[1].form == OPND_IMM) {
                    /* ld rr, d16: verify dst is a 16-bit register pair */
                    const AsmToken *dt = &toks[s->instr.ops[0].tok_start];
                    if (tok_eq_str(dt, "bc") || tok_eq_str(dt, "de") ||
                        tok_eq_str(dt, "hl") || tok_eq_str(dt, "sp"))
                        addr_op = &s->instr.ops[1];
                }

                if (addr_op) {
                    char sym_name[64] = {0};
                    long addend = 0;
                    if (expr_single_sym(toks, addr_op->tok_start,
                                        addr_op->tok_count,
                                        &st, sym_name, sizeof(sym_name),
                                        &addend)) {
                        refsite_push(&r, sec.cur_off + 1, sym_name, addend);
                    }
                }
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
                /*
                 * Reference-site tracking for DW: each element that resolves
                 * to a single symbol reference gets a refsite.  We walk the
                 * argument token span splitting on commas, and for each
                 * element check expr_single_sym.
                 */
                {
                    int dw_end  = s->dir.args_start + s->dir.args_count;
                    int elem_start = s->dir.args_start;
                    uint32_t elem_off = sec.cur_off;
                    while (elem_start < dw_end) {
                        /* Find end of this element (next comma or end) */
                        int elem_end = elem_start;
                        while (elem_end < dw_end) {
                            if (toks[elem_end].kind == TOK_PUNCT &&
                                toks[elem_end].len == 1 &&
                                toks[elem_end].text[0] == ',')
                                break;
                            elem_end++;
                        }
                        int elem_count = elem_end - elem_start;
                        if (elem_count > 0) {
                            char sym_name[64] = {0};
                            long addend = 0;
                            if (expr_single_sym(toks, elem_start, elem_count,
                                                &st, sym_name, sizeof(sym_name),
                                                &addend)) {
                                refsite_push(&r, elem_off, sym_name, addend);
                            }
                        }
                        elem_off += 2; /* each DW element is 2 bytes */
                        /* skip the comma */
                        if (elem_end < dw_end) elem_end++;
                        elem_start = elem_end;
                    }
                }
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

            case DIR_INCBIN: {
                /*
                 * Pass 2: read the file and copy bytes into ROM; mark as
                 * PROV_ASSET (-2) in prov_line; register asset and fill
                 * prov_asset / prov_asset_off per-byte provenance.
                 */
                if (s->dir.args_count < 1) break;
                const AsmToken *ft = &toks[s->dir.args_start];
                if (ft->kind != TOK_STRING) break;
                char fname[512];
                size_t flen = ft->len < sizeof(fname)-1 ? ft->len : sizeof(fname)-1;
                memcpy(fname, ft->text, flen);
                fname[flen] = '\0';
                FILE *fh = fopen(fname, "rb");
                if (!fh && filename) {
                    const char *slash = strrchr(filename, '/');
                    if (slash) {
                        size_t dirlen = (size_t)(slash - filename + 1);
                        if (dirlen + flen < sizeof(fname)) {
                            memmove(fname + dirlen, fname, flen + 1);
                            memcpy(fname, filename, dirlen);
                            fh = fopen(fname, "rb");
                        }
                    }
                }
                if (!fh) {
                    /* Error already diagnosed in pass 1 */
                    break;
                }
                fseek(fh, 0, SEEK_END);
                long fsz = ftell(fh);
                rewind(fh);
                if (fsz > 0 && sec.cur_off + (uint32_t)fsz <= r.rom_size) {
                    if (fread(&r.rom[sec.cur_off], 1, (size_t)fsz, fh) == (size_t)fsz) {
                        /* Register asset (dedup by resolved path) */
                        int asset_id = -1;
                        for (int ai = 0; ai < r.nassets; ai++) {
                            if (strcmp(r.assets[ai].path, fname) == 0) {
                                asset_id = ai;
                                break;
                            }
                        }
                        if (asset_id < 0) {
                            /* New asset — store path + copy of bytes */
                            AsmAsset *na = realloc(r.assets,
                                                   (size_t)(r.nassets + 1) * sizeof(AsmAsset));
                            if (na) {
                                r.assets = na;
                                asset_id = r.nassets++;
                                AsmAsset *a = &r.assets[asset_id];
                                size_t plen = strlen(fname);
                                if (plen >= sizeof(a->path)) plen = sizeof(a->path) - 1;
                                memcpy(a->path, fname, plen);
                                a->path[plen] = '\0';
                                a->size  = (size_t)fsz;
                                a->bytes = malloc((size_t)fsz);
                                if (a->bytes)
                                    memcpy(a->bytes, &r.rom[sec.cur_off], (size_t)fsz);
                                else
                                    asset_id = -1; /* allocation failure; skip prov */
                            }
                        }
                        /* Mark prov_line and per-byte provenance */
                        for (long k = 0; k < fsz; k++) {
                            uint32_t off = sec.cur_off + (uint32_t)k;
                            r.prov_line[off] = PROV_ASSET;
                            if (asset_id >= 0) {
                                r.prov_asset[off]     = (int32_t)asset_id;
                                r.prov_asset_off[off] = (uint32_t)k;
                            }
                        }
                        linemap_push(&r, s->line, sec.cur_off);
                    }
                }
                fclose(fh);
                break;
            }

            case DIR_INCLUDE:
                /* INCLUDE was pre-expanded; nothing to do in pass 2 */
                break;
            }
            break;
        }
    }

    /* ------------------------------------------------------------------
     * Write cartridge header (Task 5)
     *
     * The header bytes are written AFTER assembly so they overwrite any
     * user code that collides (which is user error; default org=0x0150 avoids it).
     * ------------------------------------------------------------------ */
    if (r.rom && r.rom_size >= 0x0150u) {
        /* 0x0104-0x0133: Nintendo logo */
        memcpy(&r.rom[HDR_LOGO_ADDR], NINTENDO_LOGO, sizeof(NINTENDO_LOGO));

        /* 0x0134-0x0143: title (16 bytes, ASCII, zero-padded)
         * Derived from the filename (basename without extension), or all zeros. */
        {
            char title[16];
            memset(title, 0, sizeof(title));
            if (filename) {
                const char *base = strrchr(filename, '/');
                base = base ? base + 1 : filename;
                int ti = 0;
                for (int fi = 0; base[fi] && base[fi] != '.' && ti < 16; fi++, ti++) {
                    char c = base[fi];
                    title[ti] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
                }
            }
            memcpy(&r.rom[HDR_TITLE_ADDR], title, 16);
        }

        /* 0x0147: cartridge type — detect if any ROMX bank was used */
        {
            bool has_romx = false;
            for (int i = 0; i < st.count; i++) {
                if (st.syms[i].bank > 0) { has_romx = true; break; }
            }
            /* Also check via rom_size: if > 32KB, must have multi-bank */
            if (r.rom_size > ROM_MIN_SIZE) has_romx = true;
            r.rom[HDR_TYPE_ADDR] = has_romx ? 0x01u : 0x00u;
        }

        /* 0x0148: ROM size code = log2(rom_size / 32KB) */
        {
            int code = 0;
            size_t sz = ROM_MIN_SIZE;
            while (sz < r.rom_size) { sz <<= 1; code++; }
            r.rom[HDR_ROMSIZE_ADDR] = (uint8_t)code;
        }

        /* 0x0149: RAM size = 0x00 */
        r.rom[HDR_RAMSIZE_ADDR] = 0x00u;

        /* 0x014A: destination code (0x01 = non-Japanese, 0x00 = Japanese)
         * 0x014B: old licensee code (0x33 = use new licensee at 0x0144-0x0145)
         * 0x014C: mask ROM version (0x00)
         * Leave these at 0 (already zeroed by calloc).
         * 0x014D: header checksum */
        {
            uint8_t x = 0;
            for (uint16_t a = HDR_TITLE_ADDR; a <= 0x014Cu; a++) {
                x = (uint8_t)(x - r.rom[a] - 1u);
            }
            r.rom[HDR_HCHK_ADDR] = x;
        }

        /* 0x0100-0x0102: entry-point jump.
         * gb_reset() starts PC=0x0100; the bytes 0x0104+ are the Nintendo
         * logo (data, not code). If the source defines a ROM0 global "Main",
         * write JP Main here so the CPU reaches user code. Mirrors the live
         * engine's patch_entry() so CLI- and IDE-built ROMs behave identically.
         * Done before the global checksum so that checksum stays valid. */
        for (int i = 0; i < st.count; i++) {
            if (strcmp(st.syms[i].name, "Main") == 0 && st.syms[i].defined
                && st.syms[i].bank == 0 && st.syms[i].addr < 0x4000u) {
                r.rom[0x0100] = 0xC3u;                              /* JP nn */
                r.rom[0x0101] = (uint8_t)(st.syms[i].addr & 0xFFu);
                r.rom[0x0102] = (uint8_t)((st.syms[i].addr >> 8) & 0xFFu);
                break;
            }
        }

        /* 0x014E-0x014F: global checksum (16-bit big-endian, all bytes except 0x014E/0x014F) */
        {
            uint32_t gsum = 0;
            for (size_t b = 0; b < r.rom_size; b++) {
                if (b == HDR_GCHK_ADDR || b == HDR_GCHK_ADDR + 1) continue;
                gsum += r.rom[b];
            }
            r.rom[HDR_GCHK_ADDR]     = (uint8_t)((gsum >> 8) & 0xFFu);
            r.rom[HDR_GCHK_ADDR + 1] = (uint8_t)(gsum & 0xFFu);
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
 * Public: asm_assemble (thin wrapper over asm_assemble_mem with NULL mem)
 * ----------------------------------------------------------------------- */

AsmResult asm_assemble(const char *src, const char *filename)
{
    return asm_assemble_mem(src, filename, NULL);
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
    free(r->prov_asset);
    free(r->prov_asset_off);
    for (int i = 0; i < r->nassets; i++)
        free(r->assets[i].bytes);
    free(r->assets);
    free(r->diags);
    free(r->placements);
    free(r->refs);
    memset(r, 0, sizeof(*r));
}
