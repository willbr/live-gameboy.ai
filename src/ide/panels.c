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
#include "../gb/disasm.h"
#include "../gb/debug.h"
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

        /* Watchpoint markers: draw a 1px underline under each watched byte cell.
         * Label "%04X:" = 5 chars = 40px; each byte entry " %02X" = 3 chars = 24px.
         * So byte b's hex starts at tx + (5 + b*3)*8 pixels.  Cell width = 16px
         * (the two hex-digit chars, skipping the leading space). */
        if (gb->dbg) {
            for (int b = 0; b < bytes_per_row; b++) {
                uint16_t addr = (uint16_t)(base_addr + (uint16_t)b);
                for (int wi = 0; wi < gb->dbg->wp_count; wi++) {
                    if (gb->dbg->wp[wi].addr == addr) {
                        int cell_x = tx + (5 + b * 3) * 8 + 8; /* +8: skip the space char */
                        int cell_y = ty;
                        ui_fill_rect(c, cell_x, cell_y + 8, 16, 1, 0xFF8080FF);
                        break;
                    }
                }
            }
        }

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
    int x, y, w, h;
    ide_panel_rect(PANEL_EXEC, &x, &y, &w, &h);
    GB *gb = ide_gb(s);
    ui_rect(c, x, y, w, h, 0x60A060FF);

    const char *mode = ide_exec_mode(s) == EXEC_PAUSED ? "PAUSED" : "RUNNING";
    char line[64];
    snprintf(line, sizeof line, "EXEC: %s  PC=%04X", mode, gb->cpu.pc);
    ui_text(c, x + 4, y + 2, line, 0xFFD700FF);

    GbDebug *d = gb->dbg;
    if (d && d->hit) {
        const char *k = d->hit_kind == DBG_BREAKPOINT  ? "BP"
                      : d->hit_kind == DBG_WATCH_READ  ? "WR"
                      : d->hit_kind == DBG_WATCH_WRITE ? "WW" : "?";
        snprintf(line, sizeof line, "BREAK %s @%04X (pc %04X)", k, d->hit_addr, d->hit_pc);
        ui_text(c, x + 4, y + 12, line, 0xFF8080FF);
    }
    snprintf(line, sizeof line, "BP:%d WP:%d", d ? d->bp_count : 0, d ? d->wp_count : 0);
    ui_text(c, x + 4, y + 22, line, 0xC0E0C0FF);
    ui_text(c, x + 4, y + 40, "SPC run/pause F7 ins F6 line F9 frm", 0x80A080FF);
}

void panel_disasm(Canvas *c, IdeState *s) {
    int px, py, pw, ph;
    ide_panel_rect(PANEL_DISASM, &px, &py, &pw, &ph);
    draw_panel(c, px, py, pw, ph, "DISASM");

    GB *gb = ide_gb(s);
    uint8_t bank = gb->rom_bank;
    uint16_t pc  = gb->cpu.pc;
    /* Forward-only listing starting at PC (v1 simplification; see plan Task 8). */
    uint16_t addr = pc;

    int line_h = 8, top = py + 14, rows = (ph - 16) / line_h;
    for (int r = 0; r < rows; r++) {
        char text[40];
        int len = gb_disasm(gb, bank, addr, text, sizeof text);
        bool is_pc  = (addr == pc);
        bool has_bp = (gb_debug_find_bp(gb, bank, addr) >= 0);
        int  ly     = top + r * line_h;

        if (is_pc)
            ui_fill_rect(c, px + 1, ly, pw - 2, line_h, 0x303820FF);
        if (has_bp)
            ui_fill_rect(c, px + 2, ly + 1, 6, 6, 0xFF4040FF);  /* gutter dot */

        char row[64];
        snprintf(row, sizeof row, "%c%04X %s", is_pc ? '>' : ' ', addr, text);
        ui_text(c, px + 10, ly, row, is_pc ? 0xFFD700FF : 0xC0E0C0FF);

        addr = (uint16_t)(addr + (len > 0 ? len : 1));
    }
}

void panel_palette(Canvas *c, struct GB *gb) {
    int x, y, w, h;
    ide_panel_rect(PANEL_PALETTE, &x, &y, &w, &h);
    ui_rect(c, x, y, w, h, 0x60A060FF);
    static const uint32_t DMG[4] = {0xE0F8D0FF, 0x88C070FF, 0x346856FF, 0x081820FF};
    const char *names[3] = {"BGP", "OB0", "OB1"};
    uint8_t regs[3] = {gb->bgp, gb->obp0, gb->obp1};
    int sw = 14;
    for (int p = 0; p < 3; p++) {
        int py = y + 2 + p * 11;
        ui_text(c, x + 4, py, names[p], 0xA0FFA0FF);
        for (int i = 0; i < 4; i++) {
            int shade = (regs[p] >> (i * 2)) & 3;
            ui_fill_rect(c, x + 36 + i * (sw + 2), py, sw, 9, DMG[shade]);
        }
    }
}

