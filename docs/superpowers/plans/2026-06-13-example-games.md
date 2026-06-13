# Live-coding Example Gallery (Pong, Snake, Breakout) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add three playable example games (`examples/pong.asm`, `examples/snake.asm`, `examples/breakout.asm`) that showcase the live-coding IDE (hot-reload, live tile painting, OAM/memory editing), and fix the `gbasm` CLI so assembled ROMs boot standalone.

**Architecture:** Each game is a self-contained `.asm` with a `Main:` entry, a one-time init block (the F8 / soft-reset target) and a VBlank-synced main loop calling small named procedures (the F5 / hot-reload targets). Tweakable physics/rate constants are grouped so the live-edit hook is obvious; paintable tiles are `incbin` `.2bpp` assets (like `examples/demo.asm`). Verification is via headless screenshot gates (`--ide-shot`) plus C smoke tests that assemble, run the emulator, and assert state.

**Tech Stack:** C11 emulator core (`src/gb`), `gbasm` SM83 assembler (`src/asm`), live engine (`src/live`), SDL3 IDE (`src/ide`). Tests are plain C using `tests/test.h` (`ASSERT_EQ`/`ASSERT_TRUE`/`TEST_MAIN_END`), auto-discovered via the `tests/test_*.c` glob in the Makefile.

---

## Background facts (verified against the codebase)

These ground every task below. Do not re-derive — but do re-confirm if a file has changed.

- **Entry point:** `gb_reset()` sets `PC=$0100` (`src/gb/gb.h:96`). The assembler's default origin is `$0150` (`src/asm/assemble.c:49`) and `asm_assemble()` writes the cartridge header (logo + checksums) but does **not** write a jump at `$0100`. The `JP Main` entry patch lives only in `src/live/live.c:137 patch_entry()` (static, IDE/live path only). The `gbasm` CLI (`src/asm/gbasm.c`) never patches it, so CLI-built ROMs hang. **Task 0 fixes this.**
- **Run path for examples:** The IDE (`./live-gameboy-ide examples/X.asm`) assembles through `live_new()`, which already patches `$0100 → JP Main`. So in the IDE a `Main:` global is the entry — no boilerplate, exactly like `examples/demo.asm`.
- **Assembler syntax that is known-good** (from `examples/demo.asm`, which assembles and runs): `SECTION "name", ROM0`; global labels `Foo:`; local labels `.bar` (scoped to the last global); `ld a, (hl+)`; `ld (de), a`; `ldh ($40), a`; `ldh a, ($44)`; `ld ($C000), a`; `ld a, ($C000)`; `cp $90`; `jr nz, .label`; `inc`/`dec`/`add`/`or`/`xor`/`and`/`cpl`; `ld bc, $0010`; `incbin "path"`. **Use literal hex addresses (not `EQU`)** to match the proven idiom and avoid unverified directive syntax.
- **OAM DMA** (`src/gb/ppu.c:282`) is instantaneous and may be triggered from anywhere: writing the high byte to `$FF46` immediately copies `$A0` bytes from `(value<<8)` into OAM. No HRAM trampoline needed. Use a WRAM OAM shadow at `$C000` and `ld a,$C0 / ldh ($46),a` during VBlank.
- **Joypad** (`src/gb/joypad.c`): write `$FF00` select nibble, read low nibble (0=pressed). Directions group: write `$20` (bit5=1 actions off, bit4=0 dirs on) then read bit0=Right, bit1=Left, bit2=Up, bit3=Down. Actions group: write `$10` then read bit0=A, bit1=B, bit2=Select, bit3=Start. For C tests, `gb_set_buttons(gb, mask)` uses: bit0 A, bit1 B, bit2 Select, bit3 Start, bit4 Right, bit5 Left, bit6 Up, bit7 Down.
- **LCDC** value `$93` = LCD on + BG tile data `$8000` + OBJ on + BG on, 8×8 sprites. BGP/OBP0 `$E4` maps color index 3→darkest.
- **Framebuffer:** `gb_framebuffer(gb)` returns `160*144` bytes, each shade `0..3`. `gb_read8(gb, addr)` reads bus memory (for asserting game-state bytes in WRAM).
- **Emulator step loop** (from `tests/test_asm_e2e.c:701 run_gb`): `gb_step` → `gb_tick(tc)` → `gb_ppu_tick(tc)`; a frame is done when `gb->frame_ready` flips true (clear it yourself).
- **Test build:** every `tests/test_*.c` is auto-compiled and linked against `GB_OBJ ASM_OBJ LIVE_OBJ IDE_OBJ -lz` (`Makefile:42`). `make test` runs them all.
- **Screenshot gate pattern** (`Makefile:96 ide-shot`): `./live-gameboy-ide --ide-shot <file.asm> <out.png> <frames>`.

## File Structure

**Create:**
- `examples/pong.asm` — Pong game source.
- `examples/snake.asm` — Snake game source.
- `examples/breakout.asm` — Breakout game source.
- `examples/ball.2bpp`, `examples/paddle.2bpp` — paintable solid tiles for Pong.
- `examples/snakebody.2bpp`, `examples/food.2bpp` — paintable tiles for Snake.
- `examples/brick.2bpp` — paintable brick tile for Breakout (Breakout reuses `ball.2bpp`/`paddle.2bpp`).
- `tests/example_run.h` — shared static test helper: assemble a `.asm` file, patch entry, load, run N frames, expose framebuffer + memory reads. (DRY across the three game test files.)
- `tests/test_example_pong.c`, `tests/test_example_snake.c`, `tests/test_example_breakout.c` — per-game boot + smoke tests.

**Modify:**
- `src/asm/assemble.c` — add the `JP Main` entry patch into `asm_assemble` so all paths (CLI + live) produce bootable ROMs (Task 0).
- `tests/test_asm_e2e.c` — add a test proving a CLI-style assemble boots without manual `rom[0x100]` patching (Task 0).
- `Makefile` — add `pong-shot`, `snake-shot`, `breakout-shot`, `examples` targets; add them to `.PHONY`.
- `README.md` — document the example gallery and the per-game live-edit recipes; correct the entry-point note.

---

## Task 0: Fix the `gbasm` CLI entry point

**Why:** So `./gbasm game.asm -o game.gb && ./live-gameboy game.gb` boots straight into `Main:`, matching the IDE and the README. The patch belongs in `asm_assemble()` (shared by CLI and live) and must run **before** the global checksum is computed (the global checksum at `$014E` covers all bytes; the header checksum at `$014D` covers only `$0134..$014C`, so `$0100` does not affect it).

**Files:**
- Modify: `src/asm/assemble.c` (header-build block ends ~`src/asm/assemble.c:1788`)
- Test: `tests/test_asm_e2e.c`

- [ ] **Step 1: Write the failing test** — add to `tests/test_asm_e2e.c` (and call it from `main()` near the other `test_e2e_*` calls).

