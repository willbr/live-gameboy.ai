/*
 * test_tile_codec.c — tests for 2bpp tile codec + PNG import/export.
 *
 * Tests:
 *   1. set/get round-trip for all 4 colors at various (x,y) positions.
 *   2. Bit-layout check: tile16[0]=0xA0 (lo), tile16[1]=0x00 (hi) ->
 *      get(0,0)=1, get(1,0)=0, get(2,0)=1, get(3,0)=0.
 *   3. PNG round-trip: 2-tile sheet (gradient + checkerboard) ->
 *      write PNG -> read back -> assert bytes identical.
 */

#include "test.h"
#include "../src/live/tile.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * Test 1: set/get round-trip
 * --------------------------------------------------------------------- */

static void test_set_get_roundtrip(void) {
    uint8_t tile[16];

    /* Test all 4 colors at pixel (0,0) */
    for (int c = 0; c < 4; c++) {
        memset(tile, 0, 16);
        tile2bpp_set(tile, 0, 0, (uint8_t)c);
        ASSERT_EQ(tile2bpp_get(tile, 0, 0), c);
        /* other pixels should remain 0 */
        ASSERT_EQ(tile2bpp_get(tile, 1, 0), 0);
        ASSERT_EQ(tile2bpp_get(tile, 0, 1), 0);
    }

    /* Test all 4 colors at pixel (7,7) */
    for (int c = 0; c < 4; c++) {
        memset(tile, 0, 16);
        tile2bpp_set(tile, 7, 7, (uint8_t)c);
        ASSERT_EQ(tile2bpp_get(tile, 7, 7), c);
        /* other pixels should remain 0 */
        ASSERT_EQ(tile2bpp_get(tile, 0, 7), 0);
        ASSERT_EQ(tile2bpp_get(tile, 7, 6), 0);
    }

    /* Test all 4 colors at various positions */
    static const int xs[] = {0, 1, 3, 5, 7};
    static const int ys[] = {0, 2, 4, 7};
    for (int xi = 0; xi < 5; xi++) {
        for (int yi = 0; yi < 4; yi++) {
            for (int c = 0; c < 4; c++) {
                memset(tile, 0, 16);
                tile2bpp_set(tile, xs[xi], ys[yi], (uint8_t)c);
                ASSERT_EQ(tile2bpp_get(tile, xs[xi], ys[yi]), c);
            }
        }
    }

    /* Test overwrite: set a pixel twice, second value wins */
    memset(tile, 0, 16);
    tile2bpp_set(tile, 3, 3, 3);
    ASSERT_EQ(tile2bpp_get(tile, 3, 3), 3);
    tile2bpp_set(tile, 3, 3, 1);
    ASSERT_EQ(tile2bpp_get(tile, 3, 3), 1);
    /* neighboring pixels untouched */
    ASSERT_EQ(tile2bpp_get(tile, 2, 3), 0);
    ASSERT_EQ(tile2bpp_get(tile, 4, 3), 0);
    ASSERT_EQ(tile2bpp_get(tile, 3, 2), 0);
    ASSERT_EQ(tile2bpp_get(tile, 3, 4), 0);
}

/* -----------------------------------------------------------------------
 * Test 2: Bit-layout check (external verification)
 *   tile16[0]=0xA0 (lo), tile16[1]=0x00 (hi)
 *   0xA0 = 1010 0000
 *   Pixel x: color = (hi_bit<<1) | lo_bit
 *   x=0: bit=7, lo=1, hi=0 -> color=1
 *   x=1: bit=6, lo=0, hi=0 -> color=0
 *   x=2: bit=5, lo=1, hi=0 -> color=1
 *   x=3: bit=4, lo=0, hi=0 -> color=0
 * --------------------------------------------------------------------- */

