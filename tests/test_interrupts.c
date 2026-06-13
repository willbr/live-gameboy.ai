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
    {   /* dispatch: 20 cycles, IME off, IF bit cleared, PC = vector, old PC pushed */
        GB *g = gb_with((uint8_t[]){0x00, 0x00}, 2);
        g->cpu.ime = true;
        g->ie = INT_TIMER; g->iflag |= INT_TIMER;
        ASSERT_EQ(gb_step(g), 20);
        ASSERT_EQ(g->cpu.pc, 0x0050);              /* timer vector */
        ASSERT_EQ(g->cpu.ime, false);
        ASSERT_EQ(g->iflag & INT_TIMER, 0);
        ASSERT_EQ(gb_read8(g, g->cpu.sp), 0x00);   /* pushed PC lo */
        ASSERT_EQ(gb_read8(g, g->cpu.sp + 1), 0x01);
        gb_free(g);
    }
    {   /* priority: VBlank (bit 0) wins over timer */
        GB *g = gb_with((uint8_t[]){0x00}, 1);
        g->cpu.ime = true;
        g->ie = INT_VBLANK | INT_TIMER;
        g->iflag |= INT_VBLANK | INT_TIMER;
        gb_step(g);
        ASSERT_EQ(g->cpu.pc, 0x0040);
        ASSERT_EQ(g->iflag & INT_VBLANK, 0);
        ASSERT_EQ(g->iflag & INT_TIMER, INT_TIMER); /* still pending */
        gb_free(g);
    }
    {   /* HALT wakes on pending interrupt even with IME=0, no dispatch */
        GB *g = gb_with((uint8_t[]){0x76, 0x3C}, 2);   /* HALT; INC A */
        g->cpu.ime = false;
        g->ie = INT_TIMER;
        gb_step(g);                  /* halts (no pending yet) */
        ASSERT_TRUE(g->cpu.halted);
        gb_step(g);                  /* still halted, 4cy idle */
        g->iflag |= INT_TIMER;
        gb_step(g);                  /* wakes */
        ASSERT_TRUE(!g->cpu.halted);
        gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x02);   /* INC A ran (A was 0x01 at reset) */
        gb_free(g);
    }
    {   /* RETI re-enables IME immediately */
        GB *g = gb_with((uint8_t[]){0xD9}, 1);
        g->cpu.sp = 0xFFFC;
        gb_write8(g, 0xFFFC, 0x00); gb_write8(g, 0xFFFD, 0x05);
        ASSERT_EQ(gb_step(g), 16);
        ASSERT_EQ(g->cpu.pc, 0x0500);
        ASSERT_EQ(g->cpu.ime, true);
        gb_free(g);
    }
    TEST_MAIN_END();
}
