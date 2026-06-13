# live-gameboy

A Game Boy emulator + live-coding IDE in pure C and SDL3. Edit a function or
paint a tile while the game runs — state intact. PICO-8 spirit, real DMG ROMs.

Design: docs/superpowers/specs/2026-06-12-live-gameboy-design.md

## Status

- [x] Milestone 1: headless libgb core — passes blargg cpu_instrs, instr_timing
- [x] Milestone 2: SDL3 shell, pixel-FIFO PPU, APU
- [x] Milestone 3: built-in SM83 assembler (`gbasm`) — RGBDS-inspired syntax, two-pass, build database
- [x] Milestone 4: live patching — edit a function in a running game, state intact (headless engine; editor UI in M6)
- [ ] Milestone 5: live tile editing
- [ ] Milestone 6: debug panels, ROM export

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

## Assemble

The built-in `gbasm` assembler compiles RGBDS-inspired SM83 assembly to a valid
Game Boy ROM with a complete cartridge header (logo, checksums):

    make gbasm
    ./gbasm game.asm -o game.gb [--sym game.sym]
    ./live-gameboy game.gb

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

## Sound

The APU passes all 12 blargg dmg_sound ROM tests (01-registers through 12-wave write
while on), verifying register read-back masks, power behavior, length counters,
envelopes, sweep, triggers, and wave channel edge cases. Deeper cycle-exact
sound-timing accuracy (sub-sample jitter, etc.) is not targeted.
