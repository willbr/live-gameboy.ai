/*
 * test_asm_e2e.c — end-to-end assembler tests (Task 4: single ROM0 section)
 *
 * Tests: asm_assemble() producing correct ROM bytes, symbol addresses,
 * linemap, prov_line, forward references, DB/DW/DS, and error cases.
 */

#include "test.h"
#include "../src/asm/asm.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Helper: find a symbol in AsmResult by name
 * ----------------------------------------------------------------------- */
static const AsmSymbol *find_sym(const AsmResult *r, const char *name)
{
    for (int i = 0; i < r->nsyms; i++) {
        if (strcmp(r->syms[i].name, name) == 0)
            return &r->syms[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Helper: find linemap offset for a given source line (-1 if not found)
 * ----------------------------------------------------------------------- */
static int find_linemap(const AsmResult *r, int line)
{
    for (int i = 0; i < r->nlines; i++) {
        if (r->linemap[i].line == line)
            return (int)r->linemap[i].off;
    }
    return -1;
}

/* =========================================================================
 * Test 1: basic NOP + RET, no section directive (default ROM0 @ 0x0150)
 * ======================================================================= */
static void test_basic_nop_ret(void)
{
    const char *src =
        "    nop\n"   /* line 1 */
        "    ret\n";  /* line 2 */

    AsmResult r = asm_assemble(src, "test_basic.asm");

    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.rom != NULL);
    ASSERT_TRUE(r.rom_size >= 0x8000);

    /* Default org is 0x0150 */
    ASSERT_EQ(r.rom[0x0150], 0x00);   /* nop */
    ASSERT_EQ(r.rom[0x0151], 0xC9);   /* ret */

    /* prov_line */
    ASSERT_TRUE(r.prov_line != NULL);
    ASSERT_EQ(r.prov_line[0x0150], 1);  /* nop at 0x150 from line 1 */
    ASSERT_EQ(r.prov_line[0x0151], 2);  /* ret at 0x151 from line 2 */

    /* linemap */
    ASSERT_EQ(find_linemap(&r, 1), 0x0150);
    ASSERT_EQ(find_linemap(&r, 2), 0x0151);

    asm_free(&r);
}

/* =========================================================================
 * Test 2: label definition, address, and LD with immediate
 * ======================================================================= */
static void test_label_and_ld(void)
{
    /* No SECTION: default ROM0 @ 0x0150 */
    const char *src =
        "Start:\n"           /* line 1 — label at 0x0150 */
        "    ld a, $42\n"    /* line 2 — 2 bytes: 3E 42 */
        "End:\n"             /* line 3 — label at 0x0152 */
        "    nop\n";         /* line 4 */

    AsmResult r = asm_assemble(src, "test_labels.asm");

    ASSERT_TRUE(r.ok);

    /* Bytes */
    ASSERT_EQ(r.rom[0x0150], 0x3E);  /* ld a,$42 -> 3E */
    ASSERT_EQ(r.rom[0x0151], 0x42);  /* ld a,$42 -> 42 */
    ASSERT_EQ(r.rom[0x0152], 0x00);  /* nop */

    /* Symbols */
    const AsmSymbol *start = find_sym(&r, "Start");
    const AsmSymbol *end   = find_sym(&r, "End");

    ASSERT_TRUE(start != NULL);
    ASSERT_TRUE(end   != NULL);

    ASSERT_EQ(start->bank, 0);
    ASSERT_EQ(start->addr, 0x0150);
    ASSERT_EQ(start->off,  0x0150);

    ASSERT_EQ(end->bank, 0);
    ASSERT_EQ(end->addr, 0x0152);

    asm_free(&r);
}

/* =========================================================================
 * Test 3: forward JR to a later label
 * ======================================================================= */
static void test_forward_jr(void)
{
    /*
     * Program:
     *   line 1: jr Target   -> 18 xx  where xx = Target - (jr_addr+2)
     *   line 2: nop          -> 0x0152  (1 byte)
     *   line 3: Target: nop  -> 0x0153
     *
     *   jr at 0x0150..0x0151: opcode=0x18, disp=Target-(0x0150+2)=0x0153-0x0152=1
     */
    const char *src =
        "    jr Target\n"  /* line 1 */
        "    nop\n"        /* line 2 */
        "Target:\n"        /* line 3 */
        "    nop\n";       /* line 4 */

    AsmResult r = asm_assemble(src, "test_fwd_jr.asm");

    ASSERT_TRUE(r.ok);

    ASSERT_EQ(r.rom[0x0150], 0x18);  /* jr opcode */
    ASSERT_EQ(r.rom[0x0151], 0x01);  /* displacement +1 */
    ASSERT_EQ(r.rom[0x0152], 0x00);  /* nop */
    ASSERT_EQ(r.rom[0x0153], 0x00);  /* nop at Target */

    const AsmSymbol *tgt = find_sym(&r, "Target");
    ASSERT_TRUE(tgt != NULL);
    ASSERT_EQ(tgt->addr, 0x0153);

    asm_free(&r);
}

/* =========================================================================
 * Test 4: forward JP to a later label
 * ======================================================================= */
static void test_forward_jp(void)
{
    /*
     *   line 1: jp Done      -> C3 lo hi  (3 bytes @ 0x0150)
     *   line 2: ld b, c      -> 41        (1 byte  @ 0x0153)
     *   line 3: Done:        -> label @ 0x0154
     *   line 4: ret          -> C9        (1 byte  @ 0x0154)
     *
     *   jp target = 0x0154 -> C3 54 01
     */
    const char *src =
        "    jp Done\n"    /* line 1 */
        "    ld b, c\n"    /* line 2 */
        "Done:\n"          /* line 3 */
        "    ret\n";       /* line 4 */

    AsmResult r = asm_assemble(src, "test_fwd_jp.asm");

    ASSERT_TRUE(r.ok);

    ASSERT_EQ(r.rom[0x0150], 0xC3);  /* jp opcode */
    ASSERT_EQ(r.rom[0x0151], 0x54);  /* Done lo */
    ASSERT_EQ(r.rom[0x0152], 0x01);  /* Done hi */
    ASSERT_EQ(r.rom[0x0153], 0x41);  /* ld b,c */
    ASSERT_EQ(r.rom[0x0154], 0xC9);  /* ret */

    const AsmSymbol *done = find_sym(&r, "Done");
    ASSERT_TRUE(done != NULL);
    ASSERT_EQ(done->addr, 0x0154);

    asm_free(&r);
}

/* =========================================================================
 * Test 5: DB directive (bytes, string)
 * ======================================================================= */
static void test_db(void)
{
    /*
     *   line 1: db $01, $02, $03   -> 01 02 03 @ 0x0150
     *   line 2: db "AB"            -> 41 42    @ 0x0153
     *   line 3: nop                -> 00       @ 0x0155
     */
    const char *src =
        "    db $01, $02, $03\n"  /* line 1 */
        "    db \"AB\"\n"          /* line 2 */
        "    nop\n";              /* line 3 */

    AsmResult r = asm_assemble(src, "test_db.asm");

    ASSERT_TRUE(r.ok);

    ASSERT_EQ(r.rom[0x0150], 0x01);
    ASSERT_EQ(r.rom[0x0151], 0x02);
    ASSERT_EQ(r.rom[0x0152], 0x03);
    ASSERT_EQ(r.rom[0x0153], 0x41);  /* 'A' */
    ASSERT_EQ(r.rom[0x0154], 0x42);  /* 'B' */
    ASSERT_EQ(r.rom[0x0155], 0x00);  /* nop */

    /* prov_line for DB bytes */
    ASSERT_EQ(r.prov_line[0x0150], 1);  /* db $01 from line 1 */
    ASSERT_EQ(r.prov_line[0x0153], 2);  /* db 'A' from line 2 */

    asm_free(&r);
}

/* =========================================================================
 * Test 6: DW directive (little-endian words)
 * ======================================================================= */
static void test_dw(void)
{
    /*
     *   line 1: dw $1234, $ABCD
     *   offset 0x0150: 34 12
     *   offset 0x0152: CD AB
     */
    const char *src =
        "    dw $1234, $ABCD\n";

    AsmResult r = asm_assemble(src, "test_dw.asm");

    ASSERT_TRUE(r.ok);

    ASSERT_EQ(r.rom[0x0150], 0x34);
    ASSERT_EQ(r.rom[0x0151], 0x12);
    ASSERT_EQ(r.rom[0x0152], 0xCD);
    ASSERT_EQ(r.rom[0x0153], 0xAB);

    asm_free(&r);
}

/* =========================================================================
 * Test 7: DS directive (fill N bytes)
 * ======================================================================= */
static void test_ds(void)
{
    /*
     *   line 1: ds 4            -> 00 00 00 00 @ 0x0150
     *   line 2: ds 2, $FF       -> FF FF       @ 0x0154
     *   line 3: nop             -> 00          @ 0x0156
     */
    const char *src =
        "    ds 4\n"       /* line 1 */
        "    ds 2, $FF\n"  /* line 2 */
        "    nop\n";       /* line 3 */

    AsmResult r = asm_assemble(src, "test_ds.asm");

    ASSERT_TRUE(r.ok);

    ASSERT_EQ(r.rom[0x0150], 0x00);
    ASSERT_EQ(r.rom[0x0153], 0x00);
    ASSERT_EQ(r.rom[0x0154], 0xFF);
    ASSERT_EQ(r.rom[0x0155], 0xFF);
    ASSERT_EQ(r.rom[0x0156], 0x00);  /* nop */

    asm_free(&r);
}

/* =========================================================================
 * Test 8: EQU constant and use in expression
 * ======================================================================= */
static void test_equ(void)
{
    const char *src =
        "DELAY EQU $10\n"      /* line 1 */
        "    ld a, DELAY\n";   /* line 2: 3E 10 */

    AsmResult r = asm_assemble(src, "test_equ.asm");

    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rom[0x0150], 0x3E);
    ASSERT_EQ(r.rom[0x0151], 0x10);

    const AsmSymbol *d = find_sym(&r, "DELAY");
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ(d->value, 0x10);

    asm_free(&r);
}

/* =========================================================================
 * Test 9: SECTION directive (explicit ROM0 starts at 0x0000)
 * ======================================================================= */
static void test_section(void)
{
    const char *src =
        "    SECTION \"main\", ROM0\n"  /* line 1 */
        "    nop\n"                     /* line 2 */
        "    ret\n";                    /* line 3 */

    AsmResult r = asm_assemble(src, "test_section.asm");

    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rom[0x0000], 0x00);  /* nop */
    ASSERT_EQ(r.rom[0x0001], 0xC9);  /* ret */

    asm_free(&r);
}

