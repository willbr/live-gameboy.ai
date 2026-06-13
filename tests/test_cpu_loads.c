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
    {   /* LD r,d8 for B; LD r,r'; LD r,(HL); LD (HL),r */
        GB *g = gb_with((uint8_t[]){
            0x06, 0x77,        /* LD B,0x77        8 cy */
            0x48,              /* LD C,B           4 cy */
            0x21, 0x00, 0xC0,  /* LD HL,0xC000    12 cy (needs Task 5? no: add 0x21 here) */
            0x70,              /* LD (HL),B        8 cy */
            0x5E,              /* LD E,(HL)        8 cy */
        }, 8);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(g->cpu.b, 0x77);
        ASSERT_EQ(gb_step(g), 4);  ASSERT_EQ(g->cpu.c, 0x77);
        ASSERT_EQ(gb_step(g), 12);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(gb_read8(g, 0xC000), 0x77);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(g->cpu.e, 0x77);
        gb_free(g);
    }
    {   /* LD (HL),d8 ; indirect A loads ; HL+/HL- */
        GB *g = gb_with((uint8_t[]){
            0x21, 0x00, 0xC0,  /* LD HL,0xC000 */
            0x36, 0x99,        /* LD (HL),0x99    12 cy */
            0x2A,              /* LD A,(HL+)       8 cy: A=0x99, HL=0xC001 */
            0x32,              /* LD (HL-),A       8 cy: [C001]=99, HL=C000 */
            0x0A,              /* LD A,(BC) */
        }, 8);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 12); ASSERT_EQ(gb_read8(g, 0xC000), 0x99);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(g->cpu.a, 0x99);
        ASSERT_EQ((g->cpu.h << 8) | g->cpu.l, 0xC001);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(gb_read8(g, 0xC001), 0x99);
        ASSERT_EQ((g->cpu.h << 8) | g->cpu.l, 0xC000);
        gb_free(g);
    }
    {   /* LDH and absolute */
        GB *g = gb_with((uint8_t[]){
            0x3E, 0x5A,        /* LD A,0x5A */
            0xE0, 0x80,        /* LDH (0x80),A    12 cy -> FF80 */
            0xF0, 0x80,        /* LDH A,(0x80)    12 cy */
            0xEA, 0x00, 0xC1,  /* LD (0xC100),A   16 cy */
            0xFA, 0x00, 0xC1,  /* LD A,(0xC100)   16 cy */
            0x0E, 0x81,        /* LD C,0x81 */
            0xE2,              /* LD (C),A         8 cy -> FF81 */
            0xF2,              /* LD A,(C)         8 cy */
        }, 16);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 12); ASSERT_EQ(gb_read8(g, 0xFF80), 0x5A);
        ASSERT_EQ(gb_step(g), 12); ASSERT_EQ(g->cpu.a, 0x5A);
        ASSERT_EQ(gb_step(g), 16); ASSERT_EQ(gb_read8(g, 0xC100), 0x5A);
        ASSERT_EQ(gb_step(g), 16);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(gb_read8(g, 0xFF81), 0x5A);
        ASSERT_EQ(gb_step(g), 8);
        gb_free(g);
    }
    {   /* PUSH/POP, LD (a16),SP, LD SP,HL, LD HL,SP+e8 */
        GB *g = gb_with((uint8_t[]){
            0x01, 0x34, 0x12,  /* LD BC,0x1234 */
            0xC5,              /* PUSH BC        16 cy */
            0xD1,              /* POP DE         12 cy */
            0x08, 0x00, 0xC2,  /* LD (0xC200),SP 20 cy */
            0x21, 0x00, 0xD0,  /* LD HL,0xD000 */
            0xF9,              /* LD SP,HL        8 cy */
            0xF8, 0xFE,        /* LD HL,SP-2     12 cy: H=0 ops on low byte */
        }, 14);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 16);
        ASSERT_EQ(gb_step(g), 12);
        ASSERT_EQ(g->cpu.d, 0x12); ASSERT_EQ(g->cpu.e, 0x34);
        uint16_t sp_before = g->cpu.sp;
        ASSERT_EQ(gb_step(g), 20);
        ASSERT_EQ(gb_read8(g, 0xC200), sp_before & 0xFF);
        ASSERT_EQ(gb_read8(g, 0xC201), sp_before >> 8);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 8); ASSERT_EQ(g->cpu.sp, 0xD000);
        ASSERT_EQ(gb_step(g), 12);
        ASSERT_EQ((g->cpu.h << 8) | g->cpu.l, 0xCFFE);
        /* LD HL,SP+e8 flags: Z=0 N=0; H/C from low-byte unsigned add (00+FE: no carry) */
        ASSERT_EQ(g->cpu.f & 0x80, 0);
        ASSERT_EQ(g->cpu.f & 0x10, 0);
        gb_free(g);
    }
    {   /* PUSH AF masks low nibble of F */
        GB *g = gb_with((uint8_t[]){0xF5, 0xC1}, 2);   /* PUSH AF; POP BC */
        g->cpu.a = 0x12; g->cpu.f = 0xF0;
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.b, 0x12); ASSERT_EQ(g->cpu.c, 0xF0);
        gb_free(g);
    }
    TEST_MAIN_END();
}
