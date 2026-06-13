# live-gameboy

A complete Game Boy emulator + live-coding IDE in pure C and SDL3. Edit a function or
paint a tile while the game runs — state intact. PICO-8 spirit, real DMG ROMs.

**Status: complete — emulator + live-coding IDE.**

Design: docs/superpowers/specs/2026-06-12-live-gameboy-design.md

## Status

- [x] Milestone 1: headless libgb core — passes blargg cpu_instrs, instr_timing
- [x] Milestone 2: SDL3 shell, pixel-FIFO PPU, APU
- [x] Milestone 3: built-in SM83 assembler (`gbasm`) — RGBDS-inspired syntax, two-pass, build database
- [x] Milestone 4: live patching — edit a function in a running game, state intact (headless engine; editor UI in M6)
- [x] Milestone 5: live tile editing — paint a pixel, the running screen updates via VRAM provenance (PNG import/export); headless engine, editor UI in M6
- [x] Milestone 6: PICO-8-style IDE — software-rendered debug panels (registers, VRAM tiles, memory hex, code pane, tile editor) in a 640×432 SDL3 window; headless `--ide-shot` gate; `examples/demo.asm` scrolling background demo

## Build

    make test     # unit tests
    make roms     # fetch test ROMs (or clone retrio/gb-test-roms into roms/)
    make blargg   # acceptance tests (cpu_instrs, instr_timing)
    make sound    # blargg dmg_sound 01-registers (APU register masks + power)

## Run

    make live-gameboy            # build the SDL app (needs SDL3 via pkg-config)
    ./live-gameboy game.gb [scale]
    ./live-gameboy --shot game.gb out.png [frames] [scale]   # headless screenshot

Keys: Z=A, X=B, Enter=Start, RShift=Select, Arrows=D-pad, Esc=quit.

## IDE

The live-coding IDE renders a 640×432 canvas with the game screen surrounded by
debug panels: CPU registers/flags, VRAM tile viewer, source code pane, tile
editor, and memory hex view.  The entire canvas is software-rendered (SDL-free
render path), so it can be screenshotted headlessly for CI.

    make live-gameboy-ide        # build the IDE (needs SDL3)
    ./live-gameboy-ide examples/demo.asm   # interactive window
    make ide-shot                # headless screenshot -> build/ide.png

**Controls:**