```c
/* Task 0: a Main: global makes asm_assemble() write JP Main at 0x0100,
 * so a CLI-built ROM boots without any manual entry patch. */
static void test_e2e_entry_autopatch(void)
{
    const char *src =
        "SECTION \"code\", ROM0\n"
        "Main:\n"
        "    ld a, $FF\n"
        "    ld hl, $8000\n"
        "    ld b, 16\n"
        ".fill:\n"
        "    ld (hl+), a\n"
        "    dec b\n"
        "    jr nz, .fill\n"
        "    ld hl, $9800\n"
        "    xor a\n"
        "    ld (hl), a\n"
        "    ld a, $E4\n"
        "    ldh ($47), a\n"      /* BGP */
        "    ld a, $91\n"
        "    ldh ($40), a\n"      /* LCDC on */
        "Spin:\n"
        "    jr Spin\n";

    AsmResult r = asm_assemble(src, "entry_autopatch.asm");
    ASSERT_TRUE(r.ok);

    /* The assembler itself must have written JP Main at 0x0100 — NO manual patch. */
    ASSERT_EQ(r.rom[0x0100], 0xC3);          /* JP */
    const AsmSymbol *m = find_sym(&r, "Main");
    ASSERT_TRUE(m != NULL);
    ASSERT_EQ(r.rom[0x0101], (m->addr & 0xFF));
    ASSERT_EQ(r.rom[0x0102], ((m->addr >> 8) & 0xFF));

    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    run_gb(gb, 10, 2000000);
    const uint8_t *fb = gb_framebuffer(gb);
    ASSERT_EQ(fb[0], 3);                      /* reached our code, drew a dark tile */

    gb_free(gb);
    asm_free(&r);
}
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `make build/test_asm_e2e && ./build/test_asm_e2e`
Expected: FAIL at `r.rom[0x0100] == 0xC3` (got `0x00`) and likely `fb[0]` wrong — the entry isn't patched yet.

- [ ] **Step 3: Implement the entry patch in `asm_assemble`**

In `src/asm/assemble.c`, inside the header-build `if (r.rom && r.rom_size >= 0x0150u) { ... }` block, **before** the `/* 0x014E-0x014F: global checksum */` step, insert:

```c
        /* 0x0100-0x0102: entry-point jump.
         * gb_reset() starts PC=0x0100; the bytes 0x0104+ are the Nintendo
         * logo (data, not code). If the source defines a ROM0 global "Main",
         * write JP Main here so the CPU reaches user code. Mirrors the live
         * engine's patch_entry() so CLI- and IDE-built ROMs behave identically.
         * Done before the global checksum so that checksum stays valid. */
        for (int i = 0; i < st.count; i++) {
            if (strcmp(st.syms[i].name, "Main") == 0 && st.syms[i].defined
                && st.syms[i].bank == 0 && st.syms[i].addr < 0x4000u) {
                r.rom[0x0100] = 0xC3u;                              /* JP nn */
                r.rom[0x0101] = (uint8_t)(st.syms[i].addr & 0xFFu);
                r.rom[0x0102] = (uint8_t)((st.syms[i].addr >> 8) & 0xFFu);
                break;
            }
        }
```

(If `st.syms[i]` field names differ from `name`/`defined`/`bank`/`addr`, match the names used by the existing `0x0147` cartridge-type loop a few lines above, which already iterates `st.syms[i].bank`.)

- [ ] **Step 4: Run the test to confirm it passes**

Run: `make build/test_asm_e2e && ./build/test_asm_e2e`
Expected: PASS, 0 failures. Then `make test` — all suites still green (existing e2e tests manually patch `rom[0x100]` too; re-patching to the same `JP` value is harmless).

- [ ] **Step 5: Commit**

```bash
git add src/asm/assemble.c tests/test_asm_e2e.c
git commit -m "fix(gbasm): write JP Main entry at 0x0100 so CLI ROMs boot standalone

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 1: Shared example test harness

**Files:**
- Create: `tests/example_run.h`

- [ ] **Step 1: Write the harness header**

Static inline helpers (header-only; each test TU gets its own copy — no link clash). Reads a real `.asm` file from `examples/`, assembles, relies on Task 0's auto-patched entry, runs frames, and exposes the GB for assertions.

```c
#ifndef EXAMPLE_RUN_H
#define EXAMPLE_RUN_H
#include <stdio.h>
#include <stdlib.h>
#include "../src/asm/asm.h"
#include "../src/gb/gb.h"

/* Read a whole file into a NUL-terminated heap buffer (caller frees). */
static char *ex_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "example_run: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f); fclose(f);
    b[got] = '\0';
    return b;
}

/* Assemble an examples/*.asm file. Returns an AsmResult (caller asm_free's).
 * On failure, r.ok is false and diagnostics are printed. */
static AsmResult ex_assemble(const char *path) {
    char *src = ex_read_file(path);
    AsmResult r = {0};
    if (!src) return r;
    r = asm_assemble(src, path);
    free(src);
    if (!r.ok) {
        for (int i = 0; i < r.ndiags; i++)
            fprintf(stderr, "  %s:%d: %s\n",
                    r.diags[i].filename ? r.diags[i].filename : path,
                    r.diags[i].line, r.diags[i].msg);
    }
    return r;
}

/* Step the emulator until `frames` VBlanks elapse or `max_steps` instructions run. */
static void ex_run(GB *gb, int frames, int max_steps) {
    int f = 0, s = 0;
    while (f < frames && s < max_steps) {
        int tc = gb_step(gb);
        gb_tick(gb, tc);
        gb_ppu_tick(gb, tc);
        if (gb->frame_ready) { gb->frame_ready = false; f++; }
        s++;
    }
}

/* True if any framebuffer pixel is non-zero (screen is not blank). */
static int ex_fb_nonblank(GB *gb) {
    const uint8_t *fb = gb_framebuffer(gb);
    for (int i = 0; i < 160 * 144; i++) if (fb[i] != 0) return 1;
    return 0;
}
#endif
```

- [ ] **Step 2: Commit** (header compiles when first used by Task 2; commit now so it is tracked)

```bash
git add tests/example_run.h
git commit -m "test: shared harness for assembling and running example ROMs

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Pong — assets, init, and boot screenshot

**Files:**
- Create: `examples/ball.2bpp`, `examples/paddle.2bpp`, `examples/pong.asm`
- Create: `tests/test_example_pong.c`

**WRAM map (Pong):** `$C000-$C09F` = OAM shadow (entry n at `$C000+n*4`: Y, X, tile, attr). Sprites used: 0=ball, 1=Lpad-top, 2=Lpad-bot, 3=Rpad-top, 4=Rpad-bot. Game vars: `$C0A0`=ballX, `$C0A1`=ballY, `$C0A2`=ballDX (signed), `$C0A3`=ballDY, `$C0A4`=lPadY, `$C0A5`=rPadY.

- [ ] **Step 1: Create the tile assets** (16 bytes each = one solid 8×8 tile, both planes `$FF` → color 3).

```bash
printf '\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377' > examples/ball.2bpp
printf '\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377' > examples/paddle.2bpp
# verify
wc -c examples/ball.2bpp examples/paddle.2bpp   # each must be 16
```

- [ ] **Step 2: Write the boot test (failing — no pong.asm yet)** in `tests/test_example_pong.c`

```c
#include "test.h"
#include "example_run.h"

