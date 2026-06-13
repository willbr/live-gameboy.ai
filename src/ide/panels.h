#ifndef IDE_PANELS_H
#define IDE_PANELS_H
#include "ui.h"
#include "ide.h"
struct GB;

/* Each draws one panel into the canvas at its PANEL_RECTS rectangle. The IDE's
   ide_render() calls these in order. Read-only over GB except where noted. */

/* Existing panels (moved from ide.c): */
void panel_registers(Canvas *c, struct GB *gb);
void panel_vram_tiles(Canvas *c, struct GB *gb, int selected_tile);
void panel_code(Canvas *c, const char *source);
void panel_tile_editor(Canvas *c, struct GB *gb, int selected_tile, int paint_color);
void panel_mem_hex(Canvas *c, struct GB *gb, uint16_t mem_base);
void panel_status(Canvas *c, int frame_counter, const char *status);

/* New panels (implemented in Tasks 8-13): */
void panel_exec(Canvas *c, IdeState *s);
void panel_disasm(Canvas *c, IdeState *s);
void panel_palette(Canvas *c, struct GB *gb);
void panel_oam(Canvas *c, struct GB *gb);
void panel_tilemap(Canvas *c, struct GB *gb);

#endif /* IDE_PANELS_H */