/* =========================================================================
 * Test 10: local labels scoped to last global label
 * ======================================================================= */
static void test_local_labels(void)
{
    /*
     *   Main:            ; at 0x0150
     *   .loop:           ; at 0x0150
     *       nop          ; at 0x0150 (1 byte)
     *       jr .loop     ; at 0x0151 (2 bytes): disp = 0x0150-(0x0151+2) = -3 = 0xFD
     */
    const char *src =
        "Main:\n"         /* line 1 */
        ".loop:\n"        /* line 2 */
        "    nop\n"       /* line 3 */
        "    jr .loop\n"; /* line 4 */

    AsmResult r = asm_assemble(src, "test_locals.asm");

    ASSERT_TRUE(r.ok);

    ASSERT_EQ(r.rom[0x0150], 0x00);  /* nop */
    ASSERT_EQ(r.rom[0x0151], 0x18);  /* jr opcode */
    ASSERT_EQ((uint8_t)r.rom[0x0152], 0xFD);  /* displacement -3 */

    asm_free(&r);
}

/* =========================================================================
 * Test 11: undefined symbol -> ok==false or diag emitted
 * ======================================================================= */
static void test_undefined_symbol(void)
{
    const char *src =
        "    jp Undefined\n";

    AsmResult r = asm_assemble(src, "test_undef.asm");

    /* Should produce a diag or fail */
    ASSERT_TRUE(!r.ok || r.ndiags > 0);

    asm_free(&r);
}