/* Boot: pong.asm assembles, the LCD comes on, and the screen is not blank. */
static void test_pong_boots(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }

    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);          /* 30 frames */
    ASSERT_TRUE(ex_fb_nonblank(gb));  /* paddles + ball are drawn */

    gb_free(gb);
    asm_free(&r);
}

int main(void) {
    test_pong_boots();
    TEST_MAIN_END();
}
```

- [ ] **Step 3: Run it to confirm it fails**

Run: `make build/test_example_pong && ./build/test_example_pong`
Expected: FAIL — `ex_assemble` can't open `examples/pong.asm`, `r.ok` false.

- [ ] **Step 4: Write `examples/pong.asm` — init + minimal loop**

```asm
; pong.asm — Pong for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/pong.asm
; 2. HOT-RELOAD PHYSICS (F5): find UpdateBall below. Change the ball's
;    start speed by editing the ".BALL_SPEED EQU"-style constants at the
;    top of the main loop (BALLDX/BALLDY init in Main are one-time; the
;    per-bounce speed lives in UpdateBall). Edit a value, press F5 — the
;    rally keeps going at the new speed, score/positions intact.
; 3. RESKIN (paint): click VRAM tile 1 (ball) or tile 2 (paddle) in the
;    tile viewer, paint it in the TILE editor — the ball/paddle changes
;    on screen live (ball.2bpp / paddle.2bpp are paintable assets).
; 4. TELEPORT (OAM edit): edit sprite 0's Y/X in the OAM panel to move the
;    ball by hand.  Use F8 (soft reset) if you change Main/init code.
; ======================================================================
;
; Controls: Up/Down = move left paddle. Right paddle is a simple AI.

SECTION "code", ROM0

Main:
    ld sp, $FFFE

    ; --- LCD off so we can touch VRAM ---
    xor a
    ldh ($40), a              ; LCDC = 0

    ; --- Load ball tile -> $8000 (tile 1), paddle tile -> $8010 (tile 2) ---
    ; (tile 0 stays blank = transparent for sprites / color 0 for BG)
    ld hl, .BallTile
    ld de, $8010             ; tile 1 lives at $8000+1*16
    ld bc, $0010
    call .CopyBC
    ld hl, .PaddleTile
    ld de, $8020             ; tile 2
    ld bc, $0010
    call .CopyBC

    ; --- Clear BG tilemap $9800..$9BFF to tile 0 (blank court) ---
    ld hl, $9800
    ld bc, $0400
.clrmap:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clrmap

    ; --- Palettes: BGP and OBP0 = $E4 ---
    ld a, $E4
    ldh ($47), a             ; BGP
    ldh ($48), a             ; OBP0

    ; --- Zero the OAM shadow $C000..$C09F (all sprites off-screen) ---
    ld hl, $C000
    ld bc, $00A0
.clroam:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clroam

    ; --- Sprite tiles/attrs: ball=tile1, paddles=tile2 ---
    ld a, $01
    ld ($C002), a            ; ball tile
    ld a, $02
    ld ($C006), a            ; Lpad-top tile
    ld ($C00A), a            ; Lpad-bot tile
    ld ($C00E), a            ; Rpad-top tile
    ld ($C012), a            ; Rpad-bot tile

    ; --- Init game vars ---
    ld a, 80
    ld ($C0A0), a            ; ballX
    ld a, 72
    ld ($C0A1), a            ; ballY
    ld a, 1
    ld ($C0A2), a            ; ballDX = +1
    ld a, 1
    ld ($C0A3), a            ; ballDY = +1
    ld a, 64
    ld ($C0A4), a            ; lPadY
    ld a, 64
    ld ($C0A5), a            ; rPadY

    ; --- LCD on: LCDC=$93 (LCD on, tiledata $8000, OBJ on, BG on) ---
    ld a, $93
    ldh ($40), a

; -------------------- MAIN LOOP (F5 hot-reload zone) --------------------
.loop:
.waitvbl:
    ldh a, ($44)             ; LY
    cp $90                   ; 144 == VBlank start
    jr nz, .waitvbl

    ld a, $C0                ; OAM DMA from $C000
    ldh ($46), a

    call ReadInput
    call UpdateAI
    call UpdateBall
    call DrawSprites

.waitend:
    ldh a, ($44)
    cp $90
    jr z, .waitend
    jr .loop

; --- helper: copy BC bytes HL->DE ---
.CopyBC:
    ld a, (hl+)
    ld (de), a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .CopyBC
    ret

.BallTile:
    incbin "examples/ball.2bpp"
.PaddleTile:
    incbin "examples/paddle.2bpp"

; ====================== GAMEPLAY PROCEDURES ======================
; (full bodies added in Task 3; stubs here so the boot test links/draws)

ReadInput:
    ret

UpdateAI:
    ret

UpdateBall:
    ; one-time-stub: nudge ball so DrawSprites has something to show
    ret

DrawSprites:
    ; ball -> sprite 0
    ld a, ($C0A1)            ; ballY
    add a, 16
    ld ($C000), a
    ld a, ($C0A0)            ; ballX
    add a, 8
    ld ($C001), a
    ; left paddle -> sprites 1,2 (X=16)
    ld a, ($C0A4)            ; lPadY
    add a, 16
    ld ($C004), a           ; Lpad-top Y
    add a, 8
    ld ($C008), a           ; Lpad-bot Y (8 px below)
    ld a, 16
    ld ($C005), a           ; Lpad-top X
    ld ($C009), a           ; Lpad-bot X
    ; right paddle -> sprites 3,4 (X=152)
    ld a, ($C0A5)            ; rPadY
    add a, 16
    ld ($C00C), a
    add a, 8
    ld ($C010), a
    ld a, 152
    ld ($C00D), a
    ld ($C011), a
    ret
```

- [ ] **Step 5: Run the boot test to confirm it passes**

Run: `make build/test_example_pong && ./build/test_example_pong`
Expected: PASS — `r.ok` true and `ex_fb_nonblank` true (paddles + ball visible).

If `r.ok` is false, read the printed `examples/pong.asm:LINE: msg` diagnostics and fix syntax (compare against `examples/demo.asm`). If the screen is blank, confirm LCDC=`$93`, OBP0 set, sprite tiles 1/2 loaded at `$8010`/`$8020`, and DrawSprites ran (it needs the DMA in the loop).

- [ ] **Step 6: Commit**

```bash
git add examples/ball.2bpp examples/paddle.2bpp examples/pong.asm tests/test_example_pong.c
git commit -m "feat(examples): Pong init + boot (sprites, OAM DMA, screenshot test)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Pong — input, AI, ball physics (gameplay)

