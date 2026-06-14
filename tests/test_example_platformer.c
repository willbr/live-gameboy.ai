#include "test.h"
#include "example_run.h"

/* WRAM var addresses (see platformer.asm init):
 *   $C0A0 playerX  $C0A1 playerY  $C0A2 velY  $C0A3 onGround  */
#define PLAYER_X   0xC0A0
#define PLAYER_Y   0xC0A1
#define VEL_Y      0xC0A2
#define ON_GROUND  0xC0A3

/* Button bits (gb.h): bit0 A,1 B,2 Sel,3 Start,4 Right,5 Left,6 Up,7 Down. */
#define BTN_A      0x01
#define BTN_RIGHT  0x10
#define BTN_LEFT   0x20
#define BTN_UP     0x40

static void test_platformer_boots(void) {
    AsmResult r = ex_assemble("examples/platformer.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }
    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);
    ASSERT_TRUE(ex_fb_nonblank(gb));   /* hero sprite + platforms drawn */
    gb_free(gb); asm_free(&r);
}

/* Init powers on the APU and routes channels (NR52 bit7, NR51=$FF). */
static void test_platformer_apu_on(void) {
    AsmResult r = ex_assemble("examples/platformer.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);
    ASSERT_EQ(gb_read8(gb, 0xFF25), 0xFF);
    gb_free(gb); asm_free(&r);
}

/* Gravity: with no input the hero falls (playerY increases) and then lands on a
 * platform and stops (onGround set, velY back to 0, Y stable). */
static void test_platformer_falls_and_lands(void) {
    AsmResult r = ex_assemble("examples/platformer.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);

    ex_run(gb, 3, 2000000);
    uint8_t y0 = gb_read8(gb, PLAYER_Y);
    ex_run(gb, 5, 2000000);
    uint8_t y1 = gb_read8(gb, PLAYER_Y);
    ASSERT_TRUE(y1 > y0);                 /* falling */

    /* let it settle onto the floor */
    ex_run(gb, 120, 9000000);
    ASSERT_TRUE(gb_read8(gb, ON_GROUND));         /* landed */
    ASSERT_EQ(gb_read8(gb, VEL_Y), 0);            /* stopped falling */
    uint8_t yLand = gb_read8(gb, PLAYER_Y);
    ex_run(gb, 30, 4000000);
    ASSERT_EQ(gb_read8(gb, PLAYER_Y), yLand);     /* stays put on the platform */

    gb_free(gb); asm_free(&r);
}

/* Jump: once grounded, pressing A makes the hero rise (playerY decreases) for
 * some frames before gravity brings it back. */
static void test_platformer_jump(void) {
    AsmResult r = ex_assemble("examples/platformer.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);

    /* settle on the floor first */
    ex_run(gb, 120, 9000000);
    ASSERT_TRUE(gb_read8(gb, ON_GROUND));
    uint8_t yGround = gb_read8(gb, PLAYER_Y);

    /* press jump and advance a few frames -> hero should be higher (smaller Y) */
    gb_set_buttons(gb, BTN_A);
    ex_run(gb, 4, 2000000);
    uint8_t yJump = gb_read8(gb, PLAYER_Y);
    ASSERT_TRUE(yJump < yGround);         /* rose off the ground */

    /* release and let gravity return it to the floor */
    gb_set_buttons(gb, 0);
    ex_run(gb, 120, 9000000);
    ASSERT_TRUE(gb_read8(gb, ON_GROUND));            /* landed again */
    ASSERT_EQ(gb_read8(gb, PLAYER_Y), yGround);      /* back on the same floor */

    gb_free(gb); asm_free(&r);
}

/* Horizontal: holding Right moves the hero right (playerX increases). */
static void test_platformer_move_right(void) {
    AsmResult r = ex_assemble("examples/platformer.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 120, 9000000);            /* land first */
    uint8_t x0 = gb_read8(gb, PLAYER_X);
    gb_set_buttons(gb, BTN_RIGHT);
    ex_run(gb, 20, 3000000);
    uint8_t x1 = gb_read8(gb, PLAYER_X);
    ASSERT_TRUE(x1 > x0);
    gb_free(gb); asm_free(&r);
}

/* Landing on the floor fires a CH1 tone (SfxLand). */
static void test_platformer_land_sfx(void) {
    AsmResult r = ex_assemble("examples/platformer.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    uint8_t seen = ex_run_watch_nr52(gb, 120, 9000000);
    ASSERT_TRUE(seen & 0x01);            /* CH1 active at some point (landing) */
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_platformer_boots();
    test_platformer_apu_on();
    test_platformer_falls_and_lands();
    test_platformer_jump();
    test_platformer_move_right();
    test_platformer_land_sfx();
    TEST_MAIN_END();
}
