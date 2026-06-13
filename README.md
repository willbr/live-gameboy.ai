# live-gameboy

A Game Boy emulator + live-coding IDE in pure C and SDL3. Edit a function or
paint a tile while the game runs — state intact. PICO-8 spirit, real DMG ROMs.

Design: docs/superpowers/specs/2026-06-12-live-gameboy-design.md

## Status

- [x] Milestone 1: headless libgb core — passes blargg cpu_instrs, instr_timing
- [x] Milestone 2: SDL3 shell, pixel-FIFO PPU, APU
- [ ] Milestone 3: built-in assembler
- [ ] Milestone 4: live patching
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

## Sound

The APU passes all 12 blargg dmg_sound ROM tests (01-registers through 12-wave write
while on), verifying register read-back masks, power behavior, length counters,
envelopes, sweep, triggers, and wave channel edge cases. Deeper cycle-exact
sound-timing accuracy (sub-sample jitter, etc.) is not targeted.