**Files:**
- Modify: `examples/pong.asm` (replace the `ReadInput` / `UpdateAI` / `UpdateBall` stubs)
- Modify: `tests/test_example_pong.c` (add smoke tests)

- [ ] **Step 1: Write failing smoke tests** — append to `tests/test_example_pong.c` and call them from `main()` before `TEST_MAIN_END()`.

```c
/* The ball moves on its own over a few frames. */
static void test_pong_ball_moves(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 3, 2000000);
    uint8_t x0 = gb_read8(gb, 0xC0A0), y0 = gb_read8(gb, 0xC0A1);
    ex_run(gb, 20, 6000000);
    uint8_t x1 = gb_read8(gb, 0xC0A0), y1 = gb_read8(gb, 0xC0A1);
    ASSERT_TRUE(x1 != x0 || y1 != y0);   /* it moved */
    gb_free(gb); asm_free(&r);
}

/* The ball stays on the playfield (never wraps off the top/bottom). */
static void test_pong_ball_in_bounds(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    for (int i = 0; i < 30; i++) {
        ex_run(gb, 5, 2000000);
        uint8_t y = gb_read8(gb, 0xC0A1);
        ASSERT_TRUE(y < 152);            /* 144 playfield + margin, no wrap to ~255 */
    }
    gb_free(gb); asm_free(&r);
}

/* Holding Up moves the left paddle up (lPadY decreases). */
static void test_pong_paddle_up(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t y0 = gb_read8(gb, 0xC0A4);
    gb_set_buttons(gb, 0x40);            /* bit6 = Up held */
    ex_run(gb, 20, 6000000);
    uint8_t y1 = gb_read8(gb, 0xC0A4);
    ASSERT_TRUE(y1 < y0);                /* paddle moved up */
    gb_free(gb); asm_free(&r);
}
```

Add to `main()`:

```c
    test_pong_ball_moves();
    test_pong_ball_in_bounds();
    test_pong_paddle_up();
```

- [ ] **Step 2: Run to confirm failure**

Run: `make build/test_example_pong && ./build/test_example_pong`
Expected: `test_pong_ball_moves` FAILS (stub `UpdateBall` doesn't move it) and `test_pong_paddle_up` FAILS (stub `ReadInput`).

- [ ] **Step 3: Replace the three stubs in `examples/pong.asm`**

```asm
ReadInput:
    ; select the direction keys
    ld a, $20
    ldh ($00), a
    ldh a, ($00)            ; ignore (let it settle)
    ldh a, ($00)
    ; bit2 = Up, bit3 = Down (0 = pressed)
    bit 2, a
    jr nz, .notUp
    ld a, ($C0A4)
    cp 8
    jr c, .notUp            ; clamp at top
    dec a
    dec a                   ; speed 2 px/frame
    ld ($C0A4), a
.notUp:
    ld a, $20
    ldh ($00), a
    ldh a, ($00)
    ldh a, ($00)
    bit 3, a
    jr nz, .notDown
    ld a, ($C0A4)
    cp 120
    jr nc, .notDown         ; clamp at bottom (144-24)
    inc a
    inc a
    ld ($C0A4), a
.notDown:
    ; deselect
    ld a, $30
    ldh ($00), a
    ret

UpdateAI:
    ; right paddle chases the ball's Y, 1 px/frame
    ld a, ($C0A1)           ; ballY
    ld b, a
    ld a, ($C0A5)           ; rPadY
    cp b
    jr z, .aiDone
    jr c, .aiDown
    dec a
    ld ($C0A5), a
    ret
.aiDown:
    inc a
    ld ($C0A5), a
.aiDone:
    ret

UpdateBall:
    ; --- X axis ---
    ld a, ($C0A2)           ; ballDX (signed: 1 or $FF)
    ld b, a
    ld a, ($C0A0)           ; ballX
    add a, b
    ld ($C0A0), a
    ; bounce off left paddle zone (ballX <= 24) -> DX = +1
    cp 24
    jr nc, .checkRight
    ld a, 1
    ld ($C0A2), a
    jr .yaxis
.checkRight:
    ; bounce off right paddle zone (ballX >= 144) -> DX = -1 ($FF)
    cp 144
    jr c, .yaxis
    ld a, $FF
    ld ($C0A2), a
.yaxis:
    ; --- Y axis ---
    ld a, ($C0A3)           ; ballDY
    ld b, a
    ld a, ($C0A1)           ; ballY
    add a, b
    ld ($C0A1), a
    ; bounce off top (ballY <= 8) -> DY = +1
    cp 8
    jr nc, .checkBottom
    ld a, 1
    ld ($C0A3), a
    ret
.checkBottom:
    ; bounce off bottom (ballY >= 136) -> DY = -1
    cp 136
    ret c
    ld a, $FF
    ld ($C0A3), a
    ret
```

- [ ] **Step 4: Run the tests to confirm they pass**

Run: `make build/test_example_pong && ./build/test_example_pong`
Expected: PASS (all boot + smoke tests). If `test_pong_ball_in_bounds` fails, recheck the `cp 8` / `cp 136` thresholds and the signed `$FF` reflect values. Use `superpowers:systematic-debugging` if a test misbehaves.

- [ ] **Step 5: Visual check (optional but recommended)**

Run: `./live-gameboy-ide --ide-shot examples/pong.asm build/pong.png 60` then open `build/pong.png` — confirm ball + two paddles render on a clear court.

- [ ] **Step 6: Commit**

```bash
git add examples/pong.asm tests/test_example_pong.c
git commit -m "feat(examples): Pong gameplay — input, AI paddle, ball physics

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Snake — assets, init, and boot screenshot

**Files:**
- Create: `examples/snakebody.2bpp`, `examples/food.2bpp`, `examples/snake.asm`
- Create: `tests/test_example_snake.c`

**Grid:** 20×18 tiles (160×144). Tilemap cell for (x,y) = `$9800 + y*32 + x`. **WRAM map (Snake):** `$C000`=headX, `$C001`=headY, `$C002`=dir (0=right,1=left,2=up,3=down), `$C003`=length (lo), `$C004`=tick counter, `$C005`=foodX, `$C006`=foodY, `$C007`=rng. Body cell history: `bodyX[]` at `$C100+i`, `bodyY[]` at `$C140+i` (ring buffer, max 64), `$C008`=head index, `$C009`=tail index. Tiles: 0=blank, 1=body (`snakebody.2bpp`), 2=food (`food.2bpp`).

- [ ] **Step 1: Create assets** (solid body tile; food tile = a small centered block via a simple pattern).

```bash
printf '\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377' > examples/snakebody.2bpp
# food: rows 0,7 blank; middle rows = 0x3C (center 4 px set) on both planes
printf '\000\000\074\074\074\074\074\074\074\074\074\074\074\074\000\000' > examples/food.2bpp
wc -c examples/snakebody.2bpp examples/food.2bpp   # each 16
```

- [ ] **Step 2: Write the failing boot test** in `tests/test_example_snake.c`

```c
#include "test.h"
#include "example_run.h"

static void test_snake_boots(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }
    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);
    ASSERT_TRUE(ex_fb_nonblank(gb));   /* snake + food drawn into the tilemap */
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_snake_boots();
    TEST_MAIN_END();
}
```

- [ ] **Step 3: Run to confirm failure**

Run: `make build/test_example_snake && ./build/test_example_snake`
Expected: FAIL — `examples/snake.asm` missing.

- [ ] **Step 4: Write `examples/snake.asm`**

```asm
; snake.asm — Snake for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/snake.asm
; 2. HOT-RELOAD THE RULE (F5): StepSnake holds the movement/step rate.
;    Change ".STEP_RATE" (the "cp 16" throttle in the main loop) to speed
;    up or slow down, press F5 — the snake keeps its current length/body.
; 3. RESKIN (paint): paint VRAM tile 1 (body) or tile 2 (food) in the TILE
;    editor — every body/food cell updates live (snakebody/food .2bpp).
; 4. GROW BY HAND (memory edit): bump the length byte at $C003 in the
;    MEMORY panel.  Use F8 if you edit init code.
;
; Controls: D-pad steers. Walls wrap (edit StepSnake to make them deadly).