/* decode one shade (0..3) of a tile pixel from VRAM 2bpp at tile index t. */
static uint8_t tile_pixel(GB *gb, int t, int px, int py) {
    int base = t * 16 + py * 2;
    uint8_t lo = gb->vram[base], hi = gb->vram[base + 1];
    int bit = 7 - px;
    return (uint8_t)(((hi >> bit) & 1) << 1 | ((lo >> bit) & 1));
}

void panel_oam(Canvas *c, struct GB *gb) {
    int x, y, w, h;
    ide_panel_rect(PANEL_OAM, &x, &y, &w, &h);
    ui_rect(c, x, y, w, h, 0x60A060FF);
    ui_text(c, x + 4, y + 2, "OAM", 0xA0FFA0FF);

    static const uint32_t GRAY[4] = {0xFFFFFFFF, 0xAAAAAAFF, 0x555555FF, 0x000000FF};
    int top = y + 14, row_h = 9, rows = (h - 16) / row_h;
    for (int i = 0; i < 40 && i < rows; i++) {
        uint8_t oy = gb->oam[i * 4 + 0], ox = gb->oam[i * 4 + 1];
        uint8_t tile = gb->oam[i * 4 + 2], attr = gb->oam[i * 4 + 3];
        int ly = top + i * row_h;
        /* 8x8 sprite preview at 1x */
        for (int spy = 0; spy < 8; spy++)
            for (int spx = 0; spx < 8; spx++)
                ui_fill_rect(c, x + 4 + spx, ly + spy, 1, 1,
                             GRAY[tile_pixel(gb, tile, spx, spy)]);
        char row[40];
        snprintf(row, sizeof row, "%02d Y%3d X%3d T%02X %02X", i, oy, ox, tile, attr);
        ui_text(c, x + 16, ly, row, 0xC0E0C0FF);
    }
}

void panel_tilemap(Canvas *c, struct GB *gb) {
    int x, y, w, h;
    ide_panel_rect(PANEL_TILEMAP, &x, &y, &w, &h);
    ui_rect(c, x, y, w, h, 0x60A060FF);
    ui_text(c, x + 4, y + 2, "BG MAP", 0xA0FFA0FF);

    static const uint32_t GRAY[4] = {0xFFFFFFFF, 0xAAAAAAFF, 0x555555FF, 0x000000FF};
    /* LCDC bit3 selects 0x9800 vs 0x9C00; bit4 selects tile data base. */
    int map = (gb->lcdc & 0x08) ? 0x1C00 : 0x1800;  /* VRAM offset */
    bool signed_idx = !(gb->lcdc & 0x10);
    int ox = x + 4, oy = y + 12;
    /* 32x32 tiles, draw each as a 4x4 block (128x128 px), then SCX/SCY box. */
    int cell = 4;
    for (int ty = 0; ty < 32; ty++) {
        for (int tx = 0; tx < 32; tx++) {
            uint8_t idx = gb->vram[map + ty * 32 + tx];
            /* tile-data base: 0x8000 (unsigned) or 0x9000 (signed) -> VRAM tile index */
            int t = signed_idx ? 256 + (int8_t)idx : idx;
            /* cheap thumbnail: shade of the tile's top-left pixel */
            uint8_t shade = tile_pixel(gb, t, 0, 0);
            ui_fill_rect(c, ox + tx * cell, oy + ty * cell, cell, cell, GRAY[shade]);
        }
    }
    /* Viewport rectangle (SCX/SCY), wrapping around the 256x256 BG map like real
     * hardware: when the 160x144 window scrolls off an edge it re-enters from the
     * opposite side. Drawn per-pixel in thumbnail space modulo the map span, so
     * the outline always stays clipped to the 32x32 grid. */
    int map_px = 32 * cell;                          /* 128 px thumbnail = 256 BG px */
    int vL = (gb->scx * cell) / 8;                   /* left/top edges in thumbnail px */
    int vT = (gb->scy * cell) / 8;
    int vW = 160 * cell / 8, vH = 144 * cell / 8;    /* 80 x 72 px window */
    uint32_t vcol = 0xFFD700FF;
    for (int i = 0; i < vW; i++) {                   /* top + bottom edges */
        int px = ox + (vL + i) % map_px;
        ui_fill_rect(c, px, oy + vT % map_px, 1, 1, vcol);
        ui_fill_rect(c, px, oy + (vT + vH - 1) % map_px, 1, 1, vcol);
    }
    for (int j = 0; j < vH; j++) {                   /* left + right edges */
        int py = oy + (vT + j) % map_px;
        ui_fill_rect(c, ox + vL % map_px, py, 1, 1, vcol);
        ui_fill_rect(c, ox + (vL + vW - 1) % map_px, py, 1, 1, vcol);
    }
}
