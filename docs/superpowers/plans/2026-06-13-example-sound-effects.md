# Example-Game Sound Effects Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add tweakable, event-driven sound effects to the three example games (Pong, Snake, Breakout) using the DMG APU, each effect a live-coding (F5) hook.

**Architecture:** Each game powers on the APU once in `Main` init, then triggers short SFX from its gameplay procedures via tiny per-game helper routines (`SfxTone` on CH1 pulse, `SfxNoise` on CH4 noise). Pitch/character constants sit at the call sites (the F5-reloadable main-loop zone). Verification is headless: poll NR52 (`$FF26`) channel-status bits during scripted gameplay and assert the expected channel fires on the event.

**Tech Stack:** `gbasm` SM83 assembler, DMG APU (`src/gb/apu.c`, fully implemented), C tests via `tests/example_run.h` + `tests/test.h`.

---

## Background facts (verified against the codebase)

- **APU registers** (write via `ldh ($nn), a` where the address is `$FF00+nn`):
  - CH1 pulse+sweep: `NR10=$FF10`, `NR11=$FF11`, `NR12=$FF12`, `NR13=$FF13`, `NR14=$FF14`. NR52 status **bit0** (`$01`).
  - CH4 noise: `NR41=$FF20`, `NR42=$FF21`, `NR43=$FF22`, `NR44=$FF23`. NR52 status **bit3** (`$08`).
  - Control: `NR50=$FF24` (master vol), `NR51=$FF25` (panning), `NR52=$FF26` (bit7=power).
- **Power ordering:** writes to `NR10`–`NR51` are ignored while the APU is off, so `NR52=$80` must be written **first**, then `NR50`/`NR51`.
- **Trigger:** writing `NR14`/`NR44` with bit7 set triggers the channel and synchronously sets its NR52 status bit. With bit6 (length-enable) set and a length loaded (`NR11`/`NR41` low bits), the length counter (256 Hz) auto-stops the effect. Envelope (`NR12`/`NR42`, bits7-4 vol, bit3 dir, bits2-0 period) must have bits7-3 non-zero (DAC on) or the channel won't enable. (All confirmed in `tests/test_apu.c`.)
- **NR52 read:** after power-on, `gb_read8(0xFF26)` reads `0xF0 | <active channel bits>` (bit7 power, bits4-6 read as 1). So `& 0x80` checks power; `& 0x01` checks CH1 active; `& 0x08` checks CH4 active. bit2 (CH3) and these are the meaningful bits.
- **`gb_tick` advances the APU** (`src/gb/gb.c` `gb_tick` calls `gb_apu_tick`), and the example harness `ex_run` uses `gb_step`+`gb_tick`+`gb_ppu_tick`, so SFX triggers and length counters work in tests with no harness change.
- **Assembler forms** used here are all proven: `ldh ($nn), a`, `ld de, $nnnn`, `ld b, $nn`, `or $C0` (OR A,n = opcode `0xF6`, standard SM83 in blargg cpu_instrs), `call`/`ret`. If `or $C0` were rejected, fall back to passing the pre-OR'd NR14 byte — but verify first; it should assemble.
- **Frequency note:** CH1 tone ≈ `131072 / (2048 - freq11)` Hz, where `freq11` (0..2047) is `NR13` (low 8) + `NR14` bits2-0 (high 3). `ld de, $0NNN` loads D=high byte (bits2-0 usable), E=low byte. The exact values below are starting points (they ARE the live-edit surface).

## Helper-routine contracts (used across tasks)

- **`SfxTone`** (global label): input `D`=freq hi (bits2-0), `E`=freq lo. Clobbers `A`,`D`,`E`. Preserves `B`,`C`,`H`,`L`. Plays a short CH1 blip.
- **`SfxNoise`** (global label): input `B`=`NR43` value (noise pitch). Clobbers `A`,`B`. Preserves `C`,`D`,`E`,`H`,`L`. Plays a short CH4 burst.
- **`SfxWrap`** (Snake only, global): no input; loads its own freq, `call`s `SfxTone`. Clobbers `A`,`D`,`E`.
- **`SfxWall`** (Breakout only, global): no input; loads its own freq, `call`s `SfxTone`. Clobbers `A`,`D`,`E`.

