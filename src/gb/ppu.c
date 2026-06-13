#include "gb.h"
#include <string.h>

#define MODE_HBLANK 0
#define MODE_VBLANK 1
#define MODE_OAM    2
#define MODE_DRAW   3

#define LCD_ON(g)   ((g)->lcdc & 0x80)

/* forward decls for the rendering pipeline (filled in later tasks) */
static void render_begin_line(GB *g);  /* set up mode-3 fifo state for current LY */
static void render_step(GB *g);        /* produce up to one pixel; advances g->fx */

void gb_ppu_reset(GB *g) {
    g->lcdc = 0x91; g->stat = 0x85; g->scy = 0; g->scx = 0;
    g->ly = 0; g->lyc = 0; g->bgp = 0xFC; g->obp0 = 0xFF; g->obp1 = 0xFF;
    g->wy = 0; g->wx = 0;
    g->ppu_mode = MODE_OAM; g->ppu_dot = 0; g->stat_line = false;
    g->win_line = 0; g->frame_ready = false;
    g->fx = 0; g->fetch_step = 0; g->fetch_x = 0; g->bg_fifo_n = 0;
    g->window_active = false; g->oam_dma_src = 0;
    memset(g->framebuffer, 0, sizeof g->framebuffer);
}

/* ---- STAT interrupt line (rising-edge detector) ---- */
static void update_stat_line(GB *g) {
    bool coin = (g->ly == g->lyc);
    if (coin) g->stat |= 0x04; else g->stat &= ~0x04;

    bool line = false;
    if ((g->stat & 0x40) && coin)                 line = true;
    if ((g->stat & 0x20) && g->ppu_mode == MODE_OAM)    line = true;
    if ((g->stat & 0x10) && g->ppu_mode == MODE_VBLANK) line = true;
    if ((g->stat & 0x08) && g->ppu_mode == MODE_HBLANK) line = true;

    if (line && !g->stat_line) g->iflag |= INT_STAT;   /* rising edge */
    g->stat_line = line;
}

static void set_mode(GB *g, int mode) {
    g->ppu_mode = mode;
    g->stat = (uint8_t)((g->stat & ~0x03) | (mode & 0x03));
}

static void tick_one(GB *g) {
    if (!LCD_ON(g)) {
        /* LCD off: PPU idle, LY=0, mode 0, coincidence cleared */
        g->ly = 0; g->ppu_dot = 0; set_mode(g, MODE_HBLANK);
        g->stat &= ~0x04; g->stat_line = false; g->frame_ready = false;
        return;
    }

    g->ppu_dot++;

    if (g->ly < 144) {
        if (g->ppu_dot == 80 && g->ppu_mode == MODE_OAM) {
            set_mode(g, MODE_DRAW);
            render_begin_line(g);
        } else if (g->ppu_mode == MODE_DRAW) {
            render_step(g);
            if (g->fx >= 160) set_mode(g, MODE_HBLANK);
        }
    }

    if (g->ppu_dot >= 456) {
        g->ppu_dot = 0;
        g->ly++;
        if (g->ly == 144) {
            set_mode(g, MODE_VBLANK);
            g->iflag |= INT_VBLANK;
            g->frame_ready = true;
            g->win_line = 0;
        } else if (g->ly > 153) {
            g->ly = 0;
            set_mode(g, MODE_OAM);
            g->win_line = 0;
        } else if (g->ly < 144) {
            set_mode(g, MODE_OAM);
        }
    }

    update_stat_line(g);
}

void gb_ppu_tick(GB *g, int tcycles) {
    for (int i = 0; i < tcycles; i++) tick_one(g);
}

/* ---- registers ---- */
uint8_t gb_ppu_read(GB *g, uint16_t a) {
    switch (a) {
    case 0xFF40: return g->lcdc;
    case 0xFF41: return g->stat | 0x80;
    case 0xFF42: return g->scy;
    case 0xFF43: return g->scx;
    case 0xFF44: return g->ly;
    case 0xFF45: return g->lyc;
    case 0xFF46: return g->oam_dma_src;
    case 0xFF47: return g->bgp;
    case 0xFF48: return g->obp0;
    case 0xFF49: return g->obp1;
    case 0xFF4A: return g->wy;
    case 0xFF4B: return g->wx;
    default:     return 0xFF;
    }
}

static void oam_dma(GB *g, uint8_t hi);   /* Task 3 */

void gb_ppu_write(GB *g, uint16_t a, uint8_t v) {
    switch (a) {
    case 0xFF40: {
        bool was_on = LCD_ON(g);
        g->lcdc = v;
        if (was_on && !LCD_ON(g)) {      /* turning off resets the PPU */
            g->ly = 0; g->ppu_dot = 0; set_mode(g, MODE_HBLANK);
            g->stat_line = false;
        } else if (!was_on && LCD_ON(g)) {
            g->ppu_dot = 0; g->ly = 0; set_mode(g, MODE_OAM);
        }
        break;
    }
    case 0xFF41: g->stat = (uint8_t)((g->stat & 0x87) | (v & 0x78)); break;
    case 0xFF42: g->scy = v; break;
    case 0xFF43: g->scx = v; break;
    case 0xFF44: break;                  /* LY read-only */
    case 0xFF45: g->lyc = v; update_stat_line(g); break;
    case 0xFF46: oam_dma(g, v); break;
    case 0xFF47: g->bgp = v; break;
    case 0xFF48: g->obp0 = v; break;
    case 0xFF49: g->obp1 = v; break;
    case 0xFF4A: g->wy = v; break;
    case 0xFF4B: g->wx = v; break;
    }
}

bool gb_ppu_vram_blocked(const GB *g) {
    return LCD_ON(g) && g->ppu_mode == MODE_DRAW;
}
bool gb_ppu_oam_blocked(const GB *g) {
    return LCD_ON(g) && (g->ppu_mode == MODE_OAM || g->ppu_mode == MODE_DRAW);
}

const uint8_t *gb_framebuffer(const GB *g) { return g->framebuffer; }

/* ---- rendering stubs (replaced in Tasks 4-6) ---- */
static void render_begin_line(GB *g) { g->fx = 0; }
static void render_step(GB *g) { g->fx++; }   /* advance so mode 3 terminates */

/* ---- OAM DMA (Task 3) ---- */
static void oam_dma(GB *g, uint8_t hi) {
    g->oam_dma_src = hi;
    uint16_t src = (uint16_t)hi << 8;
    for (int i = 0; i < 0xA0; i++)
        g->oam[i] = gb_read8(g, (uint16_t)(src + i));   /* simplified: instantaneous copy */
}
