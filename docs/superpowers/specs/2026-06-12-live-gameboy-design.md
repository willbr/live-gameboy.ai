# live-gameboy — a live-coding Game Boy emulator + IDE

**Date:** 2026-06-12
**Status:** Approved design

## 1. Vision

A single desktop binary (pure C, SDL3; stb single-header libraries permitted) that is a
real DMG Game Boy emulator fused with a PICO-8-style IDE. The game is never stopped:
edit an assembly function or paint a tile, and the running game changes in front of you
with all state intact. This applies the "Stop Writing Dead Programs" thesis (see
`ref/stop-writing-dead-programs-complete.md`) to the most batch-oriented platform
imaginable. Hardware behavior follows the Pan Docs (`ref/Specifications.html`).

Projects export standard `.gb` ROMs that run in any emulator or on real hardware.

### Non-goals (v1)

- Game Boy Color support (DMG only).
- C/GBDK authoring (assembly first; the design keeps the door open for a C frontend).
- Map editor, sound tracker, time-travel/rewind debugging (later versions; savestate
  support in the core lays the groundwork for rewind).
- Serial link cable, Super Game Boy features.

## 2. Architecture

Three subsystems in one process, single-threaded main loop:

```
┌─────────────────────────────────────────────┐
│ IDE shell (SDL3, custom GB-themed IMGUI)    │
│  code editor │ tile editor │ debug panels   │
├──────────────┬──────────────────────────────┤
│ libasm       │ libgb (emulator core)        │
│ SM83 asm,    │ SM83 CPU, FIFO PPU, APU,     │
│ layout mgr,  │ timers, MBC, provenance bus  │
│ patch engine │ (no SDL dependency, pure)    │
└──────────────┴──────────────────────────────┘
```

Main loop per display frame: pump SDL events → UI update (which may trigger reassembly
and live patching) → step emulator one video frame (70224 T-cycles) → render.

Audio: the APU writes samples into a ring buffer; an SDL3 audio stream consumes it.

`libgb` and `libasm` are SDL-free C libraries: deterministic, headless-testable, with
fully serializable state (savestates fall out for free).

Dependencies: SDL3, stb single-header libs (stb_image for PNG tile import/export;
others as needed). Nothing else.

## 3. Emulator core (libgb)

- **CPU:** SM83, M-cycle stepped, complete opcode set, interrupts (IME/IE/IF, exact
  dispatch timing), HALT bug, STOP.
- **PPU:** pixel-FIFO implementation — background/window/sprite fetchers and a
  per-T-cycle pixel pipeline, mode 3 variable timing, correct STAT/LYC behavior,
  OAM DMA bus conflicts.
- **APU:** all four channels (two pulse, wave, noise), frame sequencer, length/envelope/
  sweep units.
- **Memory bus:** central read/write dispatch with hook points. Provenance tracking and
  future watchpoints attach here. The bus answers "what linear ROM offset does this CPU
  address currently map to?" as a first-class query — the patch engine, debugger, and
  disassembly panel all need it.
- **Cartridge/MBC:** ROM-only, MBC1, MBC5. External RAM with battery-save persistence.
- **Acceptance bar:** blargg `cpu_instrs`, `instr_timing`, `mem_timing`; dmg-acid2;
  the mooneye-gb timing suite. All run headless in CI.

## 4. Assembler (libasm)

An in-process SM83 assembler with RGBDS-inspired syntax: labels (global and local),
`SECTION` with bank attributes (`ROM0`, `ROMX, BANK[n]`, `WRAM0`, etc.), `INCLUDE`,
`INCBIN`, `EQU`/`DEF`, `DB/DW/DS`, basic macros, character maps not required in v1.
Whole-program reassembly on every edit (target: under 10 ms at GB scale).

### Address model

Banked memory means a CPU address is meaningless without a bank. Throughout the build
database:

- Symbols are `(bank, cpu_addr)` pairs.
- Provenance and patching use **linear ROM offsets** into the cartridge image.

### Build database

Each successful assembly emits, alongside the ROM image:

- Symbol table: name → (bank, cpu_addr), size, kind (code/data), and every static
  reference site (calls, jumps, pointer loads) — the assembler sees all of them.
- Line ↔ address map (both directions) for the editor and disassembly views.
- Per-byte provenance: which source span or asset file byte produced every ROM byte.
- Diagnostics with source spans, so the editor shows errors inline as you type.

### Layout manager

Placement is remembered between builds, per bank:

- Unchanged functions keep their exact addresses.
- Each function gets a padded slot (size rounded up with small slack) so edits can grow
  in place.
- Placement memory persists with the project so layout is stable across sessions.

## 5. Live patch engine

On each successful reassembly, diff against the running build and classify every symbol:

1. **Unchanged** — nothing to do.
2. **Edited, fits its slot** — patch ROM bytes in place. Applied at a safe point:
   between emulated instructions, with PC outside the patched range **and no stack
   return address inside it** (a function mid-call-out must not have its body changed
   under the in-flight return; if the stack scan finds such a return address, the edit
   falls back to relocation, case 3). Both checks are bank-aware: an address in
   `4000-7FFF` only conflicts with a patch in switchable ROM if the currently mapped
   bank is the patched one — and for stack addresses the mapped bank is unknowable, so
   any return address in the patched CPU range conservatively triggers the fallback.