These are placed at the END of each game's file as new global labels (after the existing gameplay procedures), so they don't disturb existing local-label scopes.

## File Structure

- Modify `tests/example_run.h` — add one shared helper `ex_run_watch_nr52` (Task 1).
- Modify `examples/pong.asm` + `tests/test_example_pong.c` (Task 1).
- Modify `examples/snake.asm` + `tests/test_example_snake.c` (Task 2).
- Modify `examples/breakout.asm` + `tests/test_example_breakout.c` (Task 3).
- Modify `README.md` (Task 4).

---

## Task 1: Pong sound effects (+ shared NR52 watch helper)

**Files:** Modify `tests/example_run.h`, `examples/pong.asm`, `tests/test_example_pong.c`.

- [ ] **Step 1: Add the shared NR52 watch helper to `tests/example_run.h`**

Insert this function just before the final `#endif` line:

```c
/* Run up to `frames` VBlanks (or `max_steps`), sampling NR52 ($FF26) every
 * step and OR-ing it into the result. Returned value's bit0=CH1, bit1=CH2,
 * bit3=CH4 indicate a channel was active at some point during the run. */
static uint8_t ex_run_watch_nr52(GB *gb, int frames, int max_steps) {
    uint8_t seen = 0;
    int f = 0, s = 0;
    while (f < frames && s < max_steps) {
        int tc = gb_step(gb);
        gb_tick(gb, tc);
        gb_ppu_tick(gb, tc);
        seen |= gb_read8(gb, 0xFF26);
        if (gb->frame_ready) { gb->frame_ready = false; f++; }
        s++;
    }
    return seen;
}
```

- [ ] **Step 2: Write the failing sound tests in `tests/test_example_pong.c`**

Add these two functions and call them from `main()` (after the existing calls, before `TEST_MAIN_END()`):

```c
/* APU is powered on by init. */
static void test_pong_apu_on(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);   /* NR52 power bit */
    gb_free(gb); asm_free(&r);
}

/* A CH1 blip fires during a rally (paddle/wall/score bounces). */
static void test_pong_sfx_fires(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    uint8_t seen = ex_run_watch_nr52(gb, 250, 9000000);
    ASSERT_TRUE(seen & 0x01);                    /* CH1 was active */
    gb_free(gb); asm_free(&r);
}
```
Add to `main()`:
```c
    test_pong_apu_on();
    test_pong_sfx_fires();
```

- [ ] **Step 3: Run the tests to confirm they fail**

Run: `cd /Users/wjbr/src/live-gameboy.ai && make build/test_example_pong && ./build/test_example_pong`
Expected: `test_pong_apu_on` FAILS (NR52 power bit unset — no power-on yet) and `test_pong_sfx_fires` FAILS (no CH1 activity).

- [ ] **Step 4: Add APU power-on to Pong init**

In `examples/pong.asm`, insert the power-on block immediately before the `; --- LCD on:` comment (currently around line 86). Replace:
```asm
    ; --- LCD on: LCDC=$93 (LCD on, tiledata $8000, OBJ on, BG on) ---
    ld a, $93
    ldh ($40), a
```
with:
```asm
    ; --- Sound: power on APU, full volume, route all channels both sides ---
    ld a, $80
    ldh ($26), a             ; NR52 = APU on (write FIRST)
    ld a, $77
    ldh ($24), a             ; NR50 = master volume L/R
    ld a, $FF
    ldh ($25), a             ; NR51 = all channels to both speakers

    ; --- LCD on: LCDC=$93 (LCD on, tiledata $8000, OBJ on, BG on) ---
    ld a, $93
    ldh ($40), a
```

- [ ] **Step 5: Add the `SfxTone` helper at the end of `examples/pong.asm`**

