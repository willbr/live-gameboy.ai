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

/* flag mask helpers: f layout ZNHC0000 */
#define FLAGS(g) ((g)->cpu.f & 0xF0)

int main(void) {
    {   /* ADD A,d8: 0x3A + 0xC6 = 0x00, Z H C set (classic GB manual vector) */
        GB *g = gb_with((uint8_t[]){0x3E, 0x3A, 0xC6, 0xC6}, 4);
        gb_step(g); ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ(g->cpu.a, 0x00);
        ASSERT_EQ(FLAGS(g), 0xB0);   /* Z H C */
        gb_free(g);
    }
    {   /* ADC with carry-in: A=0xE1, C=1, ADC 0x1E -> 0x00, Z H C */
        GB *g = gb_with((uint8_t[]){0x37, 0x3E, 0xE1, 0xCE, 0x1E}, 5); /* SCF first */
        gb_step(g); gb_step(g); ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ(g->cpu.a, 0x00);
        ASSERT_EQ(FLAGS(g), 0xB0);
        gb_free(g);
    }
    {   /* SUB: A=0x3E, SUB 0x3E -> 0, Z N */
        GB *g = gb_with((uint8_t[]){0x3E, 0x3E, 0xD6, 0x3E}, 4);
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x00);
        ASSERT_EQ(FLAGS(g), 0xC0);   /* Z N */
        gb_free(g);
    }
    {   /* SUB borrow: A=0x3E, SUB 0x40 -> 0xFE, N C */
        GB *g = gb_with((uint8_t[]){0x3E, 0x3E, 0xD6, 0x40}, 4);
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0xFE);
        ASSERT_EQ(FLAGS(g), 0x50);   /* N C */
        gb_free(g);
    }
    {   /* AND/XOR/OR/CP flag conventions */
        GB *g = gb_with((uint8_t[]){
            0x3E, 0x5A, 0xE6, 0x0F,   /* AND 0x0F -> 0x0A, H always set */
            0xEE, 0x0A,               /* XOR -> 0x00, Z only */
            0xF6, 0x00,               /* OR 0 -> 0x00, Z only */
            0xFE, 0x01,               /* CP 1 -> A unchanged, N C (0 < 1) */
        }, 10);
        gb_step(g);
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x0A); ASSERT_EQ(FLAGS(g), 0x20);
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x00); ASSERT_EQ(FLAGS(g), 0x80);
        gb_step(g); ASSERT_EQ(FLAGS(g), 0x80);
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x00); ASSERT_EQ(FLAGS(g), 0x70);
        gb_free(g);
    }
    {   /* INC wraps half-carry, preserves C; DEC sets N */
        GB *g = gb_with((uint8_t[]){0x37, 0x3E, 0x0F, 0x3C, 0x3D}, 5); /* SCF;LD A,0F;INC A;DEC A */
        gb_step(g); gb_step(g);
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x10); ASSERT_EQ(FLAGS(g), 0x30); /* H + preserved C */
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x0F); ASSERT_EQ(FLAGS(g), 0x70); /* N H + preserved C */
        gb_free(g);
    }
    {   /* register-form ALU (x=2 block): ADD A,B */
        GB *g = gb_with((uint8_t[]){0x06, 0x01, 0x3E, 0xFF, 0x80}, 5);
        gb_step(g); gb_step(g);
        ASSERT_EQ(gb_step(g), 4);
        ASSERT_EQ(g->cpu.a, 0x00);
        ASSERT_EQ(FLAGS(g), 0xB0);
        gb_free(g);
    }
    TEST_MAIN_END();
}
