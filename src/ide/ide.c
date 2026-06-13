/*
 * ide.c — IDE state, panel rendering, headless shot helper.
 *
 * Canvas convention: 1024 x 720 pixels (RGBA8888).
 *
 * Panel layout (pixels):
 *   [0]  Game screen    x=  8 y=  8  w=336 h=304
 *   [1]  Registers      x=352 y=  8  w=320 h=120
 *   [2]  VRAM tiles     x=680 y=  8  w=336 h=148
 *   [3]  Code pane      x=  8 y=320  w=336 h=150
 *   [4]  Tile editor    x=  8 y=478  w=336 h=120
 *   [5]  Mem hex        x=  8 y=606  w=1008 h= 80
 *   [6]  Status line    x=  8 y=706  w=1008 h=  8
 *   [7]  Exec           x=352 y=136  w=320 h= 62
 *   [8]  Disasm         x=352 y=206  w=320 h=392
 *   [9]  Palette        x=680 y=164  w=336 h= 36
 *   [10] OAM            x=680 y=208  w=336 h=192
 *   [11] Tilemap        x=680 y=408  w=336 h=190
 *   [12] Addr input     x=  8 y=690  w=1008 h= 12
 */

#include "ide.h"
#include "panels.h"
#include "../live/live.h"
#include "../live/tile.h"
#include "../gb/debug.h"

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
    ExecMode     exec_mode; /* execution-control state machine */
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

    gb_debug_attach(s->gb);
    s->exec_mode = EXEC_RUNNING;

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
 * Execution-control state machine
 * ------------------------------------------------------------------------- */

ExecMode ide_exec_mode(IdeState *s) { return s ? s->exec_mode : EXEC_PAUSED; }
struct GbDebug *ide_debug(IdeState *s) { return s ? s->gb->dbg : NULL; }

void ide_pause(IdeState *s)           { if (s) s->exec_mode = EXEC_PAUSED; }
void ide_resume(IdeState *s)          { if (s) { gb_debug_resume(s->gb); s->exec_mode = EXEC_RUNNING; } }
void ide_step_insn(IdeState *s)       { if (s) { gb_debug_resume(s->gb); s->exec_mode = EXEC_STEP_INSN; } }
void ide_step_line(IdeState *s)       { if (s) { gb_debug_resume(s->gb); s->exec_mode = EXEC_STEP_LINE; } }
void ide_step_frame_once(IdeState *s) { if (s) { gb_debug_resume(s->gb); s->exec_mode = EXEC_STEP_FRAME; } }

static bool dbg_hit(GB *g) { return g->dbg && g->dbg->hit; }