Append after the last line (after `DrawSprites`'s `ret`):
```asm

; ====================== SOUND ======================
; SfxTone — short CH1 (pulse) blip. In: D=freq hi (bits2-0), E=freq lo.
; Clobbers A,D,E; preserves B,C,HL. The freq is the live-edit surface.
SfxTone:
    xor a
    ldh ($10), a             ; NR10 = no sweep
    ld a, $A0
    ldh ($11), a             ; NR11 = duty 50% + length (short)
    ld a, $F2
    ldh ($12), a             ; NR12 = vol 15, decay (DAC on)
    ld a, e
    ldh ($13), a             ; NR13 = freq lo
    ld a, d
    or $C0
    ldh ($14), a             ; NR14 = trigger + length-enable + freq hi
    ret
```

- [ ] **Step 6: Wire the three Pong events to sounds**

(a) **Paddle hit** (left zone) and **score cue** (right zone) in `UpdateBall`. Replace:
```asm
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
```
with:
```asm
    ; bounce off left paddle zone (ballX <= 24) -> DX = +1
    cp 24
    jr nc, .checkRight
    ld a, 1
    ld ($C0A2), a
    ld de, $076C             ; PADDLE-HIT pitch (~885 Hz)  ; TWEAK + F5
    call SfxTone
    jr .yaxis
.checkRight:
    ; bounce off right paddle zone (ballX >= 144) -> DX = -1 ($FF)
    cp 144
    jr c, .yaxis
    ld a, $FF
    ld ($C0A2), a
    ld de, $06D6             ; SCORE pitch (~440 Hz)       ; TWEAK + F5
    call SfxTone
```

(b) **Wall bounce** (top and bottom) in the Y axis. Replace:
```asm
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
with:
```asm
    ; bounce off top (ballY <= 8) -> DY = +1
    cp 8
    jr nc, .checkBottom
    ld a, 1
    ld ($C0A3), a
    ld de, $05DC             ; WALL-BOUNCE pitch (~239 Hz) ; TWEAK + F5
    call SfxTone
    ret
.checkBottom:
    ; bounce off bottom (ballY >= 136) -> DY = -1
    cp 136
    ret c
    ld a, $FF
    ld ($C0A3), a
    ld de, $05DC             ; WALL-BOUNCE pitch (~239 Hz) ; TWEAK + F5
    call SfxTone
    ret
```

- [ ] **Step 7: Add a sound line to the `TRY THIS LIVE` header**

In the header comment block, change the line:
```asm
; 4. TELEPORT (OAM edit): edit sprite 0's Y/X in the OAM panel to move the
;    ball by hand. Use F8 (soft reset) if you change Main/init code.
```
to:
```asm
; 4. TELEPORT (OAM edit): edit sprite 0's Y/X in the OAM panel to move the
;    ball by hand. Use F8 (soft reset) if you change Main/init code.
; 5. TUNE THE SOUND (F5): edit the `ld de, $....` pitch constants in
;    UpdateBall (PADDLE-HIT / WALL-BOUNCE / SCORE), press F5, hear it change.
```

- [ ] **Step 8: Run tests to confirm they pass**

Run: `make build/test_example_pong && ./build/test_example_pong`
Expected: PASS — `test_pong_apu_on` (NR52 power set) and `test_pong_sfx_fires` (CH1 active during play), plus all pre-existing Pong tests still green. Then `make test` — 0 failures.
If `test_pong_sfx_fires` fails, confirm: power-on wrote `$80` to NR52 BEFORE NR50/NR51; `SfxTone` sets NR12 with DAC on (`$F2`); NR14 got `$C0`-OR'd. If the assembler rejects `or $C0`, replace `ld a, d` / `or $C0` with passing the high byte already OR'd by the caller (e.g. `ld de, $C76C`) and `ld a, d` / `ldh ($14), a` — and report the change.

- [ ] **Step 9: Commit**

```bash
git add tests/example_run.h examples/pong.asm tests/test_example_pong.c
git commit -m "feat(examples): Pong sound effects (paddle/wall/score, F5-tweakable)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Snake sound effects

**Files:** Modify `examples/snake.asm`, `tests/test_example_snake.c`.

- [ ] **Step 1: Write the failing sound tests in `tests/test_example_snake.c`**

Add and call from `main()`:
```c
/* APU is powered on by init. */
static void test_snake_apu_on(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);
    gb_free(gb); asm_free(&r);
}

/* Eating food (default: head moves right into food at (15,9)) fires a CH1 chime. */
static void test_snake_eat_sfx(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    uint8_t seen = ex_run_watch_nr52(gb, 160, 9000000);  /* ~10 steps; eats at step 5 */
    ASSERT_TRUE(seen & 0x01);                              /* CH1 was active */
    gb_free(gb); asm_free(&r);
}
```
Add to `main()`:
```c
    test_snake_apu_on();
    test_snake_eat_sfx();
```

- [ ] **Step 2: Run to confirm failure**

Run: `cd /Users/wjbr/src/live-gameboy.ai && make build/test_example_snake && ./build/test_example_snake`
Expected: both new tests FAIL (no power-on, no CH1).

- [ ] **Step 3: Add APU power-on to Snake init**

In `examples/snake.asm`, replace:
```asm
    ld a, $91                ; LCD on, tiledata $8000, BG on (no sprites)
    ldh ($40), a
```
with:
```asm
    ; --- Sound: power on APU, full volume, route all channels both sides ---
    ld a, $80
    ldh ($26), a             ; NR52 = APU on (write FIRST)
    ld a, $77
    ldh ($24), a             ; NR50 = master volume L/R
    ld a, $FF
    ldh ($25), a             ; NR51 = all channels to both speakers

    ld a, $91                ; LCD on, tiledata $8000, BG on (no sprites)
    ldh ($40), a
```

- [ ] **Step 4: Add `SfxTone` and `SfxWrap` helpers at the end of `examples/snake.asm`**

Append after the final `ret` of `.NewFood` / `DrawFood` (end of file):
```asm

; ====================== SOUND ======================
; SfxTone — short CH1 (pulse) blip. In: D=freq hi (bits2-0), E=freq lo.
; Clobbers A,D,E; preserves B,C,HL.
SfxTone:
    xor a
    ldh ($10), a             ; NR10 = no sweep
    ld a, $A0
    ldh ($11), a             ; NR11 = duty 50% + length
    ld a, $F2
    ldh ($12), a             ; NR12 = vol 15, decay (DAC on)
    ld a, e
    ldh ($13), a             ; NR13 = freq lo
    ld a, d
    or $C0
    ldh ($14), a             ; NR14 = trigger + length-enable + freq hi
    ret

; SfxWrap — soft low blip when the snake wraps a wall. Clobbers A,D,E.
SfxWrap:
    ld de, $0400             ; WRAP pitch (~128 Hz)        ; TWEAK + F5
    call SfxTone
    ret
```

- [ ] **Step 5: Wire eat-food to a chime**

In `StepSnake`, replace:
```asm
    ld a, ($C003)
    inc a
    ld ($C003), a           ; grow (length byte; visible in MEMORY panel)
    call .NewFood
.noFood:
```
with:
```asm
    ld a, ($C003)
    inc a
    ld ($C003), a           ; grow (length byte; visible in MEMORY panel)
    call .NewFood
    ld de, $0780            ; EAT-FOOD chime (~1024 Hz)    ; TWEAK + F5
    call SfxTone
.noFood:
```

- [ ] **Step 6: Wire wall-wrap to `SfxWrap` (all four wrap branches)**

In `StepSnake`'s wrap logic, replace:
```asm
    ld a, b
    cp 20
    jr c, .xok
    cp 200
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
    ld c, 17
    jr .yok
.yhi:
    ld c, 0
.yok:
```
with:
```asm
    ld a, b
    cp 20
    jr c, .xok
    cp 200
    jr c, .xhi
    ld b, 19
    call SfxWrap
    jr .xok
.xhi:
    ld b, 0
    call SfxWrap
.xok:
    ld a, c
    cp 18
    jr c, .yok
    cp 200
    jr c, .yhi
    ld c, 17
    call SfxWrap
    jr .yok
.yhi:
    ld c, 0
    call SfxWrap
.yok:
```
(`SfxWrap`→`SfxTone` preserves B and C, so the just-computed new head survives the call; A is reloaded at `.xok`/`.yok`.)

- [ ] **Step 7: Add a sound line to the `TRY THIS LIVE` header**

Change:
```asm
; 4. GROW BY HAND (memory edit): bump the length byte at $C003 in the
;    MEMORY panel. Use F8 if you edit init code.
```
to:
```asm
; 4. GROW BY HAND (memory edit): bump the length byte at $C003 in the
;    MEMORY panel. Use F8 if you edit init code.
; 5. TUNE THE SOUND (F5): edit the EAT-FOOD pitch in StepSnake or the
;    WRAP pitch in SfxWrap, press F5, hear it change.
```

- [ ] **Step 8: Run tests to confirm they pass**

Run: `make build/test_example_snake && ./build/test_example_snake`
Expected: PASS (apu_on, eat_sfx, and all pre-existing Snake tests). Then `make test` — 0 failures.
If `test_snake_eat_sfx` fails: confirm the head actually reaches the food within 160 frames (default dir=right, throttle 16 → eats at ~frame 80) and that the eat branch's `call SfxTone` runs. Verify `SfxWrap` preserves B/C (it must — otherwise the snake head corrupts on wrap; the existing `test_snake_moves`/`test_snake_turns_*` will catch that regression).

- [ ] **Step 9: Commit**

```bash
git add examples/snake.asm tests/test_example_snake.c
git commit -m "feat(examples): Snake sound effects (eat-food, wall-wrap, F5-tweakable)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Breakout sound effects

**Files:** Modify `examples/breakout.asm`, `tests/test_example_breakout.c`.

- [ ] **Step 1: Write the failing sound tests in `tests/test_example_breakout.c`**

Add and call from `main()`:
```c
/* APU is powered on by init. */
static void test_breakout_apu_on(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);
    gb_free(gb); asm_free(&r);
}

