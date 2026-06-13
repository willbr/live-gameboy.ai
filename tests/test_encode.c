/*
 * test_encode.c — comprehensive SM83 instruction encoder tests
 *
 * All expected bytes come from the Pan Docs / gbdev SM83 opcode table,
 * NOT from our own CPU decoder.  Every discrepancy between this table
 * and the encoder is a real bug — do NOT weaken the test to make it pass.
 *
 * Test helper: parse one line of assembly through asm_lex + asm_parse,
 * then call asm_encode with the resulting statement.
 */

#include "test.h"
#include "../src/asm/asm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Forward-declare asm_encode (defined in encode.c)
 * --------------------------------------------------------------------- */

int asm_encode(const char         *mnemonic,
               const AsmToken     *toks,
               const AsmOperand   *ops,
               int                 nops,
               uint16_t            cur_addr,
               const AsmSymbolTable *syms,
               uint8_t            *out,
               AsmDiag            *err);

/* -----------------------------------------------------------------------
 * Encode one source line and return byte count (or -1).
 * Writes up to 3 bytes into `out`.
 * --------------------------------------------------------------------- */

static int encode_line(const char *src, uint16_t cur_addr,
                       const AsmSymbolTable *syms,
                       uint8_t *out, AsmDiag *err)
{
    int ntoks = 0;
    AsmDiag *diags = NULL; int ndiags = 0;
    AsmToken *toks = asm_lex(src, "<test>", &ntoks, &diags, &ndiags);
    if (!toks) { free(diags); return -1; }

    int nstmts = 0;
    AsmStmt *stmts = asm_parse(toks, ntoks, &nstmts, &diags, &ndiags);
    if (!stmts || nstmts == 0) {
        free(toks); free(stmts); free(diags);
        if (err) snprintf(err->msg, sizeof(err->msg), "parse produced no statements");
        return -1;
    }

    /* Find the first ST_INSTR statement */
    AsmStmt *s = NULL;
    for (int i = 0; i < nstmts; i++) {
        if (stmts[i].kind == ST_INSTR) { s = &stmts[i]; break; }
    }
    if (!s) {
        free(toks); free(stmts); free(diags);
        if (err) snprintf(err->msg, sizeof(err->msg), "no instruction in: %s", src);
        return -1;
    }

    int n = asm_encode(s->instr.mnemonic, toks, s->instr.ops, s->instr.nops,
                       cur_addr, syms, out, err);

    free(toks);
    free(stmts);
    free(diags);
    return n;
}

/* -----------------------------------------------------------------------
 * Test case structure
 * --------------------------------------------------------------------- */

typedef struct {
    const char *src;
    uint8_t     bytes[4];
    int         n;
} EncCase;

/* -----------------------------------------------------------------------
 * Run all test cases
 * --------------------------------------------------------------------- */

static int run_cases(const EncCase *cases, int ncases,
                     uint16_t cur_addr, const AsmSymbolTable *syms)
{
    int failures = 0;
    for (int i = 0; i < ncases; i++) {
        uint8_t out[4] = {0};
        AsmDiag err; memset(&err, 0, sizeof(err));
        int n = encode_line(cases[i].src, cur_addr, syms, out, &err);
        bool ok = (n == cases[i].n);
        if (ok) {
            for (int j = 0; j < n; j++) {
                if (out[j] != cases[i].bytes[j]) { ok = false; break; }
            }
        }
        if (!ok) {
            fprintf(stderr, "FAIL [%s]:\n  got    n=%d bytes=", cases[i].src, n);
            for (int j = 0; j < n && j < 4; j++) fprintf(stderr, "%02X ", out[j]);
            fprintf(stderr, "\n  expect n=%d bytes=", cases[i].n);
            for (int j = 0; j < cases[i].n; j++) fprintf(stderr, "%02X ", cases[i].bytes[j]);
            if (err.msg[0]) fprintf(stderr, "\n  diag: %s", err.msg);
            fprintf(stderr, "\n");
            failures++;
        }
    }
    return failures;
}

/* -----------------------------------------------------------------------
 * Main
 * --------------------------------------------------------------------- */