SECTION "code", ROM0

Main:
    ld sp, $FFFE
    xor a
    ldh ($40), a             ; LCD off

    ; tile 1 = body @ $8010, tile 2 = food @ $8020
    ld hl, .BodyTile
    ld de, $8010
    ld bc, $0010
    call .CopyBC
    ld hl, .FoodTile
    ld de, $8020
    ld bc, $0010
    call .CopyBC

    ; clear tilemap to blank tile 0
    ld hl, $9800
    ld bc, $0400
.clrmap:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clrmap

    ld a, $E4
    ldh ($47), a             ; BGP

    ; init vars: head at (10,9), moving right, length 3
    ld a, 10
    ld ($C000), a            ; headX
    ld a, 9
    ld ($C001), a            ; headY
    xor a
    ld ($C002), a            ; dir = 0 (right)
    ld a, 3
    ld ($C003), a            ; length
    xor a
    ld ($C004), a            ; tick
    ld ($C008), a            ; head idx
    ld ($C009), a            ; tail idx
    ld a, $13
    ld ($C007), a            ; rng seed (nonzero)

    ; food at (15,9)
    ld a, 15
    ld ($C005), a
    ld a, 9
    ld ($C006), a

    ; draw initial head + food so the boot screen is non-blank
    call DrawHead
    call DrawFood

    ld a, $91                ; LCD on, tiledata $8000, BG on (no sprites)
    ldh ($40), a

.loop:
.waitvbl:
    ldh a, ($44)
    cp $90
    jr nz, .waitvbl

    call ReadInput

    ; --- throttle: only step every STEP_RATE frames ---
    ld a, ($C004)
    inc a
    ld ($C004), a
    cp 16                    ; .STEP_RATE  (edit + F5 to change speed)
    jr c, .skipstep
    xor a
    ld ($C004), a
    call StepSnake
.skipstep:

.waitend:
    ldh a, ($44)
    cp $90
    jr z, .waitend
    jr .loop

; ---- HL = tilemap addr of (headX,headY): $9800 + headY*32 + headX ----
.HeadAddr:
    ld a, ($C001)           ; headY
    ld l, a
    ld h, 0
    add hl, hl              ; *2
    add hl, hl              ; *4
    add hl, hl              ; *8
    add hl, hl              ; *16
    add hl, hl              ; *32
    ld a, ($C000)           ; headX
    ld c, a
    ld b, 0
    add hl, bc
    ld bc, $9800
    add hl, bc
    ret

DrawHead:
    call .HeadAddr
    ld a, 1                 ; body tile
    ld (hl), a
    ret

DrawFood:
    ld a, ($C006)           ; foodY
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    ld a, ($C005)           ; foodX
    ld c, a
    ld b, 0
    add hl, bc
    ld bc, $9800
    add hl, bc
    ld a, 2                 ; food tile
    ld (hl), a
    ret

.CopyBC:
    ld a, (hl+)
    ld (de), a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .CopyBC
    ret

.BodyTile:
    incbin "examples/snakebody.2bpp"
.FoodTile:
    incbin "examples/food.2bpp"

; ====================== GAMEPLAY (stubs, filled in Task 5) ======================
ReadInput:
    ret
StepSnake:
    ret
```

- [ ] **Step 5: Run the boot test to confirm it passes**

Run: `make build/test_example_snake && ./build/test_example_snake`
Expected: PASS — assembles, head + food tiles drawn → non-blank. Fix any `snake.asm:LINE` diagnostics against `demo.asm` idiom (watch `add hl, hl` and `add hl, bc` are supported — they are used here intentionally; if the assembler rejects `add hl, bc`, substitute five `add hl, hl` only and add `headX` via an 8-bit `add a,l / ld l,a / adc`-style sequence — but verify first).

- [ ] **Step 6: Commit**

```bash
git add examples/snakebody.2bpp examples/food.2bpp examples/snake.asm tests/test_example_snake.c
git commit -m "feat(examples): Snake init + boot (tilemap addressing, screenshot test)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Snake — input + movement (gameplay)

**Files:**
- Modify: `examples/snake.asm` (replace `ReadInput` / `StepSnake` stubs; the ring-buffer tail erase keeps the body a fixed length unless food is eaten)
- Modify: `tests/test_example_snake.c`

- [ ] **Step 1: Write failing smoke tests** — append and call from `main()`.

```c
/* The head advances on its own (default dir = right => headX increases). */
static void test_snake_moves(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    uint8_t x0 = 0;
    ex_run(gb, 5, 2000000);
    x0 = gb_read8(gb, 0xC000);
    ex_run(gb, 60, 8000000);            /* > STEP_RATE frames */
    uint8_t x1 = gb_read8(gb, 0xC000);
    ASSERT_TRUE(x1 != x0);              /* head moved */
    gb_free(gb); asm_free(&r);
}

/* Pressing Down changes heading so headY increases over time. */
static void test_snake_turns_down(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t y0 = gb_read8(gb, 0xC001);
    gb_set_buttons(gb, 0x80);           /* bit7 = Down */
    ex_run(gb, 80, 9000000);
    uint8_t y1 = gb_read8(gb, 0xC001);
    ASSERT_TRUE(y1 > y0);
    gb_free(gb); asm_free(&r);
}
```

- [ ] **Step 2: Run to confirm failure**

