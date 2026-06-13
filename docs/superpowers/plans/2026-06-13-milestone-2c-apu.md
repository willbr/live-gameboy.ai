# Milestone 2c: APU (sound) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A functionally-correct DMG APU: 4 channels (2 pulse, wave, noise), frame sequencer, NR10–NR52 registers with correct read-back masks and power behavior, mixed to stereo float samples in a ring buffer, played through SDL3 audio in the shell. Acceptance: passes blargg `dgb_sound`/`dmg_sound` **01-registers**, plus unit-tested channel/mixer output and audible playback. (Deep cycle-exact sound-timing accuracy — the rest of the blargg sound suite — is explicitly deferred; noted inline.)

**Architecture:** A new `src/gb/apu.c` owns all APU state (added to `GB`) and registers FF10–FF3F. It is T-cycle stepped from `gb_tick` (alongside timer + PPU). The frame sequencer advances at 512 Hz (clocked off the DIV register's bit 4 falling edge, matching hardware) and steps length (256 Hz), envelope (64 Hz), and sweep (128 Hz). Each channel has a frequency timer producing a digital sample 0–15; the mixer applies per-channel panning (NR51) and master volume (NR50) and pushes stereo samples. The APU runs at the 4.19 MHz core clock; an internal downsampler emits samples at the host rate (configurable, default 48000 Hz) into a lock-free-enough ring buffer drained by the SDL audio callback. The core stays SDL-free; the SDL audio glue lives in `src/shell/`.

**Tech Stack:** C11. SDL3 audio (shell only). No new deps.

**Spec:** design §3 (APU), §9. Milestone 2, part C.

**Depends on:** Milestones 1, 2a, 2b (merged).

---

## Background (DMG APU, from Pan Docs + blargg)

**Registers** (FF10–FF26 control, FF30–FF3F wave RAM):
- CH1 (pulse+sweep): NR10 FF10 sweep, NR11 FF11 duty+length, NR12 FF12 envelope, NR13 FF13 freq lo, NR14 FF14 freq hi+trigger+length-enable.
- CH2 (pulse): NR21 FF16, NR22 FF17, NR23 FF18, NR24 FF19 (same layout as CH1 minus sweep).
- CH3 (wave): NR30 FF1A DAC power, NR31 FF1B length, NR32 FF1C output level, NR33 FF1D freq lo, NR34 FF1E freq hi+trigger+len-enable; wave RAM FF30–FF3F (32 4-bit samples).
- CH4 (noise): NR41 FF20 length, NR42 FF21 envelope, NR43 FF22 clock/divisor/width, NR44 FF23 trigger+len-enable.
- NR50 FF24 master volume + VIN, NR51 FF25 panning, NR52 FF26 power + channel status.

**Read-back OR masks** (reading returns `value | mask`; blargg 01-registers checks these exactly):
```
FF10:0x80 FF11:0x3F FF12:0x00 FF13:0xFF FF14:0xBF
FF15:0xFF FF16:0x3F FF17:0x00 FF18:0xFF FF19:0xBF
FF1A:0x7F FF1B:0xFF FF1C:0x9F FF1D:0xFF FF1E:0xBF
FF1F:0xFF FF20:0xFF FF21:0x00 FF22:0x00 FF23:0xBF
FF24:0x00 FF25:0x00 FF26:0x70  FF27..FF2F:0xFF  FF30..FF3F:0x00
```

**Power (NR52 bit7):** when cleared, all sound off; all registers FF10–FF25 are zeroed and writes to them ignored while off; NR52 itself and wave RAM remain accessible. Reading any register while off returns its mask (since value is 0). NR52 bits 0–3 = channel-on status (read only), bit 7 = power.

**Length counter:** each channel has a length timer clocked at 256 Hz; when length-enable (NRx4 bit6) is set and the counter reaches 0, the channel is disabled. CH1/2/4 lengths are 6-bit (64), CH3 is 8-bit (256).

**Envelope** (CH1/2/4): NRx2 bits 7-4 initial volume, bit3 direction (1=increase), bits 2-0 period; clocked at 64 Hz. A channel's DAC is off when NRx2 bits 7-3 are all 0 (volume 0 + decrease) — disables the channel.

**Sweep** (CH1): NR10 bits 6-4 period, bit3 direction, bits 2-0 shift; clocked at 128 Hz.

**Frequency timer:** pulse period = (2048 - freq) * 4 T-cycles; advancing the timer steps the 8-step duty waveform. Wave period = (2048 - freq) * 2. Noise uses a LFSR clocked by divisor<<shift.

**Trigger** (NRx4 bit7 write): enables the channel, reloads length if 0, reloads frequency timer, reloads envelope, (CH1) loads sweep.

---

## File structure
```
src/gb/apu.c    — NEW: registers, frame sequencer, 4 channels, mixer, ring buffer
src/gb/gb.h     — APU state + API
src/gb/bus.c    — route FF10-FF3F to apu
src/gb/gb.c     — gb_tick steps apu; gb_reset inits apu
tests/test_apu.c            — register masks, power, channel/mixer output
tests/blargg_sound.c        — runs blargg dmg_sound 01-registers headless
src/shell/main.c            — SDL3 audio: open device, callback drains gb_audio_read
```

---

### Task 1: APU state, registers (masks + power), frame sequencer

**Files:** modify `gb.h`, `bus.c`, `gb.c`; create `apu.c`, `tests/test_apu.c`.

- [ ] **Step 1: gb.h — APU state + API**

```c
    /* apu */
    uint8_t apu_reg[0x30];     /* raw FF10-FF3F backing (incl wave RAM at +0x20) */
    bool    apu_power;
    int     fs_step;           /* frame sequencer step 0..7 */
    uint16_t apu_div_prev;     /* prev DIV for 512Hz edge detection */
    /* channels: see apu.c for the per-channel structs packed here */
    struct { uint16_t timer; uint8_t duty_pos; uint8_t vol; uint8_t env_timer;
             uint16_t len; bool enabled; bool dac;
             uint8_t sweep_timer; uint16_t sweep_shadow; bool sweep_enabled; } ch1, ch2;
    struct { uint16_t timer; uint8_t pos; uint16_t len; bool enabled; bool dac; } ch3;
    struct { uint16_t timer; uint8_t vol; uint8_t env_timer; uint16_t len;
             uint16_t lfsr; bool enabled; bool dac; } ch4;
    /* output ring buffer (stereo float interleaved) */
    float   audio_ring[8192];
    int     audio_head, audio_tail;
    int     audio_sample_rate;
    double  audio_accum;        /* cycles toward next output sample */
```

API:
```c
void gb_apu_reset(GB *gb);
void gb_apu_tick(GB *gb, int tcycles);
uint8_t gb_apu_read(GB *gb, uint16_t addr);
void    gb_apu_write(GB *gb, uint16_t addr, uint8_t v);
void    gb_apu_set_sample_rate(GB *gb, int hz);
int     gb_audio_read(GB *gb, float *out, int max_samples); /* drains ring; returns samples written (stereo pairs*2) */
```

- [ ] **Step 2: tests/test_apu.c — failing tests for masks + power**

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>
static GB *fresh(void){ static uint8_t r[0x8000]; memset(r,0,sizeof r);
    GB*g=gb_new(); gb_load_rom(g,r,sizeof r); gb_reset(g); return g; }
int main(void){
    static const struct { uint16_t a; uint8_t mask; } M[] = {
        {0xFF10,0x80},{0xFF11,0x3F},{0xFF12,0x00},{0xFF13,0xFF},{0xFF14,0xBF},
        {0xFF16,0x3F},{0xFF17,0x00},{0xFF18,0xFF},{0xFF19,0xBF},
        {0xFF1A,0x7F},{0xFF1B,0xFF},{0xFF1C,0x9F},{0xFF1D,0xFF},{0xFF1E,0xBF},
        {0xFF20,0xFF},{0xFF21,0x00},{0xFF22,0x00},{0xFF23,0xBF},
        {0xFF24,0x00},{0xFF25,0x00},{0xFF26,0x70},{0xFF27,0xFF},{0xFF2F,0xFF},
    };
    {   /* write 0x00 then read: result must have all mask bits set */
        GB*g=fresh();
        gb_write8(g,0xFF26,0x80);          /* power on so writes land */
        for(unsigned i=0;i<sizeof M/sizeof M[0];i++){
            gb_write8(g,M[i].a,0x00);
            uint8_t got=gb_read8(g,M[i].a);
            ASSERT_EQ(got & M[i].mask, M[i].mask);
        }
        gb_free(g);
    }
    {   /* NR52 power bit reads back; low bits are channel status */
        GB*g=fresh(); gb_write8(g,0xFF26,0x80);
        ASSERT_EQ(gb_read8(g,0xFF26)&0x80, 0x80);
        gb_write8(g,0xFF26,0x00);                  /* power off */
        ASSERT_EQ(gb_read8(g,0xFF26)&0x80, 0x00);
        gb_free(g);
    }
    {   /* powered off: writes to FF10-FF25 ignored (read back as pure mask) */
        GB*g=fresh(); gb_write8(g,0xFF26,0x00);    /* off */
        gb_write8(g,0xFF12,0xF0);
        ASSERT_EQ(gb_read8(g,0xFF12), 0x00 | 0x00);   /* NR12 mask 0x00, value forced 0 */
        gb_free(g);
    }
    {   /* wave RAM is read/writable (mask 0x00) regardless of power */
        GB*g=fresh(); gb_write8(g,0xFF26,0x00);
        gb_write8(g,0xFF30,0xA5);
        ASSERT_EQ(gb_read8(g,0xFF30), 0xA5);
        gb_free(g);
    }
    TEST_MAIN_END();
}
```

- [ ] **Step 3: run, verify fail.**

- [ ] **Step 4: Create apu.c — registers, masks, power, frame sequencer skeleton (channels stubbed)**

Implement: `gb_apu_reset` (zero state, apu_power per reset value 0xF1→on, sample rate default 48000, lfsr=0x7FFF), the read mask table, write handler with power-off gating, NR52 handling, and `gb_apu_tick` that detects the 512 Hz frame-sequencer step via DIV bit 4 falling edge and calls (stubbed) `clock_length`/`clock_env`/`clock_sweep`, and a stubbed sample generator that pushes silence to the ring at the sample rate. Provide `gb_audio_read` draining the ring. Provide `gb_apu_set_sample_rate`.

(Key detail: the frame sequencer is clocked by the falling edge of DIV bit 4 (bit 5 in double-speed; DMG only here so bit 4). Use `g->div16` from the timer. Steps: 0,2,4,6 clock length; 2,6 clock sweep; 7 clocks envelope.)

Read mask table as a static `const uint8_t mask[0x30]` indexed by `addr-0xFF10` for FF10-FF2F, and 0x00 for wave RAM FF30-FF3F.

- [ ] **Step 5: wire bus.c (route 0xFF10-0xFF3F → gb_apu_read/write), gb.c (gb_apu_tick in gb_tick after ppu; gb_apu_reset in gb_reset).**

- [ ] **Step 6: run `make test` (all pass incl test_apu masks/power), `make blargg` + `make acid2` still PASS. Commit.**

```
git commit -am "feat: APU registers (masks, power), frame sequencer skeleton

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Pulse channels CH1 + CH2 (duty, length, envelope, sweep)

**Files:** modify `apu.c`, `tests/test_apu.c`.

- [ ] **Step 1: add failing channel tests** — trigger CH2 with a known freq+duty+volume, tick the APU, drain samples, assert the output is a non-silent square wave oscillating between +vol and −vol scaled; assert length-enable disables the channel after the right number of 256 Hz ticks; assert envelope changes volume; assert a channel with DAC off (NRx2 top 5 bits 0) produces silence; assert NR52 channel-status bit reflects enabled state.

(The implementer designs concrete numeric assertions: e.g. set CH2 freq=0x7FF gives the slowest timer; simpler — set duty=50%, volume=15, and assert that within one period the drained samples include both a high and a low value, and that the channel-status bit FF26 bit1 is set after trigger and clears after length expiry.)

- [ ] **Step 2: run, fail.**

- [ ] **Step 3: implement** — duty table `{0x01,0x81,0x87,0x7E}` (8-step patterns), frequency timer reload `(2048-freq)*4`, duty position advance, envelope (period, direction, reload on trigger), length counter (6-bit), sweep for CH1 (period, shift, direction; overflow check disables channel), trigger handling, and per-channel digital output 0–15 → DAC float in [−1,1]. The mixer sums enabled channels' DAC outputs (still per-task; full panning in Task 5, but produce mono-summed samples now so tests see output).

- [ ] **Step 4: run pass; blargg + acid2 still pass. Commit.**

---

### Task 3: Wave channel CH3
- [ ] Tests: trigger CH3 with a wave pattern in FF30-FF3F, assert output follows the wave samples at the output level shift (NR32: 0=mute,1=100%,2=50%,3=25%); length (8-bit) disables; DAC (NR30 bit7) off → silence.
- [ ] Implement: wave timer `(2048-freq)*2`, 32-sample position, 4-bit sample fetch from wave RAM, output-level shift, length counter (256).
- [ ] Pass; blargg+acid2 pass; commit.

---

### Task 4: Noise channel CH4
- [ ] Tests: trigger CH4, assert non-periodic (LFSR) non-silent output; envelope + length behave; 7-bit width mode (NR43 bit3) shortens the LFSR period; DAC off → silence.
- [ ] Implement: LFSR (15-bit, XOR taps bits 0,1; width mode also sets bit 6), divisor table `{8,16,32,48,64,80,96,112}`, timer = divisor << shift (NR43), envelope, length.
- [ ] Pass; blargg+acid2 pass; commit.

---

### Task 5: Mixer (NR50/NR51 panning, volume) + downsampled ring output
- [ ] Tests: set NR51 to route CH2 to left only; assert left samples non-zero, right zero. Set NR50 master volume; assert scaling. Assert `gb_audio_read` returns interleaved stereo and drains the ring (head advances).
- [ ] Implement: per-output (L/R) sum of the 4 channel DACs gated by NR51 bits, scaled by NR50 volume (0–7 each side, +1), divided to [−1,1]. Downsample: accumulate core cycles, emit one stereo sample every `4194304/sample_rate` cycles into the ring (drop oldest if full). 
- [ ] Pass; blargg+acid2 pass; commit.

---

### Task 6: SDL3 audio in the shell
**Files:** modify `src/shell/main.c`.
- [ ] In `run_window`: `gb_apu_set_sample_rate(g, 48000)`; open an SDL3 audio stream (`SDL_OpenAudioDeviceStream`, spec: 48000 Hz, 2 channels, `SDL_AUDIO_F32`); in the main loop after running a frame, call `gb_audio_read` into a buffer and `SDL_PutAudioStreamData`. Resume the device. The `--shot` path stays silent (no audio device).
- [ ] Build clean (`make live-gameboy`). Smoke: the controller runs the binary briefly (audio may be silent in headless CI but must not crash). Commit.

---

### Task 7: blargg dmg_sound 01-registers gate + CI/README
**Files:** create `tests/blargg_sound.c`; modify `Makefile`, CI, README.
- [ ] `tests/blargg_sound.c`: like `tests/blargg.c` but for a sound ROM (serial "Passed"/"Failed"). Fetch the dmg_sound ROMs (they're in retrio/gb-test-roms under `dmg_sound/rom_singles/` or `dmg_sound/dmg_sound.gb`; the individual `01-registers.gb` is the gate).
- [ ] Makefile target `sound: $(BUILD)/blargg_sound` running `roms/gb-test-roms/dmg_sound/rom_singles/01-registers.gb`.
- [ ] Run it. If 01-registers FAILS, the serial output names the failing register/sub-case — debug the mask/power logic with focused unit tests; do not weaken. (The other dmg_sound ROMs — 02..12 — are NOT required for this milestone; if 01-registers passes, the gate is met. Note in README which sound tests pass.)
- [ ] Update README: Milestone 2 line → `- [x] Milestone 2: SDL3 shell, pixel-FIFO PPU, APU` (all three parts done). Add CI step (guarded like the shell step). 
- [ ] Full verification: `make clean && make test && make blargg && make acid2 && make sound && make live-gameboy`. Commit.

---

## Self-review notes
- **Scope/gate honesty:** the milestone gate is blargg dmg_sound **01-registers** (register masks + power) plus unit-tested functional channels/mixer plus audible SDL playback. The deeper dmg_sound timing tests (length/trigger/sweep edge cases) require cycle-exact frame-sequencer/DIV coupling and are explicitly deferred — flagged here and in README. This yields good-sounding audio and correct register behavior without an open-ended accuracy rabbit hole.
- **Core stays SDL-free:** apu.c has no SDL; the ring buffer + gb_audio_read are the seam; SDL audio is shell-only.
- **Regression:** every task re-runs blargg + acid2 (the APU ticks every gb_tick; a bug there could stall the CPU loop).
- **Field/name consistency:** gb_apu_* API + per-channel structs declared in Task 1, used throughout.
```
