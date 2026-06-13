/*
 * ide.c — IDE state, panel rendering, headless shot helper.
 *
 * Canvas convention: 640 x 432 pixels (RGBA8888).
 *
 * Panel layout (pixels):
 *   [0] Game screen   x=  8 y=  8  w=336 h=304  (160x144 GB screen @ scale=2, inside a 336x304 box)
 *   [1] Registers     x=352 y=  8  w=280 h=152
 *   [2] VRAM viewer   x=352 y=168  w=280 h=148
 *   [3] Code pane     x=  8 y=320  w=336 h= 54
 *   [4] Tile editor   x=352 y=320  w=280 h= 54
 *   [5] Mem hex       x=  8 y=382  w=624 h= 40
 *   [6] Status line   x=  8 y=424  w=624 h=  8
 */

#include "ide.h"
#include "../live/live.h"
#include "../live/tile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * IDE state
 * ------------------------------------------------------------------------- */

struct IdeState {
    LiveSession *live;      /* non-NULL when .asm mode */
    GB          *gb;        /* always non-NULL after ide_new */
    char        *source;    /* .asm source text (heap), or NULL */
    int          selected_tile;
    int          paint_color; /* active paint color 0..3 */
    char         status[256];
    int          frame_counter;
    uint16_t     mem_base;  /* base address for hex panel (default 0xC000) */
};

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* read_file_binary — read a binary file into a heap buffer and return its byte
 * length in *out_len.  Returns NULL on error.  Caller must free the buffer. */
static uint8_t *read_file_binary(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = (size_t)len;
    return buf;
}

static int str_ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), su = strlen(suffix);
    if (sl < su) return 0;
    return strcmp(s + sl - su, suffix) == 0;
}

/* -------------------------------------------------------------------------
 * ide_new
 * ------------------------------------------------------------------------- */

IdeState *ide_new(const char *path) {
    if (!path) return NULL;

    IdeState *s = (IdeState *)calloc(1, sizeof(IdeState));
    if (!s) return NULL;

    s->mem_base = 0xC000;
    s->selected_tile = 0;
    snprintf(s->status, sizeof(s->status), "Ready");

    if (str_ends_with(path, ".asm")) {
        /* Read source and assemble */
        char *src = read_file(path);
        if (!src) {
            snprintf(s->status, sizeof(s->status), "Error: cannot open %s", path);
            free(s);
            return NULL;
        }
        LiveSession *live = live_new(src, path);
        if (!live) {
            snprintf(s->status, sizeof(s->status), "Error: assembly failed for %s", path);
            free(src);
            free(s);
            return NULL;
        }
        s->live   = live;
        s->gb     = live_gb(live);
        s->source = src;
        snprintf(s->status, sizeof(s->status), "Loaded: %s", path);
    } else {
        /* .gb mode: load ROM into a fresh GB */
        size_t rom_size = 0;
        uint8_t *rom = read_file_binary(path, &rom_size);
        if (!rom) {
            snprintf(s->status, sizeof(s->status), "Error: cannot open %s", path);
            free(s);
            return NULL;
        }
        GB *gb = gb_new();
        if (!gb) {
            free(rom);
            free(s);
            return NULL;
        }
        /* Use rom_size (actual byte count) not strlen(), which would stop at any
         * 0x00 byte in binary ROM data and report a truncated length. */
        if (!gb_load_rom(gb, rom, rom_size)) {
            /* if that fails, just use whatever rom bytes were there */
        }
        free(rom);
        gb_reset(gb);
        s->gb   = gb;
        s->live = NULL;
        snprintf(s->status, sizeof(s->status), "ROM loaded: %s", path);
    }

    return s;
}

/* -------------------------------------------------------------------------
 * ide_step_frame
 * ------------------------------------------------------------------------- */

void ide_step_frame(IdeState *s) {
    if (!s || !s->gb) return;
    s->gb->frame_ready = false;
    while (!s->gb->frame_ready)
        gb_step(s->gb);
    s->frame_counter++;
}

