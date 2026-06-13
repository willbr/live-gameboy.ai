#include "test.h"
#include "example_run.h"

static void test_snake_boots(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }
    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);
    ASSERT_TRUE(ex_fb_nonblank(gb));   /* snake + food drawn into the tilemap */
    gb_free(gb); asm_free(&r);
}

/* The head advances on its own (default dir = right => headX increases). */
static void test_snake_moves(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t x0 = gb_read8(gb, 0xC000);
    ex_run(gb, 60, 8000000);            /* > throttle frames */
    uint8_t x1 = gb_read8(gb, 0xC000);
    ASSERT_TRUE(x1 != x0);              /* head moved */
    gb_free(gb); asm_free(&r);
}

/* Pressing Down changes heading so headY increases over time. */
static void test_snake_turns_down(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t y0 = gb_read8(gb, 0xC001);
    gb_set_buttons(gb, 0x80);           /* bit7 = Down */
    ex_run(gb, 80, 9000000);
    uint8_t y1 = gb_read8(gb, 0xC001);
    ASSERT_TRUE(y1 > y0);
    gb_free(gb); asm_free(&r);
}

/* Holding Left steers the snake left so headX decreases (would fail if Left
 * collapses to Down due to the register-clobber bug). */
static void test_snake_turns_left(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t x0 = gb_read8(gb, 0xC000);
    gb_set_buttons(gb, 0x20);            /* bit5 = Left */
    ex_run(gb, 80, 9000000);
    uint8_t x1 = gb_read8(gb, 0xC000);
    ASSERT_TRUE(x1 < x0);               /* moved left */
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_snake_boots();
    test_snake_moves();
    test_snake_turns_down();
    test_snake_turns_left();
    TEST_MAIN_END();
}