void ide_run_slice(IdeState *s) {
    if (!s) return;
    GB *g = s->gb;
    switch (s->exec_mode) {
    case EXEC_PAUSED:
        return;
    case EXEC_RUNNING:
        g->frame_ready = false;
        while (!g->frame_ready) {
            gb_step(g);
            if (dbg_hit(g)) { s->exec_mode = EXEC_PAUSED; return; }
        }
        s->frame_counter++;
        return;
    case EXEC_STEP_INSN:
        gb_step(g);
        s->exec_mode = EXEC_PAUSED;
        return;
    case EXEC_STEP_LINE: {
        uint8_t ly0 = g->ly;
        do { gb_step(g); } while (g->ly == ly0 && !g->frame_ready && !dbg_hit(g));
        s->exec_mode = EXEC_PAUSED;
        return;
    }
    case EXEC_STEP_FRAME:
        g->frame_ready = false;
        while (!g->frame_ready && !dbg_hit(g)) gb_step(g);
        s->exec_mode = EXEC_PAUSED;
        return;
    }
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

/* Panel rectangles: {x, y, w, h} — single source of truth. */
static const struct { int x, y, w, h; } PANEL_RECTS[PANEL_COUNT] = {
    [PANEL_GAME]        = {   8,   8, 336, 304 },
    [PANEL_REGISTERS]   = { 352,   8, 320, 120 },
    [PANEL_EXEC]        = { 352, 136, 320,  62 },
    [PANEL_DISASM]      = { 352, 206, 320, 392 },
    [PANEL_VRAM_TILES]  = { 680,   8, 336, 148 },
    [PANEL_PALETTE]     = { 680, 164, 336,  36 },
    [PANEL_OAM]         = { 680, 208, 336, 192 },
    [PANEL_TILEMAP]     = { 680, 408, 336, 190 },
    [PANEL_CODE]        = {   8, 320, 336, 150 },
    [PANEL_TILE_EDITOR] = {   8, 478, 336, 120 },
    [PANEL_MEM_HEX]     = {   8, 606,1008,  80 },
    [PANEL_ADDR_INPUT]  = {   8, 690,1008,  12 },
    [PANEL_STATUS]      = {   8, 706,1008,   8 },
};

void ide_panel_rect(IdePanel panel, int *x, int *y, int *w, int *h) {
    int idx = (int)panel;
    if (idx < 0 || idx >= PANEL_COUNT) {
        if (x) *x = 0; if (y) *y = 0; if (w) *w = 0; if (h) *h = 0;
        return;
    }
    if (x) *x = PANEL_RECTS[idx].x;
    if (y) *y = PANEL_RECTS[idx].y;
    if (w) *w = PANEL_RECTS[idx].w;
    if (h) *h = PANEL_RECTS[idx].h;
}

/* Tile editor zoom must match ide_render's Panel[4] zoom value */
#define TILE_EDITOR_ZOOM  5  /* each GB pixel -> 5x5 canvas pixels */
#define VRAM_TILE_COLS   16  /* columns of tiles in VRAM viewer */
#define VRAM_TILE_SCALE   1  /* 1:1 pixels, gap of 1 between tiles */

/* Color palette swatches in the TILE editor panel (one per BGP shade 0..3).
 * swatch_rect is the single source of truth shared by ide_render (draw) and
 * ide_mouse_paint (hit-test) so clicking a swatch matches what's shown. */
#define SWATCH_W   16
#define SWATCH_H   16
#define SWATCH_GAP 4
static void swatch_rect(int i, int *x, int *y, int *w, int *h) {
    int px = 8, py = 478;                   /* PANEL_TILE_EDITOR origin */
    int base_x = px + 4 + 8 * TILE_EDITOR_ZOOM + 14;  /* right of the 40px tile */
    int base_y = py + 16;
    *x = base_x + i * (SWATCH_W + SWATCH_GAP);
    *y = base_y;
    *w = SWATCH_W;
    *h = SWATCH_H;
}

bool ide_mouse_paint(IdeState *s, int mx, int my) {
    if (!s || !s->live) return false;

    /* Clicking a palette swatch selects the paint color instead of painting. */
    for (int i = 0; i < 4; i++) {
        int sx, sy, sw, sh;
        swatch_rect(i, &sx, &sy, &sw, &sh);
        if (mx >= sx && mx < sx + sw && my >= sy && my < sy + sh) {
            ide_set_paint_color(s, i);
            return true;
        }
    }

    return ide_paint_at(s, mx, my);
}

bool ide_paint_at(IdeState *s, int mx, int my) {
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
 * Colors / palettes used by ide_render (game panel + background only)
 * All other panel colors live in panels.c.
 * ------------------------------------------------------------------------- */

#define COL_BG        0x1A1A2EFF  /* dark navy background */
#define COL_BORDER    0x4A4A7AFF  /* panel border */
#define COL_TITLE     0xE0E0FFFF  /* panel title text */
#define COL_PANEL_BG  0x12122AFF  /* slightly different panel bg */

/* DMG green palette: shade 0=lightest, 3=darkest (0xRRGGBBAA) */
static const uint32_t DMG_PAL[4] = {
    0xE0F8D0FF,  /* 0 — lightest green */
    0x88C070FF,  /* 1 */
    0x346856FF,  /* 2 */
    0x081820FF   /* 3 — darkest */
};

/* -------------------------------------------------------------------------
 * Panel helper used by ide_render for the GAME panel
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
        /* blit at (px+8, py+10) so there's an inner margin */
        ui_blit_gb(c, gb_framebuffer(gb), px + 8, py + 10, 2, DMG_PAL);
    }

    /* Delegate remaining panels to panels.c */
    panel_registers(c, gb);
    panel_exec(c, s);
    panel_disasm(c, s);
    panel_vram_tiles(c, gb, s->selected_tile);
    panel_palette(c, gb);
    panel_oam(c, ide_gb(s));
    panel_tilemap(c, ide_gb(s));
    panel_code(c, s->source);
    panel_tile_editor(c, gb, s->selected_tile, s->paint_color);
    panel_mem_hex(c, gb, s->mem_base);
    panel_status(c, s->frame_counter, s->status);
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

    Canvas c = canvas_new(IDE_CANVAS_W, IDE_CANVAS_H);
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
