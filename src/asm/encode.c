/*
 * encode.c — SM83 instruction encoder
 *
 * int asm_encode(const char *mnemonic,
 *                const AsmToken   *toks,
 *                const AsmOperand *ops, int nops,
 *                uint16_t cur_addr,
 *                const AsmSymbolTable *syms,
 *                uint8_t *out,
 *                AsmDiag *err);
 *
 * Returns the number of bytes written to `out` (1-3), or -1 on error.
 * `out` must have space for at least 3 bytes.
 *
 * Design notes
 * ------------
 * Registers r[8]  : b=0 c=1 d=2 e=3 h=4 l=5 (hl)=6 a=7
 * Register pairs  : rp  {bc=0 de=1 hl=2 sp=3}
 *                   rp2 {bc=0 de=1 hl=2 af=3}
 * Conditions      : nz=0 z=1 nc=2 c=3
 *
 * Regular families (x/y/z structure):
 *   LD  r,r'  : 0x40 | y<<3 | z   (y=dst, z=src)  [0x76 is HALT]
 *   LD  r,d8  : z=6, x=0 => 0x06 | y<<3  (each LD B,d8 etc)
 *   LD (HL),d8: 0x36
 *   ALU r     : 0x80 | y<<3 | z
 *   ALU d8    : 0xC6 | y<<3
 *   INC r     : 0x04 | y<<3
 *   DEC r     : 0x05 | y<<3
 *   CB prefix : 0xCB, then 0x00|y<<3|z (rot), 0x40|y<<3|z (bit), ...
 */

#include "asm.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * Helpers: case-insensitive token comparison
 * --------------------------------------------------------------------- */

static bool tok_eq(const AsmToken *t, const char *s)
{
    size_t sl = strlen(s);
    if (t->len != sl) return false;
    for (size_t i = 0; i < sl; i++)
        if (tolower((unsigned char)t->text[i]) != tolower((unsigned char)s[i]))
            return false;
    return true;
}

/* Compare mnemonic string (already in mnemonic[] which is NUL-terminated) */
static bool mnem_eq(const char *m, const char *s)
{
    while (*m && *s) {
        if (tolower((unsigned char)*m) != tolower((unsigned char)*s)) return false;
        m++; s++;
    }
    return *m == '\0' && *s == '\0';
}

/* -----------------------------------------------------------------------
 * Error helper
 * --------------------------------------------------------------------- */

