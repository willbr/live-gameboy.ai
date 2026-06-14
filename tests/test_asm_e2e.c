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
    /* With layout padding: End is placed in its own 32-byte slot after Start's
     * 32-byte slot (0x0150+32=0x0170). */
    ASSERT_EQ(end->addr, 0x0170);

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
     *   line 3: Target: nop  -> preamble ends at 0x0153; layout places Target at 0x0160
     *
     *   With layout padding: preamble (jr+nop) = 3 bytes ending at 0x0152.
     *   Target's preamble end = 0x0153. round_up(0x0153,16) = 0x0160.
     *   jr disp = 0x0160 - (0x0150+2) = 14 = 0x0E.
     */
    const char *src =
        "    jr Target\n"  /* line 1 */
        "    nop\n"        /* line 2 */
        "Target:\n"        /* line 3 */
        "    nop\n";       /* line 4 */

    AsmResult r = asm_assemble(src, "test_fwd_jr.asm");

    ASSERT_TRUE(r.ok);

    ASSERT_EQ(r.rom[0x0150], 0x18);  /* jr opcode */
    ASSERT_EQ(r.rom[0x0151], 0x0E);  /* displacement +14 (Target at 0x0160) */
    ASSERT_EQ(r.rom[0x0152], 0x00);  /* nop (preamble) */
    ASSERT_EQ(r.rom[0x0160], 0x00);  /* nop at Target (new address) */

    const AsmSymbol *tgt = find_sym(&r, "Target");
    ASSERT_TRUE(tgt != NULL);
    ASSERT_EQ(tgt->addr, 0x0160);

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
     *   line 3: Done:        -> preamble ends at 0x0154; layout places Done at 0x0160
     *   line 4: ret          -> C9        (1 byte  @ 0x0160)
     *
     *   With layout padding: preamble (jp+ld) = 4 bytes ending at 0x0153.
     *   Done's preamble end = 0x0154. round_up(0x0154,16) = 0x0160.
     *   jp target = 0x0160 -> C3 60 01
     */
    const char *src =
        "    jp Done\n"    /* line 1 */
        "    ld b, c\n"    /* line 2 */
        "Done:\n"          /* line 3 */
        "    ret\n";       /* line 4 */

    AsmResult r = asm_assemble(src, "test_fwd_jp.asm");

    ASSERT_TRUE(r.ok);

    ASSERT_EQ(r.rom[0x0150], 0xC3);  /* jp opcode */
    ASSERT_EQ(r.rom[0x0151], 0x60);  /* Done lo (new addr 0x0160) */
    ASSERT_EQ(r.rom[0x0152], 0x01);  /* Done hi */
    ASSERT_EQ(r.rom[0x0153], 0x41);  /* ld b,c (preamble unchanged) */
    ASSERT_EQ(r.rom[0x0154], 0x00);  /* padding (ret moved to Done's slot) */
    ASSERT_EQ(r.rom[0x0160], 0xC9);  /* ret at Done's new address */

    const AsmSymbol *done = find_sym(&r, "Done");
    ASSERT_TRUE(done != NULL);
    ASSERT_EQ(done->addr, 0x0160);

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
     *   Target: nop    -> preamble (dw) ends at 0x0002; layout places Target at 0x0010
     *
     *   With layout padding: preamble (dw = 2 bytes) ends at 0x0002.
     *   round_up(0x0002, 16) = 0x0010. Target placed at 0x0010.
     *   dw Target -> 0x0010 LE: 10 00
     */
    const char *src =
        "    SECTION \"x\", ROM0\n"  /* line 1 */
        "    dw Target\n"            /* line 2 */
        "Target:\n"                  /* line 3 */
        "    nop\n";                 /* line 4 */

    AsmResult r = asm_assemble(src, "test_dw_label.asm");

    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rom[0x0000], 0x10);  /* Target lo (new addr 0x0010) */
    ASSERT_EQ(r.rom[0x0001], 0x00);  /* Target hi */

    const AsmSymbol *t = find_sym(&r, "Target");
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(t->addr, 0x0010);

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
     *   jr nz, Fwd   ; 0x0150: 20 xx (disp to Fwd)
     *   nop          ; 0x0152: 00
     *   Fwd: nop     ; preamble ends at 0x0153; layout places Fwd at 0x0160
     *
     *   With layout: preamble (jr nz + nop) = 3 bytes, Fwd at 0x0153 originally.
     *   round_up(0x0153, 16) = 0x0160. Fwd placed at 0x0160.
     *   jr nz disp = 0x0160 - (0x0150+2) = 14 = 0x0E.
     */
    const char *src =
        "    jr nz, Fwd\n"  /* line 1 */
        "    nop\n"         /* line 2 */
        "Fwd:\n"            /* line 3 */
        "    nop\n";        /* line 4 */

    AsmResult r = asm_assemble(src, "test_jr_cc.asm");

    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rom[0x0150], 0x20);  /* jr nz opcode */
    ASSERT_EQ(r.rom[0x0151], 0x0E);  /* displacement +14 (Fwd at 0x0160) */

    asm_free(&r);
}

