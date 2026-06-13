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
    {   /* window covering whole screen (WX=7,WY=0) shows window tilemap */
        GB *g = fresh();
        g->lcdc = 0xB1;             /* LCD on, BG on, window enable(bit5), tiledata 8000, winmap 9800(bit6=0) */
        g->bgp = 0xE4; g->wy = 0; g->wx = 7;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;   /* tile 1 = solid c3 */
        set_tile(g, 1, solid3);
        memset(g->vram + 0x1800, 0, 0x400);   /* bg map -> tile 0 (blank) */
        memset(g->vram + 0x1800, 1, 0x400);   /* but window uses 9800 too here; set all to tile 1 */
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);
        ASSERT_EQ(px(g, 159, 143), 3);
        gb_free(g);
    }
    {   /* window offset: WX=87 (screen x=80), WY=72. left/top half = BG(blank,0), bottom-right = window */
        GB *g = fresh();
        g->lcdc = 0xB1; g->bgp = 0xE4; g->wx = 87; g->wy = 72;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;
        set_tile(g, 1, solid3);
        memset(g->vram + 0x1800, 1, 0x400);   /* both BG and window map entries -> tile 1; BG tile 0 stays blank */
        /* make BG map all tile 0 (blank) and rely on window for tile 1: */
        memset(g->vram + 0x1800, 0, 0x400);   /* 9800 used as BG map -> tile 0 blank */
        memset(g->vram + 0x1C00, 1, 0x400);   /* 9C00 used as window map -> tile 1 */
        g->lcdc = 0xF1;                        /* also set bit6=1 so window map=9C00, bit3=0 bg map=9800 */
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 0);             /* BG region blank */
        ASSERT_EQ(px(g, 159, 143), 3);         /* window region solid */
        ASSERT_EQ(px(g, 79, 71), 0);           /* just outside window */
        ASSERT_EQ(px(g, 80, 72), 3);           /* first window pixel */
        gb_free(g);
    }
    {   /* window disabled (LCDC.5=0): window map ignored */
        GB *g = fresh();
        g->lcdc = 0x91; g->bgp = 0xE4; g->wx = 7; g->wy = 0;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;
        set_tile(g, 1, solid3);
        memset(g->vram + 0x1C00, 1, 0x400);
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 0);
        gb_free(g);
    }
    {   /* a single 8x8 sprite at (8,16)=>screen(0,0), solid color2, OBP0 identity */
        GB *g = fresh();
        g->lcdc = 0x93;            /* LCD on, BG on, OBJ enable(bit1), tiledata 8000 */
        g->bgp = 0xE4; g->obp0 = 0xE4;
        memset(g->vram + 0x1800, 0, 0x400);     /* blank BG */
        uint8_t solid2[16];
        for (int i=0;i<8;i++){ solid2[i*2]=0x00; solid2[i*2+1]=0xFF; } /* hi=FF lo=00 => color 2 */
        set_tile(g, 1, solid2);
        g->oam[0]=16; g->oam[1]=8; g->oam[2]=1; g->oam[3]=0x00;  /* y,x,tile,flags */
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 2);
        ASSERT_EQ(px(g, 7, 7), 2);
        ASSERT_EQ(px(g, 8, 0), 0);    /* outside sprite */
        gb_free(g);
    }
    {   /* sprite color 0 is transparent: BG shows through */
        GB *g = fresh();
        g->lcdc = 0x93; g->bgp = 0xE4; g->obp0 = 0xE4;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;  /* BG tile0 solid c3 */
        set_tile(g, 0, solid3);
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t t[16]; memset(t,0,16);  /* sprite tile1 all color 0 */
        set_tile(g, 1, t);
        g->oam[0]=16; g->oam[1]=8; g->oam[2]=1; g->oam[3]=0x00;
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);    /* BG shows; sprite transparent */
        gb_free(g);
    }
    {   /* OBJ-behind-BG priority (flags bit7=1): sprite hidden where BG color != 0 */
        GB *g = fresh();
        g->lcdc = 0x93; g->bgp = 0xE4; g->obp0 = 0xE4;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;
        set_tile(g, 0, solid3);            /* BG solid c3 */
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t solid2[16]; for(int i=0;i<8;i++){solid2[i*2]=0;solid2[i*2+1]=0xFF;}
        set_tile(g, 1, solid2);            /* sprite solid c2 */
        g->oam[0]=16; g->oam[1]=8; g->oam[2]=1; g->oam[3]=0x80;  /* bit7 set */
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);    /* BG wins because BG color != 0 */
        gb_free(g);
    }
    {   /* X-flip: pattern reversed */
        GB *g = fresh();
        g->lcdc = 0x93; g->bgp = 0xE4; g->obp0 = 0xE4;
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t t[16]; memset(t,0,16);
        t[1] = 0x80;     /* row0 hi byte bit7 -> color 2 at leftmost pixel */
        set_tile(g, 1, t);
        g->oam[0]=16; g->oam[1]=8; g->oam[2]=1; g->oam[3]=0x20;  /* X-flip */
        render_frame(g);
        ASSERT_EQ(px(g, 7, 0), 2);    /* leftmost pixel now at rightmost */
        ASSERT_EQ(px(g, 0, 0), 0);
        gb_free(g);
    }
    {   /* DMG X priority: lower X wins regardless of OAM order */
        GB *g = fresh();
        g->lcdc = 0x93; g->bgp = 0xE4; g->obp0 = 0xE4; g->obp1 = 0xE4;
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t s2[16]; for(int i=0;i<8;i++){s2[i*2]=0;s2[i*2+1]=0xFF;}   /* color2 */
        uint8_t s3[16]; for(int i=0;i<16;i++) s3[i]=0xFF;                  /* color3 */
        set_tile(g, 1, s2); set_tile(g, 2, s3);
        /* entry0: x=10 tile1(c2); entry1: x=8 tile2(c3). lower x (entry1) wins at overlap */
        g->oam[0]=16; g->oam[1]=10; g->oam[2]=1; g->oam[3]=0;
        g->oam[4]=16; g->oam[5]=8;  g->oam[6]=2; g->oam[7]=0;
        render_frame(g);
        ASSERT_EQ(px(g, 2, 0), 3);   /* overlap region: smaller-X sprite (c3) wins */
        gb_free(g);
    }
    {   /* 8x16 sprite: lower tile used for rows 8-15 */
        GB *g = fresh();
        g->lcdc = 0x97;             /* LCD on, BG on, OBJ enable, OBJ size 8x16(bit2), tiledata 8000 */
        g->bgp = 0xE4; g->obp0 = 0xE4;
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t top[16];    for(int i=0;i<8;i++){top[i*2]=0;top[i*2+1]=0xFF;}    /* c2 */
        uint8_t bot[16];    for(int i=0;i<16;i++) bot[i]=0xFF;                    /* c3 */
        set_tile(g, 2, top);   /* tile&0xFE = 2 */
        set_tile(g, 3, bot);   /* tile|1   = 3 */
        g->oam[0]=16; g->oam[1]=8; g->oam[2]=2; g->oam[3]=0;
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 2);    /* top half */
        ASSERT_EQ(px(g, 0, 8), 3);    /* bottom half */
        gb_free(g);
    }
    {   /* 10-sprites-per-line limit: 11th on the same line is dropped */
        GB *g = fresh();
        g->lcdc = 0x93; g->bgp = 0xE4; g->obp0 = 0xE4;
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t s3[16]; for(int i=0;i<16;i++) s3[i]=0xFF;
        set_tile(g, 1, s3);
        for (int i = 0; i < 11; i++) {     /* all on line 0, increasing X */
            g->oam[i*4+0]=16; g->oam[i*4+1]=(uint8_t)(8 + i*8);
            g->oam[i*4+2]=1;  g->oam[i*4+3]=0;
        }
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);          /* sprite 0 visible */
        ASSERT_EQ(px(g, 8*9, 0), 3);        /* sprite 9 (10th) visible */
        ASSERT_EQ(px(g, 8*10, 0), 0);       /* sprite 10 (11th) dropped */
        gb_free(g);
    }
    TEST_MAIN_END();
}