/* =========================================================================
 * Test 12: prov_line for every byte of a 3-byte instruction
 * ======================================================================= */
static void test_prov_line_multi_byte(void)
{
    /*
     *   line 1: ld hl, $BEEF   -> 21 EF BE  (3 bytes @ 0x0150)
     */
    const char *src =
        "    ld hl, $BEEF\n";

    AsmResult r = asm_assemble(src, "test_prov.asm");

    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rom[0x0150], 0x21);
    ASSERT_EQ(r.rom[0x0151], 0xEF);
    ASSERT_EQ(r.rom[0x0152], 0xBE);

    ASSERT_EQ(r.prov_line[0x0150], 1);
    ASSERT_EQ(r.prov_line[0x0151], 1);
    ASSERT_EQ(r.prov_line[0x0152], 1);

    asm_free(&r);
}

/* =========================================================================
 * Test 13: DW with a forward label
 * ======================================================================= */
static void test_dw_label(void)
{
    /*
     *   SECTION "x", ROM0
     *   dw Target      -> little-endian addr of Target @ 0x0000
     *   Target: nop    -> Target at 0x0002
     *   bytes 0x0000: 02 00
     */
    const char *src =
        "    SECTION \"x\", ROM0\n"  /* line 1 */
        "    dw Target\n"            /* line 2 */
        "Target:\n"                  /* line 3 */
        "    nop\n";                 /* line 4 */

    AsmResult r = asm_assemble(src, "test_dw_label.asm");

    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rom[0x0000], 0x02);  /* Target lo */
    ASSERT_EQ(r.rom[0x0001], 0x00);  /* Target hi */

    const AsmSymbol *t = find_sym(&r, "Target");
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(t->addr, 0x0002);

    asm_free(&r);
}

