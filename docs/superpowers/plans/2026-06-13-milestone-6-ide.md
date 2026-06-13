# Milestone 6: IDE — Debug Panels + Code/Tile Editing UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`). Each subagent: do one task, commit, REPORT BACK — do NOT run any branch-finishing/merge/PR flow or switch branches.

**Goal:** A PICO-8-style desktop IDE that runs a Game Boy game and, around it, shows live debug panels (CPU registers/flags, memory hex, VRAM tile viewer) plus a code pane and a tile editor — all wired to the live session (Milestone 4) and tile editor (Milestone 5). Reload code with a hotkey and the running game hot-swaps, state intact; paint a tile with the mouse and the running screen updates. Acceptance: a headless `--ide-shot` renders the full IDE canvas to a PNG (verified visually) with the game screen + populated panels; the SDL window runs without crashing; unit tests cover the software UI primitives and panel data.

**Architecture:** The entire IDE is **software-rendered into one RGBA canvas** by a pure, SDL-free `ui`/`ide` layer — so the whole interface is headless-testable (render → PNG). SDL only (1) presents the canvas as a single streaming texture and (2) feeds keyboard/mouse events into the IDE state. This keeps rendering deterministic and verifiable and isolates SDL to a thin `main.c`.

```
src/ide/ui.c     — software canvas: fill_rect, 8x8 bitmap font text, blit GB framebuffer
src/ide/ide.c    — IDE state + ide_render(state, canvas): compose panels; input handlers (pure)
src/ide/main.c   — SDL3 window: present canvas, feed input, drive live session per frame
```

The IDE state owns a `LiveSession` (from M4), the current source text, the selected tile/asset, and panel layout. `ide_render` is pure (state → canvas) so a headless `--ide-shot` path renders it without a window. Input handlers are pure functions on state (`ide_key`, `ide_mouse`) called by main.c from SDL events, also unit-testable.

**Tech Stack:** C11, SDL3 (main.c only), zlib (PNG). Builds on libgb + libasm + liblive + tile editor.

**Spec:** design §7 (IDE shell: code editor, game screen, tile editor, debug panels). Milestone 6 — the final milestone.

**Depends on:** Milestones 1–5 (merged). Uses live_new/live_reload/live_gb/live_result + tile_paint + the 2bpp codec + gb_framebuffer.

---

## Layout (1 canvas, e.g. 640×432 = 2× of a 320×216 logical area; pick sizes that fit)
```
+-------------------------+--------------------------+
|  GAME SCREEN (160x144   |  REGISTERS / FLAGS       |
|  integer-scaled)        |  AF BC DE HL SP PC, Z N H C, mode/LY |
|                         +--------------------------+
|                         |  VRAM TILE VIEWER        |
|                         |  (first N tiles as a grid)|
+-------------------------+--------------------------+
|  CODE PANE (source text, current line highlight)  |
|  ... + status line: patch report / FPS            |
+---------------------------------------------------+
|  TILE EDITOR (8x8 zoomed grid of selected tile) + MEMORY HEX (a page) |
+---------------------------------------------------+
```
Exact pixel rects are the implementer's choice; keep panels readable with the 8×8 font.

---

### Task 1: Software UI canvas + bitmap font

**Files:** create `src/ide/ui.h`, `src/ide/ui.c`; create `tests/test_ui.c`. Makefile: an IDE_OBJ set (exclude main.c), link into tests with -lz.

