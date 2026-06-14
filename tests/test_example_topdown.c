#include "test.h"
#include "example_run.h"

/* Button mask bits (gb_set_buttons): bit4 Right,5 Left,6 Up,7 Down,
 * bit0 A,1 B,2 Sel,3 Start. 1 = pressed. */
#define BTN_RIGHT 0x10
#define BTN_LEFT  0x20
#define BTN_UP    0x40
#define BTN_DOWN  0x80

/* WRAM game vars. */
#define HERO_X 0xC0A0
#define HERO_Y 0xC0A1

static void test_topdown_boots(void) {
    AsmResult r = ex_assemble("examples/topdown.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }
    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);
    ASSERT_TRUE(ex_fb_nonblank(gb));   /* dungeon + sprites drawn */
    gb_free(gb); asm_free(&r);
}

/* Init powers on the APU and routes channels (NR51=$FF). */
static void test_topdown_apu_on(void) {
    AsmResult r = ex_assemble("examples/topdown.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);   /* NR52 bit7 = APU on */
    ASSERT_EQ(gb_read8(gb, 0xFF25), 0xFF);
    gb_free(gb); asm_free(&r);
}

/* Holding Down walks the hero into open floor: heroY increases. */
static void test_topdown_moves_down(void) {
    AsmResult r = ex_assemble("examples/topdown.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t y0 = gb_read8(gb, HERO_Y);
    gb_set_buttons(gb, BTN_DOWN);
    ex_run(gb, 20, 4000000);
    uint8_t y1 = gb_read8(gb, HERO_Y);
    ASSERT_TRUE(y1 > y0);              /* hero walked down into the room */
    gb_free(gb); asm_free(&r);
}

/* Holding Right walks the hero across open floor: heroX increases. */
static void test_topdown_moves_right(void) {
    AsmResult r = ex_assemble("examples/topdown.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t x0 = gb_read8(gb, HERO_X);
    gb_set_buttons(gb, BTN_RIGHT);
    ex_run(gb, 20, 4000000);
    uint8_t x1 = gb_read8(gb, HERO_X);
    ASSERT_TRUE(x1 > x0);
    gb_free(gb); asm_free(&r);
}

/* Wall collision: the hero spawns at tile (2,2); tile col 0 is the left
 * border wall. Holding Left, the hero walks to the wall and STOPS — heroX
 * never crosses into the wall column (x stays >= 8, i.e. tile col 1). */
static void test_topdown_wall_blocks_left(void) {
    AsmResult r = ex_assemble("examples/topdown.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    gb_set_buttons(gb, BTN_LEFT);
    ex_run(gb, 60, 9000000);          /* plenty of frames to reach the wall */
    uint8_t x = gb_read8(gb, HERO_X);
    /* Wall is tile col 0 (pixels 0..7). The 8px hero may not enter it, so
     * its top-left x must remain >= 8. */
    ASSERT_TRUE(x >= 8);
    /* And it must actually have pressed up against the wall (not still at
     * spawn x=16): it should have moved left at least one step. */
    ASSERT_TRUE(x < 16);
    gb_free(gb); asm_free(&r);
}

/* The hero cannot tunnel through a wall over many frames: hold Left for a
 * long time and confirm x is still clamped at the wall (never wraps/passes). */
static void test_topdown_wall_no_tunnel(void) {
    AsmResult r = ex_assemble("examples/topdown.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    gb_set_buttons(gb, BTN_LEFT);
    ex_run(gb, 200, 30000000);
    uint8_t x = gb_read8(gb, HERO_X);
    ASSERT_TRUE(x >= 8);              /* still blocked, no underflow/tunnel */
    gb_free(gb); asm_free(&r);
}

/* Bumping a wall fires a CH1 blip: hold Left into the border wall and watch
 * NR52 — the pulse channel (bit0) becomes active at some point. */
static void test_topdown_bump_sfx(void) {
    AsmResult r = ex_assemble("examples/topdown.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    gb_set_buttons(gb, BTN_LEFT);
    uint8_t seen = ex_run_watch_nr52(gb, 60, 9000000);
    ASSERT_TRUE(seen & 0x01);          /* CH1 fired on the bump */
    gb_free(gb); asm_free(&r);
}

/* The guard patrols on its own: guardX changes over time with no input. */
static void test_topdown_guard_patrols(void) {
    AsmResult r = ex_assemble("examples/topdown.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t g0 = gb_read8(gb, 0xC0A3);
    ex_run(gb, 20, 4000000);
    uint8_t g1 = gb_read8(gb, 0xC0A3);
    ASSERT_TRUE(g1 != g0);
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_topdown_boots();
    test_topdown_apu_on();
    test_topdown_moves_down();
    test_topdown_moves_right();
    test_topdown_wall_blocks_left();
    test_topdown_wall_no_tunnel();
    test_topdown_bump_sfx();
    test_topdown_guard_patrols();
    TEST_MAIN_END();
}