/* =========================================================================
 * Test 14: rom_size is always >= 0x8000
 * ======================================================================= */
static void test_rom_min_size(void)
{
    const char *src = "    nop\n";
    AsmResult r = asm_assemble(src, "test_romsize.asm");
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.rom_size >= 0x8000);
    asm_free(&r);
}

/* =========================================================================
 * Test 15: conditional jr nz to forward label
 * ======================================================================= */
static void test_jr_cc(void)
{
    /*
     *   jr nz, Fwd   ; 0x0150: 20 01 (disp=+1)
     *   nop          ; 0x0152: 00
     *   Fwd: nop     ; 0x0153: 00
     */
    const char *src =
        "    jr nz, Fwd\n"  /* line 1 */
        "    nop\n"         /* line 2 */
        "Fwd:\n"            /* line 3 */
        "    nop\n";        /* line 4 */

    AsmResult r = asm_assemble(src, "test_jr_cc.asm");

    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rom[0x0150], 0x20);  /* jr nz opcode */
    ASSERT_EQ(r.rom[0x0151], 0x01);  /* displacement +1 */

    asm_free(&r);
}

/* =========================================================================
 * Main
 * ======================================================================= */
int main(void)
{
    test_basic_nop_ret();
    test_label_and_ld();
    test_forward_jr();
    test_forward_jp();
    test_db();
    test_dw();
    test_ds();
    test_equ();
    test_section();
    test_local_labels();
    test_undefined_symbol();
    test_prov_line_multi_byte();
    test_dw_label();
    test_rom_min_size();
    test_jr_cc();

    TEST_MAIN_END();
}
