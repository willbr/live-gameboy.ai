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
    g->fx = 0; g->discard = 0; g->fetch_x = 0; g->bg_fifo_n = 0;
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
            if (g->fx >= 160) {
                if (g->win_started) g->win_line++;
                set_mode(g, MODE_HBLANK);
            }
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

/* ---- BG pixel FIFO rendering (Task 4) ---- */

/* apply a 2-bit palette: color index -> shade */
static uint8_t pal_shade(uint8_t pal, uint8_t color) {
    return (uint8_t)((pal >> (color * 2)) & 0x03);
}

/* read a BG/window tile row's two bytes for tile column `tx`, pixel row `py` */
static void fetch_bg_row(GB *g, int map_base, int tx, int ty, int py,
                         uint8_t *lo, uint8_t *hi) {
    int map_off = map_base + (ty & 31) * 32 + (tx & 31);
    uint8_t tile = g->vram[map_off];
    int addr;
    if (g->lcdc & 0x10) addr = tile * 16;                 /* 0x8000 unsigned */
    else                addr = 0x1000 + (int8_t)tile * 16; /* 0x9000 signed */
    *lo = g->vram[addr + py * 2];
    *hi = g->vram[addr + py * 2 + 1];
}

static void render_begin_line(GB *g) {
    g->fx = 0;
    g->bg_fifo_n = 0;
    g->fetch_x = 0;
    g->window_active = false;
    g->win_started = false;
    g->discard = g->scx & 7;          /* fine-scroll discard counter */

    /* Mode 2 OAM scan: select up to 10 sprites covering this line, in OAM order */
    g->spr_count = 0;
    int height = (g->lcdc & 0x04) ? 16 : 8;
    for (int i = 0; i < 40 && g->spr_count < 10; i++) {
        int sy = g->oam[i * 4 + 0];
        int line = g->ly + 16 - sy;
        if (line >= 0 && line < height)
            g->spr_idx[g->spr_count++] = (uint8_t)i;
    }
}

/* push 8 BG/window pixels for the next tile column into the fifo */
static void bg_fetch_tile(GB *g) {
    int map_base, tx, ty, py;
    if (g->window_active) {
        map_base = (g->lcdc & 0x40) ? 0x1C00 : 0x1800;   /* window map: LCDC.6 */
        tx = g->fetch_x;
        ty = g->win_line / 8;
        py = g->win_line % 8;
    } else {
        map_base = (g->lcdc & 0x08) ? 0x1C00 : 0x1800;   /* BG map: LCDC.3 */
        tx = (g->scx / 8) + g->fetch_x;
        ty = ((g->scy + g->ly) / 8);
        py = (g->scy + g->ly) % 8;
    }
    uint8_t lo, hi;
    fetch_bg_row(g, map_base, tx, ty, py, &lo, &hi);
    for (int b = 7; b >= 0; b--) {
        uint8_t color = (uint8_t)(((hi >> b) & 1) << 1 | ((lo >> b) & 1));
        g->bg_fifo_c[g->bg_fifo_n++] = color;
    }
    g->fetch_x++;
}

static void render_step(GB *g) {
    if (g->fx >= 160) return;

    /* window activation: when enabled, WY reached, and fx >= WX-7 */
    if (!g->window_active && (g->lcdc & 0x20) && (g->lcdc & 0x01)
        && g->ly >= g->wy && g->fx >= (g->wx - 7) && g->wx <= 166) {
        g->window_active = true;
        g->win_started = true;
        g->bg_fifo_n = 0;          /* flush BG fifo; window starts fresh */
        g->fetch_x = 0;
        g->discard = 0;            /* window has no fine X scroll */
    }

    /* ensure the fifo has pixels; the BG fetcher pushes 8 at a time */
    if (g->bg_fifo_n == 0) bg_fetch_tile(g);

    /* discard SCX&7 pixels at the very start of the line */
    while (g->discard > 0 && g->bg_fifo_n > 0) {
        memmove(g->bg_fifo_c, g->bg_fifo_c + 1, (size_t)(--g->bg_fifo_n));
        g->discard--;
        if (g->bg_fifo_n == 0) bg_fetch_tile(g);
    }
    if (g->bg_fifo_n == 0) return;

    uint8_t color = g->bg_fifo_c[0];
    memmove(g->bg_fifo_c, g->bg_fifo_c + 1, (size_t)(--g->bg_fifo_n));

    if (!(g->lcdc & 0x01)) color = 0;     /* BG/window disabled -> color 0 */
    uint8_t bg_color = color;             /* pre-palette BG color for priority test */
    uint8_t shade = pal_shade(g->bgp, color);

    /* sprites */
    if ((g->lcdc & 0x02) && g->spr_count > 0) {
        int best = -1, best_x = 256;
        uint8_t best_color = 0, best_pal = 0, best_prio = 0;
        int height = (g->lcdc & 0x04) ? 16 : 8;
        for (int k = 0; k < g->spr_count; k++) {
            int i = g->spr_idx[k];
            int sx = g->oam[i*4+1];
            int screen_x = sx - 8;
            if (g->fx < screen_x || g->fx >= screen_x + 8) continue;
            int col_in = g->fx - screen_x;          /* 0..7 from left */
            uint8_t flags = g->oam[i*4+3];
            if (flags & 0x20) col_in = 7 - col_in;  /* X-flip */
            int line = g->ly + 16 - g->oam[i*4+0];  /* 0..height-1 */
            if (flags & 0x40) line = height - 1 - line;  /* Y-flip */
            int tile = g->oam[i*4+2];
            if (height == 16) { tile &= 0xFE; if (line >= 8) { tile |= 1; line -= 8; } }
            uint8_t lo = g->vram[tile*16 + line*2];
            uint8_t hi = g->vram[tile*16 + line*2 + 1];
            int b = 7 - col_in;
            uint8_t scolor = (uint8_t)(((hi >> b) & 1) << 1 | ((lo >> b) & 1));
            if (scolor == 0) continue;              /* transparent */
            /* DMG priority: smallest X wins; tie -> earliest OAM (k order) */
            if (sx < best_x) {
                best = i; best_x = sx; best_color = scolor;
                best_pal = (flags & 0x10) ? g->obp1 : g->obp0;
                best_prio = flags & 0x80;
            }
        }
        if (best >= 0) {
            bool behind = best_prio && (bg_color != 0);
            if (!behind) shade = pal_shade(best_pal, best_color);
        }
    }

    g->framebuffer[g->ly * 160 + g->fx] = shade;
    g->fx++;
}

/* ---- OAM DMA (Task 3) ---- */
static void oam_dma(GB *g, uint8_t hi) {
    g->oam_dma_src = hi;
    uint16_t src = (uint16_t)hi << 8;
    for (int i = 0; i < 0xA0; i++) {
        uint16_t a = (uint16_t)(src + i);
        /* DMA has independent bus access — read VRAM directly so a DMA-from-VRAM
           during Mode 3 copies real data, not the PPU-blocked 0xFF. Other regions
           go through gb_read8 to respect MBC banking. (Simplified: instantaneous.) */
        g->oam[i] = (a >= 0x8000 && a < 0xA000) ? g->vram[a - 0x8000]
                                                : gb_read8(g, a);
    }
}