/* -------------------------------------------------------------------------
 * Accessors
 * ------------------------------------------------------------------------- */

GB *ide_gb(IdeState *s) { return s ? s->gb : NULL; }

void ide_select_tile(IdeState *s, int tile_index) {
    if (!s) return;
    s->selected_tile = tile_index;
}

int ide_selected_tile(IdeState *s) {
    return s ? s->selected_tile : 0;
}

const char *ide_status(IdeState *s) {
    return s ? s->status : "";
}

bool ide_is_asm(IdeState *s) {
    return s && s->live != NULL;
}

void ide_set_paint_color(IdeState *s, int color) {
    if (!s) return;
    if (color < 0) color = 0;
    if (color > 3) color = 3;
    s->paint_color = color;
}

int ide_paint_color(IdeState *s) {
    return s ? s->paint_color : 0;
}

/* -------------------------------------------------------------------------
 * Panel geometry — single source of truth used by both ide_render and the SDL
 * shell.  Keep these constants in sync with the draw calls in ide_render.
 * ------------------------------------------------------------------------- */

/* Panel rectangles: {x, y, w, h} */
static const int PANEL_RECTS[7][4] = {
    {  8,   8, 336, 304 },  /* PANEL_GAME        */
    { 352,  8, 280, 152 },  /* PANEL_REGISTERS   */
    { 352, 168, 280, 148 }, /* PANEL_VRAM_TILES  */
    {  8, 320, 336,  54 },  /* PANEL_CODE        */
    { 352, 320, 280,  54 }, /* PANEL_TILE_EDITOR */
    {  8, 382, 624,  40 },  /* PANEL_MEM_HEX     */
    {  8, 424, 624,   8 },  /* PANEL_STATUS      */
};

void ide_panel_rect(IdePanel panel, int *x, int *y, int *w, int *h) {
    int idx = (int)panel;
    if (idx < 0 || idx >= 7) {
        if (x) *x = 0; if (y) *y = 0; if (w) *w = 0; if (h) *h = 0;
        return;
    }
    if (x) *x = PANEL_RECTS[idx][0];
    if (y) *y = PANEL_RECTS[idx][1];
    if (w) *w = PANEL_RECTS[idx][2];
    if (h) *h = PANEL_RECTS[idx][3];
}

/* Tile editor zoom must match ide_render's Panel[4] zoom value */
#define TILE_EDITOR_ZOOM  5  /* each GB pixel -> 5x5 canvas pixels */
#define VRAM_TILE_COLS   16  /* columns of tiles in VRAM viewer */
#define VRAM_TILE_SCALE   1  /* 1:1 pixels, gap of 1 between tiles */

bool ide_mouse_paint(IdeState *s, int mx, int my) {
    if (!s || !s->live) return false;

    int px, py, pw, ph;
    ide_panel_rect(PANEL_TILE_EDITOR, &px, &py, &pw, &ph);

    /* Inner origin of the zoomed tile within the panel (matches ide_render) */
    int ox = px + 4;
    int oy = py + 12;

    /* Check bounds: the zoomed tile is 8*zoom x 8*zoom pixels */
    int tile_w = 8 * TILE_EDITOR_ZOOM;
    int tile_h = 8 * TILE_EDITOR_ZOOM;
    if (mx < ox || mx >= ox + tile_w) return false;
    if (my < oy || my >= oy + tile_h) return false;

    int tx = (mx - ox) / TILE_EDITOR_ZOOM;
    int ty = (my - oy) / TILE_EDITOR_ZOOM;

    /* tile_paint needs an asset_path — use NULL/"" to mean "any asset" is not
     * supported by the API; we pass the path stored in the live result. */
    AsmResult *res = live_result(s->live);
    if (!res || res->nassets == 0) return false;

    /* Find the asset that contains the selected tile */
    int ti = s->selected_tile;
    int offset = 0;
    int asset_idx = -1;
    for (int i = 0; i < res->nassets; i++) {
        int ntiles = (int)(res->assets[i].size / 16);
        if (ti < offset + ntiles) {
            asset_idx = i;
            ti -= offset;  /* tile index within this asset */
            break;
        }
        offset += ntiles;
    }
    if (asset_idx < 0) return false;

    char err[128] = "";
    return tile_paint(s->live, res->assets[asset_idx].path,
                      ti, tx, ty, (uint8_t)s->paint_color, err, sizeof(err));
}