3. **Edited, outgrew its slot** — the new version is placed at a fresh address **within
   the same bank** (or bank 0, which is always mapped); all static reference sites are
   rebound; a `JP new` trampoline is written at the old entry. The old body is kept as a
   zombie: stack return addresses into it remain valid, an in-flight call finishes in
   the old version, and every subsequent call gets the new one (Erlang-style two-version
   semantics). Cross-bank relocation is never attempted live. If the bank is full, the
   patch is refused with a clear message ("bank 2 full — soft reload required").
4. **Unsafe edit** (e.g., changed RAM data layout that running state contradicts,
   changed `SECTION` placement of variables) — the patch is refused, the reason is
   shown, and a one-keystroke soft reload (re-run from reset with the new ROM) is
   offered.

Banking subtlety, made explicit: stack return addresses into switchable ROM are
ambiguous — hardware does not record which bank they expect. The zombie strategy is
conservative by construction: relocation never overwrites old code in any bank; only
same-size in-place patches overwrite bytes, and those are guarded by the safe-point
check. A stale return therefore always lands in valid old code.

Zombie reclamation: zombies accumulate in their bank's free space and are reclaimed on
soft reload. If a bank fills with zombies, the engine reports it and offers soft reload.

The status line always shows what happened — patched in place, relocated, or refused
and why. The system never lies about what is live.

## 6. Bitmap/tile liveness

Tiles live in asset files (`assets/*.2bpp`, GB-native 2bpp format) referenced from
assembly via `INCBIN`. Painting reaches the screen through two patch paths:

1. **ROM:** asset file byte → linear ROM offset (via build database) → patched.
2. **VRAM:** the bus maintains a VRAM provenance shadow map (8 KB of linear ROM
   offsets): whenever the game copies ROM→VRAM (CPU copy loop or OAM DMA), each written
   VRAM byte records its source ROM offset — captured at copy time, when the mapped
   bank is known. Painting updates every VRAM byte whose provenance points at the
   edited ROM bytes, instantly, even if the source bank is no longer mapped.

A sprite changes mid-animation as you paint it. VRAM bytes produced procedurally (not
a clean ROM copy) have no provenance and are left alone. Provenance for a VRAM byte is
invalidated when the game itself overwrites that byte from a non-ROM source.

PNG import/export via stb_image. The same provenance map lets the tile editor highlight
which tiles are currently live in VRAM.

## 7. IDE shell

Custom immediate-mode UI rendered with SDL3 — chunky pixels, custom bitmap font,
GB-green palette. The IDE itself feels like a fantasy workstation, in the PICO-8
tradition. v1 panels:

- **Code editor:** gap buffer, syntax highlighting for our assembly dialect, inline
  diagnostics, assemble-on-keystroke (debounced).
- **Game screen:** integer-scaled, always running. Joypad mapped to keyboard.
- **Tile editor:** 8×8 pixel editing with the 4 shades, tile-sheet overview, live-in-
  VRAM highlighting.
- **Debug panels:** CPU registers/flags, disassembly around PC (bank-aware), memory hex
  view, VRAM/tilemap/OAM viewers, palette view; pause/resume and stepping by
  instruction, scanline, or frame.
- **Status line:** assemble time, patch outcome, FPS.

## 8. Project format

A project is a plain directory — no opaque container:

```
mygame/
  main.asm        ; entry point
  *.asm           ; includes
  assets/*.2bpp   ; tile data (INCBIN'd)
  .layout         ; layout-manager placement memory (generated)
```

"Export ROM" writes a standard `.gb` with a valid cartridge header (logo, checksums,
MBC/ROM-size bytes) that runs anywhere, including real hardware via flashcart.

## 9. Testing strategy

TDD throughout (per superpowers conventions).

- **CPU:** per-opcode unit tests (encodings, flags, cycle counts) plus blargg ROMs
  headless with serial/“magic write” result capture.
- **PPU/timing:** dmg-acid2 framebuffer hash comparison; mooneye suite pass/fail.
- **Assembler:** round-trip tests (source → bytes → disassembly), diagnostics tests,
  layout-stability tests (re-assemble with one edited function: all other addresses
  unchanged).
- **Patch engine:** scripted scenarios — boot a test game headless, apply a source
  edit, assert RAM/VRAM/CPU state preserved and new behavior observable; trampoline,
  zombie-return, bank-full-refusal, and unsafe-edit cases each covered.
- **UI:** kept thin over the testable libraries.

## 10. Build milestones

1. `libgb` headless: CPU + bus + timers; passes blargg `cpu_instrs`.
2. SDL3 shell plays real ROMs: pixel-FIFO PPU, APU, joypad; passes dmg-acid2.
3. `libasm` builds working ROMs with full build database.
4. **The live loop:** edit → reassemble → patch while running, all four classifications.
5. Tile editor + VRAM provenance: paint into the live game.
6. Debug panels, project export, polish.

Each milestone is independently demonstrable; milestone 4 is the thesis proof.
