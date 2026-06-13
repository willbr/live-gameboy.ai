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

int main(void) {
    test_pong_boots();
    TEST_MAIN_END();
}
