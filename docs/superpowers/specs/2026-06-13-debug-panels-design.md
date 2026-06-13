# live-gameboy — debug panels (full debugger for the IDE)

**Date:** 2026-06-13
**Status:** Approved design

## 1. Goal

Turn the IDE from a live *viewer* into a real *debugger* while preserving the
live-coding thesis (edit/paint with state intact). Adds, in one spec:

- **Execution control:** pause/resume + step by instruction, scanline, frame.
- **Breakpoints:** bank-aware PC breakpoints.
- **Watchpoints:** break on read and/or write to a memory address.
- **Disassembly panel:** bank-aware disassembly around PC.
- **Viewer panels:** OAM/sprite viewer, BG tilemap viewer, palette view.

Pause/step does not contradict the "the game never stops" thesis — it
*strengthens* it: you can pause, live-patch a function or paint a tile, and
resume with RAM/VRAM/CPU state intact.

This is the milestone-6 follow-on the original design (§7) called for
(disassembly, OAM/tilemap/palette viewers, pause/resume + stepping) plus
breakpoints/watchpoints, which the bus hook-points were always meant to enable
(see `2026-06-12-live-gameboy-design.md` §3, §7).

## 2. Architecture (Approach A: hooks in the core, control in the IDE)

The headless-testable-core philosophy is preserved. Breakpoint/watchpoint
**state and the inline checks** live in `libgb` (pure C, no SDL), because the
bus is the only choke point through which watchpoints are observable. The
**execution-control state machine** lives in the IDE, because it is about the
UI frame loop. Panel **rendering** moves into a new `src/ide/panels.c`.

```
┌─────────────────────────────────────────────────────────┐
│ IDE (src/ide/)                                          │
│  ide.c        execution state machine, input, glue      │
│  panels.c     all panel rendering (NEW)                 │
│  ui.c         + single-line text-input widget (NEW)     │
├─────────────────────────────────────────────────────────┤
│ libgb (src/gb/)  — pure C, SDL-free, headless-tested    │
│  GbDebug      breakpoints + watchpoints + hit state (NEW)│
│  gb_step()    + PC breakpoint check (fast path)         │
│  gb_read8/    + watchpoint check (fast path)            │
│   gb_write8()                                           │
│  disasm.c     one-instruction SM83 decoder (NEW)        │
└─────────────────────────────────────────────────────────┘
```

Rejected alternatives:
- **Everything in the IDE:** the IDE never sees individual bus accesses, so
  watchpoints are impossible without reaching into the core anyway.
- **Separate `libdbg` layer:** still needs the same core hook points; adds a
  layer without removing the coupling. Over-engineered for v1.

## 3. Core: `GbDebug`

A new struct, pointed to from `GB` (NULL means "no debugging" — every check is
behind a `count == 0` fast path, so a normally-running ROM pays nothing).

```c
typedef struct { uint8_t bank; uint16_t addr; bool enabled; } Breakpoint;
typedef struct { uint16_t addr; bool on_read, on_write, enabled; } Watchpoint;

typedef enum { DBG_NONE, DBG_BREAKPOINT, DBG_WATCH_READ, DBG_WATCH_WRITE } DbgHitKind;

typedef struct {
    Breakpoint  bp[DBG_MAX_BP];   int bp_count;
    Watchpoint  wp[DBG_MAX_WP];   int wp_count;
    bool        hit;              /* set by a check; cleared by the IDE on resume */
    DbgHitKind  hit_kind;
    uint16_t    hit_addr;         /* watch addr or breakpoint addr */
    uint16_t    hit_pc;           /* PC at the moment of the hit */
} GbDebug;
```

`DBG_MAX_BP` / `DBG_MAX_WP` are fixed small caps (e.g. 32 each) — no dynamic
allocation in the core. The struct is owned by the IDE and attached via
`gb->dbg`.

### Breakpoint check (in `gb_step`)

Checked once per instruction, *after* PC points at the next opcode and before it
is executed. Fast path: `if (!dbg || dbg->bp_count == 0) proceed normally`.

