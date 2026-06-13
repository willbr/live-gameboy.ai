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
    {   /* JR forward/backward; taken 12, not-taken 8 */
        GB *g = gb_with((uint8_t[]){0x18, 0x02, 0x00, 0x00, 0x18, 0xFC}, 6);
        ASSERT_EQ(gb_step(g), 12); ASSERT_EQ(g->cpu.pc, 0x0104);
        ASSERT_EQ(gb_step(g), 12); ASSERT_EQ(g->cpu.pc, 0x0102);
        gb_free(g);
    }
    {   /* JR NZ not taken after Z set */
        GB *g = gb_with((uint8_t[]){0xAF, 0x20, 0x10}, 3);  /* XOR A sets Z */
        gb_step(g);
        ASSERT_EQ(gb_step(g), 8); ASSERT_EQ(g->cpu.pc, 0x0103);
        gb_free(g);
    }
    {   /* CALL/RET: 24 + 16 cycles; stack correct */
        GB *g = gb_with((uint8_t[]){
            0xCD, 0x10, 0x01,   /* 0100: CALL 0x0110 */
            0x00,               /* 0103 */
        }, 4);
        rom[0x110] = 0xC9;      /* RET */
        gb_load_rom(g, rom, sizeof rom); gb_reset(g);
        ASSERT_EQ(gb_step(g), 24);
        ASSERT_EQ(g->cpu.pc, 0x0110);
        ASSERT_EQ(g->cpu.sp, 0xFFFC);
        ASSERT_EQ(gb_read8(g, 0xFFFC), 0x03);
        ASSERT_EQ(gb_read8(g, 0xFFFD), 0x01);
        ASSERT_EQ(gb_step(g), 16);
        ASSERT_EQ(g->cpu.pc, 0x0103);
        gb_free(g);
    }
    {   /* conditional RET timing: taken 20, not taken 8 */
        GB *g = gb_with((uint8_t[]){0xAF, 0xC8}, 2);  /* XOR A; RET Z (taken) */
        rom[0x100] = 0xAF; rom[0x101] = 0xC8;
        gb_step(g);
        g->cpu.sp = 0xFFFC;
        gb_write8(g, 0xFFFC, 0x00); gb_write8(g, 0xFFFD, 0x02);
        ASSERT_EQ(gb_step(g), 20);
        ASSERT_EQ(g->cpu.pc, 0x0200);
        gb_free(g);
    }
    {   /* RST 0x28: 16 cycles */
        GB *g = gb_with((uint8_t[]){0xEF}, 1);
        ASSERT_EQ(gb_step(g), 16);
        ASSERT_EQ(g->cpu.pc, 0x0028);
        gb_free(g);
    }
    {   /* JP HL: 4 cycles */
        GB *g = gb_with((uint8_t[]){0x21, 0x00, 0x40, 0xE9}, 4);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 4);
        ASSERT_EQ(g->cpu.pc, 0x4000);
        gb_free(g);
    }
    TEST_MAIN_END();
}