Run: `make build/test_example_snake && ./build/test_example_snake`
Expected: FAIL (`test_snake_moves`, `test_snake_turns_down`) — stubs do nothing.

- [ ] **Step 3: Replace the stubs in `examples/snake.asm`**

```asm
ReadInput:
    ld a, $20                ; select directions
    ldh ($00), a
    ldh a, ($00)
    ldh a, ($00)
    ; bit0 Right,1 Left,2 Up,3 Down (0 = pressed)
    bit 0, a
    jr nz, .nr
    xor a                    ; dir 0 = right
    ld ($C002), a
.nr:
    bit 1, a
    jr nz, .nl
    ld a, 1
    ld ($C002), a
.nl:
    bit 2, a
    jr nz, .nu
    ld a, 2
    ld ($C002), a
.nu:
    bit 3, a
    jr nz, .nd
    ld a, 3
    ld ($C002), a
.nd:
    ld a, $30
    ldh ($00), a
    ret

StepSnake:
    ; erase current head cell? No — we leave a trail then erase the tail.
    ; For a simple fixed-length snake: erase the OLD head position's "oldest"
    ; cell. Minimal version: erase current cell only if not growing.
    ; 1) compute next head from dir
    ld a, ($C000)           ; headX
    ld b, a
    ld a, ($C001)           ; headY
    ld c, a
    ld a, ($C002)           ; dir
    cp 0
    jr nz, .notR
    inc b
    jr .applied
.notR:
    cp 1
    jr nz, .notL
    dec b
    jr .applied
.notL:
    cp 2
    jr nz, .notU
    dec c
    jr .applied
.notU:
    inc c                   ; dir 3 = down
.applied:
    ; 2) wrap walls (0..19 X, 0..17 Y)
    ld a, b
    cp 20
    jr c, .xok
    ; wrapped past 19 -> 0, or below 0 ($FF) -> 19
    cp 200                  ; >=200 means it was $FF (underflow)
    jr c, .xhi
    ld b, 19
    jr .xok
.xhi:
    ld b, 0
.xok:
    ld a, c
    cp 18
    jr c, .yok
    cp 200
    jr c, .yhi
    ld c, 18 - 1
    jr .yok
.yhi:
    ld c, 0
.yok:
    ; 3) erase the OLD head cell (simple trail-of-1 behaviour: keeps motion
    ;    visible without a full ring buffer). For longer bodies, extend with
    ;    the bodyX/bodyY ring buffer described in the WRAM map.
    call .HeadAddr
    xor a
    ld (hl), a
    ; 4) commit new head
    ld a, b
    ld ($C000), a
    ld a, c
    ld ($C001), a
    ; 5) eat food?
    ld a, ($C005)
    cp b
    jr nz, .noFood
    ld a, ($C006)
    cp c
    jr nz, .noFood
    ld a, ($C003)
    inc a
    ld ($C003), a           ; grow (length byte; visible in MEMORY panel)
    call .NewFood
.noFood:
    call DrawHead
    ret

; cheap LFSR-ish RNG -> A in 0..(reg). Uses $C007 seed.
.NewFood:
    ld a, ($C007)
    add a, a
    add a, 5
    xor 173
    ld ($C007), a
    ; foodX = rng mod 20 (subtract 20 until < 20)
    and $1F
.modx:
    cp 20
    jr c, .xdone
    sub 20
    jr .modx
.xdone:
    ld ($C005), a
    ld a, ($C007)
    add a, 7
    ld ($C007), a
    and $1F
.mody:
    cp 18
    jr c, .ydone
    sub 18
    jr .mody
.ydone:
    ld ($C006), a
    call DrawFood
    ret
```

- [ ] **Step 4: Run the tests to confirm they pass**

Run: `make build/test_example_snake && ./build/test_example_snake`
Expected: PASS. If `add hl, hl`/`add hl, bc` (used by `.HeadAddr`) or `sub 20` are rejected by the assembler, that surfaces here — check `src/asm/encode.c` for the supported `ADD HL,rr` / `SUB n` forms and adjust. Use `superpowers:systematic-debugging` for any logic failure.

- [ ] **Step 5: Commit**

```bash
git add examples/snake.asm tests/test_example_snake.c
git commit -m "feat(examples): Snake gameplay — steering, stepping, food/growth

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Breakout — assets, init, and boot screenshot

**Files:**
- Create: `examples/brick.2bpp`, `examples/breakout.asm` (reuses `examples/ball.2bpp` and `examples/paddle.2bpp` from Task 2)
- Create: `tests/test_example_breakout.c`

**WRAM map (Breakout):** `$C000-$C09F` = OAM shadow (0=ball, 1=paddle-left, 2=paddle-right). Vars: `$C0A0`=ballX, `$C0A1`=ballY, `$C0A2`=ballDX, `$C0A3`=ballDY, `$C0A4`=padX. Bricks live in the BG tilemap rows 2..5 as tile 3.

- [ ] **Step 1: Create the brick asset**

```bash
# brick: solid with a 1px blank border row top/bottom for a "brick" look
printf '\000\000\377\377\377\377\377\377\377\377\377\377\377\377\000\000' > examples/brick.2bpp
wc -c examples/brick.2bpp    # 16
```

- [ ] **Step 2: Write the failing boot test** in `tests/test_example_breakout.c`

```c
#include "test.h"
#include "example_run.h"

static void test_breakout_boots(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }
    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);
    ASSERT_TRUE(ex_fb_nonblank(gb));   /* bricks + paddle + ball */
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_breakout_boots();
    TEST_MAIN_END();
}
```

- [ ] **Step 3: Run to confirm failure**

Run: `make build/test_example_breakout && ./build/test_example_breakout`
Expected: FAIL — `breakout.asm` missing.

- [ ] **Step 4: Write `examples/breakout.asm`**

```asm
; breakout.asm — Breakout for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/breakout.asm
; 2. EDIT THE LEVEL LIVE: the brick field IS the BG tilemap. Open the
;    BG MAP panel (or paint VRAM tile 3) — knock out a brick by hand, or
;    repaint tile 3 (brick.2bpp) to reskin every brick at once.
; 3. HOT-RELOAD PHYSICS (F5): UpdateBall holds the bounce math; edit the
;    reflect values / speed and press F5 mid-game.
; 4. F8 (soft reset) re-runs InitBricks to refill the field.
;
; Controls: Left/Right move the paddle.

SECTION "code", ROM0

Main:
    ld sp, $FFFE
    xor a
    ldh ($40), a             ; LCD off

    ; tiles: 1=ball @ $8010, 2=paddle @ $8020, 3=brick @ $8030
    ld hl, .BallTile
    ld de, $8010
    ld bc, $0010
    call .CopyBC
    ld hl, .PaddleTile
    ld de, $8020
    ld bc, $0010
    call .CopyBC
    ld hl, .BrickTile
    ld de, $8030
    ld bc, $0010
    call .CopyBC

    ; clear tilemap
    ld hl, $9800
    ld bc, $0400
