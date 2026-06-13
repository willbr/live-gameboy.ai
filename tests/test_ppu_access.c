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
    {   /* OAM DMA copies 0xXX00-0xXX9F into OAM (FE00-FE9F) */
        GB *g = fresh();
        g->lcdc = 0x11;                   /* LCD off so OAM is readable for the check */
        for (int i = 0; i < 0xA0; i++) gb_write8(g, 0xC000 + i, (uint8_t)(i ^ 0x5A));
        gb_write8(g, 0xFF46, 0xC0);       /* DMA from 0xC000 */
        for (int i = 0; i < 0xA0; i++)
            ASSERT_EQ(gb_read8(g, 0xFE00 + i), (uint8_t)(i ^ 0x5A));
        ASSERT_EQ(gb_ppu_read(g, 0xFF46), 0xC0);   /* register reads back source hi */
        gb_free(g);
    }
    {   /* OAM DMA from VRAM during mode 3 copies real data, not PPU-blocked 0xFF */
        GB *g = fresh();                  /* LCD on */
        for (int i = 0; i < 0xA0; i++) g->vram[i] = (uint8_t)(i ^ 0x3C);  /* seed VRAM directly */
        gb_ppu_tick(g, 80);               /* enter mode 3 (VRAM blocked on CPU bus) */
        ASSERT_EQ(g->ppu_mode, 3);
        ASSERT_EQ(gb_read8(g, 0x8000), 0xFF);     /* confirm CPU VRAM read is blocked */
        gb_write8(g, 0xFF46, 0x80);       /* DMA from 0x8000 (VRAM) */
        for (int i = 0; i < 0xA0; i++)
            ASSERT_EQ(g->oam[i], (uint8_t)(i ^ 0x3C));   /* DMA bypassed the block */
        gb_free(g);
    }
    TEST_MAIN_END();
}