| Key | Action |
|-----|--------|
| Z | GB A button |
| X | GB B button |
| Enter | GB Start |
| RShift | GB Select |
| Arrow keys | GB D-pad |
| F5 | Hot reload: patch running code from disk, keep RAM/VRAM state (asm mode) |
| F8 | Soft reset: re-read source and re-run from Main, clearing state (asm mode) |
| Space | Pause / resume execution |
| F7 | Step one instruction |
| F6 | Step one scanline |
| F9 | Step one frame |
| ` (backtick) | Focus the address field — type a hex address + Enter to set a breakpoint |
| 0-3 | Set paint colour (0=lightest, 3=darkest); top row or numpad |
| Esc / Q | Quit |

**Mouse:**
- Click a tile in the VRAM viewer to load it into the tile editor.
- Click a swatch in the tile editor's COLOR palette to choose the paint colour
  (the active swatch is highlighted; or use keys 0-3).
- Click a pixel in the zoomed tile to paint it with the current colour, or
  **click and drag to brush** a stroke. Painting a tile that's in use updates
  the running screen live (via VRAM provenance).
- Click a line in the **DISASM** panel to toggle a breakpoint there (a red gutter
  dot appears; execution pauses when PC reaches it).
- Click a byte cell in the **MEMORY** panel to toggle a read/write watchpoint
  (a red underline marks watched bytes; execution pauses on access).

**Debugger panels:** the IDE renders a full debugger alongside the live game —
**DISASM** (bank-aware disassembly around PC, current line highlighted), **EXEC**
(run/pause state, PC, break reason, breakpoint/watchpoint counts), **PALETTE**
(BGP/OBP0/OBP1 swatches), **OAM** (40 sprite entries with previews), and **BG MAP**
(32×32 tilemap thumbnail with the SCX/SCY viewport box). Breakpoints are
bank-aware; watchpoints break on read and/or write to an address. Pause, edit or
paint, then resume — debugging and live-coding compose, with state intact.

The live-edit workflow: edit `game.asm` in your editor, press F5 in the IDE,
watch the running game hot-swap with RAM/VRAM state intact. Use F8 instead when
a change is in init/setup code (e.g. tile or tilemap setup) that only runs once
at startup — F5 keeps running state, so it won't re-run that code.

### Example games

Three playable examples in `examples/`, each built to show off live-coding.
Run any of them with the IDE (`./live-gameboy-ide examples/<game>.asm`) and look
for the `TRY THIS LIVE` recipe at the top of each file.

| Game | File | Signature live-edit hook |
|------|------|--------------------------|
| Pong | `examples/pong.asm` | F5 hot-reload `UpdateBall` physics mid-rally |
| Snake | `examples/snake.asm` | F5 hot-reload `StepSnake` movement/speed rule |
| Breakout | `examples/breakout.asm` | Edit the brick field live in the BG MAP / paint the brick tile |

Build them as standalone ROMs with `make examples`, or take headless
screenshots with `make pong-shot` / `make snake-shot` / `make breakout-shot`.

## Assemble

The built-in `gbasm` assembler compiles RGBDS-inspired SM83 assembly to a valid
Game Boy ROM with a complete cartridge header (logo, checksums):

    make gbasm
    ./gbasm game.asm -o game.gb [--sym game.sym]
    ./live-gameboy game.gb

The assembler writes a `JP Main` at the entry point `$0100` whenever the source
defines a `Main:` global in ROM0, so `./gbasm game.asm -o game.gb` followed by
`./live-gameboy game.gb` boots straight into your code. (The IDE/live path does
the same patch.) A `Main:` global therefore owns the `$0100`-`$0102` entry
vector — don't hand-write your own bytes there if you use a `Main` label.

See `examples/hello.asm` for a working serial-output demo.  The `--sym` flag
writes a `BB:AAAA Name` symbol file for use with debuggers.

## Live patching

The M4 live-patching engine (`src/live/`) lets you reload a function's source
while the emulator keeps running — RAM, VRAM, and CPU registers are never
touched. The engine classifies each change:

- **IN_PLACE** — new bytes fit the function's existing slot; the ROM is
  overwritten at a safe point (PC not inside the patched range).
- **RELOCATED** — the function outgrew its slot; a JP trampoline is written at
  the old entry and the new body is placed at fresh ROM space.
- **REFUSED** — assembly failed or the change is unsafe; ROM and state are
  untouched, so the game continues unaffected.

The full scenario is headless-tested in `tests/test_live_e2e.c`: five
sequential reloads (in-place → reloc → refused → in-place recovery) with the
$C000 counter verified to never decrease across transitions. The public API is
`live_new` / `live_reload` / `live_soft_reload` / `live_gb` / `live_free` /
`patch_report_free` (see `src/live/live.h`).

## Live tiles

The M5 tile editor (`src/live/tile.c`) lets you paint a pixel in a tile asset
and watch it change on the running game's screen immediately — no reload, no
state loss:

1. **Paint a pixel** — `tile_paint(session, asset_path, tile, x, y, color)`
   updates the in-memory asset (2bpp format).
2. **Asset → live ROM** — the build database records which ROM bytes came from
   which asset offset (asset provenance); the new pixel bytes are written
   directly into `gb->rom`.
3. **Live ROM → live VRAM** — a VRAM provenance shadow map (`vram_prov[0x2000]`)
   tracks which VRAM bytes were copied from which ROM offsets (via the standard
   `ld a,(hl+) ; ld (de),a` idiom); matching VRAM bytes are updated in place.
4. **Screen updates** — the PPU renders from the updated VRAM on the very next
   frame; the on-screen tile changes without any game reload.

PNG import/export is also supported (`tile_sheet_to_png` / `tile_sheet_from_png`).
The full flow is headless-tested in `tests/test_live_tile.c`: boot a program
that copies a tile from ROM into VRAM and renders it, paint a pixel, assert the
live framebuffer pixel changes immediately with no reload and RAM untouched.
Procedurally-generated VRAM (no provenance) is correctly left untouched.

## Sound

The APU passes all 12 blargg dmg_sound ROM tests (01-registers through 12-wave write
while on), verifying register read-back masks, power behavior, length counters,
envelopes, sweep, triggers, and wave channel edge cases. Deeper cycle-exact
sound-timing accuracy (sub-sample jitter, etc.) is not targeted.
