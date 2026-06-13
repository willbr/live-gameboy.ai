#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

int main(void) {
    uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    rom[0x0000] = 0xAA;
    rom[0x7FFF] = 0xBB;

    GB *g = gb_new();
    ASSERT_TRUE(g != NULL);
    ASSERT_TRUE(gb_load_rom(g, rom, sizeof rom));
    gb_reset(g);

    /* ROM reads, ROM writes ignored (no MBC yet) */
    ASSERT_EQ(gb_read8(g, 0x0000), 0xAA);
    ASSERT_EQ(gb_read8(g, 0x7FFF), 0xBB);
    gb_write8(g, 0x0000, 0x12);
    ASSERT_EQ(gb_read8(g, 0x0000), 0xAA);

    /* WRAM + echo */
    gb_write8(g, 0xC123, 0x55);
    ASSERT_EQ(gb_read8(g, 0xC123), 0x55);
    ASSERT_EQ(gb_read8(g, 0xE123), 0x55);   /* echo of C000-DDFF */
    gb_write8(g, 0xE200, 0x66);
    ASSERT_EQ(gb_read8(g, 0xC200), 0x66);

    /* VRAM, OAM, HRAM */
    gb_write8(g, 0x8000, 0x11); ASSERT_EQ(gb_read8(g, 0x8000), 0x11);
    gb_write8(g, 0xFE00, 0x22); ASSERT_EQ(gb_read8(g, 0xFE00), 0x22);
    gb_write8(g, 0xFF80, 0x33); ASSERT_EQ(gb_read8(g, 0xFF80), 0x33);

    /* unusable region reads FF */
    ASSERT_EQ(gb_read8(g, 0xFEA0), 0xFF);

    /* IE / IF */
    gb_write8(g, 0xFFFF, 0x1F); ASSERT_EQ(gb_read8(g, 0xFFFF), 0x1F);
    gb_write8(g, 0xFF0F, 0x05); ASSERT_EQ(gb_read8(g, 0xFF0F), 0xE5); /* upper bits read 1 */

    /* reset state (DMG post-boot) */
    ASSERT_EQ(g->cpu.pc, 0x0100);
    ASSERT_EQ(g->cpu.sp, 0xFFFE);
    ASSERT_EQ(g->cpu.a, 0x01);
    ASSERT_EQ(g->cpu.f, 0xB0);

    gb_free(g);
    TEST_MAIN_END();
}