int ide_select_tile_at(IdeState *s, int mx, int my) {
    if (!s) return -1;

    int px, py, pw, ph;
    ide_panel_rect(PANEL_VRAM_TILES, &px, &py, &pw, &ph);

    /* Inner origin (matches ide_render) */
    int ox = px + 4;
    int oy = py + 12;
    int cell = 8 * VRAM_TILE_SCALE + 1;  /* tile cell width including gap */

    if (mx < ox || my < oy) return -1;

    int col = (mx - ox) / cell;
    int row = (my - oy) / cell;
    if (col < 0 || col >= VRAM_TILE_COLS) return -1;

    int max_rows = (ph - 14) / cell;
    if (row < 0 || row >= max_rows) return -1;

    int idx = row * VRAM_TILE_COLS + col;
    if (idx < 0 || idx >= 64) return -1;

    ide_select_tile(s, idx);
    return idx;
}

bool ide_reload_from_file(IdeState *s, const char *path) {
    if (!s || !s->live || !path) return false;

    char *new_src = read_file(path);
    if (!new_src) {
        snprintf(s->status, sizeof(s->status), "Reload error: cannot read %s", path);
        return false;
    }

    PatchReport rep = live_reload(s->live, new_src);
    free(new_src);

    /* Build a short status string from the report */
    bool refused = rep.any_refused;
    if (refused) {
        snprintf(s->status, sizeof(s->status), "RELOAD REFUSED (%d patches)", rep.count);
    } else {
        int changed = 0;
        for (int i = 0; i < rep.count; i++)
            if (rep.items[i].kind != PATCH_UNCHANGED) changed++;
        snprintf(s->status, sizeof(s->status), "Reloaded: %d/%d patched", changed, rep.count);
    }

    patch_report_free(&rep);
    return !refused;
}

bool ide_soft_reset_from_file(IdeState *s, const char *path) {
    if (!s || !s->live || !path) return false;

    char *new_src = read_file(path);
    if (!new_src) {
        snprintf(s->status, sizeof(s->status), "Soft reset error: cannot read %s", path);
        return false;
    }

    /* Re-run from Main with a fresh reset: clears RAM/VRAM and re-executes
       init code (unlike F5/live_reload, which keeps running state). Use this
       to pick up init-time changes (tile/tilemap setup, etc.). */
    live_soft_reload(s->live, new_src);
    free(new_src);
    snprintf(s->status, sizeof(s->status), "Soft reset: re-ran from Main");
    return true;
}

/* -------------------------------------------------------------------------
 * Panel colors
 * ------------------------------------------------------------------------- */

#define COL_BG        0x1A1A2EFF  /* dark navy background */
#define COL_BORDER    0x4A4A7AFF  /* panel border */
#define COL_TITLE     0xE0E0FFFF  /* panel title text */
#define COL_TEXT      0xC0C0D0FF  /* normal text */
#define COL_DIM       0x606070FF  /* dim text */
#define COL_HIGHLIGHT 0xFFD700FF  /* gold highlight */
#define COL_PANEL_BG  0x12122AFF  /* slightly different panel bg */

/* DMG green palette: shade 0=lightest, 3=darkest (0xRRGGBBAA) */
static const uint32_t DMG_PAL[4] = {
    0xE0F8D0FF,  /* 0 — lightest green */
    0x88C070FF,  /* 1 */
    0x346856FF,  /* 2 */
    0x081820FF   /* 3 — darkest */
};

