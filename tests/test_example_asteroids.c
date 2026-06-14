#include "test.h"
#include "example_run.h"

/* WRAM layout mirrors examples/asteroids.asm */
#define SHIPX_HI 0xC0A1
#define SHIPY_HI 0xC0A3
#define HEAD     0xC0A6
#define BULLIFE  0xC0AF

static void test_asteroids_boots(void) {
    AsmResult r = ex_assemble("examples/asteroids.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }
    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);
    ASSERT_TRUE(ex_fb_nonblank(gb));   /* ship + asteroids drawn as sprites */
    gb_free(gb); asm_free(&r);
}

/* Init powers on the APU and routes channels (NR52 bit7, NR51=$FF). */
static void test_asteroids_apu_on(void) {
    AsmResult r = ex_assemble("examples/asteroids.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);
    ASSERT_EQ(gb_read8(gb, 0xFF25), 0xFF);
    gb_free(gb); asm_free(&r);
}

/* Holding Right rotates the ship: the heading variable changes over time. */
static void test_asteroids_rotates(void) {
    AsmResult r = ex_assemble("examples/asteroids.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t h0 = gb_read8(gb, HEAD);
    gb_set_buttons(gb, 0x10);            /* bit4 = Right */
    ex_run(gb, 60, 8000000);
    uint8_t h1 = gb_read8(gb, HEAD);
    ASSERT_TRUE(h1 != h0);               /* heading changed */
    gb_free(gb); asm_free(&r);
}

/* Holding Up thrusts: momentum moves the ship. Default heading 0 = up, so the
 * ship's Y pixel decreases over time. */
static void test_asteroids_thrust_moves(void) {
    AsmResult r = ex_assemble("examples/asteroids.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t y0 = gb_read8(gb, SHIPY_HI);
    gb_set_buttons(gb, 0x40);            /* bit6 = Up (thrust) */
    ex_run(gb, 80, 9000000);
    uint8_t y1 = gb_read8(gb, SHIPY_HI);
    ASSERT_TRUE(y1 != y0);               /* ship moved under thrust */
    gb_free(gb); asm_free(&r);
}

/* Pressing A fires a bullet: BULLIFE becomes non-zero. */
static void test_asteroids_fire(void) {
    AsmResult r = ex_assemble("examples/asteroids.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_EQ(gb_read8(gb, BULLIFE), 0); /* no bullet yet */
    gb_set_buttons(gb, 0x01);            /* bit0 = A (fire) */
    /* poll for a live bullet across a few frames */
    int saw = 0;
    for (int i = 0; i < 10 && !saw; i++) {
        ex_run(gb, 3, 1000000);
        if (gb_read8(gb, BULLIFE) != 0) saw = 1;
    }
    ASSERT_TRUE(saw);                    /* a bullet spawned */
    gb_free(gb); asm_free(&r);
}

/* Firing a bullet triggers the CH1 (pulse) FIRE blip. */
static void test_asteroids_fire_sfx(void) {
    AsmResult r = ex_assemble("examples/asteroids.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    gb_set_buttons(gb, 0x01);            /* hold A */
    uint8_t seen = ex_run_watch_nr52(gb, 30, 4000000);
    ASSERT_TRUE(seen & 0x01);            /* CH1 fired */
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_asteroids_boots();
    test_asteroids_apu_on();
    test_asteroids_rotates();
    test_asteroids_thrust_moves();
    test_asteroids_fire();
    test_asteroids_fire_sfx();
    TEST_MAIN_END();
}
