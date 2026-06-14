#include "test.h"
#include "example_run.h"

/* Boots and draws the road into the tilemap -> non-blank framebuffer. */
static void test_driver_boots(void) {
    AsmResult r = ex_assemble("examples/driver.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }
    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);
    ASSERT_TRUE(ex_fb_nonblank(gb));
    gb_free(gb); asm_free(&r);
}

/* Init powers on the APU and routes channels (NR52 bit7, NR51=$FF). */
static void test_driver_apu_on(void) {
    AsmResult r = ex_assemble("examples/driver.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);
    ASSERT_EQ(gb_read8(gb, 0xFF25), 0xFF);
    gb_free(gb); asm_free(&r);
}

/* The road scrolls: SCY ($FF42) advances every frame as the road rushes up. */
static void test_driver_scrolls(void) {
    AsmResult r = ex_assemble("examples/driver.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t scy0 = gb_read8(gb, 0xFF42);
    ex_run(gb, 20, 4000000);
    uint8_t scy1 = gb_read8(gb, 0xFF42);
    ASSERT_TRUE(scy1 != scy0);           /* road scrolled */
    gb_free(gb); asm_free(&r);
}

/* Steering: holding Right increases carX ($C0A0), holding Left decreases it. */
static void test_driver_steers(void) {
    AsmResult r = ex_assemble("examples/driver.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);

    /* Steer right (bit4 = Right) */
    gb_set_buttons(gb, 0x10);
    ex_run(gb, 20, 4000000);
    uint8_t xr = gb_read8(gb, 0xC0A0);   /* carX var */

    /* Now steer left (bit5 = Left) back past the start */
    gb_set_buttons(gb, 0x20);
    ex_run(gb, 30, 5000000);
    uint8_t xl = gb_read8(gb, 0xC0A0);

    ASSERT_TRUE(xl < xr);                 /* car slid left after sliding right */
    gb_free(gb); asm_free(&r);
}

/* Off-road rumble: shove carX off the road band (as a live OAM/memory edit
 * would) and the grass-rumble CH1 blip fires. */
static void test_driver_offroad_sfx(void) {
    AsmResult r = ex_assemble("examples/driver.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    gb_write8(gb, 0xC0A0, 20);           /* carX far left of the road -> grass */
    uint8_t seen = ex_run_watch_nr52(gb, 20, 4000000);
    ASSERT_TRUE(seen & 0x01);            /* CH1 grass rumble fired */
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_driver_boots();
    test_driver_apu_on();
    test_driver_scrolls();
    test_driver_steers();
    test_driver_offroad_sfx();
    TEST_MAIN_END();
}