Bank-awareness:
- A breakpoint with `addr` in `0000–3FFF` (ROM0) or `>= 0x8000` (RAM/HRAM/etc.)
  is bank-agnostic; `bank` is ignored.
- A breakpoint with `addr` in `4000–7FFF` (switchable ROM) fires only when
  `gb->rom_bank == bp.bank`.

On a match, set `dbg->hit = true`, `hit_kind = DBG_BREAKPOINT`,
`hit_addr = bp.addr`, `hit_pc = pc`, and return without executing further. The
IDE observes `hit` and pauses; the offending instruction has **not** yet run, so
resume executes it normally.

### Watchpoint check (in `gb_read8` / `gb_write8`)

Fast path: `if (!dbg || dbg->wp_count == 0) proceed`. Otherwise, on an address
match with the matching direction, set `dbg->hit`, `hit_kind` =
`DBG_WATCH_READ`/`DBG_WATCH_WRITE`, `hit_addr = a`, `hit_pc = cpu.pc`. The
current memory operation still completes (read returns its value, write lands);
the IDE pauses at the next instruction boundary so state is consistent.

### Core API

```c
GbDebug *gb_debug_attach(GB *gb);          /* allocate + wire gb->dbg */
void     gb_debug_detach(GB *gb);
int      gb_debug_add_bp(GB*, uint8_t bank, uint16_t addr);   /* -1 if full */
void     gb_debug_clear_bp(GB*, int index);
int      gb_debug_toggle_bp(GB*, uint8_t bank, uint16_t addr);/* add or remove */
int      gb_debug_add_wp(GB*, uint16_t addr, bool rd, bool wr);
void     gb_debug_clear_wp(GB*, int index);
void     gb_debug_resume(GB*);             /* clear hit before continuing */
```

## 4. Execution control (IDE state machine)

```c
typedef enum { EXEC_RUNNING, EXEC_PAUSED,
               EXEC_STEP_INSN, EXEC_STEP_LINE, EXEC_STEP_FRAME } ExecMode;
```

`ide_step_frame` is replaced by `ide_run_slice(IdeState*)`, called once per host
frame:

- **EXEC_RUNNING** — loop `gb_step` until `gb->frame_ready` **or** `gb->dbg->hit`.
  A breakpoint/watchpoint hit transitions to `EXEC_PAUSED` and records the reason
  in the status line.
- **EXEC_PAUSED** — do nothing (the game is frozen; the canvas still renders, and
  live-patch / tile-paint remain available).
- **EXEC_STEP_INSN** — one `gb_step`, then `EXEC_PAUSED`.
- **EXEC_STEP_LINE** — step until `ly` changes (or `frame_ready`), then `EXEC_PAUSED`.
- **EXEC_STEP_FRAME** — step until `frame_ready`, then `EXEC_PAUSED`.

On any transition out of PAUSED that resumes, call `gb_debug_resume` first so a
stale `hit` does not immediately re-pause.

### Keys (additions; existing keys unchanged)

| Key | Action |
|-----|--------|
| Space | Pause / resume |
| F7 | Step one instruction |
| F6 | Step one scanline |
| F9 | Step one frame |
| ` (backtick) | Focus the address-entry field |

(Final key choices confirmed in the plan; must not collide with existing F5/F8
or the 0–3 paint-colour keys.)

## 5. Disassembler (`src/gb/disasm.c`)

```c
/* Decode one SM83 instruction at (bank, addr); write text to out, return its
   byte length so callers can walk forward. Pure function over gb memory. */
