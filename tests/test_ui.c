/*
 * test_ui.c — Unit tests for src/ide/ui.c
 *
 * Color convention used throughout: 0xRRGGBBAA
 *   r = (rgba >> 24) & 0xFF
 *   g = (rgba >> 16) & 0xFF
 *   b = (rgba >>  8) & 0xFF
 *   a =  rgba        & 0xFF
 *
 * The canvas pixel buffer stores bytes in R, G, B, A order.
 */

#include "../src/ide/ui.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Helper: read the RGBA bytes at (x,y) from a canvas */
static void get_pixel(const Canvas *c, int x, int y,
                      uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    const uint8_t *p = c->px + ((size_t)y * (size_t)c->w + (size_t)x) * 4;
    *r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
}

int main(void) {
    /* ------------------------------------------------------------------
     * Test 1: canvas_new + ui_clear — verify every pixel of a small canvas
     * ------------------------------------------------------------------ */
    Canvas c = canvas_new(64, 32);
    ASSERT_TRUE(c.px != NULL);
    ASSERT_EQ(c.w, 64);
    ASSERT_EQ(c.h, 32);

    /* 0xRRGGBBAA: R=0xDE, G=0xAD, B=0xBE, A=0xEF */
    uint32_t clear_color = 0xDEADBEEF;
    ui_clear(&c, clear_color);

    uint8_t r, g, b, a;
    get_pixel(&c,  0,  0, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xDE); ASSERT_EQ(g, 0xAD);
    ASSERT_EQ(b, 0xBE); ASSERT_EQ(a, 0xEF);

    get_pixel(&c, 63, 31, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xDE); ASSERT_EQ(g, 0xAD);
    ASSERT_EQ(b, 0xBE); ASSERT_EQ(a, 0xEF);

    get_pixel(&c, 32, 16, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xDE); ASSERT_EQ(g, 0xAD);
    ASSERT_EQ(b, 0xBE); ASSERT_EQ(a, 0xEF);

    /* ------------------------------------------------------------------
     * Test 2: ui_fill_rect — sub-region filled; outside unchanged
     * ------------------------------------------------------------------ */
    /* Fill a 10x10 rect at (5, 4) with blue (0x0000FFFF) */
    uint32_t blue  = 0x0000FFFF;  /* R=0, G=0, B=255, A=255 */
    ui_fill_rect(&c, 5, 4, 10, 10, blue);

    /* inside corner pixels */
    get_pixel(&c,  5,  4, &r, &g, &b, &a);
    ASSERT_EQ(r, 0x00); ASSERT_EQ(g, 0x00); ASSERT_EQ(b, 0xFF); ASSERT_EQ(a, 0xFF);

    get_pixel(&c, 14, 13, &r, &g, &b, &a);
    ASSERT_EQ(r, 0x00); ASSERT_EQ(g, 0x00); ASSERT_EQ(b, 0xFF); ASSERT_EQ(a, 0xFF);

    /* pixel just outside the rect (to the left) should still be clear_color */
    get_pixel(&c,  4,  4, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xDE); ASSERT_EQ(g, 0xAD); ASSERT_EQ(b, 0xBE); ASSERT_EQ(a, 0xEF);

    /* pixel just outside the rect (below) should still be clear_color */
    get_pixel(&c,  5, 14, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xDE); ASSERT_EQ(g, 0xAD); ASSERT_EQ(b, 0xBE); ASSERT_EQ(a, 0xEF);

    canvas_free(&c);

    /* ------------------------------------------------------------------
     * Test 3: ui_text_bg — glyph pixels are fg; cell bg pixels are bg
     *
     * We use ui_text_bg for full determinism: every cell pixel is either
     * fg (glyph bit set) or bg (glyph bit clear).
     * ------------------------------------------------------------------ */
    Canvas tc = canvas_new(64, 16);
    ASSERT_TRUE(tc.px != NULL);

    uint32_t text_bg = 0x111111FF; /* very dark, distinct from fg */
    ui_clear(&tc, text_bg);

    uint32_t fg_color = 0xFFFF00FF; /* yellow fg */
    uint32_t bg_color = 0x222222FF; /* slightly lighter bg for cells */

    /* Draw 'A' at (0,0) with explicit bg */
    ui_text_bg(&tc, 0, 0, "A", fg_color, bg_color);

    /*
     * font8x8 'A' = 0x41 - 0x20 = 0x21
     *   row 0: 0x0C = 0b00001100 → bit0=0, bit1=0, bit2=1, bit3=1 ...
     *   row 2: 0x33 = 0b00110011 → bit0=1 (col 0 IS set)
     *   row 4: 0x3F = 0b00111111 → bit0=1 (all 6 low bits set)
     *
     * We check a pixel that is definitely ON in the glyph:
     *   row 1 (byte=0x1E=0b00011110): bit1=1 → col 1 is fg
     * And a pixel that is definitely OFF in the glyph:
     *   row 0 (byte=0x0C=0b00001100): bit0=0 → col 0 is bg
     */

    /* col 1, row 1 of 'A' — should be fg */
    get_pixel(&tc, 1, 1, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xFF); ASSERT_EQ(g, 0xFF); ASSERT_EQ(b, 0x00); ASSERT_EQ(a, 0xFF);

    /* col 0, row 0 of 'A' — should be bg */
    get_pixel(&tc, 0, 0, &r, &g, &b, &a);
    ASSERT_EQ(r, 0x22); ASSERT_EQ(g, 0x22); ASSERT_EQ(b, 0x22); ASSERT_EQ(a, 0xFF);

    canvas_free(&tc);

    /* ------------------------------------------------------------------
     * Test 4: ui_blit_gb
     *
     * Create a 160x144 shade buffer.  Set fb[0]=3 (first pixel shade 3)
     * and fb[1]=0 (second pixel shade 0).  Blit at (0,0) scale=2 with
     * pal[3]=red, pal[0]=white.
     *
     * Expected:
     *   The 2x2 block at canvas (0,0)–(1,1) → red   (0xFF0000FF)
     *   The 2x2 block at canvas (2,0)–(3,1) → white (0xFFFFFFFF)
     * ------------------------------------------------------------------ */
    uint8_t *fb = (uint8_t *)calloc(160 * 144, 1);
    ASSERT_TRUE(fb != NULL);
    fb[0] = 3;  /* shade 3 → red */
    fb[1] = 0;  /* shade 0 → white */

    /* Canvas must be at least 4 wide × 2 tall to hold 2 scaled pixels */
    Canvas gc = canvas_new(4, 2);
    ASSERT_TRUE(gc.px != NULL);
    ui_clear(&gc, 0x00000000); /* start transparent black */

    uint32_t pal[4] = {
        0xFFFFFFFF, /* shade 0 → white */
        0x55555555, /* shade 1 → unused in this test */
        0xAAAAAAFF, /* shade 2 → unused */
        0xFF0000FF  /* shade 3 → red */
    };

    ui_blit_gb(&gc, fb, 0, 0, 2, pal);
    free(fb);

    /* 2x2 block at (0,0) → red */
    get_pixel(&gc, 0, 0, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xFF); ASSERT_EQ(g, 0x00); ASSERT_EQ(b, 0x00); ASSERT_EQ(a, 0xFF);
    get_pixel(&gc, 1, 0, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xFF); ASSERT_EQ(g, 0x00); ASSERT_EQ(b, 0x00); ASSERT_EQ(a, 0xFF);
    get_pixel(&gc, 0, 1, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xFF); ASSERT_EQ(g, 0x00); ASSERT_EQ(b, 0x00); ASSERT_EQ(a, 0xFF);
    get_pixel(&gc, 1, 1, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xFF); ASSERT_EQ(g, 0x00); ASSERT_EQ(b, 0x00); ASSERT_EQ(a, 0xFF);

    /* 2x2 block at (2,0) → white */
    get_pixel(&gc, 2, 0, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xFF); ASSERT_EQ(g, 0xFF); ASSERT_EQ(b, 0xFF); ASSERT_EQ(a, 0xFF);
    get_pixel(&gc, 3, 0, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xFF); ASSERT_EQ(g, 0xFF); ASSERT_EQ(b, 0xFF); ASSERT_EQ(a, 0xFF);
    get_pixel(&gc, 2, 1, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xFF); ASSERT_EQ(g, 0xFF); ASSERT_EQ(b, 0xFF); ASSERT_EQ(a, 0xFF);
    get_pixel(&gc, 3, 1, &r, &g, &b, &a);
    ASSERT_EQ(r, 0xFF); ASSERT_EQ(g, 0xFF); ASSERT_EQ(b, 0xFF); ASSERT_EQ(a, 0xFF);

    canvas_free(&gc);

    /* ------------------------------------------------------------------
     * Test 5: ui_save_png — writes file, returns 0, file has nonzero size
     * ------------------------------------------------------------------ */
    Canvas pc = canvas_new(32, 16);
    ASSERT_TRUE(pc.px != NULL);
    ui_clear(&pc, 0x8833CCFF); /* arbitrary color */
    ui_fill_rect(&pc, 4, 4, 8, 8, 0x00FF88FF);
    ui_text_bg(&pc, 0, 0, "H", 0xFFFFFFFF, 0x000000FF);

    const char *png_path = "build/test_ui.png";
    int save_rc = ui_save_png(&pc, png_path);
    ASSERT_EQ(save_rc, 0);

    /* verify file exists and has nonzero size */
    struct stat st;
    int stat_rc = stat(png_path, &st);
    ASSERT_EQ(stat_rc, 0);
    ASSERT_TRUE(st.st_size > 0);

    canvas_free(&pc);

    /* ------------------------------------------------------------------
     * Test 6: TextField widget
     * ------------------------------------------------------------------ */
    {
        TextField t; textfield_clear(&t);
        ASSERT_TRUE(t.len == 0);
        textfield_putc(&t, 'C'); textfield_putc(&t, '0');
        textfield_putc(&t, '0'); textfield_putc(&t, '0');
        ASSERT_TRUE(t.len == 4);
        ASSERT_TRUE(strcmp(t.text, "C000") == 0);
        textfield_backspace(&t);
        ASSERT_TRUE(t.len == 3);
        ASSERT_TRUE(strcmp(t.text, "C00") == 0);
        /* non-printable rejected */
        textfield_putc(&t, '\n');
        ASSERT_TRUE(t.len == 3);
        /* capacity guard */
        textfield_clear(&t);
        for (int i = 0; i < 100; i++) textfield_putc(&t, 'A');
        ASSERT_TRUE(t.len == TEXTFIELD_CAP - 1);
    }

    TEST_MAIN_END();
}
