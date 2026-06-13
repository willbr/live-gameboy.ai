#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static uint8_t rom[0x8000];

static GB *gb_with(const uint8_t *code, size_t n) {
    memset(rom, 0, sizeof rom);
    memcpy(rom + 0x100, code, n);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}

int main(void) {
    {   /* NOP: 4 cycles, pc+1 */
        GB *g = gb_with((uint8_t[]){0x00}, 1);
        ASSERT_EQ(gb_step(g), 4);
        ASSERT_EQ(g->cpu.pc, 0x0101);
        gb_free(g);
    }
    {   /* LD A,d8 */
        GB *g = gb_with((uint8_t[]){0x3E, 0x42}, 2);
        ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ(g->cpu.a, 0x42);
        gb_free(g);
    }
    {   /* JP a16: 16 cycles */
        GB *g = gb_with((uint8_t[]){0xC3, 0x34, 0x12}, 3);
        ASSERT_EQ(gb_step(g), 16);
        ASSERT_EQ(g->cpu.pc, 0x1234);
        gb_free(g);
    }
    TEST_MAIN_END();
}
