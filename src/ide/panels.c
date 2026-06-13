/*
 * panels.c — Per-panel rendering functions for the IDE.
 *
 * Each panel_*() function draws one panel into the canvas at the rectangle
 * given by ide_panel_rect(). Coordinates are looked up at render time so this
 * file has no hard-coded layout constants — all geometry lives in ide.c's
 * PANEL_RECTS table.
 */

#include "panels.h"
#include "../gb/gb.h"
#include "../live/tile.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Shared color / palette constants (private to this TU)
 * ------------------------------------------------------------------------- */

#define COL_BG        0x1A1A2EFF  /* dark navy background */
#define COL_BORDER    0x4A4A7AFF  /* panel border */
#define COL_TITLE     0xE0E0FFFF  /* panel title text */
#define COL_TEXT      0xC0C0D0FF  /* normal text */
#define COL_DIM       0x606070FF  /* dim text */
#define COL_HIGHLIGHT 0xFFD700FF  /* gold highlight */
#define COL_PANEL_BG  0x12122AFF  /* slightly different panel bg */

/* Gray palette for VRAM tile viewer (maps shade -> gray RGBA) */
static const uint32_t VRAM_PAL[4] = {
    0xFFFFFFFF,
    0xAAAAAAFF,
    0x555555FF,
    0x000000FF
};

/* -------------------------------------------------------------------------
 * Shared draw helper
 * ------------------------------------------------------------------------- */

static void draw_panel(Canvas *c, int x, int y, int w, int h, const char *title) {
    ui_fill_rect(c, x, y, w, h, COL_PANEL_BG);
    ui_rect(c, x, y, w, h, COL_BORDER);
    if (title)
        ui_text(c, x + 2, y + 1, title, COL_TITLE);
}

/* -------------------------------------------------------------------------
 * panel_registers
 * --------------------------------------------------------------------- */

void panel_registers(Canvas *c, struct GB *gb) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_REGISTERS, &px, &py, &pw, &ph);
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

/* -------------------------------------------------------------------------
 * panel_vram_tiles
 * --------------------------------------------------------------------- */

void panel_vram_tiles(Canvas *c, struct GB *gb, int selected_tile) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_VRAM_TILES, &px, &py, &pw, &ph);
    draw_panel(c, px, py, pw, ph, "VRAM TILES");

    /* Draw 64 tiles in 16 columns x 4 rows. Each tile cell = 9x9 (8px + 1px gap). */
    int cols = 16;
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
        if (ti == selected_tile)
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

/* -------------------------------------------------------------------------
 * panel_code
 * --------------------------------------------------------------------- */

void panel_code(Canvas *c, const char *source) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_CODE, &px, &py, &pw, &ph);
    draw_panel(c, px, py, pw, ph, "CODE");

    int tx = px + 4, ty = py + 12;
    int line_h = 9;
    int max_lines = (ph - 14) / line_h;
    int max_cols  = (pw - 6) / 8;

    if (source) {
        const char *p = source;
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

/* -------------------------------------------------------------------------
 * Tile editor swatch geometry (must match ide.c's swatch_rect for hit-test)
 * --------------------------------------------------------------------- */

#define TILE_EDITOR_ZOOM  5
#define SWATCH_W   16
#define SWATCH_H   16
#define SWATCH_GAP 4

static void swatch_rect(int i, int *x, int *y, int *w, int *h) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_TILE_EDITOR, &px, &py, &pw, &ph);
    (void)pw; (void)ph;
    int base_x = px + 4 + 8 * TILE_EDITOR_ZOOM + 14;  /* right of the 40px tile */
    int base_y = py + 16;
    *x = base_x + i * (SWATCH_W + SWATCH_GAP);
    *y = base_y;
    *w = SWATCH_W;
    *h = SWATCH_H;
}

/* -------------------------------------------------------------------------
 * panel_tile_editor
 * --------------------------------------------------------------------- */

void panel_tile_editor(Canvas *c, struct GB *gb, int selected_tile, int paint_color) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_TILE_EDITOR, &px, &py, &pw, &ph);

    char title[32];
    snprintf(title, sizeof(title), "TILE %d", selected_tile);
    draw_panel(c, px, py, pw, ph, title);

    int zoom = TILE_EDITOR_ZOOM;
    int ox = px + 4;
    int oy = py + 12;

    int ti = selected_tile;
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

    /* Color palette: 4 swatches (BGP shades). Active one is highlighted.
     * Click a swatch (or press 0-3) to choose the paint color. */
    for (int i = 0; i < 4; i++) {
        int sx, sy, sw, sh;
        swatch_rect(i, &sx, &sy, &sw, &sh);
        ui_fill_rect(c, sx, sy, sw, sh, VRAM_PAL[i]);
        if (i == paint_color)
            ui_rect(c, sx - 2, sy - 2, sw + 3, sh + 3, COL_HIGHLIGHT);
        else
            ui_rect(c, sx - 1, sy - 1, sw + 1, sh + 1, COL_BORDER);
    }
    {
        int lx, ly, lw, lh;
        swatch_rect(0, &lx, &ly, &lw, &lh);
        ui_text(c, lx, ly + SWATCH_H + 3, "COLOR", COL_DIM);
    }
}

/* -------------------------------------------------------------------------
 * panel_mem_hex
 * --------------------------------------------------------------------- */

void panel_mem_hex(Canvas *c, struct GB *gb, uint16_t mem_base) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_MEM_HEX, &px, &py, &pw, &ph);
    draw_panel(c, px, py, pw, ph, "MEMORY");

    int tx = px + 4, ty = py + 12;
    int line_h = 9;
    int rows = (ph - 14) / line_h;
    if (rows > 6) rows = 6;  /* cap at 6 rows */
    int bytes_per_row = 16;

    char buf[128];
    for (int r = 0; r < rows; r++) {
        uint16_t base_addr = (uint16_t)(mem_base + (uint16_t)(r * bytes_per_row));
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

/* -------------------------------------------------------------------------
 * panel_status
 * --------------------------------------------------------------------- */

void panel_status(Canvas *c, int frame_counter, const char *status) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_STATUS, &px, &py, &pw, &ph);
    (void)pw; (void)ph;
    char status_buf[256];
    snprintf(status_buf, sizeof(status_buf), "[F%d] %s", frame_counter, status);
    ui_text(c, px, py, status_buf, COL_DIM);
}

/* -------------------------------------------------------------------------
 * New panels (stubs — implemented in Tasks 8-13)
 * --------------------------------------------------------------------- */

void panel_exec(Canvas *c, IdeState *s) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_EXEC, &px, &py, &pw, &ph);
    draw_panel(c, px, py, pw, ph, "EXEC");
    (void)s;
}

void panel_disasm(Canvas *c, IdeState *s) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_DISASM, &px, &py, &pw, &ph);
    draw_panel(c, px, py, pw, ph, "DISASM");
    (void)s;
}

void panel_palette(Canvas *c, struct GB *gb) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_PALETTE, &px, &py, &pw, &ph);
    draw_panel(c, px, py, pw, ph, "PALETTE");
    (void)gb;
}

void panel_oam(Canvas *c, struct GB *gb) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_OAM, &px, &py, &pw, &ph);
    draw_panel(c, px, py, pw, ph, "OAM");
    (void)gb;
}

void panel_tilemap(Canvas *c, struct GB *gb) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_TILEMAP, &px, &py, &pw, &ph);
    draw_panel(c, px, py, pw, ph, "TILEMAP");
    (void)gb;
}