int main(void)
{
    int failures = 0;

    /* ================================================================== */
    /* Misc / no-operand instructions                                      */
    /* ================================================================== */

    static const EncCase misc_cases[] = {
        { "nop",    {0x00}, 1 },
        { "rlca",   {0x07}, 1 },
        { "rrca",   {0x0F}, 1 },
        { "rla",    {0x17}, 1 },
        { "rra",    {0x1F}, 1 },
        { "daa",    {0x27}, 1 },
        { "cpl",    {0x2F}, 1 },
        { "scf",    {0x37}, 1 },
        { "ccf",    {0x3F}, 1 },
        { "halt",   {0x76}, 1 },
        { "stop",   {0x10, 0x00}, 2 },
        { "di",     {0xF3}, 1 },
        { "ei",     {0xFB}, 1 },
        { "ret",    {0xC9}, 1 },
        { "reti",   {0xD9}, 1 },
    };
    failures += run_cases(misc_cases,
                          (int)(sizeof(misc_cases)/sizeof(misc_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* RET cc                                                              */
    /* ================================================================== */

    static const EncCase ret_cc_cases[] = {
        { "ret nz", {0xC0}, 1 },
        { "ret z",  {0xC8}, 1 },
        { "ret nc", {0xD0}, 1 },
        { "ret c",  {0xD8}, 1 },
    };
    failures += run_cases(ret_cc_cases,
                          (int)(sizeof(ret_cc_cases)/sizeof(ret_cc_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* RST vectors                                                         */
    /* ================================================================== */

    static const EncCase rst_cases[] = {
        { "rst $00", {0xC7}, 1 },
        { "rst $08", {0xCF}, 1 },
        { "rst $10", {0xD7}, 1 },
        { "rst $18", {0xDF}, 1 },
        { "rst $20", {0xE7}, 1 },
        { "rst $28", {0xEF}, 1 },
        { "rst $30", {0xF7}, 1 },
        { "rst $38", {0xFF}, 1 },
    };
    failures += run_cases(rst_cases,
                          (int)(sizeof(rst_cases)/sizeof(rst_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* JP                                                                  */
    /* ================================================================== */

    static const EncCase jp_cases[] = {
        { "jp $1234",    {0xC3, 0x34, 0x12}, 3 },
        { "jp nz,$1234", {0xC2, 0x34, 0x12}, 3 },
        { "jp z,$1234",  {0xCA, 0x34, 0x12}, 3 },
        { "jp nc,$1234", {0xD2, 0x34, 0x12}, 3 },
        { "jp c,$1234",  {0xDA, 0x34, 0x12}, 3 },
        { "jp hl",       {0xE9}, 1 },
    };
    failures += run_cases(jp_cases,
                          (int)(sizeof(jp_cases)/sizeof(jp_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* JR — displacement                                                   */
    /* ================================================================== */

    /*
     * jr $0002 with cur_addr=0x0000: disp = 0x0002 - (0x0000+2) = 0 -> 18 00
     * jr nz,Target where Target=0x0010, cur_addr=0x000E: disp=0x10-0x10=0 -> 20 00
     */
    {
        static const EncCase jr_cases[] = {
            /* jr $0002 from 0: disp=0 */
            { "jr $0002", {0x18, 0x00}, 2 },
        };
        failures += run_cases(jr_cases,
                              (int)(sizeof(jr_cases)/sizeof(jr_cases[0])),
                              0x0000, NULL);
    }

    /* jr with a symbol table */
    {
        /* Target at 0x0100, assembled at 0x00FE: disp = 0x0100 - (0x00FE+2) = 0 */
        AsmSymbol sym;
        memset(&sym, 0, sizeof(sym));
        strncpy(sym.name, "Target", sizeof(sym.name)-1);
        sym.addr  = 0x0100;
        sym.value = 0x0100;
        sym.defined = true;
        AsmSymbolTable st = { &sym, 1, 1 };

        uint8_t out[4]; AsmDiag err; memset(&err, 0, sizeof(err));
        int n = encode_line("jr Target", 0x00FE, &st, out, &err);
        ASSERT_TRUE(n == 2);
        ASSERT_TRUE(out[0] == 0x18 && out[1] == 0x00);

        /* jr nz, Target */
        n = encode_line("jr nz, Target", 0x00FE, &st, out, &err);
        ASSERT_TRUE(n == 2);
        ASSERT_TRUE(out[0] == 0x20 && out[1] == 0x00);

        /* Negative displacement: Target at 0x10, assembled at 0x14 -> disp=-6 */
        sym.addr  = 0x0010;
        sym.value = 0x0010;
        n = encode_line("jr Target", 0x0014, &st, out, &err);
        ASSERT_TRUE(n == 2);
        ASSERT_TRUE(out[0] == 0x18 && out[1] == (uint8_t)(int8_t)-6);

        /* Out-of-range jr: Target at 0x0200, cur_addr=0x0000 -> disp=0x1FE > 127 */
        sym.addr  = 0x0200;
        sym.value = 0x0200;
        n = encode_line("jr Target", 0x0000, &st, out, &err);
        ASSERT_TRUE(n == -1);

        /* jr cc conditions */
        sym.addr  = 0x0100; sym.value = 0x0100;
        n = encode_line("jr z, Target",  0x00FE, &st, out, &err);
        ASSERT_TRUE(n == 2); ASSERT_EQ(out[0], 0x28);

        n = encode_line("jr nc, Target", 0x00FE, &st, out, &err);
        ASSERT_TRUE(n == 2); ASSERT_EQ(out[0], 0x30);

        n = encode_line("jr c, Target",  0x00FE, &st, out, &err);
        ASSERT_TRUE(n == 2); ASSERT_EQ(out[0], 0x38);
    }

    /* ================================================================== */
    /* CALL                                                                */
    /* ================================================================== */

    static const EncCase call_cases[] = {
        { "call $1234",    {0xCD, 0x34, 0x12}, 3 },
        { "call nz,$1234", {0xC4, 0x34, 0x12}, 3 },
        { "call z,$1234",  {0xCC, 0x34, 0x12}, 3 },
        { "call nc,$1234", {0xD4, 0x34, 0x12}, 3 },
        { "call c,$1234",  {0xDC, 0x34, 0x12}, 3 },
    };
    failures += run_cases(call_cases,
                          (int)(sizeof(call_cases)/sizeof(call_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* PUSH / POP                                                          */
    /* ================================================================== */

    static const EncCase push_pop_cases[] = {
        { "push bc", {0xC5}, 1 },
        { "push de", {0xD5}, 1 },
        { "push hl", {0xE5}, 1 },
        { "push af", {0xF5}, 1 },
        { "pop bc",  {0xC1}, 1 },
        { "pop de",  {0xD1}, 1 },
        { "pop hl",  {0xE1}, 1 },
        { "pop af",  {0xF1}, 1 },
    };
    failures += run_cases(push_pop_cases,
                          (int)(sizeof(push_pop_cases)/sizeof(push_pop_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* LD r, r' — all 49 non-HALT combinations (7x7 minus (hl),(hl))     */
    /* ================================================================== */

    /*
     * LD dst,src -> 0x40 | (dst_r << 3) | src_r
     * r index: b=0 c=1 d=2 e=3 h=4 l=5 (hl)=6 a=7
     */
    static const EncCase ld_rr_cases[] = {
        { "ld b,b",    {0x40}, 1 },
        { "ld b,c",    {0x41}, 1 },
        { "ld b,d",    {0x42}, 1 },
        { "ld b,e",    {0x43}, 1 },
        { "ld b,h",    {0x44}, 1 },
        { "ld b,l",    {0x45}, 1 },
        { "ld b,(hl)", {0x46}, 1 },
        { "ld b,a",    {0x47}, 1 },
        { "ld c,b",    {0x48}, 1 },
        { "ld c,c",    {0x49}, 1 },
        { "ld c,d",    {0x4A}, 1 },
        { "ld c,e",    {0x4B}, 1 },
        { "ld c,h",    {0x4C}, 1 },
        { "ld c,l",    {0x4D}, 1 },
        { "ld c,(hl)", {0x4E}, 1 },
        { "ld c,a",    {0x4F}, 1 },
        { "ld d,b",    {0x50}, 1 },
        { "ld d,c",    {0x51}, 1 },
        { "ld d,d",    {0x52}, 1 },
        { "ld d,e",    {0x53}, 1 },
        { "ld d,h",    {0x54}, 1 },
        { "ld d,l",    {0x55}, 1 },
        { "ld d,(hl)", {0x56}, 1 },
        { "ld d,a",    {0x57}, 1 },
        { "ld e,b",    {0x58}, 1 },
        { "ld e,c",    {0x59}, 1 },
        { "ld e,d",    {0x5A}, 1 },
        { "ld e,e",    {0x5B}, 1 },
        { "ld e,h",    {0x5C}, 1 },
        { "ld e,l",    {0x5D}, 1 },
        { "ld e,(hl)", {0x5E}, 1 },
        { "ld e,a",    {0x5F}, 1 },
        { "ld h,b",    {0x60}, 1 },
        { "ld h,c",    {0x61}, 1 },
        { "ld h,d",    {0x62}, 1 },
        { "ld h,e",    {0x63}, 1 },
        { "ld h,h",    {0x64}, 1 },
        { "ld h,l",    {0x65}, 1 },
        { "ld h,(hl)", {0x66}, 1 },
        { "ld h,a",    {0x67}, 1 },
        { "ld l,b",    {0x68}, 1 },
        { "ld l,c",    {0x69}, 1 },
        { "ld l,d",    {0x6A}, 1 },
        { "ld l,e",    {0x6B}, 1 },
        { "ld l,h",    {0x6C}, 1 },
        { "ld l,l",    {0x6D}, 1 },
        { "ld l,(hl)", {0x6E}, 1 },
        { "ld l,a",    {0x6F}, 1 },
        { "ld (hl),b", {0x70}, 1 },
        { "ld (hl),c", {0x71}, 1 },
        { "ld (hl),d", {0x72}, 1 },
        { "ld (hl),e", {0x73}, 1 },
        { "ld (hl),h", {0x74}, 1 },
        { "ld (hl),l", {0x75}, 1 },
        /* 0x76 = HALT, skip */
        { "ld (hl),a", {0x77}, 1 },
        { "ld a,b",    {0x78}, 1 },
        { "ld a,c",    {0x79}, 1 },
        { "ld a,d",    {0x7A}, 1 },
        { "ld a,e",    {0x7B}, 1 },
        { "ld a,h",    {0x7C}, 1 },
        { "ld a,l",    {0x7D}, 1 },
        { "ld a,(hl)", {0x7E}, 1 },
        { "ld a,a",    {0x7F}, 1 },
    };
    failures += run_cases(ld_rr_cases,
                          (int)(sizeof(ld_rr_cases)/sizeof(ld_rr_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* LD r, d8 — all 8 registers including (hl)                          */
    /* ================================================================== */

    static const EncCase ld_r_d8_cases[] = {
        { "ld b,$10",    {0x06, 0x10}, 2 },
        { "ld c,$20",    {0x0E, 0x20}, 2 },
        { "ld d,$30",    {0x16, 0x30}, 2 },
        { "ld e,$40",    {0x1E, 0x40}, 2 },
        { "ld h,$50",    {0x26, 0x50}, 2 },
        { "ld l,$60",    {0x2E, 0x60}, 2 },
        { "ld (hl),$12", {0x36, 0x12}, 2 },
        { "ld a,$42",    {0x3E, 0x42}, 2 },
    };
    failures += run_cases(ld_r_d8_cases,
                          (int)(sizeof(ld_r_d8_cases)/sizeof(ld_r_d8_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* LD rr, d16                                                          */
    /* ================================================================== */

    static const EncCase ld_rr_d16_cases[] = {
        { "ld bc,$1234", {0x01, 0x34, 0x12}, 3 },
        { "ld de,$5678", {0x11, 0x78, 0x56}, 3 },
        { "ld hl,$ABCD", {0x21, 0xCD, 0xAB}, 3 },
        { "ld sp,$FFFF", {0x31, 0xFF, 0xFF}, 3 },
    };
    failures += run_cases(ld_rr_d16_cases,
                          (int)(sizeof(ld_rr_d16_cases)/sizeof(ld_rr_d16_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* LD indirect A loads/stores                                          */
    /* ================================================================== */

    static const EncCase ld_indirect_cases[] = {
        { "ld (bc),a",  {0x02}, 1 },
        { "ld (de),a",  {0x12}, 1 },
        { "ld (hl+),a", {0x22}, 1 },
        { "ld (hl-),a", {0x32}, 1 },
        { "ld a,(bc)",  {0x0A}, 1 },
        { "ld a,(de)",  {0x1A}, 1 },
        { "ld a,(hl+)", {0x2A}, 1 },
        { "ld a,(hl-)", {0x3A}, 1 },
        /* hli / hld aliases */
        { "ld (hli),a", {0x22}, 1 },
        { "ld (hld),a", {0x32}, 1 },
        { "ld a,(hli)", {0x2A}, 1 },
        { "ld a,(hld)", {0x3A}, 1 },
        /* absolute */
        { "ld a,($8000)", {0xFA, 0x00, 0x80}, 3 },
        { "ld ($C000),a", {0xEA, 0x00, 0xC0}, 3 },
        /* SP save */
        { "ld ($FF80),sp",{0x08, 0x80, 0xFF}, 3 },
        /* SP <- HL */
        { "ld sp,hl",    {0xF9}, 1 },
        /* HL <- SP+e8 */
        { "ld hl,sp+4",  {0xF8, 0x04}, 2 },
        { "ld hl,sp-2",  {0xF8, 0xFE}, 2 },   /* -2 as signed byte = 0xFE */
        /* (C), A and A, (C) */
        { "ld (c),a",    {0xE2}, 1 },
        { "ld a,(c)",    {0xF2}, 1 },
    };
    failures += run_cases(ld_indirect_cases,
                          (int)(sizeof(ld_indirect_cases)/sizeof(ld_indirect_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* LDH                                                                 */
    /* ================================================================== */

    static const EncCase ldh_cases[] = {
        { "ldh ($FF)  ,a", {0xE0, 0xFF}, 2 },
        { "ldh a,($FF)",   {0xF0, 0xFF}, 2 },
        { "ldh ($40),a",   {0xE0, 0x40}, 2 },
        { "ldh a,($40)",   {0xF0, 0x40}, 2 },
    };
    failures += run_cases(ldh_cases,
                          (int)(sizeof(ldh_cases)/sizeof(ldh_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* ADD HL, rp                                                          */
    /* ================================================================== */

    static const EncCase add_hl_cases[] = {
        { "add hl,bc", {0x09}, 1 },
        { "add hl,de", {0x19}, 1 },
        { "add hl,hl", {0x29}, 1 },
        { "add hl,sp", {0x39}, 1 },
    };
    failures += run_cases(add_hl_cases,
                          (int)(sizeof(add_hl_cases)/sizeof(add_hl_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* ADD SP, e8                                                          */
    /* ================================================================== */

    static const EncCase add_sp_cases[] = {
        { "add sp,4",   {0xE8, 0x04}, 2 },
        { "add sp,-2",  {0xE8, 0xFE}, 2 },
        { "add sp,$10", {0xE8, 0x10}, 2 },
    };
    failures += run_cases(add_sp_cases,
                          (int)(sizeof(add_sp_cases)/sizeof(add_sp_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* INC / DEC r (8-bit)                                                */
    /* ================================================================== */

    static const EncCase inc_r_cases[] = {
        { "inc b",    {0x04}, 1 },
        { "inc c",    {0x0C}, 1 },
        { "inc d",    {0x14}, 1 },
        { "inc e",    {0x1C}, 1 },
        { "inc h",    {0x24}, 1 },
        { "inc l",    {0x2C}, 1 },
        { "inc (hl)", {0x34}, 1 },
        { "inc a",    {0x3C}, 1 },
        { "dec b",    {0x05}, 1 },
        { "dec c",    {0x0D}, 1 },
        { "dec d",    {0x15}, 1 },
        { "dec e",    {0x1D}, 1 },
        { "dec h",    {0x25}, 1 },
        { "dec l",    {0x2D}, 1 },
        { "dec (hl)", {0x35}, 1 },
        { "dec a",    {0x3D}, 1 },
    };
    failures += run_cases(inc_r_cases,
                          (int)(sizeof(inc_r_cases)/sizeof(inc_r_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* INC / DEC rr (16-bit)                                              */
    /* ================================================================== */

    static const EncCase inc_rr_cases[] = {
        { "inc bc", {0x03}, 1 },
        { "inc de", {0x13}, 1 },
        { "inc hl", {0x23}, 1 },
        { "inc sp", {0x33}, 1 },
        { "dec bc", {0x0B}, 1 },
        { "dec de", {0x1B}, 1 },
        { "dec hl", {0x2B}, 1 },
        { "dec sp", {0x3B}, 1 },
    };
    failures += run_cases(inc_rr_cases,
                          (int)(sizeof(inc_rr_cases)/sizeof(inc_rr_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* ALU: ADD A, r / ADD A, d8                                          */
    /* y=0: ADD=0x80-0x87 / 0xC6                                          */
    /* ================================================================== */

    static const EncCase add_cases[] = {
        { "add a,b",    {0x80}, 1 },
        { "add a,c",    {0x81}, 1 },
        { "add a,d",    {0x82}, 1 },
        { "add a,e",    {0x83}, 1 },
        { "add a,h",    {0x84}, 1 },
        { "add a,l",    {0x85}, 1 },
        { "add a,(hl)", {0x86}, 1 },
        { "add a,a",    {0x87}, 1 },
        { "add a,$10",  {0xC6, 0x10}, 2 },
    };
    failures += run_cases(add_cases,
                          (int)(sizeof(add_cases)/sizeof(add_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* ALU: ADC, SUB, SBC, AND, XOR, OR, CP — one reg + d8 each          */
    /* ================================================================== */

    static const EncCase alu_cases[] = {
        /* ADC A,r (y=1: 0x88..0x8F / 0xCE) */
        { "adc a,b",    {0x88}, 1 },
        { "adc a,c",    {0x89}, 1 },
        { "adc a,d",    {0x8A}, 1 },
        { "adc a,e",    {0x8B}, 1 },
        { "adc a,h",    {0x8C}, 1 },
        { "adc a,l",    {0x8D}, 1 },
        { "adc a,(hl)", {0x8E}, 1 },
        { "adc a,a",    {0x8F}, 1 },
        { "adc a,$01",  {0xCE, 0x01}, 2 },
        /* SUB (y=2: 0x90..0x97 / 0xD6) */
        { "sub b",      {0x90}, 1 },
        { "sub c",      {0x91}, 1 },
        { "sub d",      {0x92}, 1 },
        { "sub e",      {0x93}, 1 },
        { "sub h",      {0x94}, 1 },
        { "sub l",      {0x95}, 1 },
        { "sub (hl)",   {0x96}, 1 },
        { "sub a",      {0x97}, 1 },
        { "sub $05",    {0xD6, 0x05}, 2 },
        /* also accept 2-operand form */
        { "sub a,b",    {0x90}, 1 },
        { "sub a,$05",  {0xD6, 0x05}, 2 },
        /* SBC (y=3: 0x98..0x9F / 0xDE) */
        { "sbc a,b",    {0x98}, 1 },
        { "sbc a,c",    {0x99}, 1 },
        { "sbc a,d",    {0x9A}, 1 },
        { "sbc a,e",    {0x9B}, 1 },
        { "sbc a,h",    {0x9C}, 1 },
        { "sbc a,l",    {0x9D}, 1 },
        { "sbc a,(hl)", {0x9E}, 1 },
        { "sbc a,a",    {0x9F}, 1 },
        { "sbc a,$02",  {0xDE, 0x02}, 2 },
        /* AND (y=4: 0xA0..0xA7 / 0xE6) */
        { "and b",      {0xA0}, 1 },
        { "and c",      {0xA1}, 1 },
        { "and d",      {0xA2}, 1 },
        { "and e",      {0xA3}, 1 },
        { "and h",      {0xA4}, 1 },
        { "and l",      {0xA5}, 1 },
        { "and (hl)",   {0xA6}, 1 },
        { "and a",      {0xA7}, 1 },
        { "and $FF",    {0xE6, 0xFF}, 2 },
        /* XOR (y=5: 0xA8..0xAF / 0xEE) */
        { "xor b",      {0xA8}, 1 },
        { "xor c",      {0xA9}, 1 },
        { "xor d",      {0xAA}, 1 },
        { "xor e",      {0xAB}, 1 },
        { "xor h",      {0xAC}, 1 },
        { "xor l",      {0xAD}, 1 },
        { "xor (hl)",   {0xAE}, 1 },
        { "xor a",      {0xAF}, 1 },
        { "xor $00",    {0xEE, 0x00}, 2 },
        /* OR (y=6: 0xB0..0xB7 / 0xF6) */
        { "or b",       {0xB0}, 1 },
        { "or c",       {0xB1}, 1 },
        { "or d",       {0xB2}, 1 },
        { "or e",       {0xB3}, 1 },
        { "or h",       {0xB4}, 1 },
        { "or l",       {0xB5}, 1 },
        { "or (hl)",    {0xB6}, 1 },
        { "or a",       {0xB7}, 1 },
        { "or $80",     {0xF6, 0x80}, 2 },
        /* CP (y=7: 0xB8..0xBF / 0xFE) */
        { "cp b",       {0xB8}, 1 },
        { "cp c",       {0xB9}, 1 },
        { "cp d",       {0xBA}, 1 },
        { "cp e",       {0xBB}, 1 },
        { "cp h",       {0xBC}, 1 },
        { "cp l",       {0xBD}, 1 },
        { "cp (hl)",    {0xBE}, 1 },
        { "cp a",       {0xBF}, 1 },
        { "cp $42",     {0xFE, 0x42}, 2 },
    };
    failures += run_cases(alu_cases,
                          (int)(sizeof(alu_cases)/sizeof(alu_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* CB: RLC RRC RL RR SLA SRA SWAP SRL — all 8 regs                   */
    /* CB opcode = 0xCB, then: y<<3 | z (x=0)                            */
    /* ================================================================== */

    /* RLC r — y=0, opcode = 0x00 | z */
    static const EncCase cb_rlc[] = {
        { "rlc b",    {0xCB, 0x00}, 2 },
        { "rlc c",    {0xCB, 0x01}, 2 },
        { "rlc d",    {0xCB, 0x02}, 2 },
        { "rlc e",    {0xCB, 0x03}, 2 },
        { "rlc h",    {0xCB, 0x04}, 2 },
        { "rlc l",    {0xCB, 0x05}, 2 },
        { "rlc (hl)", {0xCB, 0x06}, 2 },
        { "rlc a",    {0xCB, 0x07}, 2 },
    };
    failures += run_cases(cb_rlc, (int)(sizeof(cb_rlc)/sizeof(cb_rlc[0])),
                          0x0000, NULL);

    /* RRC r — y=1 => 0x08..0x0F */
    static const EncCase cb_rrc[] = {
        { "rrc b",    {0xCB, 0x08}, 2 },
        { "rrc c",    {0xCB, 0x09}, 2 },
        { "rrc d",    {0xCB, 0x0A}, 2 },
        { "rrc e",    {0xCB, 0x0B}, 2 },
        { "rrc h",    {0xCB, 0x0C}, 2 },
        { "rrc l",    {0xCB, 0x0D}, 2 },
        { "rrc (hl)", {0xCB, 0x0E}, 2 },
        { "rrc a",    {0xCB, 0x0F}, 2 },
    };
    failures += run_cases(cb_rrc, (int)(sizeof(cb_rrc)/sizeof(cb_rrc[0])),
                          0x0000, NULL);

    /* RL r — y=2 => 0x10..0x17 */
    static const EncCase cb_rl[] = {
        { "rl b",    {0xCB, 0x10}, 2 },
        { "rl c",    {0xCB, 0x11}, 2 },
        { "rl d",    {0xCB, 0x12}, 2 },
        { "rl e",    {0xCB, 0x13}, 2 },
        { "rl h",    {0xCB, 0x14}, 2 },
        { "rl l",    {0xCB, 0x15}, 2 },
        { "rl (hl)", {0xCB, 0x16}, 2 },
        { "rl a",    {0xCB, 0x17}, 2 },
    };
    failures += run_cases(cb_rl, (int)(sizeof(cb_rl)/sizeof(cb_rl[0])),
                          0x0000, NULL);

    /* RR r — y=3 => 0x18..0x1F */
    static const EncCase cb_rr[] = {
        { "rr b",    {0xCB, 0x18}, 2 },
        { "rr c",    {0xCB, 0x19}, 2 },
        { "rr d",    {0xCB, 0x1A}, 2 },
        { "rr e",    {0xCB, 0x1B}, 2 },
        { "rr h",    {0xCB, 0x1C}, 2 },
        { "rr l",    {0xCB, 0x1D}, 2 },
        { "rr (hl)", {0xCB, 0x1E}, 2 },
        { "rr a",    {0xCB, 0x1F}, 2 },
    };
    failures += run_cases(cb_rr, (int)(sizeof(cb_rr)/sizeof(cb_rr[0])),
                          0x0000, NULL);

    /* SLA r — y=4 => 0x20..0x27 */
    static const EncCase cb_sla[] = {
        { "sla b",    {0xCB, 0x20}, 2 },
        { "sla c",    {0xCB, 0x21}, 2 },
        { "sla d",    {0xCB, 0x22}, 2 },
        { "sla e",    {0xCB, 0x23}, 2 },
        { "sla h",    {0xCB, 0x24}, 2 },
        { "sla l",    {0xCB, 0x25}, 2 },
        { "sla (hl)", {0xCB, 0x26}, 2 },
        { "sla a",    {0xCB, 0x27}, 2 },
    };
    failures += run_cases(cb_sla, (int)(sizeof(cb_sla)/sizeof(cb_sla[0])),
                          0x0000, NULL);

    /* SRA r — y=5 => 0x28..0x2F */
    static const EncCase cb_sra[] = {
        { "sra b",    {0xCB, 0x28}, 2 },
        { "sra c",    {0xCB, 0x29}, 2 },
        { "sra d",    {0xCB, 0x2A}, 2 },
        { "sra e",    {0xCB, 0x2B}, 2 },
        { "sra h",    {0xCB, 0x2C}, 2 },
        { "sra l",    {0xCB, 0x2D}, 2 },
        { "sra (hl)", {0xCB, 0x2E}, 2 },
        { "sra a",    {0xCB, 0x2F}, 2 },
    };
    failures += run_cases(cb_sra, (int)(sizeof(cb_sra)/sizeof(cb_sra[0])),
                          0x0000, NULL);

    /* SWAP r — y=6 => 0x30..0x37 */
    static const EncCase cb_swap[] = {
        { "swap b",    {0xCB, 0x30}, 2 },
        { "swap c",    {0xCB, 0x31}, 2 },
        { "swap d",    {0xCB, 0x32}, 2 },
        { "swap e",    {0xCB, 0x33}, 2 },
        { "swap h",    {0xCB, 0x34}, 2 },
        { "swap l",    {0xCB, 0x35}, 2 },
        { "swap (hl)", {0xCB, 0x36}, 2 },
        { "swap a",    {0xCB, 0x37}, 2 },
    };
    failures += run_cases(cb_swap, (int)(sizeof(cb_swap)/sizeof(cb_swap[0])),
                          0x0000, NULL);

    /* SRL r — y=7 => 0x38..0x3F */
    static const EncCase cb_srl[] = {
        { "srl b",    {0xCB, 0x38}, 2 },
        { "srl c",    {0xCB, 0x39}, 2 },
        { "srl d",    {0xCB, 0x3A}, 2 },
        { "srl e",    {0xCB, 0x3B}, 2 },
        { "srl h",    {0xCB, 0x3C}, 2 },
        { "srl l",    {0xCB, 0x3D}, 2 },
        { "srl (hl)", {0xCB, 0x3E}, 2 },
        { "srl a",    {0xCB, 0x3F}, 2 },
    };
    failures += run_cases(cb_srl, (int)(sizeof(cb_srl)/sizeof(cb_srl[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* CB: BIT b, r — x=1: opcode = 0x40 | (b<<3) | r                   */
    /* ================================================================== */

    /* All 8 bits on register B (z=0) */
    static const EncCase bit_b_cases[] = {
        { "bit 0,b",    {0xCB, 0x40}, 2 },
        { "bit 1,b",    {0xCB, 0x48}, 2 },
        { "bit 2,b",    {0xCB, 0x50}, 2 },
        { "bit 3,b",    {0xCB, 0x58}, 2 },
        { "bit 4,b",    {0xCB, 0x60}, 2 },
        { "bit 5,b",    {0xCB, 0x68}, 2 },
        { "bit 6,b",    {0xCB, 0x70}, 2 },
        { "bit 7,b",    {0xCB, 0x78}, 2 },
        /* Spot checks on other regs */
        { "bit 0,c",    {0xCB, 0x41}, 2 },
        { "bit 7,(hl)", {0xCB, 0x7E}, 2 },
        { "bit 7,h",    {0xCB, 0x7C}, 2 },   /* from Pan Docs example */
        { "bit 3,a",    {0xCB, 0x5F}, 2 },
        { "bit 0,(hl)", {0xCB, 0x46}, 2 },
    };
    failures += run_cases(bit_b_cases,
                          (int)(sizeof(bit_b_cases)/sizeof(bit_b_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* CB: RES b, r — x=2: opcode = 0x80 | (b<<3) | r                   */
    /* ================================================================== */

    static const EncCase res_cases[] = {
        { "res 0,b",    {0xCB, 0x80}, 2 },
        { "res 1,b",    {0xCB, 0x88}, 2 },
        { "res 2,b",    {0xCB, 0x90}, 2 },
        { "res 3,b",    {0xCB, 0x98}, 2 },
        { "res 4,b",    {0xCB, 0xA0}, 2 },
        { "res 5,b",    {0xCB, 0xA8}, 2 },
        { "res 6,b",    {0xCB, 0xB0}, 2 },
        { "res 7,b",    {0xCB, 0xB8}, 2 },
        { "res 0,(hl)", {0xCB, 0x86}, 2 },
        { "res 7,a",    {0xCB, 0xBF}, 2 },
        { "res 3,c",    {0xCB, 0x99}, 2 },
    };
    failures += run_cases(res_cases,
                          (int)(sizeof(res_cases)/sizeof(res_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* CB: SET b, r — x=3: opcode = 0xC0 | (b<<3) | r                   */
    /* ================================================================== */

    static const EncCase set_cases[] = {
        { "set 0,b",    {0xCB, 0xC0}, 2 },
        { "set 1,b",    {0xCB, 0xC8}, 2 },
        { "set 2,b",    {0xCB, 0xD0}, 2 },
        { "set 3,b",    {0xCB, 0xD8}, 2 },
        { "set 4,b",    {0xCB, 0xE0}, 2 },
        { "set 5,b",    {0xCB, 0xE8}, 2 },
        { "set 6,b",    {0xCB, 0xF0}, 2 },
        { "set 7,b",    {0xCB, 0xF8}, 2 },
        { "set 0,(hl)", {0xCB, 0xC6}, 2 },
        { "set 7,a",    {0xCB, 0xFF}, 2 },
        { "set 4,e",    {0xCB, 0xE3}, 2 },
    };
    failures += run_cases(set_cases,
                          (int)(sizeof(set_cases)/sizeof(set_cases[0])),
                          0x0000, NULL);

    /* ================================================================== */
    /* Report                                                              */
    /* ================================================================== */

    /* Merge table failures into the t_fail counter */
    t_fail += failures;
    t_count += failures; /* approximate: count table failures as assertions */

    TEST_MAIN_END();
}