.clrmap:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clrmap

    ld a, $E4
    ldh ($47), a             ; BGP
    ldh ($48), a             ; OBP0

    ; zero OAM shadow
    ld hl, $C000
    ld bc, $00A0
.clroam:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clroam

    ; sprite tiles: ball=1, paddle halves=2
    ld a, 1
    ld ($C002), a
    ld a, 2
    ld ($C006), a
    ld ($C00A), a

    ; vars
    ld a, 80
    ld ($C0A0), a            ; ballX
    ld a, 100
    ld ($C0A1), a            ; ballY
    ld a, 1
    ld ($C0A2), a            ; ballDX
    ld a, $FF
    ld ($C0A3), a            ; ballDY = -1 (upward)
    ld a, 72
    ld ($C0A4), a            ; padX

    call InitBricks

    ld a, $93                ; LCD on, OBJ on, BG on
    ldh ($40), a

.loop:
.waitvbl:
    ldh a, ($44)
    cp $90
    jr nz, .waitvbl

    ld a, $C0
    ldh ($46), a             ; OAM DMA

    call ReadInput
    call UpdateBall
    call DrawSprites

.waitend:
    ldh a, ($44)
    cp $90
    jr z, .waitend
    jr .loop

; --- fill tilemap rows 2..5 with brick tile (tile 3) ---
InitBricks:
    ld hl, $9840             ; $9800 + 2*32  (row 2)
    ld c, 4                  ; 4 rows
.row:
    ld b, 20                 ; 20 columns visible
.col:
    ld a, 3
    ld (hl+), a
    dec b
    jr nz, .col
    ; advance HL to next row start: 32 - 20 = 12 more
    ld de, 12
    add hl, de
    dec c
    jr nz, .row
    ret

DrawSprites:
    ld a, ($C0A1)
    add a, 16
    ld ($C000), a           ; ball Y
    ld a, ($C0A0)
    add a, 8
    ld ($C001), a           ; ball X
    ; paddle at bottom (Y=144 area -> screen Y 136 => OAM 152)
    ld a, 152
    ld ($C004), a
    ld ($C008), a
    ld a, ($C0A4)
    add a, 8
    ld ($C005), a           ; paddle-left X
    add a, 8
    ld ($C009), a           ; paddle-right X (8 px right)
    ret

.CopyBC:
    ld a, (hl+)
    ld (de), a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .CopyBC
    ret

.BallTile:
    incbin "examples/ball.2bpp"
.PaddleTile:
    incbin "examples/paddle.2bpp"
.BrickTile:
    incbin "examples/brick.2bpp"

; ====================== GAMEPLAY (stubs, filled in Task 7) ======================
ReadInput:
    ret
UpdateBall:
    ret
```

- [ ] **Step 5: Run the boot test to confirm it passes**

Run: `make build/test_example_breakout && ./build/test_example_breakout`
Expected: PASS — bricks + paddle + ball drawn. Fix diagnostics against the `demo.asm` idiom; confirm `add hl, de` is accepted (it is used in `InitBricks`; if not, replace with twelve `inc hl`).

- [ ] **Step 6: Commit**

```bash
git add examples/brick.2bpp examples/breakout.asm tests/test_example_breakout.c
git commit -m "feat(examples): Breakout init + boot (brick tilemap, screenshot test)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Breakout — input, ball physics, brick collision (gameplay)

**Files:**
- Modify: `examples/breakout.asm` (replace `ReadInput` / `UpdateBall` stubs; add `.BrickAt` helper that maps ballX/ballY to a tilemap cell and clears it on hit)
- Modify: `tests/test_example_breakout.c`

- [ ] **Step 1: Write failing smoke tests** — append and call from `main()`.

```c
/* The ball moves. */
static void test_breakout_ball_moves(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 3, 2000000);
    uint8_t y0 = gb_read8(gb, 0xC0A1);
    ex_run(gb, 20, 6000000);
    uint8_t y1 = gb_read8(gb, 0xC0A1);
    ASSERT_TRUE(y1 != y0);
    gb_free(gb); asm_free(&r);
}

/* At least one brick cell ($9840 row) gets cleared as the ball travels up. */
static void test_breakout_clears_brick(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    /* count brick tiles (==3) in rows 2..5 at start */
    ex_run(gb, 5, 2000000);
    int before = 0;
    for (int row = 2; row <= 5; row++)
        for (int col = 0; col < 20; col++)
            if (gb_read8(gb, 0x9800 + row*32 + col) == 3) before++;
    ASSERT_TRUE(before > 0);
    ex_run(gb, 240, 30000000);          /* let the ball reach the bricks */
    int after = 0;
    for (int row = 2; row <= 5; row++)
        for (int col = 0; col < 20; col++)
            if (gb_read8(gb, 0x9800 + row*32 + col) == 3) after++;
    ASSERT_TRUE(after < before);        /* a brick was knocked out */
    gb_free(gb); asm_free(&r);
}

/* Holding Left moves the paddle left. */
static void test_breakout_paddle_left(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t x0 = gb_read8(gb, 0xC0A4);
    gb_set_buttons(gb, 0x20);           /* bit5 = Left */
    ex_run(gb, 20, 6000000);
    uint8_t x1 = gb_read8(gb, 0xC0A4);
    ASSERT_TRUE(x1 < x0);
    gb_free(gb); asm_free(&r);
}
```

- [ ] **Step 2: Run to confirm failure**

Run: `make build/test_example_breakout && ./build/test_example_breakout`
Expected: FAIL on the three new tests (stubs).

- [ ] **Step 3: Replace the stubs in `examples/breakout.asm`**

