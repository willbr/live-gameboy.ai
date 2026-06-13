#ifndef IDE_H
#define IDE_H
/*
 * ide.h — IDE state + panel rendering.
 *
 * Canvas dimensions: 1024 x 720 pixels (fixed convention).
 *
 * Layout:
 *   [0]  Game screen    (  8,  8) w=336 h=304
 *   [1]  Registers      (352,  8) w=320 h=120
 *   [2]  VRAM tiles     (680,  8) w=336 h=148
 *   [3]  Code pane      (  8,320) w=336 h=150
 *   [4]  Tile editor    (  8,478) w=336 h=120
 *   [5]  Mem hex        (  8,606) w=1008 h=80
 *   [6]  Status line    (  8,706) w=1008 h=8
 *   [7]  Exec           (352,136) w=320 h=62
 *   [8]  Disasm         (352,206) w=320 h=392
 *   [9]  Palette        (680,164) w=336 h=36
 *   [10] OAM            (680,208) w=336 h=192
 *   [11] Tilemap        (680,408) w=336 h=190
 *   [12] Addr input     (  8,690) w=1008 h=12
 *
 * Note: panels are clipped to canvas; ide_render must not write outside canvas.
 */

#include "ui.h"
#include "../gb/gb.h"
#include <stdbool.h>

typedef struct IdeState IdeState;

struct GbDebug;  /* forward decl — full type in gb/debug.h */

typedef enum {
    EXEC_RUNNING = 0,
    EXEC_PAUSED,
    EXEC_STEP_INSN,
    EXEC_STEP_LINE,
    EXEC_STEP_FRAME
} ExecMode;

/* Advance execution by one host-frame "slice", honoring the current ExecMode.
 * RUNNING: run until frame_ready or a debug hit. STEP_*: do the step then PAUSE.
 * PAUSED: no-op. A breakpoint/watchpoint hit transitions to PAUSED. */
void     ide_run_slice(IdeState *s);

ExecMode ide_exec_mode(IdeState *s);
void     ide_pause(IdeState *s);
void     ide_resume(IdeState *s);              /* clears any hit, runs */
void     ide_step_insn(IdeState *s);
void     ide_step_line(IdeState *s);
void     ide_step_frame_once(IdeState *s);

/* The debugger attached to the session's GB (never NULL after ide_new). */
struct GbDebug *ide_debug(IdeState *s);

/* ide_new — open a .asm or .gb file.
 *   .asm: reads the source, calls live_new(src, path); keeps the LiveSession.
 *   .gb : loads the ROM into a fresh GB (no live session, no source text).
 * Returns NULL on error. */
IdeState *ide_new(const char *path);

/* ide_step_frame — advance the emulator by exactly one full frame. */
void      ide_step_frame(IdeState *s);

/* ide_render — compose all IDE panels into canvas *c.
 * The canvas should be 640x432; smaller canvases are handled safely (clipped). */
void      ide_render(IdeState *s, Canvas *c);

/* ide_gb — return the GB owned by the session (never NULL after ide_new). */
GB       *ide_gb(IdeState *s);

/* ide_free — destroy the session and all owned resources. */
void      ide_free(IdeState *s);

/* ide_shot — headless render: ide_new, step `frames` frames, render to PNG, ide_free.
 * Returns 0 on success, nonzero on error. */
int       ide_shot(const char *in_path, const char *out_png, int frames);

/* ide_select_tile / ide_selected_tile — tile editor selection (0-based index). */
void      ide_select_tile(IdeState *s, int tile_index);
int       ide_selected_tile(IdeState *s);

/* ide_status — null-terminated status string (patch report / info). */
const char *ide_status(IdeState *s);

/* -------------------------------------------------------------------------
 * Extended accessors added for SDL interactive shell (Task 3)
 * ------------------------------------------------------------------------- */

/* Canvas dimensions */
#define IDE_CANVAS_W 1024
#define IDE_CANVAS_H  720

