#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static GB *fresh(void) {
    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}

/* write tile `idx` (8000 method) as 8 rows of (lo,hi) byte pairs */
static void set_tile(GB *g, int idx, const uint8_t rows[16]) {
    for (int i = 0; i < 16; i++) g->vram[idx * 16 + i] = rows[i];
}
/* run one whole frame so every visible line renders */
static void render_frame(GB *g) {
    g->frame_ready = false;
    gb_ppu_tick(g, 456 * 154);
}
static uint8_t px(GB *g, int x, int y) { return g->framebuffer[y * 160 + x]; }

int main(void) {
    {   /* solid color-3 tile across the whole BG, identity palette -> shade 3 */
        GB *g = fresh();
        g->lcdc = 0x91;                 /* LCD on, BG on, tiledata=8000, bgmap=9800 */
        g->bgp = 0xE4;                  /* identity: 3->3,2->2,1->1,0->0 */
        uint8_t solid3[16];
        for (int i = 0; i < 16; i++) solid3[i] = 0xFF;   /* lo=FF,hi=FF => color 3 */
        set_tile(g, 0, solid3);
        memset(g->vram + 0x1800, 0, 0x400);    /* 9800 map all -> tile 0 */
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);
        ASSERT_EQ(px(g, 159, 143), 3);
        gb_free(g);
    }
    {   /* palette remap: color 3 -> shade 1 */
        GB *g = fresh();
        g->lcdc = 0x91;
        g->bgp = 0x1B;          /* bits7-6=00(c3->0)? compute below */
        /* BGP bits: [7:6]=c3 [5:4]=c2 [3:2]=c1 [1:0]=c0. We want c3->1 => set bits7-6=01 */
        g->bgp = (1 << 6);      /* c3->1, others ->0 */
        uint8_t solid3[16]; for (int i=0;i<16;i++) solid3[i]=0xFF;
        set_tile(g, 0, solid3);
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        ASSERT_EQ(px(g, 5, 5), 1);
        gb_free(g);
    }
    {   /* horizontal pattern within a tile: lo=0xA0 hi=0x00 -> pixels: c1 0 c1 0 0 0 0 0 */
        GB *g = fresh();
        g->lcdc = 0x91; g->bgp = 0xE4;
        uint8_t t[16]; memset(t, 0, 16);
        t[0] = 0xA0;            /* row 0 low byte = 1010_0000 */
        set_tile(g, 0, t);
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 1);   /* bit7 set */
        ASSERT_EQ(px(g, 1, 0), 0);   /* bit6 clear */
        ASSERT_EQ(px(g, 2, 0), 1);   /* bit5 set */
        ASSERT_EQ(px(g, 3, 0), 0);
        gb_free(g);
    }
    {   /* SCX fine scroll by 2: the pattern shifts left by 2 */
        GB *g = fresh();
        g->lcdc = 0x91; g->bgp = 0xE4; g->scx = 2;
        uint8_t t[16]; memset(t, 0, 16);
        t[0] = 0xA0;
        set_tile(g, 0, t);
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        /* original col2 (c1) now at col0 */
        ASSERT_EQ(px(g, 0, 0), 1);
        gb_free(g);
    }
    {   /* LCDC.0=0 -> BG off -> all shade 0 */
        GB *g = fresh();
        g->lcdc = 0x90;                /* BG/win disable */
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;
        set_tile(g, 0, solid3);
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        ASSERT_EQ(px(g, 80, 72), 0);
        gb_free(g);
    }
    {   /* signed tile addressing (LCDC.4=0): tile index 0 at 0x9000 */
        GB *g = fresh();
        g->lcdc = 0x81;               /* LCD on, BG on, tiledata=8800/signed, bgmap=9800 */
        g->bgp = 0xE4;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;
        /* 0x9000 is vram offset 0x1000; tile index 0 signed => 0x9000 */
        for (int i=0;i<16;i++) g->vram[0x1000 + i] = solid3[i];
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);
        gb_free(g);
    }
    TEST_MAIN_END();
}
