/*
 * test_ide_render.c — Milestone 6 Task 2: IDE render smoke test.
 *
 * Creates a small .asm program, runs it through ide_new + ide_step_frame,
 * renders the IDE panels into a 640x432 canvas, and asserts:
 *   1. The game-screen region (x=16..336, y=18..312) has at least one
 *      non-background pixel (the tile rendered and blitted).
 *   2. The registers panel region (x=352..632, y=8..160) has at least one
 *      bright text pixel (the CPU text was drawn).
 *   3. ui_save_png to build/test_ide.png returns 0 and the file is readable.
 *   4. ide_shot to build/test_ide_shot.png returns 0 and that file exists too.
 */

#include "test.h"
#include "../src/ide/ide.h"
#include "../src/ide/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * A tiny GB program that:
 *   - disables the LCD
 *   - loads tile data (a solid shade-1 tile) into VRAM at $8000
 *   - sets BGP so shade 1 maps to a non-white colour
 *   - sets the tilemap ($9800) to use tile 0
 *   - enables the LCD (window off, BG on, tile-base $8000)
 *   - loops
 * This ensures the framebuffer has at least some non-shade-0 pixels.
 * --------------------------------------------------------------------- */
static const char ASM_SOURCE[] =
    "Main:\n"
    "    ; disable LCD\n"
    "    ld a, $00\n"
    "    ld ($FF40), a\n"
    "\n"
    "    ; clear VRAM (2000h bytes at $8000) so previous garbage is gone\n"
    "    ld hl, $8000\n"
    "    ld bc, $2000\n"
    ".clr:\n"
    "    ld (hl), $00\n"
    "    inc hl\n"
    "    dec bc\n"
    "    ld a, b\n"
    "    or c\n"
    "    jr nz, .clr\n"
    "\n"
    "    ; write tile 0 at $8000: all pixels = shade 1\n"
    "    ; 2bpp: lo byte = $FF, hi byte = $00 => shade (0<<1)|1 = 1\n"
    "    ld hl, $8000\n"
    "    ld b, 8\n"
    ".tile_row:\n"
    "    ld a, $FF\n"
    "    ld (hl), a\n"
    "    inc hl\n"
    "    ld a, $00\n"
    "    ld (hl), a\n"
    "    inc hl\n"
    "    dec b\n"
    "    jr nz, .tile_row\n"
    "\n"
    "    ; fill tilemap $9800 with tile 0 (already zero — just to be explicit)\n"
    "    ld hl, $9800\n"
    "    ld b, 32\n"
    ".map_row:\n"
    "    ld a, $00\n"
    "    ld (hl), a\n"
    "    inc hl\n"
    "    dec b\n"
    "    jr nz, .map_row\n"
    "\n"
    "    ; BGP: shade 1 -> dark color (binary: 11 01 00 00 = $D0 wrong, let's do:\n"
    "    ;   bits[7:6]=shade3, bits[5:4]=shade2, bits[3:2]=shade1, bits[1:0]=shade0\n"
    "    ;   shade1 -> color 2 (dark) => bits[3:2]=10 => BGP = xxxxxx10xxxxxx\n"
    "    ;   simple: BGP = $E4 (standard) -> shade0=0,shade1=1,shade2=2,shade3=3\n"
    "    ld a, $E4\n"
    "    ld ($FF47), a\n"
    "\n"
    "    ; enable LCD: LCDC = $91 (LCD on, BG tile map $9800, BG tile data $8000, BG on)\n"
    "    ld a, $91\n"
    "    ld ($FF40), a\n"
    "\n"
    ".loop:\n"
    "    jr .loop\n";

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Write the ASM source to a temp file; caller must fclose and unlink. */
static int write_temp_asm(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return 1;
    fputs(ASM_SOURCE, f);
    fclose(f);
    return 0;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Scan a rectangular region of the canvas for any pixel with R component > threshold. */
static int region_has_bright_red(const Canvas *c, int x0, int y0, int x1, int y1,
                                  uint8_t threshold) {
    for (int y = y0; y < y1 && y < c->h; y++) {
        for (int x = x0; x < x1 && x < c->w; x++) {
            uint8_t r = c->px[(y * c->w + x) * 4 + 0];
            if (r > threshold) return 1;
        }
    }
    return 0;
}

/* Scan a region for any pixel that differs from the background color (0x1A1A2E). */
static int region_has_non_bg(const Canvas *c, int x0, int y0, int x1, int y1) {
    /* Background is COL_BG = 0x1A1A2EFF -> R=0x1A, G=0x1A, B=0x2E */
    for (int y = y0; y < y1 && y < c->h; y++) {
        for (int x = x0; x < x1 && x < c->w; x++) {
            uint8_t r = c->px[(y * c->w + x) * 4 + 0];
            uint8_t g = c->px[(y * c->w + x) * 4 + 1];
            uint8_t b = c->px[(y * c->w + x) * 4 + 2];
            if (r != 0x1A || g != 0x1A || b != 0x2E) return 1;
        }
    }
    return 0;
}

/* =========================================================================
 * Tests
 * ========================================================================= */

static void test_ide_render(void) {
    /* Write temp ASM file */
    const char *asm_path = "build/tmp_ide_test.asm";
    int wrc = write_temp_asm(asm_path);
    ASSERT_EQ(wrc, 0);
    if (wrc) return;

    /* Create IDE session */
    IdeState *s = ide_new(asm_path);
    ASSERT_TRUE(s != NULL);
    if (!s) return;

    /* Step 5 frames */
    for (int i = 0; i < 5; i++)
        ide_step_frame(s);

    /* Render */
    Canvas c = canvas_new(640, 432);
    ASSERT_TRUE(c.px != NULL);
    if (!c.px) { ide_free(s); return; }

    ide_render(s, &c);

    /* 1. Game screen region (inside the GAME panel, where GB pixels land).
     *    GAME panel: x=8, y=8, w=336, h=304.
     *    The blit starts at (16, 18) and covers 320x288 pixels.
     *    Check a region inside it for any non-background pixel. */
    int game_non_bg = region_has_non_bg(&c, 16, 18, 320, 288);
    ASSERT_TRUE(game_non_bg);

    /* 2. Registers panel region (x=352..632, y=8..160) must have some bright text.
     *    COL_TITLE = 0xE0E0FFFF -> R=0xE0, G=0xE0.  The title "REGISTERS" and
     *    register lines will have pixels with R > 0xC0. */
    int regs_has_text = region_has_bright_red(&c, 352, 8, 632, 160, 0xC0);
    ASSERT_TRUE(regs_has_text);

    /* 3. Save PNG */
    int png_rc = ui_save_png(&c, "build/test_ide.png");
    ASSERT_EQ(png_rc, 0);
    ASSERT_TRUE(file_exists("build/test_ide.png"));

    canvas_free(&c);
    ide_free(s);

    fprintf(stderr, "[test_ide_render] build/test_ide.png written\n");
}

static void test_ide_shot(void) {
    const char *asm_path = "build/tmp_ide_test.asm";
    /* File should already exist from test_ide_render; write it again in case order varies. */
    write_temp_asm(asm_path);

    int rc = ide_shot(asm_path, "build/test_ide_shot.png", 5);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(file_exists("build/test_ide_shot.png"));

    fprintf(stderr, "[test_ide_shot] build/test_ide_shot.png written\n");
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(void) {
    test_ide_render();
    test_ide_shot();
    TEST_MAIN_END();
}
