/*
 * test_asm_e2e.c — end-to-end assembler tests (Task 4 + Task 5)
 *
 * Task 4: asm_assemble() producing correct ROM bytes, symbol addresses,
 *         linemap, prov_line, forward references, DB/DW/DS, and error cases.
 * Task 5: ROMX multi-bank, cartridge header (logo/checksums), INCBIN, INCLUDE,
 *         and emulator load test.
 */

#include "test.h"
#include "../src/asm/asm.h"
#include "../src/gb/gb.h"

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
 * Task 5, Test 16: ROMX multi-bank section
 *
 * SECTION "x", ROMX, BANK[2] places code at cpu addr 0x4000, bank 2.
 * Linear offset = 2 * 0x4000 + (0x4000 - 0x4000) = 0x8000.
 * Symbol bank should be 2.  rom_size >= 0x10000 (64 KB), ROM size code 0x01.
 * ======================================================================= */
static void test_romx_bank(void)
{
    const char *src =
        "    SECTION \"x\", ROMX, BANK[2]\n"   /* line 1 */
        "BankLabel:\n"                           /* line 2 */
        "    nop\n"                              /* line 3 */
        "    ret\n";                             /* line 4 */

    AsmResult r = asm_assemble(src, "test_romx.asm");

    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.rom_size >= 0x10000u);

    /* nop at linear offset 0x8000, ret at 0x8001 */
    ASSERT_EQ(r.rom[0x8000], 0x00);  /* nop */
    ASSERT_EQ(r.rom[0x8001], 0xC9);  /* ret */

    /* Symbol BankLabel -> bank 2, addr 0x4000, off 0x8000 */
    const AsmSymbol *lbl = find_sym(&r, "BankLabel");
    ASSERT_TRUE(lbl != NULL);
    ASSERT_EQ(lbl->bank, 2);
    ASSERT_EQ(lbl->addr, 0x4000);
    ASSERT_EQ(lbl->off,  0x8000u);

    /* ROM size code at 0x0148: log2(rom_size/32KB).  64KB = code 0x01 */
    ASSERT_EQ(r.rom[0x0148], 0x01);

    asm_free(&r);
}

/* =========================================================================
 * Task 5, Test 17: cartridge header correctness
 * ======================================================================= */

/* Canonical Nintendo logo (48 bytes from Pan Docs) */
static const uint8_t EXPECTED_LOGO[48] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
    0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E
};

static void test_cartridge_header(void)
{
    /*
     * A minimal program in a single ROM0 section:
     *  - DS  0x100 zeros to pad from 0x0000 to 0x0100
     *  - Entry at 0x0100: nop; jp Main (4 bytes: fits before logo at 0x0104)
     *  - DS  0x4C more bytes to advance past the header to 0x0150
     *    (assembler overwrites 0x0104-0x014F with logo+checksums, so this padding
     *     will be overwritten anyway — we just need cur_addr at 0x0150)
     *  - Main at 0x0150: halt loop
     *
     * The header (logo, checksums) is written by the assembler after assembly.
     */
    const char *src =
        "    SECTION \"boot\", ROM0\n"
        "    ds $100\n"                  /* 0x0000..0x00FF: 256 zero bytes */
        "    nop\n"                      /* 0x0100: nop */
        "    jp Main\n"                  /* 0x0101..0x0103: jp Main */
        "    ds $150 - $\n"             /* 0x0104..0x014F: pad (overwritten by header) */
        "Main:\n"                        /* 0x0150 */
        "Loop:\n"
        "    halt\n"                     /* 0x0150: halt */
        "    jr Loop\n"                  /* 0x0151..0x0152: jr -3 */
        ;

    AsmResult r = asm_assemble(src, "hdrtest.asm");

    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.rom_size >= 0x8000u);

    /* Nintendo logo at 0x0104..0x0133 */
    ASSERT_TRUE(memcmp(&r.rom[0x0104], EXPECTED_LOGO, 48) == 0);

    /* Header checksum at 0x014D:
     * x = 0; for addr 0x0134..0x014C: x = x - rom[addr] - 1; result & 0xFF */
    {
        uint8_t x = 0;
        for (int a = 0x0134; a <= 0x014C; a++) {
            x = (uint8_t)(x - r.rom[a] - 1u);
        }
        ASSERT_EQ(r.rom[0x014D], x);
    }

    /* Global checksum at 0x014E-0x014F (big-endian, skip 0x014E/0x014F themselves) */
    {
        uint32_t gsum = 0;
        for (size_t b = 0; b < r.rom_size; b++) {
            if (b == 0x014Eu || b == 0x014Fu) continue;
            gsum += r.rom[b];
        }
        uint8_t hi = (uint8_t)((gsum >> 8) & 0xFFu);
        uint8_t lo = (uint8_t)(gsum & 0xFFu);
        ASSERT_EQ(r.rom[0x014E], hi);
        ASSERT_EQ(r.rom[0x014F], lo);
    }

    /* Emulator load test: gb_load_rom + gb_reset + run a few frames */
    {
        GB *gb = gb_new();
        ASSERT_TRUE(gb != NULL);

        bool loaded = gb_load_rom(gb, r.rom, r.rom_size);
        ASSERT_TRUE(loaded);

        gb_reset(gb);

        /* Run ~5 frames worth of steps without crashing */
        int frames = 0;
        int steps  = 0;
        while (frames < 5 && steps < 500000) {
            int tc = gb_step(gb);
            gb_tick(gb, tc);
            gb_ppu_tick(gb, tc);
            if (gb->frame_ready) {
                gb->frame_ready = false;
                frames++;
            }
            steps++;
        }
        ASSERT_TRUE(frames > 0 || steps > 0); /* didn't crash */

        gb_free(gb);
    }

    asm_free(&r);
}

