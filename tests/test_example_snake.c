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

int main(void) {
    test_snake_boots();
    TEST_MAIN_END();
}