- [ ] ui.h API:
```c
typedef struct { uint8_t *px; int w, h; } Canvas;   /* RGBA8888, px = w*h*4 */
Canvas canvas_new(int w, int h);
void   canvas_free(Canvas *c);
void   ui_clear(Canvas *c, uint32_t rgba);
void   ui_fill_rect(Canvas *c, int x, int y, int w, int h, uint32_t rgba);
void   ui_rect(Canvas *c, int x, int y, int w, int h, uint32_t rgba);   /* outline */
void   ui_text(Canvas *c, int x, int y, const char *s, uint32_t fg);    /* 8x8 font */
void   ui_text_bg(Canvas *c, int x, int y, const char *s, uint32_t fg, uint32_t bg);
/* blit the 160x144 GB shade buffer (0..3) into the canvas at (x,y), integer scale, using a palette */
void   ui_blit_gb(Canvas *c, const uint8_t *fb160x144, int x, int y, int scale, const uint32_t pal[4]);
int    ui_save_png(const Canvas *c, const char *path);   /* zlib PNG (reuse tile.c's writer or inline) */
```
- [ ] An embedded 8×8 bitmap font covering ASCII 0x20–0x7E (a compact `static const uint8_t font8x8[95][8]`). You can use a well-known public-domain 8x8 font table (e.g. the "font8x8_basic" set) — include enough glyphs for letters, digits, punctuation used by the panels. Document the source.
- [ ] tests/test_ui.c: create a canvas, clear, fill a rect, draw text "AB 12", assert specific pixels (a known glyph pixel is fg, background is bg); blit a tiny GB framebuffer (set a couple shades) and assert the scaled pixels map through the palette; ui_save_png writes a file (assert it exists + nonzero). Keep assertions concrete (check a few pixel RGBA values).
- [ ] `make test` green; gates (blargg/acid2/sound) green; build clean -Werror. Commit `feat: software UI canvas + 8x8 bitmap font`.

---

### Task 2: IDE state + panel rendering (pure) + headless --ide-shot

**Files:** create `src/ide/ide.h`, `src/ide/ide.c`; create `tests/test_ide_render.c`.

- [ ] ide.h:
```c
typedef struct IdeState IdeState;
IdeState *ide_new(const char *rom_or_asm_path);  /* if .asm: live_new from source; if .gb: load rom into a GB for debug-only */
void      ide_step_frame(IdeState *s);            /* run the emulator one frame */
void      ide_render(IdeState *s, Canvas *c);     /* compose all panels into the canvas (pure draw) */
GB       *ide_gb(IdeState *s);
void      ide_free(IdeState *s);
/* selection / panel state setters used by input (Task 3) */
```
- [ ] ide.c: ide_render draws the layout: ui_blit_gb for the game screen; a registers panel (format AF/BC/DE/HL/SP/PC hex + Z N H C flags + PPU mode/LY from the GB struct); a VRAM tile viewer (decode the first e.g. 8×8=64 tiles from gb->vram via the 2bpp codec into small swatches using BGP); a code pane (render the source text lines, with the line for the current PC highlighted IF a build-db linemap maps PC→line — optional, else just show text); a memory hex panel (one 256-byte page from a base address, hex + ascii); a tile-editor panel (the selected tile zoomed as an 8×8 grid of color cells). A status line (patch report text + simple frame counter).
- [ ] ide_new: if the path ends in .asm, read it, live_new(src), keep the LiveSession; if .gb, gb_new+load+reset (debug-only, no live session). Store source text for the code pane.
- [ ] Add an `--ide-shot <path.asm|.gb> <out.png> [frames]` mode (in main.c or a small headless entry) that ide_new, ide_step_frame×frames, ide_render to a canvas, ui_save_png. (Put the arg parse in main.c but keep the render path SDL-free so it works headless.)
- [ ] tests/test_ide_render.c: ide_new on a small assembled program (write a temp .asm that draws a tile, like M5), step a few frames, ide_render into a canvas, and assert: the canvas is non-blank in the game-screen region (some pixel != background), the registers panel region contains text (spot-check a glyph pixel), and ui_save_png works. (Assertions are structural — exact text is hard; assert regions are populated.)
- [ ] `make test` green; gates green; build clean. Commit `feat: IDE state + panel rendering + headless --ide-shot`.

---

### Task 3: SDL window + input (game keys, code reload hotkey, mouse tile paint)

**Files:** create `src/ide/main.c`; modify Makefile (a `live-gameboy-ide` target linking SDL + IDE_OBJ + everything).

