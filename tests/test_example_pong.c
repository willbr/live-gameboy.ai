#include "test.h"
#include "example_run.h"

/* Boot: pong.asm assembles, the LCD comes on, and the screen is not blank. */
static void test_pong_boots(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }

    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);          /* 30 frames */
    ASSERT_TRUE(ex_fb_nonblank(gb));  /* paddles + ball are drawn */

    gb_free(gb);
    asm_free(&r);
}

/* The ball moves on its own over a few frames. */
static void test_pong_ball_moves(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 3, 2000000);
    uint8_t x0 = gb_read8(gb, 0xC0A0), y0 = gb_read8(gb, 0xC0A1);
    ex_run(gb, 20, 6000000);
    uint8_t x1 = gb_read8(gb, 0xC0A0), y1 = gb_read8(gb, 0xC0A1);
    ASSERT_TRUE(x1 != x0 || y1 != y0);   /* it moved */
    gb_free(gb); asm_free(&r);
}

/* The ball stays on the playfield (never wraps off the top/bottom). */
static void test_pong_ball_in_bounds(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    for (int i = 0; i < 30; i++) {
        ex_run(gb, 5, 2000000);
        uint8_t y = gb_read8(gb, 0xC0A1);
        ASSERT_TRUE(y < 152);            /* 144 playfield + margin, no wrap to ~255 */
    }
    gb_free(gb); asm_free(&r);
}

/* Holding Up moves the left paddle up (lPadY decreases). */
static void test_pong_paddle_up(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t y0 = gb_read8(gb, 0xC0A4);
    gb_set_buttons(gb, 0x40);            /* bit6 = Up held */
    ex_run(gb, 20, 6000000);
    uint8_t y1 = gb_read8(gb, 0xC0A4);
    ASSERT_TRUE(y1 < y0);                /* paddle moved up */
    gb_free(gb); asm_free(&r);
}

/* Init powers on the APU and routes all channels to both speakers.
 * NR51=$FF distinguishes our init from the DMG reset default ($F3), so this
 * actually exercises the added power-on block (not just the reset state). */
static void test_pong_apu_on(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);   /* NR52 power bit */
    ASSERT_EQ(gb_read8(gb, 0xFF25), 0xFF);      /* NR51 set by init (reset=$F3) */
    gb_free(gb); asm_free(&r);
}

/* A CH1 blip fires during a rally (paddle/wall/score bounces). */
static void test_pong_sfx_fires(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    uint8_t seen = ex_run_watch_nr52(gb, 250, 9000000);
    ASSERT_TRUE(seen & 0x01);
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_pong_boots();
    test_pong_ball_moves();
    test_pong_ball_in_bounds();
    test_pong_paddle_up();
    test_pong_apu_on();
    test_pong_sfx_fires();
    TEST_MAIN_END();
}