/* =========================================================================
 * Task 5, Test 18: INCBIN — raw bytes + provenance marked as asset (-2)
 * ======================================================================= */
static void test_incbin(void)
{
    /* Write a temp file with known bytes */
    const char *tmpfile = "/tmp/test_asm_incbin_data.bin";
    {
        FILE *fh = fopen(tmpfile, "wb");
        ASSERT_TRUE(fh != NULL);
        if (!fh) return;
        uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF };
        fwrite(data, 1, sizeof(data), fh);
        fclose(fh);
    }

    char src[512];
    snprintf(src, sizeof(src),
        "    nop\n"                              /* line 1: 1 byte @ 0x0150 */
        "    incbin \"%s\"\n"                    /* line 2: 4 bytes @ 0x0151 */
        "    ret\n",                             /* line 3: 1 byte @ 0x0155 */
        tmpfile);

    AsmResult r = asm_assemble(src, "test_incbin.asm");

    ASSERT_TRUE(r.ok);

    ASSERT_EQ(r.rom[0x0150], 0x00);  /* nop */

    /* incbin bytes */
    ASSERT_EQ(r.rom[0x0151], 0xDE);
    ASSERT_EQ(r.rom[0x0152], 0xAD);
    ASSERT_EQ(r.rom[0x0153], 0xBE);
    ASSERT_EQ(r.rom[0x0154], 0xEF);

    ASSERT_EQ(r.rom[0x0155], 0xC9);  /* ret */

    /* Provenance: nop = source line 1, incbin bytes = PROV_ASSET (-2), ret = 3 */
    ASSERT_EQ(r.prov_line[0x0150], 1);
    ASSERT_EQ(r.prov_line[0x0151], -2);   /* PROV_ASSET sentinel */
    ASSERT_EQ(r.prov_line[0x0152], -2);
    ASSERT_EQ(r.prov_line[0x0153], -2);
    ASSERT_EQ(r.prov_line[0x0154], -2);
    ASSERT_EQ(r.prov_line[0x0155], 3);

    asm_free(&r);
    remove(tmpfile);
}

/* =========================================================================
 * Task 5, Test 19: INCLUDE — inline assembly from external file
 * ======================================================================= */
static void test_include(void)
{
    /* Write a temp included file with a label + instruction */
    const char *tmpfile = "/tmp/test_asm_include_lib.asm";
    {
        FILE *fh = fopen(tmpfile, "w");
        ASSERT_TRUE(fh != NULL);
        if (!fh) return;
        /* Include file: a label IncLabel and a nop */
        fprintf(fh, "IncLabel:\n    nop\n");
        fclose(fh);
    }

    char src[512];
    snprintf(src, sizeof(src),
        "    nop\n"                  /* line 1: nop @ 0x0150 */
        "    include \"%s\"\n"       /* line 2: include -> IncLabel at 0x0151, nop @ 0x0151 */
        "    ret\n",                 /* after include: ret */
        tmpfile);

    AsmResult r = asm_assemble(src, "test_include.asm");

    ASSERT_TRUE(r.ok);

    /* nop from main file */
    ASSERT_EQ(r.rom[0x0150], 0x00);  /* nop */

    /* Included label IncLabel should be at 0x0151 */
    const AsmSymbol *inc_lbl = find_sym(&r, "IncLabel");
    ASSERT_TRUE(inc_lbl != NULL);
    if (inc_lbl) {
        ASSERT_EQ(inc_lbl->addr, 0x0151);
    }

    /* nop from included file at 0x0151 */
    ASSERT_EQ(r.rom[0x0151], 0x00);  /* nop from included file */

    /* ret from main file after include */
    ASSERT_EQ(r.rom[0x0152], 0xC9);  /* ret */

    asm_free(&r);
    remove(tmpfile);
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

    /* Task 5 tests */
    test_romx_bank();
    test_cartridge_header();
    test_incbin();
    test_include();

    TEST_MAIN_END();
}
