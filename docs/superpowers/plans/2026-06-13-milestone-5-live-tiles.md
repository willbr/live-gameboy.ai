# Milestone 5: Live Tile/Bitmap Editing (VRAM Provenance) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax. Each subagent: do one task, commit, REPORT BACK — do NOT run any branch-finishing/merge/PR flow or switch branches.

**Goal:** Paint a pixel in a tile and watch it change on the running game's screen — no reload, state intact. A tile editor sits on the live session: editing a pixel updates the source asset, the live ROM bytes, and every live VRAM byte that was copied from those ROM bytes (tracked by VRAM provenance). Acceptance (headless): boot a program that copies a tile from ROM into VRAM and renders it; paint a pixel via the editor; assert the live framebuffer pixel changes immediately, without any reload.

**Architecture:** Two new capabilities plus an editor layer.
1. **Asset provenance** (libasm): the build database records, for each INCBIN'd ROM byte, which asset file and offset produced it, and keeps the asset bytes available. (M3 only marked asset bytes with a `-2` sentinel; M5 records `(asset_id, asset_offset)`.)
2. **VRAM provenance** (libgb bus): a shadow map `vram_prov[0x2000]` of linear ROM offsets. The bus taints VRAM writes with the source ROM offset using the standard copy idiom — a VRAM write immediately preceded by a ROM read is recorded as `vram_prov[vram_off] = rom_read_offset`; any non-ROM read or a computed value clears the pending taint. OAM DMA from ROM also records provenance.
3. **Tile editor** (`src/live/tile.c`): `tile_paint(session, asset, tile, x, y, color)` updates the in-memory asset (2bpp), the live `gb->rom` bytes for those asset offsets (via build-db asset provenance, reverse-mapped), and every live `gb->vram` byte whose `vram_prov` points at those ROM offsets. Plus a 2bpp↔PNG codec for import/export (reusing the shell's PNG writer + a new reader).

**Tech Stack:** C11, no new deps. Builds on libgb + libasm + the M4 live session.

**Spec:** design §6 (bitmap/tile liveness), §9. Milestone 5.

**Depends on:** Milestones 1–4 (merged). Uses the live session (`src/live/live.{c,h}`) and the build database.

---

## Background

- **2bpp tile format**: 16 bytes/tile, 2 bytes/row; row r uses byte `r*2` (low bits) and `r*2+1` (high bits); pixel column x uses bit `7-x`; color = (hi_bit<<1)|lo_bit (0–3).
- **The standard copy idiom** the provenance heuristic targets: `ld a,(hl+) ; ld (de),a ; inc de ; dec bc ; ...` — a ROM read into A immediately followed by a VRAM write. The bus records a pending source offset on each ROM read (0x0000–0x7FFF, mapped to linear via the current bank) and, on the very next VRAM write, stamps `vram_prov`. A read from non-ROM clears the pending taint so unrelated writes aren't mis-attributed. This is a documented heuristic that covers the standard idiom (and OAM DMA); procedurally-generated VRAM simply has no provenance and is left untouched by the editor.

---

## File structure
```
src/asm/asm.h        — asset table + asset-provenance fields in AsmResult
src/asm/assemble.c   — record (asset_id, asset_off) per INCBIN byte; keep asset bytes
src/gb/gb.h          — vram_prov[0x2000] + pending-taint state + API
src/gb/bus.c         — taint on ROM read / stamp on VRAM write / clear on other reads
src/gb/gb.c          — reset provenance
src/live/tile.h      — tile editor + codec API
src/live/tile.c      — tile_paint, get/set pixel, 2bpp<->PNG
tests/test_asset_prov.c
tests/test_vram_prov.c
tests/test_tile_codec.c
tests/test_live_tile.c     — the gate: paint -> live framebuffer changes
```

---

### Task 1: Asset provenance in the build database

**Files:** modify `asm.h`, `assemble.c`; create `tests/test_asset_prov.c`.

- [ ] Add to AsmResult: an asset table `AsmAsset {char path[256]; uint8_t *bytes; size_t size;} *assets; int nassets;` and per-ROM-byte asset provenance: `int32_t *prov_asset; uint32_t *prov_asset_off;` (rom_size entries; prov_asset = asset index or -1; prov_asset_off = offset within that asset). Keep the existing `prov_line` (set to -2 for asset bytes as before, OR derive). Free everything in asm_free.
- [ ] On INCBIN: register the asset (path + a copy of its bytes) if not already present; for each byte emitted, set prov_asset[off]=asset_id, prov_asset_off[off]=i. 
- [ ] tests/test_asset_prov.c: write a temp asset file, incbin it, assert: assets[] has the path + bytes; for each ROM offset of the included region, prov_asset/prov_asset_off map back to the right asset byte; non-asset bytes have prov_asset == -1.
- [ ] `make test` green; gates (blargg/acid2/sound) green; build clean -Werror. Commit `feat: asset provenance in build database`.

---

### Task 2: VRAM provenance in the emulator bus

**Files:** modify `gb.h`, `bus.c`, `gb.c`; create `tests/test_vram_prov.c`.

- [ ] gb.h: add `uint32_t vram_prov[0x2000];` (0xFFFFFFFF = none), and pending-taint state `uint32_t prov_pending; bool prov_pending_valid;`. API: `void gb_vram_prov_reset(GB*);` and a way for tests to read provenance (just access the array).
- [ ] bus.c:
  - In `gb_read8`, when reading from ROM (a < 0x8000): set `prov_pending = linear_offset(a)` (bank-aware, same mapping as the read), `prov_pending_valid = true`.
  - In `gb_read8`, when reading from any NON-ROM region: `prov_pending_valid = false`.
  - In `gb_write8`, when writing to VRAM (0x8000–0x9FFF) and not blocked: if `prov_pending_valid`, set `vram_prov[a-0x8000] = prov_pending`; else set it to 0xFFFFFFFF (a non-ROM-sourced write clears provenance for that byte). (Do this regardless of whether the write itself is mode-blocked? Only stamp when the write actually lands — i.e. inside the not-blocked branch.)
  - OAM DMA reads through gb_read8 already (from M2c) — those reads taint pending but the writes go to OAM not VRAM, so no VRAM effect; that's fine. (Tile data normally reaches VRAM via CPU copy, which this covers.)
  - IMPORTANT: the CPU's instruction FETCH also reads ROM (every opcode fetch is a ROM read), which would set prov_pending right before... but a VRAM write instruction's operand read happens AFTER the opcode fetch. E.g. `ld (de),a`: fetch opcode (ROM read → pending=opcode offset!), then write VRAM. That would mis-attribute provenance to the OPCODE byte, not the tile data! FIX: the tile data was read by the PREVIOUS instruction (`ld a,(hl+)`), but the opcode fetch of `ld (de),a` is a ROM read that clobbers pending. So the naive heuristic breaks due to opcode fetches.
    SOLUTION: distinguish operand reads from opcode fetches. The cleanest: only taint on ROM reads that are NOT opcode fetches. Add a flag the CPU sets around opcode fetch. Simplest: in cpu.c, the opcode fetch uses `fetch8` at the start of gb_step; mark `g->in_opcode_fetch = true` during that fetch and false otherwise, and in bus.c only set prov_pending when NOT in_opcode_fetch. OR: have the CPU expose the last operand-read source differently.
    RECOMMENDED IMPLEMENTATION: add `bool gb_fetching_opcode;` to GB; set it true in cpu.c immediately before the opcode `fetch8(g)` in gb_step and the CB-prefix fetch, false immediately after. In bus.c gb_read8 ROM branch: `if (!gb->fetching_opcode) { prov_pending = off; prov_pending_valid = true; }` — opcode fetches don't disturb the pending taint. Then `ld a,(hl+)` (operand read sets pending=tile offset), `ld (de),a` (opcode fetch doesn't clear it; the VRAM write stamps it). This works. Implement this.
