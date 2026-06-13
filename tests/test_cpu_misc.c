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
    {   /* DAA after BCD add: 0x45 + 0x38 = 0x7D -> DAA -> 0x83 */
        GB *g = gb_with((uint8_t[]){0x3E, 0x45, 0xC6, 0x38, 0x27}, 5);
        gb_step(g); gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x83);
        ASSERT_EQ(FLAGS(g) & 0x10, 0);      /* no carry */
        gb_free(g);
    }
    {   /* DAA after BCD sub: 0x83 - 0x38 = 0x4B -> DAA -> 0x45 */
        GB *g = gb_with((uint8_t[]){0x3E, 0x83, 0xD6, 0x38, 0x27}, 5);
        gb_step(g); gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x45);
        gb_free(g);
    }
    {   /* CPL: N H set, others preserved */
        GB *g = gb_with((uint8_t[]){0x3E, 0x35, 0x2F}, 3);
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0xCA);
        ASSERT_TRUE((g->cpu.f & 0x60) == 0x60);
        gb_free(g);
    }
    {   /* CCF flips carry, clears N/H */
        GB *g = gb_with((uint8_t[]){0x37, 0x3F, 0x3F}, 3);
        gb_step(g);
        gb_step(g); ASSERT_EQ(FLAGS(g) & 0x10, 0x00);
        gb_step(g); ASSERT_EQ(FLAGS(g) & 0x10, 0x10);
        gb_free(g);
    }
    {   /* EI is delayed one instruction; DI immediate */
        GB *g = gb_with((uint8_t[]){0xFB, 0x00, 0x00, 0xF3}, 4);
        gb_step(g);                       /* EI */
        ASSERT_EQ(g->cpu.ime, false);     /* not yet */
        gb_step(g);                       /* NOP — IME becomes true before this */
        ASSERT_EQ(g->cpu.ime, true);
        gb_step(g);
        gb_step(g);                       /* DI */
        ASSERT_EQ(g->cpu.ime, false);
        gb_free(g);
    }
    {   /* HALT with pending+enabled interrupt and IME=0: halt bug (PC fails to advance) */
        GB *g = gb_with((uint8_t[]){0x76, 0x3C, 0x00}, 3); /* HALT; INC A */
        g->ie = INT_TIMER; g->iflag |= INT_TIMER;          /* pending immediately */
        g->cpu.ime = false;
        gb_step(g);                       /* HALT: does not halt, sets halt_bug */
        uint8_t a0 = g->cpu.a;
        gb_step(g);                       /* INC A executes... */
        gb_step(g);                       /* ...and executes AGAIN (PC stuck once) */
        ASSERT_EQ(g->cpu.a, (uint8_t)(a0 + 2));
        ASSERT_EQ(g->cpu.pc, 0x0102);     /* both INCs came from 0x0101 */
        gb_free(g);
    }
    TEST_MAIN_END();
}