static int enc_err(AsmDiag *err, const char *msg)
{
    if (err) {
        err->line = 0; err->col = 0;
        snprintf(err->msg, sizeof(err->msg), "%s", msg);
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Register / condition mapping from an OPND_REG operand token
 *
 * For a single-token REG operand, the "name" is at toks[op->tok_start].
 * These return -1 on no match.
 * --------------------------------------------------------------------- */

/* r[8]: b=0 c=1 d=2 e=3 h=4 l=5 (hl)=6 a=7 */
static int reg_r(const AsmToken *t)
{
    if (tok_eq(t, "b"))  return 0;
    if (tok_eq(t, "c"))  return 1;
    if (tok_eq(t, "d"))  return 2;
    if (tok_eq(t, "e"))  return 3;
    if (tok_eq(t, "h"))  return 4;
    if (tok_eq(t, "l"))  return 5;
    if (tok_eq(t, "a"))  return 7;
    return -1;
}

/* rp: bc=0 de=1 hl=2 sp=3 */
static int reg_rp(const AsmToken *t)
{
    if (tok_eq(t, "bc")) return 0;
    if (tok_eq(t, "de")) return 1;
    if (tok_eq(t, "hl")) return 2;
    if (tok_eq(t, "sp")) return 3;
    return -1;
}

/* rp2: bc=0 de=1 hl=2 af=3 */
static int reg_rp2(const AsmToken *t)
{
    if (tok_eq(t, "bc")) return 0;
    if (tok_eq(t, "de")) return 1;
    if (tok_eq(t, "hl")) return 2;
    if (tok_eq(t, "af")) return 3;
    return -1;
}

/* condition: nz=0 z=1 nc=2 c=3 */
static int reg_cc(const AsmToken *t)
{
    if (tok_eq(t, "nz")) return 0;
    if (tok_eq(t, "z"))  return 1;
    if (tok_eq(t, "nc")) return 2;
    if (tok_eq(t, "c"))  return 3;
    return -1;
}

/* -----------------------------------------------------------------------
 * Operand analysis helpers
 *
 * These inspect the token span of an AsmOperand to determine what it is.
 * --------------------------------------------------------------------- */

/* Get the single register name token for a OPND_REG operand */
static const AsmToken *opnd_reg_tok(const AsmToken *toks, const AsmOperand *op)
{
    if (op->form != OPND_REG || op->tok_count < 1) return NULL;
    return &toks[op->tok_start];
}

/*
 * For an OPND_MEM operand (hl+), (hl-), (bc), (de), (hl), (c), or (addr),
 * inspect the inner tokens.
 *
 * Returns:
 *   'B'  -> (bc)
 *   'D'  -> (de)
 *   'H'  -> (hl)
 *   '+'  -> (hl+) or (hli)
 *   '-'  -> (hl-) or (hld)
 *   'C'  -> (c)
 *   'A'  -> (addr) — some immediate/label
 *   0    -> unknown/error
 */
static char mem_kind(const AsmToken *toks, const AsmOperand *op)
{
    if (op->form != OPND_MEM || op->tok_count < 3) return 0;
    /* tokens: '(' inner... ')' */
    const AsmToken *inner = &toks[op->tok_start + 1];
    int inner_n = op->tok_count - 2; /* excluding '(' and ')' */
    if (inner_n <= 0) return 0;

    const AsmToken *t0 = inner;

    if (inner_n == 1) {
        if (tok_eq(t0, "bc"))  return 'B';
        if (tok_eq(t0, "de"))  return 'D';
        if (tok_eq(t0, "hl"))  return 'H';
        if (tok_eq(t0, "c"))   return 'C';
        if (tok_eq(t0, "hli")) return '+';
        if (tok_eq(t0, "hld")) return '-';
        /* anything else is an address immediate */
        return 'A';
    }
    if (inner_n == 2) {
        /* (hl+) or (hl-) when the lexer gives hl and +/- as separate tokens */
        if (tok_eq(t0, "hl+")) return '+';
        if (tok_eq(t0, "hl-")) return '-';
    }
    if (inner_n >= 2 && tok_eq(t0, "hl")) {
        const AsmToken *t1 = &toks[op->tok_start + 2];
        if (t1->kind == TOK_PUNCT && t1->len == 1 && t1->text[0] == '+') return '+';
        if (t1->kind == TOK_PUNCT && t1->len == 1 && t1->text[0] == '-') return '-';
    }
    /* multi-token: assume address expression */
    return 'A';
}

/* Evaluate the inner expression of an OPND_MEM or OPND_IMM or OPND_SP_DISP */
static bool eval_imm(const AsmToken *toks, const AsmOperand *op,
                     const AsmSymbolTable *syms, long cur_addr,
                     long *out, AsmDiag *err)
{
    int start, count;
    if (op->form == OPND_MEM) {
        /* skip '(' and ')' */
        start = op->tok_start + 1;
        count = op->tok_count - 2;
    } else if (op->form == OPND_SP_DISP) {
        /* sp+e8: skip "sp" and "+"/"- "*/
        /* tok_start points at 'sp'; we need the displacement */
        /* format: sp + expr  OR  sp - expr */
        start = op->tok_start + 2; /* skip 'sp' and '+'/'-' */
        count = op->tok_count - 2;
        /* detect negative sign */
        const AsmToken *sign = &toks[op->tok_start + 1];
        if (sign->kind == TOK_PUNCT && sign->len == 1 && sign->text[0] == '-') {
            /* need to negate the result after eval — handled at call site */
            /* pass just the expr part */
        }
    } else {
        start = op->tok_start;
        count = op->tok_count;
    }
    return asm_eval_expr(toks, start, count, syms, cur_addr, out, err);
}

/* Evaluate OPND_SP_DISP returning signed byte: e.g. sp+5 -> 5, sp-3 -> -3 */
static bool eval_sp_disp(const AsmToken *toks, const AsmOperand *op,
                          const AsmSymbolTable *syms, long cur_addr,
                          long *out, AsmDiag *err)
{
    /* tokens: sp [+|-] expr */
    if (op->tok_count < 3) {
        if (err) snprintf(err->msg, sizeof(err->msg), "malformed sp+e8");
        return false;
    }
    const AsmToken *sign_tok = &toks[op->tok_start + 1];
    bool negative = (sign_tok->kind == TOK_PUNCT && sign_tok->len == 1 &&
                     sign_tok->text[0] == '-');
    int start = op->tok_start + 2;
    int count = op->tok_count - 2;
    long v;
    if (!asm_eval_expr(toks, start, count, syms, cur_addr, &v, err)) return false;
    *out = negative ? -v : v;
    return true;
}

/*
 * Evaluate the immediate address inside a (addr) operand.
 * Skips '(' and ')'.
 */
static bool eval_mem_addr(const AsmToken *toks, const AsmOperand *op,
                           const AsmSymbolTable *syms, long cur_addr,
                           long *out, AsmDiag *err)
{
    int start = op->tok_start + 1;
    int count = op->tok_count - 2;
    return asm_eval_expr(toks, start, count, syms, cur_addr, out, err);
}

/* -----------------------------------------------------------------------
 * r[8] from operand (handles OPND_REG or OPND_MEM for (hl))
 * --------------------------------------------------------------------- */

/*
 * Returns 0..7 for b/c/d/e/h/l/(hl)/a, or -1 if not a simple r operand.
 * For OPND_MEM, only (hl) -> 6 is accepted.
 */
static int opnd_as_r(const AsmToken *toks, const AsmOperand *op)
{
    if (op->form == OPND_REG) {
        const AsmToken *t = &toks[op->tok_start];
        return reg_r(t);
    }
    if (op->form == OPND_MEM) {
        char mk = mem_kind(toks, op);
        if (mk == 'H') return 6; /* (hl) */
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

int asm_encode(const char         *mnemonic,
               const AsmToken     *toks,
               const AsmOperand   *ops,
               int                 nops,
               uint16_t            cur_addr,
               const AsmSymbolTable *syms,
               uint8_t            *out,
               AsmDiag            *err)
{
    /* Lowercase copy of mnemonic for comparisons */
    char mn[16];
    {
        size_t i = 0;
        while (mnemonic[i] && i < sizeof(mn) - 1) {
            mn[i] = (char)tolower((unsigned char)mnemonic[i]);
            i++;
        }
        mn[i] = '\0';
    }

#define MN(s) mnem_eq(mn, s)

    /* ------------------------------------------------------------------ */
    /* NOP, HALT, STOP, DI, EI, RET, RETI                                 */
    /* ------------------------------------------------------------------ */

    if (MN("nop"))  { out[0] = 0x00; return 1; }
    if (MN("rlca")) { out[0] = 0x07; return 1; }
    if (MN("rrca")) { out[0] = 0x0F; return 1; }
    if (MN("rla"))  { out[0] = 0x17; return 1; }
    if (MN("rra"))  { out[0] = 0x1F; return 1; }
    if (MN("daa"))  { out[0] = 0x27; return 1; }
    if (MN("cpl"))  { out[0] = 0x2F; return 1; }
    if (MN("scf"))  { out[0] = 0x37; return 1; }
    if (MN("ccf"))  { out[0] = 0x3F; return 1; }
    if (MN("halt")) { out[0] = 0x76; return 1; }
    if (MN("stop")) { out[0] = 0x10; out[1] = 0x00; return 2; }
    if (MN("di"))   { out[0] = 0xF3; return 1; }
    if (MN("ei"))   { out[0] = 0xFB; return 1; }
    if (MN("reti")) { out[0] = 0xD9; return 1; }

    /* ------------------------------------------------------------------ */
    /* RET [cc]                                                            */
    /* ------------------------------------------------------------------ */

    if (MN("ret")) {
        if (nops == 0) { out[0] = 0xC9; return 1; }
        if (nops == 1 && ops[0].form == OPND_REG) {
            const AsmToken *t = opnd_reg_tok(toks, &ops[0]);
            int cc = t ? reg_cc(t) : -1;
            if (cc < 0) return enc_err(err, "ret: invalid condition");
            out[0] = (uint8_t)(0xC0 | (cc << 3));
            return 1;
        }
        return enc_err(err, "ret: unexpected operands");
    }

    /* ------------------------------------------------------------------ */
    /* RST n                                                               */
    /* ------------------------------------------------------------------ */

    if (MN("rst")) {
        if (nops != 1 || ops[0].form != OPND_IMM)
            return enc_err(err, "rst: expected immediate");
        long v;
        if (!eval_imm(toks, &ops[0], syms, cur_addr, &v, err)) return -1;
        if (v % 8 != 0 || v < 0 || v > 0x38)
            return enc_err(err, "rst: vector must be 0,8,$10,$18,$20,$28,$30,$38");
        out[0] = (uint8_t)(0xC7 | (v & 0x38));
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* JP                                                                  */
    /* ------------------------------------------------------------------ */

    if (MN("jp")) {
        if (nops == 1) {
            if (ops[0].form == OPND_REG) {
                const AsmToken *t = opnd_reg_tok(toks, &ops[0]);
                if (t && tok_eq(t, "hl")) { out[0] = 0xE9; return 1; }
                return enc_err(err, "jp: register operand must be HL");
            }
            if (ops[0].form == OPND_IMM) {
                long v;
                if (!eval_imm(toks, &ops[0], syms, cur_addr, &v, err)) return -1;
                out[0] = 0xC3;
                out[1] = (uint8_t)(v & 0xFF);
                out[2] = (uint8_t)((v >> 8) & 0xFF);
                return 3;
            }
            return enc_err(err, "jp: invalid operand");
        }
        if (nops == 2) {
            /* jp cc, a16 */
            if (ops[0].form != OPND_REG)
                return enc_err(err, "jp cc: first operand must be condition");
            const AsmToken *ct = opnd_reg_tok(toks, &ops[0]);
            int cc = ct ? reg_cc(ct) : -1;
            if (cc < 0) return enc_err(err, "jp cc: invalid condition");
            if (ops[1].form != OPND_IMM)
                return enc_err(err, "jp cc: second operand must be address");
            long v;
            if (!eval_imm(toks, &ops[1], syms, cur_addr, &v, err)) return -1;
            out[0] = (uint8_t)(0xC2 | (cc << 3));
            out[1] = (uint8_t)(v & 0xFF);
            out[2] = (uint8_t)((v >> 8) & 0xFF);
            return 3;
        }
        return enc_err(err, "jp: wrong number of operands");
    }

    /* ------------------------------------------------------------------ */
    /* JR [cc,] e8                                                         */
    /* ------------------------------------------------------------------ */

    if (MN("jr")) {
        long target;
        if (nops == 1) {
            if (ops[0].form != OPND_IMM)
                return enc_err(err, "jr: expected immediate");
            if (!eval_imm(toks, &ops[0], syms, cur_addr, &target, err)) return -1;
            long disp = target - ((long)cur_addr + 2);
            if (disp < -128 || disp > 127) {
                char msg[80];
                snprintf(msg, sizeof(msg),
                         "jr: displacement %ld out of range [-128,127]", disp);
                return enc_err(err, msg);
            }
            out[0] = 0x18;
            out[1] = (uint8_t)(int8_t)disp;
            return 2;
        }
        if (nops == 2) {
            if (ops[0].form != OPND_REG)
                return enc_err(err, "jr cc: first operand must be condition");
            const AsmToken *ct = opnd_reg_tok(toks, &ops[0]);
            int cc = ct ? reg_cc(ct) : -1;
            if (cc < 0) return enc_err(err, "jr cc: invalid condition");
            if (ops[1].form != OPND_IMM)
                return enc_err(err, "jr cc: second operand must be address");
            if (!eval_imm(toks, &ops[1], syms, cur_addr, &target, err)) return -1;
            long disp = target - ((long)cur_addr + 2);
            if (disp < -128 || disp > 127) {
                char msg[80];
                snprintf(msg, sizeof(msg),
                         "jr cc: displacement %ld out of range [-128,127]", disp);
                return enc_err(err, msg);
            }
            out[0] = (uint8_t)(0x20 | (cc << 3));
            out[1] = (uint8_t)(int8_t)disp;
            return 2;
        }
        return enc_err(err, "jr: wrong number of operands");
    }

    /* ------------------------------------------------------------------ */
    /* CALL [cc,] a16                                                      */
    /* ------------------------------------------------------------------ */

    if (MN("call")) {
        if (nops == 1 && ops[0].form == OPND_IMM) {
            long v;
            if (!eval_imm(toks, &ops[0], syms, cur_addr, &v, err)) return -1;
            out[0] = 0xCD;
            out[1] = (uint8_t)(v & 0xFF);
            out[2] = (uint8_t)((v >> 8) & 0xFF);
            return 3;
        }
        if (nops == 2 && ops[0].form == OPND_REG) {
            const AsmToken *ct = opnd_reg_tok(toks, &ops[0]);
            int cc = ct ? reg_cc(ct) : -1;
            if (cc < 0) return enc_err(err, "call cc: invalid condition");
            if (ops[1].form != OPND_IMM)
                return enc_err(err, "call cc: second operand must be address");
            long v;
            if (!eval_imm(toks, &ops[1], syms, cur_addr, &v, err)) return -1;
            out[0] = (uint8_t)(0xC4 | (cc << 3));
            out[1] = (uint8_t)(v & 0xFF);
            out[2] = (uint8_t)((v >> 8) & 0xFF);
            return 3;
        }
        return enc_err(err, "call: invalid operands");
    }

    /* ------------------------------------------------------------------ */
    /* PUSH / POP rp2                                                      */
    /* ------------------------------------------------------------------ */

    if (MN("push") || MN("pop")) {
        bool is_push = MN("push");
        if (nops != 1 || ops[0].form != OPND_REG)
            return enc_err(err, "push/pop: expected register pair");
        const AsmToken *t = opnd_reg_tok(toks, &ops[0]);
        int rp2 = t ? reg_rp2(t) : -1;
        if (rp2 < 0) return enc_err(err, "push/pop: expected bc/de/hl/af");
        /* push: C5 D5 E5 F5; pop: C1 D1 E1 F1 */
        out[0] = (uint8_t)((is_push ? 0xC5 : 0xC1) | (rp2 << 4));
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* ADD                                                                 */
    /* ------------------------------------------------------------------ */

    if (MN("add")) {
        /* ADD A,r / ADD A,d8 / ADD HL,rr / ADD SP,e8 */
        if (nops == 2) {
            const AsmToken *t0 = (ops[0].form == OPND_REG)
                                 ? opnd_reg_tok(toks, &ops[0]) : NULL;
            /* ADD HL, rr */
            if (t0 && tok_eq(t0, "hl") && ops[1].form == OPND_REG) {
                const AsmToken *t1 = opnd_reg_tok(toks, &ops[1]);
                int rp = t1 ? reg_rp(t1) : -1;
                if (rp < 0) return enc_err(err, "add hl: expected rp");
                /* 09 19 29 39 */
                out[0] = (uint8_t)(0x09 | (rp << 4));
                return 1;
            }
            /* ADD SP, e8 */
            if (t0 && tok_eq(t0, "sp")) {
                long disp;
                /* second operand can be IMM or SP_DISP-like */
                if (ops[1].form == OPND_IMM) {
                    if (!eval_imm(toks, &ops[1], syms, cur_addr, &disp, err)) return -1;
                } else {
                    return enc_err(err, "add sp: expected immediate displacement");
                }
                if (disp < -128 || disp > 127)
                    return enc_err(err, "add sp: displacement out of range");
                out[0] = 0xE8;
                out[1] = (uint8_t)(int8_t)disp;
                return 2;
            }
            /* ADD A, r / ADD A, d8 */
            if (t0 && tok_eq(t0, "a")) {
                int rz = opnd_as_r(toks, &ops[1]);
                if (rz >= 0) { out[0] = (uint8_t)(0x80 | rz); return 1; }
                if (ops[1].form == OPND_IMM) {
                    long v;
                    if (!eval_imm(toks, &ops[1], syms, cur_addr, &v, err)) return -1;
                    out[0] = 0xC6; out[1] = (uint8_t)(v & 0xFF); return 2;
                }
                return enc_err(err, "add a: invalid second operand");
            }
        }
        /* ADD r (1-op, treated as ADD A, r) */
        if (nops == 1) {
            int rz = opnd_as_r(toks, &ops[0]);
            if (rz >= 0) { out[0] = (uint8_t)(0x80 | rz); return 1; }
            if (ops[0].form == OPND_IMM) {
                long v;
                if (!eval_imm(toks, &ops[0], syms, cur_addr, &v, err)) return -1;
                out[0] = 0xC6; out[1] = (uint8_t)(v & 0xFF); return 2;
            }
        }
        return enc_err(err, "add: invalid operands");
    }

    /* ------------------------------------------------------------------ */
    /* ADC, SUB, SBC, AND, XOR, OR, CP                                    */
    /* ALU y-index: ADD=0 ADC=1 SUB=2 SBC=3 AND=4 XOR=5 OR=6 CP=7       */
    /* ------------------------------------------------------------------ */

    {
        int alu_y = -1;
        if (MN("adc")) alu_y = 1;
        else if (MN("sub")) alu_y = 2;
        else if (MN("sbc")) alu_y = 3;
        else if (MN("and")) alu_y = 4;
        else if (MN("xor")) alu_y = 5;
        else if (MN("or"))  alu_y = 6;
        else if (MN("cp"))  alu_y = 7;

        if (alu_y >= 0) {
            /* Accept: mnem A,r / mnem A,d8 / mnem r / mnem d8 */
            const AsmOperand *src_op = NULL;
            if (nops == 2) {
                /* first must be A */
                if (ops[0].form == OPND_REG) {
                    const AsmToken *t0 = opnd_reg_tok(toks, &ops[0]);
                    if (!t0 || !tok_eq(t0, "a"))
                        return enc_err(err, "alu: first operand must be A");
                }
                src_op = &ops[1];
            } else if (nops == 1) {
                src_op = &ops[0];
            } else {
                return enc_err(err, "alu: wrong number of operands");
            }
            int rz = opnd_as_r(toks, src_op);
            if (rz >= 0) {
                out[0] = (uint8_t)(0x80 | (alu_y << 3) | rz);
                return 1;
            }
            if (src_op->form == OPND_IMM) {
                long v;
                if (!eval_imm(toks, src_op, syms, cur_addr, &v, err)) return -1;
                out[0] = (uint8_t)(0xC6 | (alu_y << 3));
                out[1] = (uint8_t)(v & 0xFF);
                return 2;
            }
            return enc_err(err, "alu: invalid source operand");
        }
    }

    /* ------------------------------------------------------------------ */
    /* INC / DEC                                                           */
    /* ------------------------------------------------------------------ */

    if (MN("inc") || MN("dec")) {
        bool is_dec = MN("dec");
        if (nops != 1) return enc_err(err, "inc/dec: expected 1 operand");

        /* inc/dec r */
        int ry = opnd_as_r(toks, &ops[0]);
        if (ry >= 0) {
            out[0] = (uint8_t)((is_dec ? 0x05 : 0x04) | (ry << 3));
            return 1;
        }
        /* inc/dec rr */
        if (ops[0].form == OPND_REG) {
            const AsmToken *t = opnd_reg_tok(toks, &ops[0]);
            int rp = t ? reg_rp(t) : -1;
            if (rp >= 0) {
                out[0] = (uint8_t)((is_dec ? 0x0B : 0x03) | (rp << 4));
                return 1;
            }
        }
        return enc_err(err, "inc/dec: invalid operand");
    }

    /* ------------------------------------------------------------------ */
    /* CB-prefixed: RLC RRC RL RR SLA SRA SWAP SRL BIT RES SET            */
    /* ------------------------------------------------------------------ */

    {
        int rot_y = -1;
        if (MN("rlc"))  rot_y = 0;
        else if (MN("rrc"))  rot_y = 1;
        else if (MN("rl"))   rot_y = 2;
        else if (MN("rr"))   rot_y = 3;
        else if (MN("sla"))  rot_y = 4;
        else if (MN("sra"))  rot_y = 5;
        else if (MN("swap")) rot_y = 6;
        else if (MN("srl"))  rot_y = 7;

        if (rot_y >= 0) {
            if (nops != 1)
                return enc_err(err, "cb rot/shift: expected 1 operand");
            int rz = opnd_as_r(toks, &ops[0]);
            if (rz < 0) return enc_err(err, "cb rot/shift: invalid register");
            out[0] = 0xCB;
            out[1] = (uint8_t)(rot_y << 3 | rz);
            return 2;
        }
    }

    if (MN("bit") || MN("res") || MN("set")) {
        int x_val;
        if      (MN("bit")) x_val = 1;
        else if (MN("res")) x_val = 2;
        else                x_val = 3;

        if (nops != 2)
            return enc_err(err, "bit/res/set: expected 2 operands");
        if (ops[0].form != OPND_IMM)
            return enc_err(err, "bit/res/set: first operand must be bit number");
        long bit;
        if (!eval_imm(toks, &ops[0], syms, cur_addr, &bit, err)) return -1;
        if (bit < 0 || bit > 7)
            return enc_err(err, "bit/res/set: bit number must be 0..7");
        int rz = opnd_as_r(toks, &ops[1]);
        if (rz < 0) return enc_err(err, "bit/res/set: invalid register");
        out[0] = 0xCB;
        out[1] = (uint8_t)((x_val << 6) | ((int)bit << 3) | rz);
        return 2;
    }

    /* ------------------------------------------------------------------ */
    /* LD — the big one                                                    */
    /* ------------------------------------------------------------------ */

    if (MN("ld")) {
        if (nops != 2)
            return enc_err(err, "ld: expected 2 operands");

        const AsmOperand *dst = &ops[0];
        const AsmOperand *src = &ops[1];

        /* ---- LD rr, d16 -------------------------------------------- */
        /* dst = REG (rp), src = IMM */
        if (dst->form == OPND_REG && src->form == OPND_IMM) {
            const AsmToken *dt = opnd_reg_tok(toks, dst);
            int rp = dt ? reg_rp(dt) : -1;
            if (rp >= 0) {
                long v;
                if (!eval_imm(toks, src, syms, cur_addr, &v, err)) return -1;
                /* 01 11 21 31 */
                out[0] = (uint8_t)(0x01 | (rp << 4));
                out[1] = (uint8_t)(v & 0xFF);
                out[2] = (uint8_t)((v >> 8) & 0xFF);
                return 3;
            }
        }

        /* ---- LD SP, HL -------------------------------------------- */
        if (dst->form == OPND_REG && src->form == OPND_REG) {
            const AsmToken *dt = opnd_reg_tok(toks, dst);
            const AsmToken *st = opnd_reg_tok(toks, src);
            if (dt && st && tok_eq(dt, "sp") && tok_eq(st, "hl")) {
                out[0] = 0xF9; return 1;
            }
        }

        /* ---- LD HL, SP+e8 ------------------------------------------ */
        if (dst->form == OPND_REG && src->form == OPND_SP_DISP) {
            const AsmToken *dt = opnd_reg_tok(toks, dst);
            if (dt && tok_eq(dt, "hl")) {
                long disp;
                if (!eval_sp_disp(toks, src, syms, cur_addr, &disp, err)) return -1;
                if (disp < -128 || disp > 127)
                    return enc_err(err, "ld hl,sp+e8: displacement out of range");
                out[0] = 0xF8;
                out[1] = (uint8_t)(int8_t)disp;
                return 2;
            }
        }

        /* ---- LD (a16), SP ------------------------------------------ */
        if (dst->form == OPND_MEM && src->form == OPND_REG) {
            const AsmToken *st = opnd_reg_tok(toks, src);
            if (st && tok_eq(st, "sp")) {
                char mk = mem_kind(toks, dst);
                if (mk == 'A') {
                    long v;
                    if (!eval_mem_addr(toks, dst, syms, cur_addr, &v, err)) return -1;
                    out[0] = 0x08;
                    out[1] = (uint8_t)(v & 0xFF);
                    out[2] = (uint8_t)((v >> 8) & 0xFF);
                    return 3;
                }
            }
        }

        /* ---- LD r, r' (register to register, including (hl)) ------- */
        {
            int ry = opnd_as_r(toks, dst);
            int rz = opnd_as_r(toks, src);
            if (ry >= 0 && rz >= 0) {
                if (ry == 6 && rz == 6)
                    return enc_err(err, "ld (hl),(hl) is HALT — not a valid LD");
                out[0] = (uint8_t)(0x40 | (ry << 3) | rz);
                return 1;
            }
        }

        /* ---- LD r, d8 (including LD (HL),d8) ----------------------- */
        {
            int ry = opnd_as_r(toks, dst);
            if (ry >= 0 && src->form == OPND_IMM) {
                long v;
                if (!eval_imm(toks, src, syms, cur_addr, &v, err)) return -1;
                out[0] = (uint8_t)(0x06 | (ry << 3));
                out[1] = (uint8_t)(v & 0xFF);
                return 2;
            }
        }

        /* ---- LD A, (BC) / LD A, (DE) / LD A, (HL+) / LD A, (HL-) -- */
        if (dst->form == OPND_REG && src->form == OPND_MEM) {
            const AsmToken *dt = opnd_reg_tok(toks, dst);
            if (dt && tok_eq(dt, "a")) {
                char mk = mem_kind(toks, src);
                if (mk == 'B') { out[0] = 0x0A; return 1; }
                if (mk == 'D') { out[0] = 0x1A; return 1; }
                if (mk == '+') { out[0] = 0x2A; return 1; }
                if (mk == '-') { out[0] = 0x3A; return 1; }
                /* LD A, (a16) */
                if (mk == 'A') {
                    long v;
                    if (!eval_mem_addr(toks, src, syms, cur_addr, &v, err)) return -1;
                    out[0] = 0xFA;
                    out[1] = (uint8_t)(v & 0xFF);
                    out[2] = (uint8_t)((v >> 8) & 0xFF);
                    return 3;
                }
                /* LD A, (C) */
                if (mk == 'C') { out[0] = 0xF2; return 1; }
            }
        }

        /* ---- LD (BC),A / (DE),A / (HL+),A / (HL-),A --------------- */
        if (dst->form == OPND_MEM && src->form == OPND_REG) {
            const AsmToken *st = opnd_reg_tok(toks, src);
            if (st && tok_eq(st, "a")) {
                char mk = mem_kind(toks, dst);
                if (mk == 'B') { out[0] = 0x02; return 1; }
                if (mk == 'D') { out[0] = 0x12; return 1; }
                if (mk == '+') { out[0] = 0x22; return 1; }
                if (mk == '-') { out[0] = 0x32; return 1; }
                /* LD (a16), A */
                if (mk == 'A') {
                    long v;
                    if (!eval_mem_addr(toks, dst, syms, cur_addr, &v, err)) return -1;
                    out[0] = 0xEA;
                    out[1] = (uint8_t)(v & 0xFF);
                    out[2] = (uint8_t)((v >> 8) & 0xFF);
                    return 3;
                }
                /* LD (C), A */
                if (mk == 'C') { out[0] = 0xE2; return 1; }
            }
        }

        /* ---- LD (a16), SP (already handled above) ------------------- */

        return enc_err(err, "ld: unrecognised form");
    }

    /* ------------------------------------------------------------------ */
    /* LDH: ldh (a8),a   and   ldh a,(a8)                                 */
    /* ------------------------------------------------------------------ */

    if (MN("ldh")) {
        if (nops != 2) return enc_err(err, "ldh: expected 2 operands");
        const AsmOperand *dst = &ops[0];
        const AsmOperand *src = &ops[1];

        /* ldh (a8), a */
        if (dst->form == OPND_MEM && src->form == OPND_REG) {
            const AsmToken *st = opnd_reg_tok(toks, src);
            if (st && tok_eq(st, "a")) {
                long v;
                if (!eval_mem_addr(toks, dst, syms, cur_addr, &v, err)) return -1;
                out[0] = 0xE0;
                out[1] = (uint8_t)(v & 0xFF);
                return 2;
            }
        }
        /* ldh a, (a8) */
        if (dst->form == OPND_REG && src->form == OPND_MEM) {
            const AsmToken *dt = opnd_reg_tok(toks, dst);
            if (dt && tok_eq(dt, "a")) {
                long v;
                if (!eval_mem_addr(toks, src, syms, cur_addr, &v, err)) return -1;
                out[0] = 0xF0;
                out[1] = (uint8_t)(v & 0xFF);
                return 2;
            }
        }
        return enc_err(err, "ldh: unrecognised form");
    }

    /* ------------------------------------------------------------------ */
    /* Unrecognised mnemonic                                               */
    /* ------------------------------------------------------------------ */

    {
        char msg[80];
        snprintf(msg, sizeof(msg), "unknown mnemonic '%s'", mn);
        return enc_err(err, msg);
    }
}
