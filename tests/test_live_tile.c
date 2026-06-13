/*
 * test_live_tile.c — Milestone 5 Task 4 gate: tile_paint live painting.
 *
 * Gate: paint a pixel into a live asset, verify it propagates through the
 * build database → gb->rom → gb->vram, and that the rendered framebuffer
 * pixel changes — all WITHOUT calling live_reload().
 *
 * Test outline:
 *   1. Write an all-zero 16-byte tile file (build/paint_tile.2bpp).
 *   2. Assemble and load a program that copies the tile to VRAM and loops
 *      with LCDC=$91 so the PPU renders it.
 *   3. Run ~5 frames.  Assert pixel (0,0) shade == 0 (color 0 → BGP $E4 → 0).
 *   4. tile_paint(s, tile 0, x=0, y=0, color=3).  Assert returns true.
 *   5. Run 1 more frame.  Assert vram[0] and vram[1] have bit7 set (color 3),
 *      and framebuffer pixel (0,0) shade == 3.
 *   6. Negative: bad asset path returns false.
 *   7. Negative: paint a tile index that was never copied to VRAM — asset and
 *      ROM bytes change but no VRAM byte changes.
 */

#include "test.h"
#include "../src/live/live.h"
#include "../src/live/tile.h"
#include "../src/gb/gb.h"
#include "../src/asm/asm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Asset file path
 * ------------------------------------------------------------------------- */
#define ASSET_PATH "build/paint_tile.2bpp"

/* -------------------------------------------------------------------------
 * Assembly source:
 *   - Disable LCD
 *   - Copy 16 bytes from TileData to VRAM $8000 (tile 0)
 *   - Set BGP = $E4
 *   - Set tilemap byte at $9800 to 0  (bg tile 0)
 *   - Enable LCD ($91: LCD on, BG on, BG tile data at $8000)
 *   - Spin forever so PPU renders frames
 *
 * Note: code and data are in the same SECTION so the assembler places them
 * contiguously and the data label follows the spin-loop code.
 * ------------------------------------------------------------------------- */
static const char *PROG_SRC =
    "SECTION \"Code\", ROM0\n"
    "Main:\n"
    "    ld sp, $FFFE\n"
    /* Disable LCD */
    "    xor a\n"
    "    ldh ($40), a\n"
    /* Copy 16 bytes: HL = TileData, DE = $8000, BC = 16 */
    "    ld hl, .TileData\n"
    "    ld de, $8000\n"
    "    ld bc, $0010\n"
    ".copyloop:\n"
    "    ld a, (hl+)\n"
    "    ld (de), a\n"
    "    inc de\n"
    "    dec bc\n"
    "    ld a, b\n"
    "    or c\n"
    "    jr nz, .copyloop\n"
    /* Set BGP = $E4 */
    "    ld a, $E4\n"
    "    ldh ($47), a\n"
    /* Write tile index 0 to tilemap at $9800 */
    "    ld hl, $9800\n"
    "    xor a\n"
    "    ld (hl), a\n"
    /* Enable LCD: LCDC = $91 (LCD on, BG tile data $8000, BG map $9800, BG on) */
    "    ld a, $91\n"
    "    ldh ($40), a\n"
    ".loop:\n"
    "    jr .loop\n"
    /* Local label keeps TileData inside Main's layout slot, no overlap */
    ".TileData:\n"
    "    incbin \"" ASSET_PATH "\"\n";

/* -------------------------------------------------------------------------
 * Run enough CPU steps to advance N frames.
 * One GB frame = 70224 T-cycles; gb_step returns T-cycles per instruction.
 * We use frame_ready flag to count frames.
 * ------------------------------------------------------------------------- */