- [ ] main.c: parse args — `--ide-shot ...` (headless, calls into ide.c render path) OR `<file>` (interactive). Interactive: SDL window sized to the canvas; create a streaming RGBA texture; each frame: feed input → ide_step_frame → ide_render(canvas) → upload texture → present at ~60fps.
- [ ] Input wiring (pure handlers in ide.c, called from main.c):
  - Keyboard → GB joypad (Z/X/Enter/RShift/arrows) via gb_set_buttons when the game pane is focused (default).
  - A hotkey (e.g. F5) → reload: re-read the source file (or use the in-memory buffer) and call live_reload; stash the PatchReport for the status line.
  - Mouse click in the tile-editor panel → compute (tile,x,y) from the click and call tile_paint with the current color; mouse in the VRAM viewer → select a tile into the editor; number keys 0–3 → current paint color.
  - Esc → quit.
  (Full in-pane text editing of code is OUT OF SCOPE for this milestone — the code pane is read-only display + file-reload-on-F5; note this. The live-edit loop is: edit the .asm in your editor, press F5 in the IDE, watch it hot-swap. This still demonstrates the whole thesis through the UI.)
- [ ] Build `make live-gameboy-ide` clean. Smoke: `timeout 3 ./live-gameboy-ide <a .asm or .gb>` exits 124 (ran without crashing) — controller verifies. `make test` still green.
- [ ] Commit `feat: SDL IDE window + input (joypad, F5 reload, mouse tile paint)`.

---

### Task 4: Gate (full IDE screenshot) + CI/README + final verification

**Files:** create `examples/demo.asm` (a small game: moves a tile or animates, maintains WRAM state); modify README, CI.

- [ ] `examples/demo.asm`: a small but visually meaningful program — e.g. sets up a tile + tilemap, increments a WRAM counter each frame, and uses it to move the BG (SCX = counter) or toggle a tile, so the screen visibly changes over frames and there's editable state. Entry at Main (live_new patches 0x100).
- [ ] Gate: `make ide-shot` runs `./live-gameboy-ide --ide-shot examples/demo.asm build/ide.png 60`. The controller reads build/ide.png and verifies the full IDE is visible: game screen showing the demo, registers panel with hex values, VRAM tile viewer with tiles, code pane with the source, tile-editor + memory panels. Iterate on layout until it reads well.
- [ ] README: mark Milestone 6 done; ALL milestones now complete. Add an "IDE" section: `make live-gameboy-ide && ./live-gameboy-ide game.asm` ; controls (game keys, F5 reload, mouse paint, 0–3 color, Esc). Update the project status to "complete: emulator + live-coding IDE".
- [ ] CI: add a guarded `ide-shot` step (like the shell step — only if SDL3 present; but --ide-shot is headless and SDL-free for the render path, so it can run if it links; if linking needs SDL, guard it). Keep CI green.
- [ ] Final full verification: `make clean && make test && make blargg && make acid2 && make sound && make gbasm && make live-gameboy && make live-gameboy-ide && make ide-shot`. All green.
- [ ] Commit `feat: IDE demo + screenshot gate; milestone 6 complete`.

---

## Self-review notes
- **Verifiable despite being UI:** rendering the whole IDE into a software canvas makes it headless-testable and screenshot-verifiable (the controller reads the PNG), sidestepping the usual "GUI can't be tested" problem. SDL is confined to main.c (window + event plumbing), smoke-tested.
- **Reuses everything:** the IDE is a thin presentation/input layer over the live session (M4) and tile editor (M5); debug panels read straight from the GB struct + build database. No emulator/assembler logic is duplicated.
- **Scoped honestly:** in-pane code text-editing is deferred — the live workflow is "edit the .asm, press F5, watch it hot-swap," which fully exercises the live-patching thesis through the UI; mouse tile painting is in-IDE. Full structured code editing is a natural follow-up beyond this milestone.
- **Final gate is the whole vision:** one screenshot shows a running game surrounded by live debug panels + editors — the PICO-8-style fantasy-workstation the project set out to build — with all prior acceptance gates (blargg/acid2/sound/assembler/live-patch/live-tile) still green.
```