/* Panel identifiers for ide_panel_rect() */
typedef enum {
    PANEL_GAME        = 0,
    PANEL_REGISTERS   = 1,
    PANEL_VRAM_TILES  = 2,
    PANEL_CODE        = 3,
    PANEL_TILE_EDITOR = 4,
    PANEL_MEM_HEX     = 5,
    PANEL_STATUS      = 6,
    PANEL_EXEC        = 7,
    PANEL_DISASM      = 8,
    PANEL_PALETTE     = 9,
    PANEL_OAM         = 10,
    PANEL_TILEMAP     = 11,
    PANEL_ADDR_INPUT  = 12,
    PANEL_COUNT
} IdePanel;

/* ide_panel_rect — return the pixel rectangle of the named panel.
 * All out-pointers are optional (may be NULL). */
void ide_panel_rect(IdePanel panel, int *x, int *y, int *w, int *h);

/* ide_set_paint_color — set the active paint color (0..3) used by ide_mouse_paint. */
void ide_set_paint_color(IdeState *s, int color);

/* ide_paint_color — return the current paint color (0..3). */
int  ide_paint_color(IdeState *s);

/* ide_mouse_paint — given a canvas-space mouse position inside the tile editor
 * panel, compute the tile pixel (tx,ty), then call tile_paint on the selected
 * tile.  Does nothing if (mx,my) is outside the tile editor panel.
 * Returns true on a successful paint, false if out-of-bounds or live==NULL. */
bool ide_mouse_paint(IdeState *s, int mx, int my);

/* ide_paint_at — paint a single pixel at canvas-space (mx,my) with the current
 * color, WITHOUT the colour-swatch check. Use for click-and-drag brush strokes
 * (so dragging over the palette doesn't change colour mid-stroke).
 * Returns true on a successful paint, false if outside the zoomed tile. */
bool ide_paint_at(IdeState *s, int mx, int my);

/* ide_select_tile_at — given a canvas-space mouse position inside the VRAM
 * tiles panel, compute the tile index and call ide_select_tile.
 * Returns the selected tile index, or -1 if (mx,my) is outside the panel. */
int  ide_select_tile_at(IdeState *s, int mx, int my);

/* ide_reload_from_file — re-read the source file on disk and call live_reload.
 * Sets ide_status() to a summary of the PatchReport.
 * No-op (returns false) if the session is not in .asm mode.
 * Returns true on success. */
bool ide_reload_from_file(IdeState *s, const char *path);

/* ide_soft_reset_from_file — re-read the source file and live_soft_reload:
 * clears RAM/VRAM and re-runs from Main (picks up init-time changes that a
 * hot reload won't, since hot reload keeps the running state). Returns true
 * on success. Sets ide_status() to a summary. */
bool ide_soft_reset_from_file(IdeState *s, const char *path);

/* ide_is_asm — return true if the session was opened from a .asm file
 * (i.e. has a live session and source text). */
bool ide_is_asm(IdeState *s);

/* -------------------------------------------------------------------------
 * Task 13: Panel-click and address-field glue
 * ------------------------------------------------------------------------- */

/* Toggle a breakpoint at the disasm line under canvas-space (mx,my). No-op if
   outside the disasm panel. Returns true if a line was hit. */
bool ide_disasm_click(IdeState *s, int mx, int my);

/* Toggle a read+write watchpoint at the mem-hex cell under (mx,my). Returns
   true if a cell was hit. */
bool ide_memhex_click(IdeState *s, int mx, int my);

/* Address-entry field (PANEL_ADDR_INPUT). */
void ide_addr_focus(IdeState *s, bool on);
bool ide_addr_focused(IdeState *s);
void ide_addr_putc(IdeState *s, char ch);
void ide_addr_backspace(IdeState *s);
/* Commit the typed hex address: toggles a PC breakpoint (current bank). */
void ide_addr_commit(IdeState *s);
/* Render the address field into canvas c (called at end of ide_render). */
void ide_addr_render(IdeState *s, Canvas *c);

#endif /* IDE_H */