- [ ] gb.c: gb_vram_prov_reset in gb_reset (fill 0xFFFFFFFF, clear pending).
- [ ] tests/test_vram_prov.c: assemble+run (or hand-build) a small program that copies a 16-byte tile from a known ROM address into VRAM 0x8000 via the `ld a,(hl+); ld (de),a` idiom; after running, assert vram_prov[0..15] == the linear ROM offsets of the tile source bytes. Also assert a VRAM byte written from a computed value (e.g. `ld a,0; ld (de),a`) has provenance 0xFFFFFFFF.
- [ ] `make test` green; **blargg/acid2/sound MUST still pass** (the fetching_opcode flag touches cpu.c's hot path — verify no regression). build clean. Commit `feat: VRAM provenance tracking in bus (ROM->VRAM copy taint)`.

---

### Task 3: 2bpp tile codec + PNG import/export

**Files:** create `src/live/tile.h`, `src/live/tile.c`; create `tests/test_tile_codec.c`. (PNG writer exists at src/shell/png.c — but that's shell; for liblive keep it independent: add a minimal PNG read/write in tile.c OR factor png into a shared spot. Simplest: add a small PNG writer/reader in tile.c using zlib, independent of the shell.)

- [ ] tile.h API:
```c
uint8_t tile2bpp_get(const uint8_t *tile16, int x, int y);          /* color 0..3 */
void    tile2bpp_set(uint8_t *tile16, int x, int y, uint8_t color);
/* convert an array of tiles (n tiles, 16 bytes each) to/from an indexed image
   laid out as tiles_per_row columns; colors 0..3. */
int tile_sheet_to_png(const char *path, const uint8_t *tiles, int ntiles, int tiles_per_row);
int tile_sheet_from_png(const char *path, uint8_t *tiles, int max_tiles, int *out_ntiles);
```
- [ ] tests/test_tile_codec.c: set/get round-trip for all 4 colors at several (x,y); build a 2-tile sheet, write PNG, read it back, assert tiles identical; assert get matches the 2bpp bit layout (externally: e.g. a tile with row0 bytes lo=0xA0,hi=0x00 → pixel(0,0)=1, pixel(1,0)=0, pixel(2,0)=1).
- [ ] `make test` green; gates green; build clean. Commit `feat: 2bpp tile codec + PNG import/export`.

---

### Task 4: Tile editor — paint into the live game (the gate)

**Files:** modify `tile.h`, `tile.c` (depends on live session + build db); create `tests/test_live_tile.c`.

- [ ] tile.h: `bool tile_paint(LiveSession *s, const char *asset_path, int tile_index, int x, int y, uint8_t color, char *err, int errlen);`
- [ ] tile.c tile_paint:
  1. Find the asset in the session's build db by path; compute the byte offsets within the asset for pixel (x,y) of tile `tile_index` (asset_off_lo = tile_index*16 + y*2, asset_off_hi = +1).
  2. Update the asset bytes (in the build db's asset copy) via tile2bpp_set on the 16-byte tile slice.
  3. For each changed asset byte, reverse-map to live ROM offset(s): scan the build db's prov_asset/prov_asset_off for entries matching (asset_id, asset_off) and write the new byte into `gb->rom[rom_off]`. (Usually one ROM offset per asset byte.)
  4. For each updated ROM offset, scan `gb->vram_prov` for entries == that ROM offset and write the new byte into `gb->vram[that index]`. The on-screen tile updates live.
  5. Return true; on any failure (asset not found, pixel out of range) set err and return false, changing nothing.
- [ ] tests/test_live_tile.c — THE GATE:
  - Build a project: a small .asm that INCBINs a tile asset (a temp .2bpp file with a known tile), copies it to VRAM 0x8000 via the copy idiom, sets BGP/tilemap, enables LCD, loops. (Entry at 0x100 handled by live_new's Main patch.)
  - live_new; run ~10 frames; assert a known framebuffer pixel reflects the original tile (e.g. pixel (0,0) shade matches original color via BGP).
  - tile_paint(session, asset_path, tile 0, x=0, y=0, new color). Run a frame (so the PPU re-renders from the now-updated VRAM — note: the VRAM byte was updated directly, so even the SAME frame's next render shows it).
  - Assert: gb->vram[0/1] changed to the new pattern; the framebuffer pixel (0,0) now shows the NEW color's shade; AND the program was NEVER reloaded (no live_reload call) and RAM state untouched. This proves live bitmap editing.
  - Also: paint a pixel of a tile that was NOT copied to VRAM (or a procedurally-written VRAM byte) and assert only the ROM/asset updated, VRAM with no matching provenance is left alone.
- [ ] `make test` green; gates green; build clean. Commit `feat: live tile painting (asset -> ROM -> live VRAM)`.

---

### Task 5: Integration + CI/README + verification

**Files:** Makefile (tile.c into LIVE_OBJ — likely automatic), `.github/workflows/ci.yml`, `README.md`.

- [ ] Confirm tile.c is in LIVE_OBJ and the new tests run in `make test`.
- [ ] README: mark Milestone 5 done; add a "Live tiles" blurb (paint a pixel → running screen updates via VRAM provenance; PNG import/export). 
- [ ] CI: covered by `make test`; no new step unless needed.
- [ ] Full verification: `make clean && make test && make blargg && make acid2 && make sound && make gbasm && make live-gameboy`. All green.
- [ ] Commit `chore: milestone 5 integration + docs`.

---

## Self-review notes
- **Headless gate proves the feature:** paint a pixel → the live framebuffer changes with no reload and no state loss, exactly the spec §6 promise, testable without UI (the editor UI is Milestone 6).
- **The opcode-fetch trap is handled:** VRAM provenance would be wrong without distinguishing operand reads from opcode fetches; the `fetching_opcode` flag (set around cpu.c's opcode fetch) fixes it. Every task re-runs blargg/acid2/sound because this touches the CPU hot path.
- **Heuristic, honestly scoped:** provenance covers the standard `ld a,(hl+); ld (de),a` copy idiom and OAM-DMA; procedurally generated VRAM has no provenance and is correctly left untouched (asserted in the gate). This matches the spec's stated approach.
- **Builds on M3/M4 cleanly:** asset provenance extends the build db; the tile editor is a thin layer over the live session; VRAM provenance is a localized bus hook.
- **Deferred:** the editor UI (Milestone 6); CGB palettes; provenance through non-standard copy routines.
```
