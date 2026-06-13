#include "test.h"
#include "example_run.h"

static void test_breakout_boots(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }
    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);
    ASSERT_TRUE(ex_fb_nonblank(gb));   /* bricks + paddle + ball */
    gb_free(gb); asm_free(&r);
}

/* The ball moves. */
static void test_breakout_ball_moves(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 3, 2000000);
    uint8_t y0 = gb_read8(gb, 0xC0A1);
    ex_run(gb, 20, 6000000);
    uint8_t y1 = gb_read8(gb, 0xC0A1);
    ASSERT_TRUE(y1 != y0);
    gb_free(gb); asm_free(&r);
}

/* At least one brick cell gets cleared as the ball travels up. */
static void test_breakout_clears_brick(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    int before = 0;
    for (int row = 2; row <= 5; row++)
        for (int col = 0; col < 20; col++)
            if (gb_read8(gb, 0x9800 + row*32 + col) == 3) before++;
    ASSERT_TRUE(before > 0);
    ex_run(gb, 240, 30000000);
    int after = 0;
    for (int row = 2; row <= 5; row++)
        for (int col = 0; col < 20; col++)
            if (gb_read8(gb, 0x9800 + row*32 + col) == 3) after++;
    ASSERT_TRUE(after < before);        /* a brick was knocked out */
    gb_free(gb); asm_free(&r);
}

/* Holding Left moves the paddle left. */
static void test_breakout_paddle_left(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t x0 = gb_read8(gb, 0xC0A4);
    gb_set_buttons(gb, 0x20);           /* bit5 = Left */
    ex_run(gb, 20, 6000000);
    uint8_t x1 = gb_read8(gb, 0xC0A4);
    ASSERT_TRUE(x1 < x0);
    gb_free(gb); asm_free(&r);
}

/* Init powers on the APU and routes channels (NR51=$FF vs reset $F3). */
static void test_breakout_apu_on(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);
    ASSERT_EQ(gb_read8(gb, 0xFF25), 0xFF);
    gb_free(gb); asm_free(&r);
}

/* Breaking a brick fires the CH4 noise burst as the ball reaches the field. */
static void test_breakout_break_sfx(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    uint8_t seen = ex_run_watch_nr52(gb, 150, 12000000);
    ASSERT_TRUE(seen & 0x08);   /* CH4 noise */
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_breakout_boots();
    test_breakout_ball_moves();
    test_breakout_clears_brick();
    test_breakout_paddle_left();
    test_breakout_apu_on();
    test_breakout_break_sfx();
    TEST_MAIN_END();
}
