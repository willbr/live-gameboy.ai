#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static GB *fresh(void) {
    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    g->lcdc = 0x91;          /* LCD on, BG on, tiledata 8000 */
    return g;
}

int main(void) {
    {   /* line 0 starts in mode 2, LY=0 */
        GB *g = fresh();
        ASSERT_EQ(g->ly, 0);
        ASSERT_EQ(g->ppu_mode, 2);
        gb_free(g);
    }
    {   /* mode 2 lasts 80 dots, then mode 3 */
        GB *g = fresh();
        gb_ppu_tick(g, 79);
        ASSERT_EQ(g->ppu_mode, 2);
        gb_ppu_tick(g, 1);
        ASSERT_EQ(g->ppu_mode, 3);
        gb_free(g);
    }
    {   /* a full line is 456 dots; after 456, LY=1 and back to mode 2 */
        GB *g = fresh();
        gb_ppu_tick(g, 456);
        ASSERT_EQ(g->ly, 1);
        ASSERT_EQ(g->ppu_mode, 2);
        gb_free(g);
    }
    {   /* VBlank begins at LY=144: mode 1 and INT_VBLANK requested */
        GB *g = fresh();
        g->iflag = 0;
        gb_ppu_tick(g, 456 * 144);
        ASSERT_EQ(g->ly, 144);
        ASSERT_EQ(g->ppu_mode, 1);
        ASSERT_EQ(g->iflag & INT_VBLANK, INT_VBLANK);
        ASSERT_TRUE(g->frame_ready);
        gb_free(g);
    }
    {   /* frame wraps at 154 lines: 456*154 dots returns to LY=0 mode 2 */
        GB *g = fresh();
        gb_ppu_tick(g, 456 * 154);
        ASSERT_EQ(g->ly, 0);
        ASSERT_EQ(g->ppu_mode, 2);
        gb_free(g);
    }
    {   /* LYC coincidence sets STAT bit 2 and, if enabled, requests STAT int */
        GB *g = fresh();
        g->lyc = 2;
        g->stat = 0x40;           /* enable LYC interrupt (bit 6) */
        g->iflag = 0;
        gb_ppu_tick(g, 456 * 2);  /* advance to LY=2 */
        ASSERT_EQ(g->ly, 2);
        ASSERT_EQ(g->stat & 0x04, 0x04);          /* coincidence flag */
        ASSERT_EQ(g->iflag & INT_STAT, INT_STAT);
        gb_free(g);
    }
    {   /* LCD disabled (LCDC.7=0): LY=0, mode 0, no progress */
        GB *g = fresh();
        g->lcdc = 0x11;           /* LCD off */
        gb_ppu_reset(g);          /* reset re-reads lcdc state */
        g->lcdc = 0x11;
        gb_ppu_tick(g, 456 * 10);
        ASSERT_EQ(g->ly, 0);
        ASSERT_EQ(g->ppu_mode, 0);
        gb_free(g);
    }
    {   /* register read-back: STAT bit7=1, mode in low bits; LY read */
        GB *g = fresh();
        gb_ppu_tick(g, 80);                 /* now mode 3 */
        uint8_t st = gb_ppu_read(g, 0xFF41);
        ASSERT_EQ(st & 0x80, 0x80);
        ASSERT_EQ(st & 0x03, 3);
        ASSERT_EQ(gb_ppu_read(g, 0xFF44), g->ly);
        gb_free(g);
    }
    TEST_MAIN_END();
}