int gb_disasm(GB *gb, uint8_t bank, uint16_t addr, char *out, int out_sz);
```

Standalone decoder (the assembler *encodes*; this *decodes*). Handles the full
opcode set including the `CB` prefix, signed displacements (`JR`, `LD HL,SP+e`),
and immediate operands. The panel walks forward from a start address a few
instructions before PC to fill ~24 lines.

## 6. Panels (`src/ide/panels.c`)

Rendering for **all** panels (existing ones moved here from `ide.c`, plus new):

- **Disassembly:** ~24 instructions from `start..`, PC line highlighted, a
  breakpoint dot in the left gutter. A cursor (arrows / PgUp-PgDn) selects a
  line; click or a hotkey toggles a breakpoint at the cursor's address (bank =
  current `rom_bank` for switchable space).
- **OAM viewer:** 40 entries from `oam[]` — index, Y, X, tile #, attribute byte
  — each with an 8×8 sprite preview rendered from VRAM (respecting the tile
  index and palette). Scrollable.
- **Tilemap viewer:** the active BG map (`9800`/`9C00` per LCDC bit 3) as a
  32×32 grid of tile indices, with the SCX/SCY viewport rectangle overlaid.
- **Palette view:** BGP, OBP0, OBP1 each decoded into four DMG-green swatches.
- **Memory hex:** existing panel, extended with a watchpoint marker on watched
  cells; click a cell to toggle a read+write watchpoint there.

## 7. Text-input widget (`src/ide/ui.c`)

A minimal single-line field: accumulates printable ASCII, handles backspace and
Enter, renders with a caret. Reused immediately for the address-entry field and
designed to be the seed of the future editable code pane.

- Typing a hex address + Enter: if the address is in code space, toggles a PC
  breakpoint; otherwise prompts (a single keystroke `r`/`w`/`b`) for watchpoint
  direction. Exact micro-flow finalised in the plan.

## 8. Canvas layout

Grow the canvas from **640×432** to approximately **1024×640** (exact pixels set
in the plan). `--ide-shot` output dimensions grow to match; the
`test_ide_render` / `--ide-shot` baseline hash is re-baked after manual
eyeball-verification (same procedure as the dmg-acid2 hash gate).

Three columns:
- **Left (game + assets):** Game screen → Code pane → Tile editor.
- **Middle (CPU + execution):** Registers/flags → Execution-control status
  (mode, step granularity, last break reason) → Disassembly.
- **Right (memory + video state):** VRAM tile viewer → Palette → OAM viewer →
  Tilemap viewer.
- **Bottom span:** Memory hex (wide) → address-entry field + status line.

The panel-rect table in the `ide.c` header comment is rewritten to match.

## 9. Testing strategy

TDD throughout; everything in the core is headless and SDL-free.

- **`tests/test_dbg.c` (new):** breakpoint halts at the expected PC; a
  bank-gated breakpoint does *not* fire when the wrong bank is mapped; read and
  write watchpoints each fire with the correct `hit_kind`/`hit_addr`/`hit_pc`;
  the `count == 0` fast path leaves execution unchanged (cycle-for-cycle
  identical to a no-debug run).
- **`tests/test_disasm.c` (new):** decode known encodings (round-trip a handful
  of `gbasm`-produced bytes: each instruction's text + length is correct,
  including `CB`-prefixed and signed-displacement forms).
- **Execution control:** a headless test drives `ide_run_slice` through
  step-instruction / step-scanline / step-frame and asserts the expected
  `pc`/`ly`/frame deltas, and that a breakpoint set mid-run pauses at the right
  instruction.
- **Panel rendering:** verified through the re-baked `--ide-shot` FNV-hash gate
  plus targeted `test_ide_render` assertions on the new panels.
- **Text-input widget:** unit-tested in `tests/test_ui.c` (printable chars,
  backspace, Enter, caret position).

## 10. Incidental fix

While in the Makefile: wire `mem_timing` into the `test`/`blargg` path. The core
already passes it (`PASS roms/gb-test-roms/mem_timing/mem_timing.gb`); it was
simply never gated in CI. Free correctness win flagged in the project review.

## 11. Out of scope (v1 of this spec)

- Conditional breakpoints / hit counts.
- Watchpoint value-change conditions (break only when the written value differs).
- Time-travel / rewind (the original design's later milestone).
- Editable code pane (the text-input widget is groundwork, not the editor).
- Build changes are limited to adding `disasm.o`, `panels.o`, `test_dbg`,
  `test_disasm` and the `mem_timing` gate — no other Makefile restructuring.
