#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

int main(void) {
    uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);

    {   /* DIV: increments every 256 T-cycles; write resets to 0 */
        gb_reset(g);
        g->div16 = 0;
        gb_tick(g, 255);
        ASSERT_EQ(gb_read8(g, 0xFF04), 0x00);
        gb_tick(g, 1);
        ASSERT_EQ(gb_read8(g, 0xFF04), 0x01);
        gb_write8(g, 0xFF04, 0x55);             /* any write clears */
        ASSERT_EQ(gb_read8(g, 0xFF04), 0x00);
    }
    {   /* TIMA at 262144 Hz (TAC=0b101 -> every 16 T-cycles) */
        gb_reset(g);
        g->div16 = 0;
        gb_write8(g, 0xFF07, 0x05);
        gb_write8(g, 0xFF05, 0x00);
        gb_tick(g, 16 * 10);
        ASSERT_EQ(gb_read8(g, 0xFF05), 10);
    }
    {   /* overflow: TIMA reloads from TMA and raises INT_TIMER */
        gb_reset(g);
        g->div16 = 0;
        gb_write8(g, 0xFF07, 0x05);
        gb_write8(g, 0xFF06, 0xAB);             /* TMA */
        gb_write8(g, 0xFF05, 0xFF);
        g->iflag = 0;
        gb_tick(g, 16);
        ASSERT_EQ(gb_read8(g, 0xFF05), 0xAB);
        ASSERT_EQ(g->iflag & INT_TIMER, INT_TIMER);
    }
    {   /* disabled timer doesn't count */
        gb_reset(g);
        gb_write8(g, 0xFF07, 0x01);             /* freq bits set but enable bit clear */
        gb_write8(g, 0xFF05, 0x00);
        gb_tick(g, 4096);
        ASSERT_EQ(gb_read8(g, 0xFF05), 0x00);
    }
    gb_free(g);
    TEST_MAIN_END();
}