/* Breaking a brick fires the CH4 noise burst as the ball reaches the field. */
static void test_breakout_break_sfx(void) {
    AsmResult r = ex_assemble("examples/breakout.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    uint8_t seen = ex_run_watch_nr52(gb, 150, 12000000); /* ball hits bricks ~frame 53 */
    ASSERT_TRUE(seen & 0x08);                             /* CH4 (noise) was active */
    gb_free(gb); asm_free(&r);
}
```
Add to `main()`:
```c
    test_breakout_apu_on();
    test_breakout_break_sfx();
```

- [ ] **Step 2: Run to confirm failure**

Run: `cd /Users/wjbr/src/live-gameboy.ai && make build/test_example_breakout && ./build/test_example_breakout`
Expected: both new tests FAIL.

- [ ] **Step 3: Add APU power-on to Breakout init**

In `examples/breakout.asm`, replace:
```asm
    ld a, $93                ; LCD on, OBJ on, BG on
    ldh ($40), a
```
with:
```asm
    ; --- Sound: power on APU, full volume, route all channels both sides ---
    ld a, $80
    ldh ($26), a             ; NR52 = APU on (write FIRST)
    ld a, $77
    ldh ($24), a             ; NR50 = master volume L/R
    ld a, $FF
    ldh ($25), a             ; NR51 = all channels to both speakers

    ld a, $93                ; LCD on, OBJ on, BG on
    ldh ($40), a
```

- [ ] **Step 4: Add `SfxTone`, `SfxNoise`, `SfxWall` helpers at the end of `examples/breakout.asm`**

Append after the final `ret` (end of `.BrickHit`/`.setUp`):
```asm

; ====================== SOUND ======================
; SfxTone — short CH1 (pulse) blip. In: D=freq hi (bits2-0), E=freq lo.
; Clobbers A,D,E; preserves B,C,HL.
SfxTone:
    xor a
    ldh ($10), a             ; NR10 = no sweep
    ld a, $A0
    ldh ($11), a             ; NR11 = duty 50% + length
    ld a, $F2
    ldh ($12), a             ; NR12 = vol 15, decay (DAC on)
    ld a, e
    ldh ($13), a             ; NR13 = freq lo
    ld a, d
    or $C0
    ldh ($14), a             ; NR14 = trigger + length-enable + freq hi
    ret

; SfxNoise — short CH4 (noise) burst. In: B=NR43 value (noise pitch).
; Clobbers A,B; preserves C,D,E,HL.
SfxNoise:
    ld a, $20
    ldh ($20), a             ; NR41 = length (short)
    ld a, $F2
    ldh ($21), a             ; NR42 = vol 15, decay (DAC on)
    ld a, b
    ldh ($22), a             ; NR43 = clock/divisor (pitch)
    ld a, $C0
    ldh ($23), a             ; NR44 = trigger + length-enable
    ret

; SfxWall — soft blip on a side/top wall bounce. Clobbers A,D,E.
SfxWall:
    ld de, $0780             ; WALL pitch (~1024 Hz)       ; TWEAK + F5
    call SfxTone
    ret
```

- [ ] **Step 5: Wire brick-break to the noise burst**

In `.BrickHit`, replace:
```asm
    xor a
    ld (hl), a             ; clear brick cell (live tilemap edit by the game)
    ; reflect DY: going up ($FF) -> down (1); going down (1) -> up ($FF)
    ld a, ($C0A3)
```
with:
```asm
    xor a
    ld (hl), a             ; clear brick cell (live tilemap edit by the game)
    ld b, $33             ; BRICK-BREAK noise pitch (NR43) ; TWEAK + F5
    call SfxNoise
    ; reflect DY: going up ($FF) -> down (1); going down (1) -> up ($FF)
    ld a, ($C0A3)
```

- [ ] **Step 6: Wire paddle-hit (bottom reflect) and wall bounces (top + 2 sides)**

(a) **Paddle hit** — bottom Y reflect. In `UpdateBall`, replace:
```asm
.ylo:
    cp 140
    jr c, .brick
    ld a, $FF
    ld ($C0A3), a
.brick:
    call .BrickHit
    ret
```
with:
```asm
.ylo:
    cp 140
    jr c, .brick
    ld a, $FF
    ld ($C0A3), a
    ld de, $076C             ; PADDLE-HIT pitch (~885 Hz)  ; TWEAK + F5
    call SfxTone
.brick:
    call .BrickHit
    ret
```

(b) **Wall bounce** — top Y reflect and both X reflects. Replace the X-axis block:
```asm
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
```
with:
```asm
    cp 8
    jr nc, .xhi
    ld a, 1
    ld ($C0A2), a
    call SfxWall
    jr .yax
.xhi:
    cp 152
    jr c, .yax
    ld a, $FF
    ld ($C0A2), a
    call SfxWall
.yax:
```
Then replace the top Y reflect:
```asm
    cp 8
    jr nc, .ylo
    ld a, 1
    ld ($C0A3), a
    jr .brick
```
with:
```asm
    cp 8
    jr nc, .ylo
    ld a, 1
    ld ($C0A3), a
    call SfxWall
    jr .brick
```
(`SfxWall`→`SfxTone` preserves B/C; after each, `.yax`/`.brick` reload A.)

- [ ] **Step 7: Add a sound line to the `TRY THIS LIVE` header**

Change:
```asm
; 4. F8 (soft reset) re-runs InitBricks to refill the field.
```
to:
```asm
; 4. F8 (soft reset) re-runs InitBricks to refill the field.
; 5. TUNE THE SOUND (F5): edit the BRICK-BREAK noise pitch in .BrickHit,
;    the PADDLE-HIT pitch in UpdateBall, or the WALL pitch in SfxWall;
;    press F5 and hear it change.
```

- [ ] **Step 8: Run tests to confirm they pass**

Run: `make build/test_example_breakout && ./build/test_example_breakout`
Expected: PASS (apu_on, break_sfx, and all pre-existing Breakout tests incl. `test_breakout_clears_brick`). Then `make test` — 0 failures.
If `test_breakout_break_sfx` fails: confirm a brick actually breaks within 150 frames (the existing `test_breakout_clears_brick` proves it does by ~frame 53+) and that `.BrickHit`'s `call SfxNoise` runs after the cell is cleared. Confirm `SfxNoise` sets NR42 DAC-on (`$F2`) and NR44 trigger (`$C0`).

- [ ] **Step 9: Commit**

```bash
git add examples/breakout.asm tests/test_example_breakout.c
git commit -m "feat(examples): Breakout sound effects (brick/paddle/wall, F5-tweakable)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: README note + final sweep

**Files:** Modify `README.md`.

- [ ] **Step 1: Document sound in the Example games section**

In `README.md`, find the "Example games" subsection (added earlier). After its table, add:
```markdown
Each game also plays **sound effects** on key events (Pong: paddle/wall/score;
Snake: eat-food/wall-wrap; Breakout: brick-break/paddle/wall). The effect
pitches are grouped at their call sites as `ld de, $....` (CH1 tone) or
`ld b, $..` (CH4 noise) constants — edit one and press **F5** to hear it change.
```

- [ ] **Step 2: Full test sweep**

Run: `cd /Users/wjbr/src/live-gameboy.ai && make test`
Expected: all suites pass, 0 failures, including the three `test_example_*` suites with their new sound tests.

- [ ] **Step 3: Confirm the ROMs still build**

Run: `make examples`
Expected: `build/{pong,snake,breakout}.gb` build with no error.

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: note tweakable sound effects in the example games

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review (completed during planning)

**Spec coverage:**
- APU power-on (NR52 first, then NR50/NR51) → Tasks 1/2/3 Step "power-on", + `apu_on` tests.
- `SfxTone` (CH1) and `SfxNoise` (CH4) helpers → added per game (self-contained, no shared include, matching the gallery's style).
- Per-game events: Pong paddle/wall/score (Task 1 Step 6); Snake eat-food/wall-wrap (Task 2 Steps 5-6); Breakout brick-break/paddle/wall (Task 3 Steps 5-6). Matches the spec's event→channel map (Pong all CH1; Snake CH1; Breakout brick=CH4 noise, paddle/wall=CH1).
- Live-edit hook: pitch constants at call sites in the F5 main-loop zone + a `TRY THIS LIVE` sound line per game (Steps 7).
- Testing: NR52 per-step polling via shared `ex_run_watch_nr52` asserting the expected channel fires during real gameplay (CH1 for Pong/Snake, CH4 for Breakout), plus power-on baseline. Ties sound to actual events, guarding "passes for the wrong reason."
- README note → Task 4.
- Spec "open considerations": short lengths (NR11/NR41 length + NR12/NR42 decay) chosen; concrete starting freqs given; per-step tick rejected (Snake uses eat-food + wall-wrap). Resolved.

**Placeholder scan:** No TODO/TBD. Every code step shows the exact old→new asm or full C. Frequencies are concrete hex.

**Type/label consistency:** `SfxTone` (D/E in), `SfxNoise` (B in), `SfxWrap`/`SfxWall` (no in) used consistently with their definitions. NR52 bit masks consistent (`0x80` power, `0x01` CH1, `0x08` CH4). The shared helper `ex_run_watch_nr52` is defined once (Task 1) and used by all three test files. Register-preservation contracts are stated and the hook sites respect them (B/C preserved across `SfxWrap`/`SfxWall`/`SfxTone` so head/ball state survives; A reloaded after each call).

**Risk note for the executor:** The only assembler-form risk is `or $C0` (OR A,n, opcode `0xF6`) — standard and almost certainly supported; if not, the fallback (caller passes the pre-OR'd NR14 high byte) is in Task 1 Step 8. Everything else reuses forms already proven in the existing example files.