static void run_frames(GB *gb, int n)
{
    /* gb_step() internally calls gb_tick() which drives the PPU.
     * We simply step until frame_ready fires n times.
     * Safety bound: one GB frame is 70224 T-cycles; each step ~4-20 T-cycles;
     * so ~20000 steps per frame is generous. */
    int frames = 0;
    int limit = n * 50000;
    gb->frame_ready = false;
    for (int i = 0; i < limit && frames < n; i++) {
        gb_step(gb);
        if (gb->frame_ready) {
            frames++;
            gb->frame_ready = false;
        }
    }
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    /* ------------------------------------------------------------------
     * Step 1: Write the all-zero 16-byte tile file.
     * ------------------------------------------------------------------ */
    {
        FILE *f = fopen(ASSET_PATH, "wb");
        ASSERT_TRUE(f != NULL);
        if (!f) goto cleanup;
        uint8_t zeros[16] = {0};
        size_t written = fwrite(zeros, 1, 16, f);
        fclose(f);
        ASSERT_EQ((int)written, 16);
    }

    /* ------------------------------------------------------------------
     * Step 2: Assemble and load the program.
     * ------------------------------------------------------------------ */
    LiveSession *s = live_new(PROG_SRC, "paint_tile.asm");
    ASSERT_TRUE(s != NULL);
    if (!s) goto cleanup;

    GB *gb = live_gb(s);
    ASSERT_TRUE(gb != NULL);
    if (!gb) { live_free(s); goto cleanup; }

    /* Confirm JP patch installed at 0x100 */
    ASSERT_EQ(gb->rom[0x100], (uint8_t)0xC3);

    /* ------------------------------------------------------------------
     * Step 3: Run ~5 frames.  Pixel (0,0) should be shade 0 (all-zero tile,
     * color 0 → BGP $E4 bits 1:0 = 0 → shade 0).
     * ------------------------------------------------------------------ */
    run_frames(gb, 5);

    {
        const uint8_t *fb = gb_framebuffer(gb);
        ASSERT_TRUE(fb != NULL);
        if (fb) {
            fprintf(stderr, "[tile_paint] after 5 frames: fb[0]=%u (expect 0)\n",
                    (unsigned)fb[0]);
            ASSERT_EQ((int)fb[0], 0);
        }
    }

    /* Verify VRAM[0..15] contains all-zero tile (as copied) */
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(gb->vram[i], (uint8_t)0);
    }

    /* ------------------------------------------------------------------
     * Step 4: tile_paint — set pixel (0,0) of tile 0 to color 3.
     * ------------------------------------------------------------------ */
    char err[128] = {0};
    bool ok = tile_paint(s, ASSET_PATH, 0, 0, 0, 3, err, sizeof err);
    fprintf(stderr, "[tile_paint] tile_paint returned %s err='%s'\n",
            ok ? "true" : "false", err);
    ASSERT_TRUE(ok);

    /* ------------------------------------------------------------------
     * Step 5: Run 1 more frame.  The PPU re-renders using the updated VRAM.
     * Assert:
     *   - vram[0] bit7 set (lo plane, pixel 0 of row 0)
     *   - vram[1] bit7 set (hi plane, pixel 0 of row 0)
     *   - framebuffer pixel (0,0) shade == 3
     * ------------------------------------------------------------------ */
    run_frames(gb, 1);

    {
        /* vram[0] = lo byte of row 0: bit7 = pixel 0 lo-bit */
        fprintf(stderr, "[tile_paint] vram[0]=0x%02x vram[1]=0x%02x\n",
                (unsigned)gb->vram[0], (unsigned)gb->vram[1]);
        ASSERT_TRUE((gb->vram[0] & 0x80) != 0);  /* lo-plane bit7 set */
        ASSERT_TRUE((gb->vram[1] & 0x80) != 0);  /* hi-plane bit7 set */

        const uint8_t *fb = gb_framebuffer(gb);
        ASSERT_TRUE(fb != NULL);
        if (fb) {
            fprintf(stderr, "[tile_paint] after paint+1 frame: fb[0]=%u (expect 3)\n",
                    (unsigned)fb[0]);
            ASSERT_EQ((int)fb[0], 3);
        }
    }

    /* ------------------------------------------------------------------
     * Negative case 1: bad asset path → returns false, err set.
     * ------------------------------------------------------------------ */
    {
        char berr[128] = {0};
        bool bad = tile_paint(s, "nonexistent_asset_xyz.2bpp", 0, 0, 0, 2,
                              berr, sizeof berr);
        fprintf(stderr, "[tile_paint] bad asset: returned %s err='%s'\n",
                bad ? "true" : "false", berr);
        ASSERT_TRUE(!bad);
        ASSERT_TRUE(berr[0] != '\0');
    }

    /* ------------------------------------------------------------------
     * Negative case 2: paint tile index 1 (a tile that was never copied
     * to VRAM).  Asset bytes and ROM bytes should update, but no VRAM
     * byte should change (since vram_prov for those ROM offsets won't
     * match any VRAM entry).
     *
     * First, record the current VRAM state.  Then paint tile 1.
     * Verify: asset bytes at offset 16 and 17 changed.  VRAM unchanged.
     * ------------------------------------------------------------------ */
    {
        /* The asset is only 16 bytes (one tile), so tile index 1 is out of
         * bounds.  Use a fresh asset file with 32 bytes to test this.       */
        const char *ASSET2 = "build/paint_tile2.2bpp";
        {
            FILE *f2 = fopen(ASSET2, "wb");
            ASSERT_TRUE(f2 != NULL);
            if (f2) {
                uint8_t zeros32[32] = {0};
                fwrite(zeros32, 1, 32, f2);
                fclose(f2);
            }
        }

        /* Build a second session with a 32-byte asset that has TWO tiles.
         * The program copies only tile 0 (first 16 bytes) to VRAM.
         * Tile 1's ROM bytes have no vram_prov match. */
        static const char prog2_src[] =
            "SECTION \"Code\", ROM0\n"
            "Main:\n"
            "    ld sp, $FFFE\n"
            "    xor a\n"
            "    ldh ($40), a\n"
            "    ld hl, .TileData\n"
            "    ld de, $8000\n"
            "    ld bc, $0010\n"    /* copy only tile 0 (16 bytes) */
            ".copyloop:\n"
            "    ld a, (hl+)\n"
            "    ld (de), a\n"
            "    inc de\n"
            "    dec bc\n"
            "    ld a, b\n"
            "    or c\n"
            "    jr nz, .copyloop\n"
            "    ld a, $E4\n"
            "    ldh ($47), a\n"
            "    ld hl, $9800\n"
            "    xor a\n"
            "    ld (hl), a\n"
            "    ld a, $91\n"
            "    ldh ($40), a\n"
            ".loop:\n"
            "    jr .loop\n"
            /* Local label keeps data inside Main's slot, no overlap */
            ".TileData:\n"
            "    incbin \"build/paint_tile2.2bpp\"\n";

        LiveSession *s2 = live_new(prog2_src, "paint_tile2.asm");
        ASSERT_TRUE(s2 != NULL);
        if (s2) {
            GB *gb2 = live_gb(s2);
            AsmResult *r2 = live_result(s2);
            ASSERT_TRUE(gb2 != NULL);
            ASSERT_TRUE(r2 != NULL);

            /* Run a few frames to populate VRAM prov */
            run_frames(gb2, 3);

            /* Snapshot VRAM */
            uint8_t vram_snap[0x2000];
            memcpy(vram_snap, gb2->vram, 0x2000);

            /* Paint tile 1 (bytes 16..31 of asset), pixel (0,0), color 1 */
            char perr[128] = {0};
            bool pok = tile_paint(s2, "build/paint_tile2.2bpp", 1, 0, 0, 1,
                                  perr, sizeof perr);
            fprintf(stderr, "[tile_paint] tile1 paint returned %s err='%s'\n",
                    pok ? "true" : "false", perr);
            ASSERT_TRUE(pok);

            if (pok && r2->nassets > 0) {
                /* Asset byte at offset 16 (lo plane, row 0 of tile 1) should
                 * have bit7 set (pixel 0, color 1 → lo bit = 1) */
                ASSERT_TRUE((r2->assets[0].bytes[16] & 0x80) != 0);

                /* Verify VRAM is UNCHANGED (tile 1 was never in VRAM) */
                int vram_changed = 0;
                for (int vi = 0; vi < 0x2000; vi++) {
                    if (gb2->vram[vi] != vram_snap[vi]) vram_changed++;
                }
                fprintf(stderr, "[tile_paint] tile1 VRAM changed bytes: %d (expect 0)\n",
                        vram_changed);
                ASSERT_EQ(vram_changed, 0);
            }

            live_free(s2);
        }
        remove(ASSET2);
    }

    /* ------------------------------------------------------------------
     * Clean up
     * ------------------------------------------------------------------ */
    live_free(s);

cleanup:
    remove(ASSET_PATH);

    TEST_MAIN_END();
}
