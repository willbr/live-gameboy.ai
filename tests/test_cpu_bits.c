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
#define FLAGS(g) ((g)->cpu.f & 0xF0)

int main(void) {
    {   /* RLCA: A=0x85 -> 0x0B, C=1, Z always 0 */
        GB *g = gb_with((uint8_t[]){0x3E, 0x85, 0x07}, 3);
        gb_step(g); ASSERT_EQ(gb_step(g), 4);
        ASSERT_EQ(g->cpu.a, 0x0B); ASSERT_EQ(FLAGS(g), 0x10);
        gb_free(g);
    }
    {   /* RRA: A=0x01, C=0 -> A=0x00, C=1, Z=0 (accumulator rotates never set Z) */
        GB *g = gb_with((uint8_t[]){0x3E, 0x01, 0x1F}, 3);
        g->cpu.f = 0;
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x00); ASSERT_EQ(FLAGS(g), 0x10);
        gb_free(g);
    }
    {   /* CB RLC B: B=0x85 -> 0x0B C=1 ; CB forms DO set Z */
        GB *g = gb_with((uint8_t[]){0x06, 0x00, 0xCB, 0x00}, 4);
        gb_step(g); ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ(g->cpu.b, 0x00); ASSERT_EQ(FLAGS(g), 0x80); /* Z */
        gb_free(g);
    }
    {   /* SWAP A: 0xF1 -> 0x1F, only Z possible */
        GB *g = gb_with((uint8_t[]){0x3E, 0xF1, 0xCB, 0x37}, 4);
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x1F); ASSERT_EQ(FLAGS(g), 0x00);
        gb_free(g);
    }
    {   /* SRA keeps sign; SRL doesn't */
        GB *g = gb_with((uint8_t[]){0x3E, 0x81, 0xCB, 0x2F, 0xCB, 0x3F}, 6);
        gb_step(g);
        gb_step(g); ASSERT_EQ(g->cpu.a, 0xC0); ASSERT_EQ(FLAGS(g), 0x10); /* SRA: C from bit0 */
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x60); ASSERT_EQ(FLAGS(g), 0x00); /* SRL */
        gb_free(g);
    }
    {   /* BIT 7,H: Z reflects complement of bit; H always set, C preserved */
        GB *g = gb_with((uint8_t[]){0x26, 0x80, 0xCB, 0x7C, 0xCB, 0x6C}, 6);
        g->cpu.f = 0x10;
        gb_step(g);
        ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ(FLAGS(g), 0x30);          /* bit set: Z=0; H=1; C preserved */
        gb_step(g);                          /* BIT 5,H: bit clear */
        ASSERT_EQ(FLAGS(g), 0xB0);          /* Z=1 H=1 C=1 */
        gb_free(g);
    }
    {   /* SET/RES on (HL): 16 cycles */
        GB *g = gb_with((uint8_t[]){
            0x21, 0x00, 0xC0,
            0xCB, 0xC6,        /* SET 0,(HL) */
            0xCB, 0x86,        /* RES 0,(HL) */
        }, 7);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 16); ASSERT_EQ(gb_read8(g, 0xC000), 0x01);
        ASSERT_EQ(gb_step(g), 16); ASSERT_EQ(gb_read8(g, 0xC000), 0x00);
        gb_free(g);
    }
    TEST_MAIN_END();
}