/* Gray palette for VRAM tile viewer (maps shade -> gray RGBA) */
static const uint32_t VRAM_PAL[4] = {
    0xFFFFFFFF,
    0xAAAAAAFF,
    0x555555FF,
    0x000000FF
};

/* -------------------------------------------------------------------------
 * Panel helpers
 * ------------------------------------------------------------------------- */

static void draw_panel(Canvas *c, int x, int y, int w, int h, const char *title) {
    ui_fill_rect(c, x, y, w, h, COL_PANEL_BG);
    ui_rect(c, x, y, w, h, COL_BORDER);
    if (title)
        ui_text(c, x + 2, y + 1, title, COL_TITLE);
}

/* -------------------------------------------------------------------------
 * ide_render — compose all panels
 * ------------------------------------------------------------------------- */

void ide_render(IdeState *s, Canvas *c) {
    if (!s || !c || !c->px) return;
    GB *gb = s->gb;

    /* 1. Clear background */
    ui_clear(c, COL_BG);

    /* -----------------------------------------------------------------------
     * Panel [0]: Game screen — x=8, y=8, w=336, h=304
     * The GB screen is 160x144 @ scale=2 => 320x288. Centered in 336x304.
     * --------------------------------------------------------------------- */
    {
        int px = 8, py = 8, pw = 336, ph = 304;
        draw_panel(c, px, py, pw, ph, "GAME");
        /* blit at (px+8, py+8) so there's an inner margin */
        ui_blit_gb(c, gb_framebuffer(gb), px + 8, py + 10, 2, DMG_PAL);
    }

    /* -----------------------------------------------------------------------
     * Panel [1]: Registers — x=352, y=8, w=280, h=152
     * --------------------------------------------------------------------- */
    {
        int px = 352, py = 8, pw = 280, ph = 152;
        draw_panel(c, px, py, pw, ph, "REGISTERS");

        CPU *cpu = &gb->cpu;
        char buf[80];
        int tx = px + 4, ty = py + 12;

        /* AF, BC, DE, HL */
        snprintf(buf, sizeof(buf), "AF=%02X%02X  BC=%02X%02X",
                 cpu->a, cpu->f, cpu->b, cpu->c);
        ui_text(c, tx, ty, buf, COL_TEXT); ty += 10;

        snprintf(buf, sizeof(buf), "DE=%02X%02X  HL=%02X%02X",
                 cpu->d, cpu->e, cpu->h, cpu->l);
        ui_text(c, tx, ty, buf, COL_TEXT); ty += 10;

        snprintf(buf, sizeof(buf), "SP=%04X  PC=%04X",
                 cpu->sp, cpu->pc);
        ui_text(c, tx, ty, buf, COL_TEXT); ty += 12;

        /* Flags: Z N H C — bright if set, dim if clear */
        uint8_t f = cpu->f;
        uint32_t cZ = (f & 0x80) ? COL_HIGHLIGHT : COL_DIM;
        uint32_t cN = (f & 0x40) ? COL_HIGHLIGHT : COL_DIM;
        uint32_t cH = (f & 0x20) ? COL_HIGHLIGHT : COL_DIM;
        uint32_t cC = (f & 0x10) ? COL_HIGHLIGHT : COL_DIM;

        ui_text(c, tx,      ty, "F:", COL_TEXT);
        ui_text(c, tx + 20, ty, "Z", cZ);
        ui_text(c, tx + 30, ty, "N", cN);
        ui_text(c, tx + 40, ty, "H", cH);
        ui_text(c, tx + 50, ty, "C", cC);
        ty += 12;

        /* PPU state */
        snprintf(buf, sizeof(buf), "MODE=%d  LY=%3d", gb->ppu_mode, gb->ly);
        ui_text(c, tx, ty, buf, COL_TEXT); ty += 10;

        snprintf(buf, sizeof(buf), "LCDC=%02X  BGP=%02X", gb->lcdc, gb->bgp);
        ui_text(c, tx, ty, buf, COL_TEXT); ty += 10;

        snprintf(buf, sizeof(buf), "SCX=%3d  SCY=%3d", gb->scx, gb->scy);
        ui_text(c, tx, ty, buf, COL_TEXT); ty += 10;

        snprintf(buf, sizeof(buf), "STAT=%02X", gb->stat);
        ui_text(c, tx, ty, buf, COL_DIM);
    }

    /* -----------------------------------------------------------------------
     * Panel [2]: VRAM tile viewer — x=352, y=168, w=280, h=148
     * Draw the first 64 tiles in an 8-column grid.
     * Each tile is drawn as 8x8 pixels (1:1), so 8 cols * 8px = 64px wide,
     * with a 2px gap -> 8*(8+1)+1 = 73px. Tiles are 2 bytes per row each.
     * --------------------------------------------------------------------- */
    {
        int px = 352, py = 168, pw = 280, ph = 148;
        draw_panel(c, px, py, pw, ph, "VRAM TILES");

        /* Draw 64 tiles in 8 columns x 8 rows. Each tile cell = 9x9 (8px + 1px gap). */
        int cols = 16;      /* 16 tiles per row */
        int scale = 1;
        int ox = px + 4;
        int oy = py + 12;

        for (int ti = 0; ti < 64 && ti < (0x1800 / 16); ti++) {
            int col = ti % cols;
            int row = ti / cols;
            int tx = ox + col * (8 * scale + 1);
            int ty = oy + row * (8 * scale + 1);

            const uint8_t *tile16 = &gb->vram[ti * 16];

            /* Draw a highlight border if this is the selected tile */
            if (ti == s->selected_tile)
                ui_rect(c, tx - 1, ty - 1, 8 * scale + 2, 8 * scale + 2, COL_HIGHLIGHT);

            for (int py2 = 0; py2 < 8; py2++) {
                for (int px2 = 0; px2 < 8; px2++) {
                    uint8_t shade = tile2bpp_get(tile16, px2, py2);
                    uint32_t col_rgba = VRAM_PAL[shade];
                    ui_fill_rect(c, tx + px2 * scale, ty + py2 * scale,
                                 scale, scale, col_rgba);
                }
            }
        }
    }

    /* -----------------------------------------------------------------------
     * Panel [3]: Code pane — x=8, y=320, w=336, h=54
     * Show up to 5 lines of source (8px font, 10px line spacing).
     * --------------------------------------------------------------------- */
    {
        int px = 8, py = 320, pw = 336, ph = 54;
        draw_panel(c, px, py, pw, ph, "CODE");

        int tx = px + 4, ty = py + 12;
        int line_h = 9;
        int max_lines = (ph - 14) / line_h;
        int max_cols  = (pw - 6) / 8;

        if (s->source) {
            const char *p = s->source;
            int ln = 0;
            while (*p && ln < max_lines) {
                /* find end of line */
                const char *eol = p;
                while (*eol && *eol != '\n') eol++;

                /* copy at most max_cols chars */
                char line[128];
                int len = (int)(eol - p);
                if (len > max_cols - 1) len = max_cols - 1;
                if (len < 0) len = 0;
                memcpy(line, p, (size_t)len);
                line[len] = '\0';

                ui_text(c, tx, ty, line, COL_TEXT);
                ty += line_h;
                ln++;

                if (*eol == '\n') eol++;
                p = eol;
            }
        } else {
            ui_text(c, tx, ty, "(no source)", COL_DIM);
        }
    }

    /* -----------------------------------------------------------------------
     * Panel [4]: Tile editor — x=352, y=320, w=280, h=54
     * Draw the selected tile zoomed: each pixel = 6x6, with a grid.
     * --------------------------------------------------------------------- */
    {
        int px = 352, py = 320, pw = 280, ph = 54;
        char title[32];
        snprintf(title, sizeof(title), "TILE %d", s->selected_tile);
        draw_panel(c, px, py, pw, ph, title);

        int zoom = 5;  /* each GB pixel -> zoom x zoom canvas pixels */
        int ox = px + 4;
        int oy = py + 12;

        int ti = s->selected_tile;
        if (ti < 0) ti = 0;
        if (ti >= 384) ti = 383;  /* VRAM holds 384 tiles (0x1800 bytes / 16) */

        const uint8_t *tile16 = &gb->vram[ti * 16];

        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                uint8_t shade = tile2bpp_get(tile16, col, row);
                uint32_t color = VRAM_PAL[shade];
                int sx = ox + col * zoom;
                int sy = oy + row * zoom;
                ui_fill_rect(c, sx, sy, zoom - 1, zoom - 1, color);
            }
        }
        /* Outer border around the zoomed tile */
        ui_rect(c, ox - 1, oy - 1, 8 * zoom + 1, 8 * zoom + 1, COL_BORDER);
    }

    /* -----------------------------------------------------------------------
     * Panel [5]: Memory hex — x=8, y=382, w=624, h=40
     * Show 4 rows x 8 bytes of memory from mem_base.
     * --------------------------------------------------------------------- */
    {
        int px = 8, py = 382, pw = 624, ph = 40;
        draw_panel(c, px, py, pw, ph, "MEMORY");

        int tx = px + 4, ty = py + 12;
        int line_h = 9;
        int rows = 3, bytes_per_row = 16;

        char buf[128];
        for (int r = 0; r < rows; r++) {
            uint16_t base_addr = (uint16_t)(s->mem_base + (uint16_t)(r * bytes_per_row));
            int pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%04X:", base_addr);
            char ascii[17];
            for (int b = 0; b < bytes_per_row && pos + 4 < (int)sizeof(buf); b++) {
                uint16_t addr = (uint16_t)(base_addr + (uint16_t)b);
                uint8_t val = gb_read8(gb, addr);
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, " %02X", val);
                ascii[b] = (val >= 0x20 && val < 0x7F) ? (char)val : '.';
            }
            ascii[bytes_per_row] = '\0';
            /* append ascii */
            if (pos + 3 < (int)sizeof(buf))
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "  |%s|", ascii);

            ui_text(c, tx, ty, buf, COL_TEXT);
            ty += line_h;
        }
    }

    /* -----------------------------------------------------------------------
     * Panel [6]: Status line — x=8, y=424, w=624, h=8
     * --------------------------------------------------------------------- */
    {
        char status_buf[256];
        snprintf(status_buf, sizeof(status_buf), "[F%d] %s",
                 s->frame_counter, s->status);
        ui_text(c, 8, 424, status_buf, COL_DIM);
    }
}

/* -------------------------------------------------------------------------
 * ide_free
 * ------------------------------------------------------------------------- */

void ide_free(IdeState *s) {
    if (!s) return;
    if (s->live) {
        live_free(s->live);  /* live_free frees the GB too */
    } else if (s->gb) {
        gb_free(s->gb);
    }
    free(s->source);
    free(s);
}

/* -------------------------------------------------------------------------
 * ide_shot — headless render
 * ------------------------------------------------------------------------- */

int ide_shot(const char *in_path, const char *out_png, int frames) {
    IdeState *s = ide_new(in_path);
    if (!s) return 1;

    for (int i = 0; i < frames; i++)
        ide_step_frame(s);

    Canvas c = canvas_new(640, 432);
    if (!c.px) {
        ide_free(s);
        return 1;
    }

    ide_render(s, &c);
    int rc = ui_save_png(&c, out_png);

    canvas_free(&c);
    ide_free(s);
    return rc;
}
