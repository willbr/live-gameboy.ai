# live-gameboy

A Game Boy emulator + live-coding IDE in pure C and SDL3. Edit a function or
paint a tile while the game runs — state intact. PICO-8 spirit, real DMG ROMs.

Design: docs/superpowers/specs/2026-06-12-live-gameboy-design.md

## Status

- [x] Milestone 1: headless libgb core — passes blargg cpu_instrs, instr_timing
- [ ] Milestone 2: SDL3 shell, pixel-FIFO PPU, APU
- [ ] Milestone 3: built-in assembler
- [ ] Milestone 4: live patching
- [ ] Milestone 5: live tile editing
- [ ] Milestone 6: debug panels, ROM export

## Build

    make test     # unit tests
    make roms     # fetch test ROMs (or clone retrio/gb-test-roms into roms/)
    make blargg   # acceptance tests
