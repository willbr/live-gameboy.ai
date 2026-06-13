#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static GB *fresh(void) {
    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    g->lcdc = 0x91;
    return g;
}

int main(void) {
    {   /* during mode 3: VRAM reads 0xFF, writes ignored */
        GB *g = fresh();
        gb_write8(g, 0x8000, 0x11);   /* mode 2 at dot 0: VRAM writable */
        gb_ppu_tick(g, 80);           /* enter mode 3 */
        ASSERT_EQ(g->ppu_mode, 3);
        ASSERT_EQ(gb_read8(g, 0x8000), 0xFF);   /* blocked read */
        gb_write8(g, 0x8000, 0x22);             /* blocked write */
        gb_ppu_tick(g, 376);          /* finish line -> back to mode 2 */
        ASSERT_EQ(gb_read8(g, 0x8000), 0x11);   /* original survived */
        gb_free(g);
    }
    {   /* during mode 2 and 3: OAM blocked; during HBlank/VBlank: OK */
        GB *g = fresh();
        ASSERT_EQ(g->ppu_mode, 2);
        ASSERT_EQ(gb_read8(g, 0xFE00), 0xFF);   /* mode 2 blocks OAM */
        gb_ppu_tick(g, 80);
        ASSERT_EQ(gb_read8(g, 0xFE00), 0xFF);   /* mode 3 blocks OAM */
        gb_free(g);
    }
    {   /* LCD off: VRAM/OAM always accessible regardless of stale mode */
        GB *g = fresh();
        gb_ppu_tick(g, 80);           /* mode 3 */
        g->lcdc = 0x11;               /* turn LCD off */
        gb_write8(g, 0x8000, 0x33);
        ASSERT_EQ(gb_read8(g, 0x8000), 0x33);
        gb_write8(g, 0xFE00, 0x44);
        ASSERT_EQ(gb_read8(g, 0xFE00), 0x44);
        gb_free(g);
    }
    TEST_MAIN_END();
}