/* =========================================================================
 * Fixed-address ROM0 section: SECTION "v", ROM0[$0048] pins code at $0048
 * (interrupt vector). With a low located section present, plain
 * `SECTION "code", ROM0` must auto-place its code above the vectors/header
 * (>= $0150) even when the first function is tiny — so it can't stomp them.
 * ======================================================================= */
static void test_rom0_fixed_addr(void)
{
    const char *src =
        "    SECTION \"stat\", ROM0[$0048]\n"
        "    jp Handler\n"
        "    SECTION \"code\", ROM0\n"   /* plain ROM0: no explicit origin */
        "Main:\n"
        "    nop\n"                       /* tiny: would land at $0000 otherwise */
        "Handler:\n"
        "    reti\n";

    AsmResult r = asm_assemble(src, "test_fixed.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }

    /* jp Handler emitted at the STAT vector ($0048): C3 lo hi */
    ASSERT_EQ(r.rom[0x0048], 0xC3);
    const AsmSymbol *h = find_sym(&r, "Handler");
    ASSERT_TRUE(h != NULL);
    ASSERT_EQ(r.rom[0x0049], (uint8_t)(h->addr & 0xFF));
    ASSERT_EQ(r.rom[0x004A], (uint8_t)(h->addr >> 8));

    /* Auto-cleared: Main (and the vector target) sit at/above $0150, clear of
     * the vectors — even though Main is a single byte. */
    const AsmSymbol *m = find_sym(&r, "Main");
    ASSERT_TRUE(m != NULL);
    ASSERT_TRUE(m->addr >= 0x0150);
    ASSERT_TRUE(h->addr >= 0x0150);
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

    /* Included label IncLabel: preamble (nop) = 1 byte, originally at 0x0151.
     * Layout: HWM=0x0151, slot placed at round_up(0x0151,16)=0x0160.
     * IncLabel.size = 2 (nop+ret), slot_size=32. */
    const AsmSymbol *inc_lbl = find_sym(&r, "IncLabel");
    ASSERT_TRUE(inc_lbl != NULL);
    if (inc_lbl) {
        ASSERT_EQ(inc_lbl->addr, 0x0160);
    }

    /* nop from included file at 0x0160 (new address) */
    ASSERT_EQ(r.rom[0x0151], 0x00);  /* padding (IncLabel moved to 0x0160) */
    ASSERT_EQ(r.rom[0x0160], 0x00);  /* nop at IncLabel's new address */

    /* ret from main file at 0x0161 (inside IncLabel's slot) */
    ASSERT_EQ(r.rom[0x0161], 0xC9);  /* ret */

    asm_free(&r);
    remove(tmpfile);
}

/* =========================================================================
 * Helper: run GB for up to max_steps steps / max_frames frames.
 * Returns 1 if frame count was reached, 0 on step cap.
 * ======================================================================= */
static int run_gb(GB *gb, int max_frames, int max_steps)
{
    int frames = 0, steps = 0;
    while (frames < max_frames && steps < max_steps) {
        int tc = gb_step(gb);
        gb_tick(gb, tc);
        gb_ppu_tick(gb, tc);
        if (gb->frame_ready) {
            gb->frame_ready = false;
            frames++;
        }
        steps++;
    }
    return frames >= max_frames;
}

/* =========================================================================
 * Task 6, Test 20: Serial output — assemble a program that writes "HI" to
 * the serial port and verify g->serial_buf contains "HI".
 *
 * Entry-point handling:
 *   gb_reset() sets PC=0x0100.  The Nintendo logo area (0x0104-0x0133) is
 *   filled with logo bytes by the assembler, which are garbage as opcodes.
 *   The safest approach: after asm_assemble, manually patch bytes 0x0100-
 *   0x0102 to "JP $0150" (C3 50 01).  This is documented here.
 *   The assembled code lives at 0x0150 (default org).
 *
 * Serial mechanism: write char to SB (FF01), then write $81 to SC (FF02);
 *   the emulator appends SB to g->serial_buf.
 *
 * Program (assembled at 0x0150):
 *   ; Send 'H' (0x48)
 *   ld a, 'H'       ; 3E 48
 *   ldh ($01), a    ; E0 01   -> SB = 'H'
 *   ld a, $81       ; 3E 81
 *   ldh ($02), a    ; E0 02   -> SC trigger, 'H' appended to serial_buf
 *   ; Send 'I' (0x49)
 *   ld a, 'I'       ; 3E 49
 *   ldh ($01), a    ; E0 01
 *   ld a, $81       ; 3E 81
 *   ldh ($02), a    ; E0 02
 *   ; Spin forever
 *   Loop: jr Loop   ; 18 FE
 * ======================================================================= */
static void test_e2e_serial(void)
{
    const char *src =
        "; Serial output: write 'HI' to the serial port\n"
        "    ld a, $48\n"        /* 'H' */
        "    ldh ($01), a\n"     /* SB = 'H' */
        "    ld a, $81\n"        /* trigger byte */
        "    ldh ($02), a\n"     /* SC = $81 -> serial transfer */
        "    ld a, $49\n"        /* 'I' */
        "    ldh ($01), a\n"     /* SB = 'I' */
        "    ld a, $81\n"
        "    ldh ($02), a\n"
        "Loop:\n"
        "    jr Loop\n";         /* infinite spin */

    AsmResult r = asm_assemble(src, "serial_test.asm");

    ASSERT_TRUE(r.ok);
    if (!r.ok) {
        for (int i = 0; i < r.ndiags; i++)
            fprintf(stderr, "  diag: %s\n", r.diags[i].msg);
        asm_free(&r);
        return;
    }
    ASSERT_TRUE(r.rom_size >= 0x8000u);

    /*
     * Patch a JP $0150 at the entry point 0x0100 so the CPU reaches our code.
     * gb_reset() starts PC=0x0100; without this patch it would execute
     * header/logo bytes (garbage opcodes) instead of our assembled program.
     */
    r.rom[0x0100] = 0xC3;   /* JP */
    r.rom[0x0101] = 0x50;   /* lo(0x0150) */
    r.rom[0x0102] = 0x01;   /* hi(0x0150) */
    /* Recompute header checksum after patching (0x0100-0x0102 are outside
       the checksum range 0x0134-0x014C so no recomputation needed). */

    GB *gb = gb_new();
    ASSERT_TRUE(gb != NULL);
    if (!gb) { asm_free(&r); return; }

    bool loaded = gb_load_rom(gb, r.rom, r.rom_size);
    ASSERT_TRUE(loaded);

    gb_reset(gb);

    /* Run until serial_buf contains "HI" or we hit the step cap */
    int steps = 0;
    const int MAX_STEPS = 500000;
    while (steps < MAX_STEPS) {
        int tc = gb_step(gb);
        gb_tick(gb, tc);
        gb_ppu_tick(gb, tc);
        steps++;
        if (gb->serial_len >= 2) break;
    }

    ASSERT_TRUE(gb->serial_len >= 2);
    if (gb->serial_len >= 2) {
        ASSERT_EQ((unsigned char)gb->serial_buf[0], 'H');
        ASSERT_EQ((unsigned char)gb->serial_buf[1], 'I');
    }

    gb_free(gb);
    asm_free(&r);
}

/* =========================================================================
 * Task 6, Test 21: Draw a solid tile — assemble a program that:
 *   1. Disables the LCD (so VRAM is accessible without mode-3 blocking)
 *   2. Writes a solid tile (16 bytes of $FF) to VRAM at 0x8000
 *   3. Sets tilemap entry 0 (0x9800) to tile index 0
 *   4. Sets BGP = $E4 (color 3 -> shade 3)
 *   5. Enables LCD with LCDC=$91 (LCD on, BG on, tile data 0x8000)
 *   6. Loops forever
 * Then runs 10 frames and asserts framebuffer pixel (0,0) is shade 3.
 *
 * Entry-point: same JP $0150 patch as the serial test.
 *
 * Tile data note: each 8-pixel row uses two bytes (lo, hi).  With both
 * bytes = $FF, every pixel has color index 3 (both bits set), which the
 * $E4 palette maps to shade 3 (darkest).
 *
 * LCDC=$91 = 0b10010001:
 *   bit 7: LCD enable
 *   bit 4: BG tile data area = 0x8000 (unsigned indexing)
 *   bit 0: BG/window enable
 * ======================================================================= */
static void test_e2e_draw_tile(void)
{
    const char *src =
        "; Draw-tile: solid dark tile at screen position 0,0\n"
        "    ; Turn LCD off so we can write VRAM freely\n"
        "    ld a, $00\n"
        "    ldh ($40), a\n"         /* LCDC = 0 (LCD off) */
        "    ; Write solid tile: 16 bytes of $FF to VRAM 0x8000\n"
        "    ld a, $FF\n"
        "    ld hl, $8000\n"
        "    ld (hl+), a\n"          /* byte 0 */
        "    ld (hl+), a\n"          /* byte 1 */
        "    ld (hl+), a\n"          /* byte 2 */
        "    ld (hl+), a\n"          /* byte 3 */
        "    ld (hl+), a\n"          /* byte 4 */
        "    ld (hl+), a\n"          /* byte 5 */
        "    ld (hl+), a\n"          /* byte 6 */
        "    ld (hl+), a\n"          /* byte 7 */
        "    ld (hl+), a\n"          /* byte 8 */
        "    ld (hl+), a\n"          /* byte 9 */
        "    ld (hl+), a\n"          /* byte 10 */
        "    ld (hl+), a\n"          /* byte 11 */
        "    ld (hl+), a\n"          /* byte 12 */
        "    ld (hl+), a\n"          /* byte 13 */
        "    ld (hl+), a\n"          /* byte 14 */
        "    ld (hl+), a\n"          /* byte 15 */
        "    ; Set tilemap entry 0 at 0x9800 to tile index 0\n"
        "    ld hl, $9800\n"
        "    ld a, $00\n"
        "    ld (hl), a\n"
        "    ; Set palette: BGP = $E4 (color3->shade3)\n"
        "    ld a, $E4\n"
        "    ldh ($47), a\n"         /* BGP */
        "    ; Enable LCD: LCDC = $91\n"
        "    ld a, $91\n"
        "    ldh ($40), a\n"         /* LCDC = $91 */
        "Spin:\n"
        "    jr Spin\n";             /* infinite loop */

    AsmResult r = asm_assemble(src, "tile_test.asm");

    ASSERT_TRUE(r.ok);
    if (!r.ok) {
        for (int i = 0; i < r.ndiags; i++)
            fprintf(stderr, "  diag: %s\n", r.diags[i].msg);
        asm_free(&r);
        return;
    }
    ASSERT_TRUE(r.rom_size >= 0x8000u);

    /* Patch JP $0150 at entry point 0x0100 */
    r.rom[0x0100] = 0xC3;
    r.rom[0x0101] = 0x50;
    r.rom[0x0102] = 0x01;

    GB *gb = gb_new();
    ASSERT_TRUE(gb != NULL);
    if (!gb) { asm_free(&r); return; }

    bool loaded = gb_load_rom(gb, r.rom, r.rom_size);
    ASSERT_TRUE(loaded);

    gb_reset(gb);

    /* Run 10 frames; the tile program should be drawing by then */
    run_gb(gb, 10, 2000000);

    /* Verify pixel (0,0) is shade 3 (darkest) */
    const uint8_t *fb = gb_framebuffer(gb);
    ASSERT_EQ(fb[0], 3);   /* pixel (0,0) == shade 3 */

    gb_free(gb);
    asm_free(&r);
}

/* Task 0: a Main: global makes asm_assemble() write JP Main at 0x0100,
 * so a CLI-built ROM boots without any manual entry patch. */
static void test_e2e_entry_autopatch(void)
{
    const char *src =
        "SECTION \"code\", ROM0\n"
        "Main:\n"
        "    xor a\n"
        "    ldh ($40), a\n"      /* LCDC off so VRAM writes aren't mode-3 blocked */
        "    ld a, $FF\n"
        "    ld hl, $8000\n"
        "    ld b, 16\n"
        ".fill:\n"
        "    ld (hl+), a\n"
        "    dec b\n"
        "    jr nz, .fill\n"
        "    ld hl, $9800\n"
        "    xor a\n"
        "    ld (hl), a\n"
        "    ld a, $E4\n"
        "    ldh ($47), a\n"      /* BGP */
        "    ld a, $91\n"
        "    ldh ($40), a\n"      /* LCDC on */
        "Spin:\n"
        "    jr Spin\n";

    AsmResult r = asm_assemble(src, "entry_autopatch.asm");
    ASSERT_TRUE(r.ok);

    /* The assembler itself must have written JP Main at 0x0100 — NO manual patch. */
    ASSERT_EQ(r.rom[0x0100], 0xC3);          /* JP */
    const AsmSymbol *m = find_sym(&r, "Main");
    ASSERT_TRUE(m != NULL);
    ASSERT_EQ(r.rom[0x0101], (m->addr & 0xFF));
    ASSERT_EQ(r.rom[0x0102], ((m->addr >> 8) & 0xFF));

    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    run_gb(gb, 10, 2000000);
    const uint8_t *fb = gb_framebuffer(gb);
    ASSERT_EQ(fb[0], 3);                      /* reached our code, drew a dark tile */

    gb_free(gb);
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

    /* Task 5 tests */
    test_romx_bank();
    test_rom0_fixed_addr();
    test_cartridge_header();
    test_incbin();
    test_include();

    /* Task 6 tests: assemble -> run on real emulator -> assert */
    test_e2e_serial();
    test_e2e_draw_tile();
    test_e2e_entry_autopatch();

    TEST_MAIN_END();
}
