/*
 * test_vram_prov.c — VRAM provenance tracking tests.
 *
 * Verifies that:
 *   - Copying a tile from ROM to VRAM via "ld a,(hl+) ; ld (de),a" idiom
 *     records the correct linear ROM offsets in vram_prov[].
 *   - gb->vram[0..15] contains the actual tile bytes (copy worked).
 *   - A VRAM byte written from a computed value (xor a ; ld (de),a) has
 *     provenance 0xFFFFFFFF (none).
 */

#include "test.h"
#include "../src/gb/gb.h"
#include <string.h>
#include <stdint.h>

/* Tile data: 16 bytes of known values */
static const uint8_t tile_data[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
};

/* ROM layout:
 *   0x0100: code (copy loop + halt)
 *   0x0200: TileSrc (16 tile bytes)
 *
 * Assembly pseudocode placed at 0x0100:
 *   xor a
 *   ldh ($40), a       ; disable LCD (LCDC = 0)
 *   ld hl, $0200       ; HL = TileSrc
 *   ld de, $8000       ; DE = VRAM dest
 *   ld bc, $0010       ; BC = 16
 * .copy:
 *   ld a, (hl+)        ; 2A
 *   ld (de), a         ; 12
 *   inc de             ; 13
 *   dec bc             ; 0B
 *   ld a, b            ; 78
 *   or c               ; B1
 *   jr nz, .copy       ; 20 F8  (offset = -8)
 *   ; DE now points to $8010; do a WRAM read to clear prov_pending
 *   ld hl, $C000       ; LD HL, 0xC000 (WRAM — non-ROM source)
 *   ld a, (hl)         ; LD A,(HL) — reads WRAM, clears prov_pending_valid
 *   ld (de), a         ; LD (DE),A — VRAM[$8010] = wram[0], prov = 0xFFFFFFFF
 *   halt               ; 76
 *
 * Note: "xor a" alone does NOT clear the taint because xor is register-only
 * (no bus read). A WRAM read is needed to clear prov_pending_valid, since
 * the heuristic only fires on bus reads.
 */

static uint8_t make_rom(uint8_t *rom, size_t rom_size) {
    memset(rom, 0, rom_size);

    /* Place tile data at 0x0200 */
    memcpy(rom + 0x0200, tile_data, 16);

    /* Place code at 0x0100 */
    uint8_t *p = rom + 0x0100;
    /* xor a */
    *p++ = 0xAF;
    /* ldh ($40), a  — LD (FF40), A disables LCD */
    *p++ = 0xE0;
    *p++ = 0x40;
    /* ld hl, $0200 */
    *p++ = 0x21;
    *p++ = 0x00;
    *p++ = 0x02;
    /* ld de, $8000 */
    *p++ = 0x11;
    *p++ = 0x00;
    *p++ = 0x80;
    /* ld bc, $0010 */
    *p++ = 0x01;
    *p++ = 0x10;
    *p++ = 0x00;
    /* .copy: (starts at 0x010C) */
    /* ld a, (hl+) */
    *p++ = 0x2A;
    /* ld (de), a */
    *p++ = 0x12;
    /* inc de */
    *p++ = 0x13;
    /* dec bc */
    *p++ = 0x0B;
    /* ld a, b */
    *p++ = 0x78;
    /* or c */
    *p++ = 0xB1;
    /* jr nz, -8 (back to 0x010C) */
    *p++ = 0x20;
    *p++ = (uint8_t)(-8);
    /* DE now points to $8010.
     * Load HL with WRAM address $C000 to clear the prov taint via a non-ROM read.
     * ld hl, $C000 */
    *p++ = 0x21;
    *p++ = 0x00;
    *p++ = 0xC0;
    /* ld a, (hl) — reads WRAM, clears prov_pending_valid */
    *p++ = 0x7E;
    /* ld (de), a — writes WRAM value to VRAM[$8010]; prov should be 0xFFFFFFFF */
    *p++ = 0x12;
    /* halt */
    *p++ = 0x76;

    return 0;
}

int main(void) {
    /* ------------------------------------------------------------------ */
    /* Build the ROM                                                        */
    /* ------------------------------------------------------------------ */
    static uint8_t rom[0x8000];
    make_rom(rom, sizeof rom);

    /* ------------------------------------------------------------------ */
    /* Boot the emulator                                                    */
    /* ------------------------------------------------------------------ */
    GB *g = gb_new();
    ASSERT_TRUE(g != NULL);
    ASSERT_TRUE(gb_load_rom(g, rom, sizeof rom));
    gb_reset(g);

    /* Verify initial provenance state: all 0xFFFFFFFF */
    for (int i = 0; i < 0x2000; i++) {
        ASSERT_EQ(g->vram_prov[i], (uint32_t)0xFFFFFFFF);
    }

    /* ------------------------------------------------------------------ */
    /* Run until HALT (or a generous bound to avoid infinite loops)         */
    /* ------------------------------------------------------------------ */
    int steps = 0;
    int max_steps = 100000;
    while (!g->cpu.halted && steps < max_steps) {
        gb_step(g);
        steps++;
    }

    ASSERT_TRUE(g->cpu.halted);

    /* ------------------------------------------------------------------ */
    /* Assert: vram[0..15] == tile_data (copy worked)                      */
    /* ------------------------------------------------------------------ */
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(g->vram[i], tile_data[i]);
    }

    /* ------------------------------------------------------------------ */
    /* Assert: vram_prov[0..15] == linear ROM offset of TileSrc[i]         */
    /* TileSrc is at ROM offset 0x0200, so TileSrc[i] -> prov = 0x0200 + i */
    /* ------------------------------------------------------------------ */
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(g->vram_prov[i], (uint32_t)(0x0200 + i));
    }

    /* ------------------------------------------------------------------ */
    /* Assert: VRAM[$8010] (vram[0x10]) has provenance 0xFFFFFFFF          */
    /* Written by "xor a ; ld (de),a" — computed value, no ROM taint.      */
    /* ------------------------------------------------------------------ */
    ASSERT_EQ(g->vram[0x10], (uint8_t)0x00);
    ASSERT_EQ(g->vram_prov[0x10], (uint32_t)0xFFFFFFFF);

    gb_free(g);

    TEST_MAIN_END();
}