static void test_bit_layout(void) {
    uint8_t tile[16];
    memset(tile, 0, 16);
    tile[0] = 0xA0;  /* lo plane, row 0 */
    tile[1] = 0x00;  /* hi plane, row 0 */

    ASSERT_EQ(tile2bpp_get(tile, 0, 0), 1);
    ASSERT_EQ(tile2bpp_get(tile, 1, 0), 0);
    ASSERT_EQ(tile2bpp_get(tile, 2, 0), 1);
    ASSERT_EQ(tile2bpp_get(tile, 3, 0), 0);

    /* bits 3..0 of 0xA0 are 0, so pixels 4..7 should be 0 */
    ASSERT_EQ(tile2bpp_get(tile, 4, 0), 0);
    ASSERT_EQ(tile2bpp_get(tile, 5, 0), 0);
    ASSERT_EQ(tile2bpp_get(tile, 6, 0), 0);
    ASSERT_EQ(tile2bpp_get(tile, 7, 0), 0);

    /* row 1 should be all 0 */
    for (int x = 0; x < 8; x++) {
        ASSERT_EQ(tile2bpp_get(tile, x, 1), 0);
    }

    /* Test with hi plane set: tile16[2]=0x00 (lo), tile16[3]=0xF0 (hi)
     * 0xF0 = 1111 0000
     * x=0..3: lo=0, hi=1 -> color=2; x=4..7: lo=0, hi=0 -> color=0 */
    memset(tile, 0, 16);
    tile[2] = 0x00;
    tile[3] = 0xF0;
    for (int x = 0; x < 4; x++) ASSERT_EQ(tile2bpp_get(tile, x, 1), 2);
    for (int x = 4; x < 8; x++) ASSERT_EQ(tile2bpp_get(tile, x, 1), 0);

    /* Test color 3: both planes set
     * tile16[4]=0xFF (lo), tile16[5]=0xFF (hi) -> all pixels color 3 in row 2 */
    tile[4] = 0xFF;
    tile[5] = 0xFF;
    for (int x = 0; x < 8; x++) ASSERT_EQ(tile2bpp_get(tile, x, 2), 3);
}

/* -----------------------------------------------------------------------
 * Test 3: PNG round-trip
 * --------------------------------------------------------------------- */

static void build_gradient_tile(uint8_t *tile16) {
    /* tile 0: gradient — pixel (x,y) has color (x+y) & 3 */
    memset(tile16, 0, 16);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            tile2bpp_set(tile16, x, y, (uint8_t)((x + y) & 3));
}

static void build_checkerboard_tile(uint8_t *tile16) {
    /* tile 1: checkerboard — pixel (x,y) has color (x^y) & 3 */
    memset(tile16, 0, 16);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            tile2bpp_set(tile16, x, y, (uint8_t)((x ^ y) & 3));
}

static void test_png_roundtrip(void) {
    const char *tmp_path = "build/test_sheet.png";

    uint8_t orig[32];
    build_gradient_tile(orig);
    build_checkerboard_tile(orig + 16);

    /* write PNG: 2 tiles, 2 per row */
    int rc = tile_sheet_to_png(tmp_path, orig, 2, 2);
    ASSERT_EQ(rc, 0);
    if (rc != 0) return; /* can't read if write failed */

    /* read PNG back */
    uint8_t recovered[32];
    int out_ntiles = 0;
    rc = tile_sheet_from_png(tmp_path, recovered, 2, &out_ntiles);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(out_ntiles, 2);

    if (rc == 0 && out_ntiles == 2) {
        /* compare byte by byte */
        for (int i = 0; i < 32; i++) {
            ASSERT_EQ(recovered[i], orig[i]);
        }
    }

    /* also test 1-tile sheet with 1 per row */
    rc = tile_sheet_to_png(tmp_path, orig, 1, 1);
    ASSERT_EQ(rc, 0);
    if (rc == 0) {
        uint8_t r2[16];
        int n2 = 0;
        rc = tile_sheet_from_png(tmp_path, r2, 1, &n2);
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(n2, 1);
        if (rc == 0 && n2 == 1) {
            for (int i = 0; i < 16; i++) {
                ASSERT_EQ(r2[i], orig[i]);
            }
        }
    }

    /* clean up */
    remove(tmp_path);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

int main(void) {
    test_set_get_roundtrip();
    test_bit_layout();
    test_png_roundtrip();
    TEST_MAIN_END();
}