```asm
ReadInput:
    ld a, $20                ; directions
    ldh ($00), a
    ldh a, ($00)
    ldh a, ($00)
    bit 1, a                 ; Left
    jr nz, .nl
    ld a, ($C0A4)
    cp 2
    jr c, .nl
    dec a
    dec a
    ld ($C0A4), a
.nl:
    ld a, $20
    ldh ($00), a
    ldh a, ($00)
    ldh a, ($00)
    bit 0, a                 ; Right
    jr nz, .nr
    ld a, ($C0A4)
    cp 144
    jr nc, .nr
    inc a
    inc a
    ld ($C0A4), a
.nr:
    ld a, $30
    ldh ($00), a
    ret

UpdateBall:
    ; X axis
    ld a, ($C0A2)
    ld b, a
    ld a, ($C0A0)
    add a, b
    ld ($C0A0), a
    cp 8
    jr nc, .xhi
    ld a, 1
    ld ($C0A2), a
    jr .yax
.xhi:
    cp 152
    jr c, .yax
    ld a, $FF
    ld ($C0A2), a
.yax:
    ; Y axis
    ld a, ($C0A3)
    ld b, a
    ld a, ($C0A1)
    add a, b
    ld ($C0A1), a
    cp 8
    jr nc, .ylo
    ld a, 1
    ld ($C0A3), a
    jr .brick
.ylo:
    cp 140
    jr c, .brick
    ld a, $FF
    ld ($C0A3), a
.brick:
    call .BrickHit
    ret

; map ball (pixel) -> tilemap cell; if it's a brick (tile 3), clear it
; and reflect DY. col = ballX/8, row = ballY/8.
.BrickHit:
    ld a, ($C0A1)           ; ballY
    srl a
    srl a
    srl a                   ; /8 -> row
    ld d, a                 ; row
    ld a, ($C0A0)           ; ballX
    srl a
    srl a
    srl a                   ; /8 -> col
    ld e, a                 ; col
    ; HL = $9800 + row*32 + col
    ld a, d
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl              ; *32
    ld a, e
    ld c, a
    ld b, 0
    add hl, bc
    ld bc, $9800
    add hl, bc
    ld a, (hl)
    cp 3                    ; brick?
    ret nz
    xor a
    ld (hl), a             ; clear brick cell (live tilemap edit by the game)
    ld a, $FF
    ; reflect: if going up ($FF) bounce down (+1), else up
    ld a, ($C0A3)
    cp 1
    jr z, .setUp
    ld a, 1
    ld ($C0A3), a
    ret
.setUp:
    ld a, $FF
    ld ($C0A3), a
    ret
```

- [ ] **Step 4: Run the tests to confirm they pass**

Run: `make build/test_example_breakout && ./build/test_example_breakout`
Expected: PASS. `test_breakout_clears_brick` needs the ball to physically reach the brick rows — if it never does within 240 frames, widen the frame budget or adjust the start position/velocity. Confirm `srl a` is supported (it is part of the CB-prefix set covered by blargg cpu_instrs). Use `superpowers:systematic-debugging` if collision misbehaves.

- [ ] **Step 5: Commit**

```bash
git add examples/breakout.asm tests/test_example_breakout.c
git commit -m "feat(examples): Breakout gameplay — paddle, physics, brick collision

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Makefile + README integration

**Files:**
- Modify: `Makefile`
- Modify: `README.md`

- [ ] **Step 1: Add screenshot + standalone-ROM targets to the `Makefile`**

After the existing `ide-shot` target (`Makefile:96`), add:

```make
# Example games — headless IDE screenshots (CI-friendly, no window)
pong-shot: live-gameboy-ide
	./live-gameboy-ide --ide-shot examples/pong.asm build/pong.png 60

snake-shot: live-gameboy-ide
	./live-gameboy-ide --ide-shot examples/snake.asm build/snake.png 60

breakout-shot: live-gameboy-ide
	./live-gameboy-ide --ide-shot examples/breakout.asm build/breakout.png 60

# Build each example as a standalone .gb (relies on the Task 0 entry patch)
examples: gbasm
	./gbasm examples/pong.asm     -o build/pong.gb
	./gbasm examples/snake.asm    -o build/snake.gb
	./gbasm examples/breakout.asm -o build/breakout.gb
	@echo "examples: OK (build/{pong,snake,breakout}.gb)"
```

And extend the `.PHONY` line (`Makefile:102`) to include the new targets:

```make
.PHONY: all test blargg roms clean acid2 shell-shot sound gbasm asm-demo live-gameboy-ide ide-shot pong-shot snake-shot breakout-shot examples
```

- [ ] **Step 2: Verify the targets work**

Run:
```bash
make examples
make pong-shot snake-shot breakout-shot
```
Expected: `build/{pong,snake,breakout}.gb` produced; `build/{pong,snake,breakout}.png` written with no errors. Spot-check one PNG visually.

- [ ] **Step 3: Document the gallery in `README.md`**

In the IDE section, after the live-edit workflow paragraph, add an "Example games" subsection listing the three games and their signature live-edit hooks, and **correct the entry-point note** in the Assemble section. Replace the sentence claiming the assembler always auto-patches with the accurate version:

> The assembler writes a `JP Main` at the entry point `$0100` whenever the source
> defines a `Main:` global in ROM0, so `./gbasm game.asm -o game.gb` followed by
> `./live-gameboy game.gb` boots straight into your code. (The IDE/live path does
> the same patch.)

Add:

```markdown
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
```

- [ ] **Step 4: Full test sweep**

Run: `make test`
Expected: all suites pass, including `test_example_pong`, `test_example_snake`, `test_example_breakout`, and the updated `test_asm_e2e`.

- [ ] **Step 5: Commit**

```bash
git add Makefile README.md
git commit -m "build,docs: wire example games into Makefile + README, fix entry-point note

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review (completed during planning)

**Spec coverage:**
- Pong / Snake / Breakout, each a self-contained file with `TRY THIS LIVE` recipe → Tasks 2–7, recipes in each `.asm` header.
- Common Main(init)/loop(procs) structure for F5/F8 → every game file.
- Signature hooks: Pong physics F5, Snake rule F5, Breakout brick-field/tile edit → recipes + isolated `UpdateBall`/`StepSnake`/brick tilemap.
- Paintable `incbin` `.2bpp` assets → ball/paddle/snakebody/food/brick assets.
- Input via `$FF00` select-then-read; OAM via DMA from `$C0xx` shadow; VRAM writes safe (LCD off in init, VBlank in loop) → all game files.
- Testing: per-game boot screenshot + smoke logic asserting state bytes → Tasks 2–7; CI screenshot path + standalone build → Task 8.
- Spec "open considerations": Snake RNG = cheap LFSR (`.NewFood`); trivial AI paddle (Pong `UpdateAI`); implementation order Pong→Snake→Breakout (Tasks 2→7). Resolved.
- **Added beyond spec:** Task 0 (CLI entry-point fix) — approved by the user during planning so the games also run as standalone ROMs and the README becomes accurate.

**Placeholder scan:** No TODO/TBD. The stub procedures in Tasks 2/4/6 are intentional, named, and each is fully replaced with real code in the next task (3/5/7) — not left as placeholders.

**Type/name consistency:** WRAM addresses are fixed per game and reused consistently across asm and tests (`$C0A0` ballX etc. in Pong; `$C000` headX etc. in Snake; `$C0A4` padX in Breakout). Test helper names (`ex_assemble`, `ex_run`, `ex_fb_nonblank`) match `tests/example_run.h`. Button masks match `src/gb/joypad.c`.

**Risk note for the executor:** The exact SM83 forms `add hl, bc` / `add hl, de` / `srl a` / `sub n` / `bit n, a` are used by the games. They are all standard SM83 and covered by the blargg `cpu_instrs` suite the assembler passes, but if the *assembler's parser* rejects any specific form, that will surface immediately at the boot test (`r.ok` false with a line diagnostic) — the relevant task notes a concrete fallback. Treat any assembler-syntax gap as a real finding and check `src/asm/encode.c`/`parser.c` rather than guessing.
