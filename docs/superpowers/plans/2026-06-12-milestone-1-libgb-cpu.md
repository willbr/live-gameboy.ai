# Milestone 1: libgb Headless Core (CPU + Bus + Timers) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A headless, SDL-free, deterministic SM83 emulator core in pure C that passes blargg's `cpu_instrs` and `instr_timing` test ROMs.

**Architecture:** `libgb` is a plain C library (`src/gb/`). A single `GB` struct holds all state (serializable later). The CPU is M-cycle stepped: every bus access goes through `rd`/`wr` helpers that tick the rest of the system 4 T-cycles, so timing falls out of the memory traffic. Opcodes are decoded algebraically (x/y/z bit fields) into small per-family functions. Blargg ROMs report results over the serial port; we capture serial writes into a buffer and grep it.

**Tech Stack:** C11, no dependencies (stb headers allowed later; none needed in this milestone). Plain Makefile. Hand-rolled single-header test harness. Test ROMs from `retrio/gb-test-roms` (fetched, gitignored).

**Spec:** `docs/superpowers/specs/2026-06-12-live-gameboy-design.md` §3, §9, §10 (milestone 1).

**Roadmap context (separate plans, in order):**
1. **This plan** — headless CPU core.
2. SDL3 shell + pixel-FIFO PPU + APU (plays real ROMs, passes dmg-acid2).
3. libasm assembler + build database.
4. Live patch engine.
5. Tile editor + VRAM provenance.
6. Debug panels + export + polish.

---

## File structure

```
Makefile                 — build lib, tests, blargg runner
.gitignore               — build outputs, roms/
src/gb/gb.h              — public API: GB struct, lifecycle, step, bus access
src/gb/gb.c              — gb_new/gb_free/gb_load_rom/gb_reset, gb_tick
src/gb/bus.c             — gb_read8/gb_write8 memory-map dispatch
src/gb/cpu.c             — SM83 core: fetch/decode/execute, interrupts
src/gb/timer.c           — DIV/TIMA/TMA/TAC
tests/test.h             — assertion macros
tests/test_bus.c
tests/test_cpu_loads.c
tests/test_cpu_alu.c
tests/test_cpu_bits.c
tests/test_cpu_flow.c
tests/test_cpu_misc.c
tests/test_interrupts.c
tests/test_timer.c
tests/blargg.c           — acceptance runner (also a CLI tool)
roms/                    — fetched test ROMs (gitignored)
```

Responsibility boundaries: `bus.c` knows the memory map and nothing about instructions; `cpu.c` knows instructions and nothing about the memory map (it only calls `gb_read8`/`gb_write8`/`gb_tick`); `timer.c` only consumes ticks. This is the hook structure the spec's provenance tracking and watchpoints attach to later — keep all memory traffic flowing through `bus.c`.

---

### Task 1: Scaffold — Makefile, test harness, first test

**Files:**
- Create: `Makefile`, `.gitignore`, `tests/test.h`, `tests/test_bus.c`, `src/gb/gb.h`

- [ ] **Step 1: Create `.gitignore`**

```gitignore
build/
roms/
.DS_Store
```

- [ ] **Step 2: Create `tests/test.h`**

```c
#ifndef TEST_H
#define TEST_H
#include <stdio.h>

static int t_fail, t_count;

#define ASSERT_EQ(got, want) do {                                          \
    long long g_ = (long long)(got), w_ = (long long)(want); t_count++;    \
    if (g_ != w_) { t_fail++;                                              \
        printf("FAIL %s:%d: %s == %lld (0x%llx), want %lld (0x%llx)\n",    \
               __FILE__, __LINE__, #got, g_, g_, w_, w_); }                \
} while (0)

#define ASSERT_TRUE(x) ASSERT_EQ(!!(x), 1)

#define TEST_MAIN_END() do {                                               \
    printf("%-24s %3d assertions, %d failures\n", __FILE__, t_count, t_fail); \
    return t_fail ? 1 : 0;                                                 \
} while (0)

#endif
```

- [ ] **Step 3: Create `src/gb/gb.h` (public API, full struct)**

```c
#ifndef GB_H
#define GB_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    bool ime;
    uint8_t ime_pending;   /* EI takes effect after the next instruction */
    bool halted;
    bool halt_bug;
} CPU;

typedef struct GB {
    CPU cpu;

    uint8_t *rom;
    size_t rom_size;
    uint8_t vram[0x2000];
    uint8_t wram[0x2000];
    uint8_t oam[0xA0];
    uint8_t hram[0x7F];
    uint8_t io[0x80];      /* raw backing for not-yet-modeled IO regs */
    uint8_t ie, iflag;     /* FFFF, FF0F (iflag upper 3 bits read as 1) */

    /* timer */
    uint16_t div16;        /* internal divider; DIV (FF04) is its high byte */
    uint8_t tima, tma, tac;

    /* serial (test-ROM result channel) */
    char serial_buf[8192];
    size_t serial_len;

    uint64_t cycles;       /* T-cycles since reset */
} GB;

GB  *gb_new(void);
void gb_free(GB *gb);
bool gb_load_rom(GB *gb, const uint8_t *data, size_t size); /* copies data */
void gb_reset(GB *gb);   /* DMG post-boot-ROM register state */
int  gb_step(GB *gb);    /* one instruction or interrupt dispatch; returns T-cycles */
void gb_tick(GB *gb, int tcycles);  /* advance subsystems (timer) */

/* Untimed bus access (tests, future debugger). CPU wraps these with ticks. */
uint8_t gb_read8(GB *gb, uint16_t addr);
void    gb_write8(GB *gb, uint16_t addr, uint8_t v);

/* interrupt bits in IE/IF */
#define INT_VBLANK 0x01
#define INT_STAT   0x02
#define INT_TIMER  0x04
#define INT_SERIAL 0x08
#define INT_JOYPAD 0x10

#endif
```

- [ ] **Step 4: Write the failing test `tests/test_bus.c`**

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

int main(void) {
    uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    rom[0x0000] = 0xAA;
    rom[0x7FFF] = 0xBB;

    GB *g = gb_new();
    ASSERT_TRUE(g != NULL);
    ASSERT_TRUE(gb_load_rom(g, rom, sizeof rom));
    gb_reset(g);

    /* ROM reads, ROM writes ignored (no MBC yet) */
    ASSERT_EQ(gb_read8(g, 0x0000), 0xAA);
    ASSERT_EQ(gb_read8(g, 0x7FFF), 0xBB);
    gb_write8(g, 0x0000, 0x12);
    ASSERT_EQ(gb_read8(g, 0x0000), 0xAA);

    /* WRAM + echo */
    gb_write8(g, 0xC123, 0x55);
    ASSERT_EQ(gb_read8(g, 0xC123), 0x55);
    ASSERT_EQ(gb_read8(g, 0xE123), 0x55);   /* echo of C000-DDFF */
    gb_write8(g, 0xE200, 0x66);
    ASSERT_EQ(gb_read8(g, 0xC200), 0x66);

    /* VRAM, OAM, HRAM */
    gb_write8(g, 0x8000, 0x11); ASSERT_EQ(gb_read8(g, 0x8000), 0x11);
    gb_write8(g, 0xFE00, 0x22); ASSERT_EQ(gb_read8(g, 0xFE00), 0x22);
    gb_write8(g, 0xFF80, 0x33); ASSERT_EQ(gb_read8(g, 0xFF80), 0x33);

    /* unusable region reads FF */
    ASSERT_EQ(gb_read8(g, 0xFEA0), 0xFF);

    /* IE / IF */
    gb_write8(g, 0xFFFF, 0x1F); ASSERT_EQ(gb_read8(g, 0xFFFF), 0x1F);
    gb_write8(g, 0xFF0F, 0x05); ASSERT_EQ(gb_read8(g, 0xFF0F), 0xE5); /* upper bits read 1 */

    /* reset state (DMG post-boot) */
    ASSERT_EQ(g->cpu.pc, 0x0100);
    ASSERT_EQ(g->cpu.sp, 0xFFFE);
    ASSERT_EQ(g->cpu.a, 0x01);
    ASSERT_EQ(g->cpu.f, 0xB0);

    gb_free(g);
    TEST_MAIN_END();
}
```

- [ ] **Step 5: Create `Makefile`**

```makefile
CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -O2 -g
BUILD   = build

GB_SRC  = $(wildcard src/gb/*.c)
GB_OBJ  = $(GB_SRC:src/gb/%.c=$(BUILD)/gb/%.o)

TESTS   = $(wildcard tests/test_*.c)
TESTBIN = $(TESTS:tests/%.c=$(BUILD)/%)

all: test

$(BUILD)/gb/%.o: src/gb/%.c src/gb/gb.h | $(BUILD)/gb
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%: tests/%.c $(GB_OBJ) tests/test.h | $(BUILD)
	$(CC) $(CFLAGS) $< $(GB_OBJ) -o $@

$(BUILD) $(BUILD)/gb:
	mkdir -p $@

test: $(TESTBIN)
	@rc=0; for t in $(TESTBIN); do ./$$t || rc=1; done; exit $$rc

$(BUILD)/blargg: tests/blargg.c $(GB_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< $(GB_OBJ) -o $@

roms:
	mkdir -p roms
	test -d roms/gb-test-roms || git clone --depth 1 https://github.com/retrio/gb-test-roms roms/gb-test-roms

blargg: $(BUILD)/blargg
	./$(BUILD)/blargg roms/gb-test-roms/cpu_instrs/cpu_instrs.gb
	./$(BUILD)/blargg roms/gb-test-roms/instr_timing/instr_timing.gb

clean:
	rm -rf $(BUILD)

.PHONY: all test blargg roms clean
```

- [ ] **Step 6: Run, verify failure (no implementation yet)**

Run: `make test`
Expected: compile/link FAILURE (`gb_new` undefined — `src/gb/*.c` doesn't exist yet).

- [ ] **Step 7: Commit scaffold (red)**

```bash
git add Makefile .gitignore tests/test.h tests/test_bus.c src/gb/gb.h
git commit -m "test: scaffold libgb with failing bus test"
```

---

### Task 2: gb.c + bus.c — lifecycle, memory map

**Files:**
- Create: `src/gb/gb.c`, `src/gb/bus.c`, `src/gb/timer.c` (stub), `src/gb/cpu.c` (stub)

- [ ] **Step 1: Create `src/gb/gb.c`**

```c
#include "gb.h"
#include <stdlib.h>
#include <string.h>

GB *gb_new(void) {
    GB *g = calloc(1, sizeof(GB));
    return g;
}

void gb_free(GB *gb) {
    if (!gb) return;
    free(gb->rom);
    free(gb);
}

bool gb_load_rom(GB *gb, const uint8_t *data, size_t size) {
    if (size < 0x8000) return false;
    free(gb->rom);
    gb->rom = malloc(size);
    if (!gb->rom) return false;
    memcpy(gb->rom, data, size);
    gb->rom_size = size;
    return true;
}

void gb_reset(GB *gb) {
    CPU *c = &gb->cpu;
    memset(c, 0, sizeof *c);
    c->a = 0x01; c->f = 0xB0;
    c->b = 0x00; c->c = 0x13;
    c->d = 0x00; c->e = 0xD8;
    c->h = 0x01; c->l = 0x4D;
    c->sp = 0xFFFE; c->pc = 0x0100;

    memset(gb->vram, 0, sizeof gb->vram);
    memset(gb->wram, 0, sizeof gb->wram);
    memset(gb->oam, 0, sizeof gb->oam);
    memset(gb->hram, 0, sizeof gb->hram);
    memset(gb->io, 0xFF, sizeof gb->io);
    gb->ie = 0; gb->iflag = 0xE1;
    gb->div16 = 0xABCC;          /* DIV reads 0xAB at PC=0100 on DMG */
    gb->tima = 0; gb->tma = 0; gb->tac = 0xF8;
    gb->serial_len = 0;
    gb->cycles = 0;
}

void gb_tick(GB *gb, int tcycles) {
    gb->cycles += (uint64_t)tcycles;
    gb_timer_tick(gb, tcycles);
}
```

- [ ] **Step 2: Create `src/gb/bus.c`**

```c
#include "gb.h"

/* timer.c owns FF04-FF07 */
uint8_t gb_timer_read(GB *gb, uint16_t addr);
void    gb_timer_write(GB *gb, uint16_t addr, uint8_t v);

static uint8_t io_read(GB *gb, uint8_t r) {
    switch (r) {
    case 0x04: case 0x05: case 0x06: case 0x07:
        return gb_timer_read(gb, 0xFF00 | r);
    case 0x0F: return gb->iflag | 0xE0;
    case 0x44: return 0x90;  /* LY stub: report VBlank so ROMs that wait make progress */
    default:   return gb->io[r];
    }
}

static void io_write(GB *gb, uint8_t r, uint8_t v) {
    switch (r) {
    case 0x01: gb->io[0x01] = v; break;                       /* SB */
    case 0x02:                                                 /* SC */
        if (v & 0x80) {  /* transfer start: capture for test ROMs */
            if (gb->serial_len < sizeof gb->serial_buf - 1)
                gb->serial_buf[gb->serial_len++] = (char)gb->io[0x01];
            gb->iflag |= INT_SERIAL;
            v &= 0x7F;
        }
        gb->io[0x02] = v;
        break;
    case 0x04: case 0x05: case 0x06: case 0x07:
        gb_timer_write(gb, 0xFF00 | r, v);
        break;
    case 0x0F: gb->iflag = v & 0x1F; break;
    default:   gb->io[r] = v; break;
    }
}

uint8_t gb_read8(GB *gb, uint16_t a) {
    if (a < 0x8000)  return gb->rom[a % gb->rom_size];
    if (a < 0xA000)  return gb->vram[a - 0x8000];
    if (a < 0xC000)  return 0xFF;                       /* cart RAM: none yet */
    if (a < 0xE000)  return gb->wram[a - 0xC000];
    if (a < 0xFE00)  return gb->wram[a - 0xE000];       /* echo */
    if (a < 0xFEA0)  return gb->oam[a - 0xFE00];
    if (a < 0xFF00)  return 0xFF;                       /* unusable */
    if (a < 0xFF80)  return io_read(gb, a & 0x7F);
    if (a < 0xFFFF)  return gb->hram[a - 0xFF80];
    return gb->ie;
}

void gb_write8(GB *gb, uint16_t a, uint8_t v) {
    if (a < 0x8000)  return;                            /* MBC writes: later plan */
    if (a < 0xA000)  { gb->vram[a - 0x8000] = v; return; }
    if (a < 0xC000)  return;
    if (a < 0xE000)  { gb->wram[a - 0xC000] = v; return; }
    if (a < 0xFE00)  { gb->wram[a - 0xE000] = v; return; }
    if (a < 0xFEA0)  { gb->oam[a - 0xFE00] = v; return; }
    if (a < 0xFF00)  return;
    if (a < 0xFF80)  { io_write(gb, a & 0x7F, v); return; }
    if (a < 0xFFFF)  { gb->hram[a - 0xFF80] = v; return; }
    gb->ie = v;
}
```

- [ ] **Step 3: Create stubs so the build links**

`src/gb/timer.c`:

```c
#include "gb.h"

uint8_t gb_timer_read(GB *gb, uint16_t addr) { (void)gb; (void)addr; return 0xFF; }
void    gb_timer_write(GB *gb, uint16_t addr, uint8_t v) { (void)gb; (void)addr; (void)v; }
void    gb_timer_tick(GB *gb, int tcycles) { (void)gb; (void)tcycles; }
```

`src/gb/cpu.c`:

```c
#include "gb.h"

int gb_step(GB *gb) { (void)gb; return 4; }
```

Add the timer prototypes to `gb.h` (below `gb_tick`):

```c
/* internal: timer (timer.c) */
uint8_t gb_timer_read(GB *gb, uint16_t addr);
void    gb_timer_write(GB *gb, uint16_t addr, uint8_t v);
void    gb_timer_tick(GB *gb, int tcycles);
```

- [ ] **Step 4: Run test, verify pass**

Run: `make test`
Expected: `tests/test_bus.c ... 0 failures`

- [ ] **Step 5: Commit**

```bash
git add src/gb/ tests/
git commit -m "feat: GB lifecycle, memory-map bus, serial capture"
```

---

### Task 3: CPU plumbing — flags, pairs, fetch, exec skeleton

**Files:**
- Rewrite: `src/gb/cpu.c`
- Create: `tests/test_cpu_loads.c` (first few cases)

- [ ] **Step 1: Write failing test `tests/test_cpu_loads.c`**

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static uint8_t rom[0x8000];

static GB *gb_with(const uint8_t *code, size_t n) {
    memset(rom, 0, sizeof rom);
    memcpy(rom + 0x100, code, n);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}

int main(void) {
    {   /* NOP: 4 cycles, pc+1 */
        GB *g = gb_with((uint8_t[]){0x00}, 1);
        ASSERT_EQ(gb_step(g), 4);
        ASSERT_EQ(g->cpu.pc, 0x0101);
        gb_free(g);
    }
    {   /* LD A,d8 */
        GB *g = gb_with((uint8_t[]){0x3E, 0x42}, 2);
        ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ(g->cpu.a, 0x42);
        gb_free(g);
    }
    {   /* JP a16: 16 cycles */
        GB *g = gb_with((uint8_t[]){0xC3, 0x34, 0x12}, 3);
        ASSERT_EQ(gb_step(g), 16);
        ASSERT_EQ(g->cpu.pc, 0x1234);
        gb_free(g);
    }
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run, verify failure**

Run: `make test`
Expected: FAIL (stub `gb_step` returns 4, pc never moves).

- [ ] **Step 3: Rewrite `src/gb/cpu.c` with full plumbing + 3 opcodes**

This file's helpers are used by every later task — names matter, keep them exactly:

```c
#include "gb.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- flags ---- */
#define FZ 0x80
#define FN 0x40
#define FH 0x20
#define FC 0x10

static void set_flag(CPU *c, uint8_t flag, bool on) {
    if (on) c->f |= flag; else c->f &= ~flag;
    c->f &= 0xF0;
}
static bool get_flag(const CPU *c, uint8_t flag) { return (c->f & flag) != 0; }

/* ---- 16-bit pairs ---- */
static uint16_t BC(const CPU *c) { return (uint16_t)(c->b << 8 | c->c); }
static uint16_t DE(const CPU *c) { return (uint16_t)(c->d << 8 | c->e); }
static uint16_t HL(const CPU *c) { return (uint16_t)(c->h << 8 | c->l); }
static uint16_t AF(const CPU *c) { return (uint16_t)(c->a << 8 | (c->f & 0xF0)); }
static void set_BC(CPU *c, uint16_t v) { c->b = v >> 8; c->c = (uint8_t)v; }
static void set_DE(CPU *c, uint16_t v) { c->d = v >> 8; c->e = (uint8_t)v; }
static void set_HL(CPU *c, uint16_t v) { c->h = v >> 8; c->l = (uint8_t)v; }
static void set_AF(CPU *c, uint16_t v) { c->a = v >> 8; c->f = v & 0xF0; }

/* ---- timed bus access: every access costs one M-cycle (4 T) ---- */
static uint8_t rd(GB *g, uint16_t a) { gb_tick(g, 4); return gb_read8(g, a); }
static void    wr(GB *g, uint16_t a, uint8_t v) { gb_tick(g, 4); gb_write8(g, a, v); }
static void    internal(GB *g) { gb_tick(g, 4); }   /* internal M-cycle, no bus */

static uint8_t  fetch8(GB *g)  { return rd(g, g->cpu.pc++); }
static uint16_t fetch16(GB *g) { uint8_t lo = fetch8(g), hi = fetch8(g);
                                 return (uint16_t)(hi << 8 | lo); }

static void push16(GB *g, uint16_t v) {
    internal(g);
    wr(g, --g->cpu.sp, v >> 8);
    wr(g, --g->cpu.sp, (uint8_t)v);
}
static uint16_t pop16(GB *g) {
    uint8_t lo = rd(g, g->cpu.sp++);
    uint8_t hi = rd(g, g->cpu.sp++);
    return (uint16_t)(hi << 8 | lo);
}

/* ---- r-table access: 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A ---- */
static uint8_t get_r(GB *g, int i) {
    CPU *c = &g->cpu;
    switch (i) {
    case 0: return c->b; case 1: return c->c; case 2: return c->d;
    case 3: return c->e; case 4: return c->h; case 5: return c->l;
    case 6: return rd(g, HL(c)); default: return c->a;
    }
}
static void set_r(GB *g, int i, uint8_t v) {
    CPU *c = &g->cpu;
    switch (i) {
    case 0: c->b = v; break; case 1: c->c = v; break; case 2: c->d = v; break;
    case 3: c->e = v; break; case 4: c->h = v; break; case 5: c->l = v; break;
    case 6: wr(g, HL(c), v); break; default: c->a = v; break;
    }
}

/* rp table: 0=BC 1=DE 2=HL 3=SP */
static uint16_t get_rp(const CPU *c, int i) {
    switch (i) { case 0: return BC(c); case 1: return DE(c);
                 case 2: return HL(c); default: return c->sp; }
}
static void set_rp(CPU *c, int i, uint16_t v) {
    switch (i) { case 0: set_BC(c, v); break; case 1: set_DE(c, v); break;
                 case 2: set_HL(c, v); break; default: c->sp = v; break; }
}

/* cc table: 0=NZ 1=Z 2=NC 3=C */
static bool cond(const CPU *c, int i) {
    switch (i) { case 0: return !get_flag(c, FZ); case 1: return get_flag(c, FZ);
                 case 2: return !get_flag(c, FC); default: return get_flag(c, FC); }
}

static void exec(GB *g, uint8_t op);

int gb_step(GB *g) {
    uint64_t start = g->cycles;
    CPU *c = &g->cpu;

    /* interrupt dispatch lives here from Task 9 on */

    if (c->ime_pending && --c->ime_pending == 0)
        c->ime = true;

    if (c->halted) { gb_tick(g, 4); return (int)(g->cycles - start); }

    uint8_t op = fetch8(g);
    if (c->halt_bug) { c->pc--; c->halt_bug = false; }
    exec(g, op);
    return (int)(g->cycles - start);
}

static void exec(GB *g, uint8_t op) {
    CPU *c = &g->cpu;
    switch (op) {
    case 0x00: break;                                     /* NOP */
    case 0x3E: c->a = fetch8(g); break;                   /* LD A,d8 (general form: Task 4) */
    case 0xC3: { uint16_t t = fetch16(g); internal(g); c->pc = t; break; } /* JP a16 */
    default:
        fprintf(stderr, "unimplemented opcode 0x%02X at PC=0x%04X\n", op, c->pc - 1);
        abort();
    }
}
```

(Several helpers are unused until later tasks; silence `-Werror` for now by adding
`__attribute__((unused))` is NOT the approach — instead reference them: add
`(void)get_r; (void)set_r; (void)get_rp; (void)set_rp; (void)cond; (void)push16; (void)pop16; (void)set_flag; (void)AF; (void)set_AF;`
inside `gb_step` temporarily, and delete those lines as tasks consume the helpers. All are consumed by Task 9.)

- [ ] **Step 4: Run, verify pass**

Run: `make test`
Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/gb/cpu.c tests/test_cpu_loads.c
git commit -m "feat: CPU plumbing (flags, pairs, timed bus) + NOP/LD A,d8/JP"
```

---

### Task 4: 8-bit loads

**Files:**
- Modify: `src/gb/cpu.c` (`exec`)
- Modify: `tests/test_cpu_loads.c`

- [ ] **Step 1: Extend `tests/test_cpu_loads.c` with failing tests** (insert before `TEST_MAIN_END()`)

```c
    {   /* LD r,d8 for B; LD r,r'; LD r,(HL); LD (HL),r */
        GB *g = gb_with((uint8_t[]){
            0x06, 0x77,        /* LD B,0x77        8 cy */
            0x48,              /* LD C,B           4 cy */
            0x21, 0x00, 0xC0,  /* LD HL,0xC000    12 cy (needs Task 5? no: add 0x21 here) */
            0x70,              /* LD (HL),B        8 cy */
            0x5E,              /* LD E,(HL)        8 cy */
        }, 8);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(g->cpu.b, 0x77);
        ASSERT_EQ(gb_step(g), 4);  ASSERT_EQ(g->cpu.c, 0x77);
        ASSERT_EQ(gb_step(g), 12);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(gb_read8(g, 0xC000), 0x77);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(g->cpu.e, 0x77);
        gb_free(g);
    }
    {   /* LD (HL),d8 ; indirect A loads ; HL+/HL- */
        GB *g = gb_with((uint8_t[]){
            0x21, 0x00, 0xC0,  /* LD HL,0xC000 */
            0x36, 0x99,        /* LD (HL),0x99    12 cy */
            0x2A,              /* LD A,(HL+)       8 cy: A=0x99, HL=0xC001 */
            0x32,              /* LD (HL-),A       8 cy: [C001]=99, HL=C000 */
            0x0A,              /* LD A,(BC) */
        }, 8);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 12); ASSERT_EQ(gb_read8(g, 0xC000), 0x99);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(g->cpu.a, 0x99);
        ASSERT_EQ((g->cpu.h << 8) | g->cpu.l, 0xC001);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(gb_read8(g, 0xC001), 0x99);
        ASSERT_EQ((g->cpu.h << 8) | g->cpu.l, 0xC000);
        gb_free(g);
    }
    {   /* LDH and absolute */
        GB *g = gb_with((uint8_t[]){
            0x3E, 0x5A,        /* LD A,0x5A */
            0xE0, 0x80,        /* LDH (0x80),A    12 cy -> FF80 */
            0xF0, 0x80,        /* LDH A,(0x80)    12 cy */
            0xEA, 0x00, 0xC1,  /* LD (0xC100),A   16 cy */
            0xFA, 0x00, 0xC1,  /* LD A,(0xC100)   16 cy */
            0x0E, 0x81,        /* LD C,0x81 */
            0xE2,              /* LD (C),A         8 cy -> FF81 */
            0xF2,              /* LD A,(C)         8 cy */
        }, 16);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 12); ASSERT_EQ(gb_read8(g, 0xFF80), 0x5A);
        ASSERT_EQ(gb_step(g), 12); ASSERT_EQ(g->cpu.a, 0x5A);
        ASSERT_EQ(gb_step(g), 16); ASSERT_EQ(gb_read8(g, 0xC100), 0x5A);
        ASSERT_EQ(gb_step(g), 16);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 8);  ASSERT_EQ(gb_read8(g, 0xFF81), 0x5A);
        ASSERT_EQ(gb_step(g), 8);
        gb_free(g);
    }
```

- [ ] **Step 2: Run, verify failure**

Run: `make test`
Expected: abort with `unimplemented opcode 0x06`.

- [ ] **Step 3: Implement — replace the `0x3E` case in `exec` with general decode**

At the **top** of `exec`'s switch, handle the regular blocks by pattern *before* the switch:

```c
static void exec(GB *g, uint8_t op) {
    CPU *c = &g->cpu;
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;

    if (x == 1) {                       /* 0x40-0x7F: LD r,r' (0x76 = HALT, Task 8) */
        if (op == 0x76) { halt(g); return; }
        set_r(g, y, get_r(g, z));
        return;
    }
    if (x == 0 && z == 6) {             /* LD r,d8 / LD (HL),d8 */
        set_r(g, y, fetch8(g));
        return;
    }

    switch (op) {
    case 0x00: break;
    /* indirect A loads */
    case 0x02: wr(g, BC(c), c->a); break;
    case 0x12: wr(g, DE(c), c->a); break;
    case 0x0A: c->a = rd(g, BC(c)); break;
    case 0x1A: c->a = rd(g, DE(c)); break;
    case 0x22: wr(g, HL(c), c->a); set_HL(c, HL(c) + 1); break;
    case 0x2A: c->a = rd(g, HL(c)); set_HL(c, HL(c) + 1); break;
    case 0x32: wr(g, HL(c), c->a); set_HL(c, HL(c) - 1); break;
    case 0x3A: c->a = rd(g, HL(c)); set_HL(c, HL(c) - 1); break;
    case 0xE0: wr(g, 0xFF00 | fetch8(g), c->a); break;
    case 0xF0: c->a = rd(g, 0xFF00 | fetch8(g)); break;
    case 0xE2: wr(g, 0xFF00 | c->c, c->a); break;
    case 0xF2: c->a = rd(g, 0xFF00 | c->c); break;
    case 0xEA: wr(g, fetch16(g), c->a); break;
    case 0xFA: c->a = rd(g, fetch16(g)); break;
    /* needed by the test: LD rr,d16 (full family in Task 5) */
    case 0x01: case 0x11: case 0x21: case 0x31:
        set_rp(c, (op >> 4) & 3, fetch16(g)); break;
    case 0xC3: { uint16_t t = fetch16(g); internal(g); c->pc = t; break; }
    default:
        fprintf(stderr, "unimplemented opcode 0x%02X at PC=0x%04X\n", op, c->pc - 1);
        abort();
    }
}
```

Add a `halt` stub above `exec` for now (full behavior in Task 8):

```c
static void halt(GB *g) { g->cpu.halted = true; }
```

Delete the temporary `(void)` references that are now consumed.

- [ ] **Step 4: Run, verify pass**

Run: `make test` — Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add -u
git commit -m "feat: 8-bit load instructions"
```

---

### Task 5: 16-bit loads + stack

**Files:**
- Modify: `src/gb/cpu.c`, `tests/test_cpu_loads.c`

- [ ] **Step 1: Add failing tests** (before `TEST_MAIN_END()`)

```c
    {   /* PUSH/POP, LD (a16),SP, LD SP,HL, LD HL,SP+e8 */
        GB *g = gb_with((uint8_t[]){
            0x01, 0x34, 0x12,  /* LD BC,0x1234 */
            0xC5,              /* PUSH BC        16 cy */
            0xD1,              /* POP DE         12 cy */
            0x08, 0x00, 0xC2,  /* LD (0xC200),SP 20 cy */
            0x21, 0x00, 0xD0,  /* LD HL,0xD000 */
            0xF9,              /* LD SP,HL        8 cy */
            0xF8, 0xFE,        /* LD HL,SP-2     12 cy: H=0 ops on low byte */
        }, 14);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 16);
        ASSERT_EQ(gb_step(g), 12);
        ASSERT_EQ(g->cpu.d, 0x12); ASSERT_EQ(g->cpu.e, 0x34);
        uint16_t sp_before = g->cpu.sp;
        ASSERT_EQ(gb_step(g), 20);
        ASSERT_EQ(gb_read8(g, 0xC200), sp_before & 0xFF);
        ASSERT_EQ(gb_read8(g, 0xC201), sp_before >> 8);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 8); ASSERT_EQ(g->cpu.sp, 0xD000);
        ASSERT_EQ(gb_step(g), 12);
        ASSERT_EQ((g->cpu.h << 8) | g->cpu.l, 0xCFFE);
        /* LD HL,SP+e8 flags: Z=0 N=0; H/C from low-byte unsigned add (00+FE: no carry) */
        ASSERT_EQ(g->cpu.f & 0x80, 0);
        ASSERT_EQ(g->cpu.f & 0x10, 0);
        gb_free(g);
    }
    {   /* PUSH AF masks low nibble of F */
        GB *g = gb_with((uint8_t[]){0xF5, 0xC1}, 2);   /* PUSH AF; POP BC */
        g->cpu.a = 0x12; g->cpu.f = 0xF0;
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.b, 0x12); ASSERT_EQ(g->cpu.c, 0xF0);
        gb_free(g);
    }
```

- [ ] **Step 2: Run, verify failure** — `make test`, abort on 0xC5.

- [ ] **Step 3: Implement — add to `exec`'s switch**

```c
    case 0xC5: push16(g, BC(c)); break;
    case 0xD5: push16(g, DE(c)); break;
    case 0xE5: push16(g, HL(c)); break;
    case 0xF5: push16(g, AF(c)); break;
    case 0xC1: set_BC(c, pop16(g)); break;
    case 0xD1: set_DE(c, pop16(g)); break;
    case 0xE1: set_HL(c, pop16(g)); break;
    case 0xF1: set_AF(c, pop16(g)); break;
    case 0x08: { uint16_t a = fetch16(g);
                 wr(g, a, (uint8_t)c->sp); wr(g, a + 1, c->sp >> 8); break; }
    case 0xF9: internal(g); c->sp = HL(c); break;
    case 0xF8: { int8_t e = (int8_t)fetch8(g); internal(g);
                 uint16_t r = (uint16_t)(c->sp + e);
                 set_flag(c, FZ, false); set_flag(c, FN, false);
                 set_flag(c, FH, (c->sp & 0x0F) + (e & 0x0F) > 0x0F);
                 set_flag(c, FC, (c->sp & 0xFF) + (e & 0xFF) > 0xFF);
                 set_HL(c, r); break; }
```

- [ ] **Step 4: Run, verify pass** — `make test`.

- [ ] **Step 5: Commit**

```bash
git add -u && git commit -m "feat: 16-bit loads, stack ops"
```

---

### Task 6: 8-bit ALU

**Files:**
- Modify: `src/gb/cpu.c`
- Create: `tests/test_cpu_alu.c`

- [ ] **Step 1: Write failing test `tests/test_cpu_alu.c`**

Reuse the `gb_with` helper (copy it; it's 10 lines — each test binary is standalone).

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static uint8_t rom[0x8000];
static GB *gb_with(const uint8_t *code, size_t n) {
    memset(rom, 0, sizeof rom);
    memcpy(rom + 0x100, code, n);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}

/* flag mask helpers: f layout ZNHC0000 */
#define FLAGS(g) ((g)->cpu.f & 0xF0)

int main(void) {
    {   /* ADD A,d8: 0x3A + 0xC6 = 0x00, Z H C set (classic GB manual vector) */
        GB *g = gb_with((uint8_t[]){0x3E, 0x3A, 0xC6, 0xC6}, 4);
        gb_step(g); ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ(g->cpu.a, 0x00);
        ASSERT_EQ(FLAGS(g), 0xB0);   /* Z H C */
        gb_free(g);
    }
    {   /* ADC with carry-in: A=0xE1, C=1, ADC 0x1E -> 0x00, Z H C */
        GB *g = gb_with((uint8_t[]){0x37, 0x3E, 0xE1, 0xCE, 0x1E}, 5); /* SCF first */
        gb_step(g); gb_step(g); ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ(g->cpu.a, 0x00);
        ASSERT_EQ(FLAGS(g), 0xB0);
        gb_free(g);
    }
    {   /* SUB: A=0x3E, SUB 0x3E -> 0, Z N */
        GB *g = gb_with((uint8_t[]){0x3E, 0x3E, 0xD6, 0x3E}, 4);
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x00);
        ASSERT_EQ(FLAGS(g), 0xC0);   /* Z N */
        gb_free(g);
    }
    {   /* SUB borrow: A=0x3E, SUB 0x40 -> 0xFE, N C */
        GB *g = gb_with((uint8_t[]){0x3E, 0x3E, 0xD6, 0x40}, 4);
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0xFE);
        ASSERT_EQ(FLAGS(g), 0x50);   /* N C */
        gb_free(g);
    }
    {   /* AND/XOR/OR/CP flag conventions */
        GB *g = gb_with((uint8_t[]){
            0x3E, 0x5A, 0xE6, 0x0F,   /* AND 0x0F -> 0x0A, H always set */
            0xEE, 0x0A,               /* XOR -> 0x00, Z only */
            0xF6, 0x00,               /* OR 0 -> 0x00, Z only */
            0xFE, 0x01,               /* CP 1 -> A unchanged, N C (0 < 1) */
        }, 10);
        gb_step(g);
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x0A); ASSERT_EQ(FLAGS(g), 0x20);
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x00); ASSERT_EQ(FLAGS(g), 0x80);
        gb_step(g); ASSERT_EQ(FLAGS(g), 0x80);
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x00); ASSERT_EQ(FLAGS(g), 0x50);
        gb_free(g);
    }
    {   /* INC wraps half-carry, preserves C; DEC sets N */
        GB *g = gb_with((uint8_t[]){0x37, 0x3E, 0x0F, 0x3C, 0x3D}, 5); /* SCF;LD A,0F;INC A;DEC A */
        gb_step(g); gb_step(g);
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x10); ASSERT_EQ(FLAGS(g), 0x30); /* H + preserved C */
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x0F); ASSERT_EQ(FLAGS(g), 0x70); /* N H + preserved C */
        gb_free(g);
    }
    {   /* register-form ALU (x=2 block): ADD A,B */
        GB *g = gb_with((uint8_t[]){0x06, 0x01, 0x3E, 0xFF, 0x80}, 5);
        gb_step(g); gb_step(g);
        ASSERT_EQ(gb_step(g), 4);
        ASSERT_EQ(g->cpu.a, 0x00);
        ASSERT_EQ(FLAGS(g), 0xB0);
        gb_free(g);
    }
    TEST_MAIN_END();
}
```

Note: the SCF test cases require opcode 0x37 (SCF). Implement SCF in this task's step 3 (it is one line; Task 8 covers its siblings).

- [ ] **Step 2: Run, verify failure** — `make test`.

- [ ] **Step 3: Implement ALU in `cpu.c`**

Add the ALU dispatcher above `exec`:

```c
/* y: 0 ADD 1 ADC 2 SUB 3 SBC 4 AND 5 XOR 6 OR 7 CP */
static void alu(GB *g, int y, uint8_t v) {
    CPU *c = &g->cpu;
    uint8_t a = c->a;
    int carry = get_flag(c, FC) ? 1 : 0;
    switch (y) {
    case 0: carry = 0; /* fallthrough */
    case 1: {
        unsigned r = a + v + (y == 0 ? 0 : carry);
        unsigned cin = (y == 0 ? 0 : (unsigned)carry);
        set_flag(c, FH, (a & 0x0F) + (v & 0x0F) + cin > 0x0F);
        set_flag(c, FC, r > 0xFF);
        c->a = (uint8_t)r;
        set_flag(c, FZ, c->a == 0); set_flag(c, FN, false);
        break;
    }
    case 2: carry = 0; /* fallthrough */
    case 3: case 7: {
        unsigned cin = (y == 2 || y == 7) ? 0 : (unsigned)carry;
        unsigned r = a - v - cin;
        set_flag(c, FH, (a & 0x0F) < (v & 0x0F) + cin);
        set_flag(c, FC, (unsigned)a < (unsigned)v + cin);
        set_flag(c, FZ, (uint8_t)r == 0); set_flag(c, FN, true);
        if (y != 7) c->a = (uint8_t)r;       /* CP discards result */
        break;
    }
    case 4: c->a = a & v; c->f = 0; set_flag(c, FZ, c->a == 0); set_flag(c, FH, true); break;
    case 5: c->a = a ^ v; c->f = 0; set_flag(c, FZ, c->a == 0); break;
    case 6: c->a = a | v; c->f = 0; set_flag(c, FZ, c->a == 0); break;
    }
}

static uint8_t inc8(CPU *c, uint8_t v) {
    v++;
    set_flag(c, FZ, v == 0); set_flag(c, FN, false);
    set_flag(c, FH, (v & 0x0F) == 0);
    return v;
}
static uint8_t dec8(CPU *c, uint8_t v) {
    v--;
    set_flag(c, FZ, v == 0); set_flag(c, FN, true);
    set_flag(c, FH, (v & 0x0F) == 0x0F);
    return v;
}
```

Wire into `exec` — pattern blocks (with the x==1 block, before the switch):

```c
    if (x == 2) { alu(g, y, get_r(g, z)); return; }          /* 0x80-0xBF */
    if (x == 3 && z == 6) { alu(g, y, fetch8(g)); return; }  /* d8 forms C6,CE,D6,DE,E6,EE,F6,FE */
    if (x == 0 && z == 4) { set_r(g, y, inc8(c, get_r(g, y))); return; }  /* INC r */
    if (x == 0 && z == 5) { set_r(g, y, dec8(c, get_r(g, y))); return; }  /* DEC r */
```

And one switch case for SCF (rest of its family in Task 8):

```c
    case 0x37: set_flag(c, FN, false); set_flag(c, FH, false); set_flag(c, FC, true); break;
```

Careful: the `x == 3 && z == 6` pattern must be checked AFTER the explicit switch would
catch nothing — but 0xC6..0xFE with z==6 are exactly the 8 immediate-ALU opcodes, no
collisions. Order in `exec`: x==1 block, x==0&&z==6 (LD r,d8), x==2, x==3&&z==6, INC, DEC,
then the switch. Note `x==0&&z==6` (LD) and `x==3&&z==6` (ALU) differ in x — no overlap.

- [ ] **Step 4: Run, verify pass** — `make test`.

- [ ] **Step 5: Commit**

```bash
git add src/gb/cpu.c tests/test_cpu_alu.c
git commit -m "feat: 8-bit ALU, INC/DEC, SCF"
```

---

### Task 7: 16-bit ALU

**Files:**
- Modify: `src/gb/cpu.c`, `tests/test_cpu_alu.c`

- [ ] **Step 1: Add failing tests** (before `TEST_MAIN_END()`)

```c
    {   /* ADD HL,rr: 8 cycles; Z preserved, N=0, H/C from bit 11/15 */
        GB *g = gb_with((uint8_t[]){
            0x21, 0xFF, 0x8F,   /* LD HL,0x8FFF */
            0x01, 0x01, 0x00,   /* LD BC,0x0001 */
            0x09,               /* ADD HL,BC -> 0x9000, H set */
        }, 7);
        g->cpu.f = 0x80;  /* pre-set Z to check preservation */
        gb_step(g); gb_step(g);
        ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ((g->cpu.h << 8) | g->cpu.l, 0x9000);
        ASSERT_EQ(g->cpu.f & 0xF0, 0xA0);   /* Z preserved, H set, N/C clear */
        gb_free(g);
    }
    {   /* INC rr / DEC rr: 8 cycles, no flags */
        GB *g = gb_with((uint8_t[]){0x01, 0xFF, 0xFF, 0x03, 0x0B}, 5);
        g->cpu.f = 0xF0;
        gb_step(g);
        ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ((g->cpu.b << 8) | g->cpu.c, 0x0000);
        ASSERT_EQ(g->cpu.f & 0xF0, 0xF0);   /* untouched */
        ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ((g->cpu.b << 8) | g->cpu.c, 0xFFFF);
        gb_free(g);
    }
    {   /* ADD SP,e8: 16 cycles, Z=N=0, H/C from low byte */
        GB *g = gb_with((uint8_t[]){0x31, 0xF8, 0xFF, 0xE8, 0x08}, 5); /* SP=FFF8; ADD SP,8 */
        gb_step(g);
        ASSERT_EQ(gb_step(g), 16);
        ASSERT_EQ(g->cpu.sp, 0x0000);
        ASSERT_EQ(g->cpu.f & 0xF0, 0x30);   /* H C (F8+08 carries both) */
        gb_free(g);
    }
```

- [ ] **Step 2: Run, verify failure** — `make test`, abort on 0x09.

- [ ] **Step 3: Implement — add patterns + cases to `exec`**

Pattern block (with the others):

```c
    if (x == 0 && z == 1 && (op & 8)) {                  /* ADD HL,rp: 09 19 29 39 */
        int p = (op >> 4) & 3;
        uint16_t hl = HL(c), v = get_rp(c, p);
        internal(g);
        set_flag(c, FN, false);
        set_flag(c, FH, (hl & 0x0FFF) + (v & 0x0FFF) > 0x0FFF);
        set_flag(c, FC, (uint32_t)hl + v > 0xFFFF);
        set_HL(c, hl + v);
        return;
    }
    if (x == 0 && z == 3) {                              /* INC/DEC rp: 03 13 23 33 / 0B 1B 2B 3B */
        int p = (op >> 4) & 3;
        internal(g);
        set_rp(c, p, get_rp(c, p) + ((op & 8) ? -1 : 1));
        return;
    }
```

Switch case:

```c
    case 0xE8: { int8_t e = (int8_t)fetch8(g); internal(g); internal(g);
                 set_flag(c, FZ, false); set_flag(c, FN, false);
                 set_flag(c, FH, (c->sp & 0x0F) + (e & 0x0F) > 0x0F);
                 set_flag(c, FC, (c->sp & 0xFF) + (e & 0xFF) > 0xFF);
                 c->sp = (uint16_t)(c->sp + e); break; }
```

- [ ] **Step 4: Run, verify pass** — `make test`.

- [ ] **Step 5: Commit**

```bash
git add -u && git commit -m "feat: 16-bit ALU"
```

---

### Task 8: Rotates, CB prefix, remaining misc (DAA, CPL, CCF, HALT, STOP, DI/EI)

**Files:**
- Modify: `src/gb/cpu.c`
- Create: `tests/test_cpu_bits.c`, `tests/test_cpu_misc.c`

- [ ] **Step 1: Write failing test `tests/test_cpu_bits.c`** (copy the `gb_with` helper again)

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static uint8_t rom[0x8000];
static GB *gb_with(const uint8_t *code, size_t n) {
    memset(rom, 0, sizeof rom);
    memcpy(rom + 0x100, code, n);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}
#define FLAGS(g) ((g)->cpu.f & 0xF0)

int main(void) {
    {   /* RLCA: A=0x85 -> 0x0B, C=1, Z always 0 */
        GB *g = gb_with((uint8_t[]){0x3E, 0x85, 0x07}, 3);
        gb_step(g); ASSERT_EQ(gb_step(g), 4);
        ASSERT_EQ(g->cpu.a, 0x0B); ASSERT_EQ(FLAGS(g), 0x10);
        gb_free(g);
    }
    {   /* RRA: A=0x01, C=0 -> A=0x00, C=1, Z=0 (accumulator rotates never set Z) */
        GB *g = gb_with((uint8_t[]){0x3E, 0x01, 0x1F}, 3);
        g->cpu.f = 0;
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x00); ASSERT_EQ(FLAGS(g), 0x10);
        gb_free(g);
    }
    {   /* CB RLC B: B=0x85 -> 0x0B C=1 ; CB forms DO set Z */
        GB *g = gb_with((uint8_t[]){0x06, 0x00, 0xCB, 0x00}, 4);
        gb_step(g); ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ(g->cpu.b, 0x00); ASSERT_EQ(FLAGS(g), 0x80); /* Z */
        gb_free(g);
    }
    {   /* SWAP A: 0xF1 -> 0x1F, only Z possible */
        GB *g = gb_with((uint8_t[]){0x3E, 0xF1, 0xCB, 0x37}, 4);
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x1F); ASSERT_EQ(FLAGS(g), 0x00);
        gb_free(g);
    }
    {   /* SRA keeps sign; SRL doesn't */
        GB *g = gb_with((uint8_t[]){0x3E, 0x81, 0xCB, 0x2F, 0xCB, 0x3F}, 6);
        gb_step(g);
        gb_step(g); ASSERT_EQ(g->cpu.a, 0xC0); ASSERT_EQ(FLAGS(g), 0x10); /* SRA: C from bit0 */
        gb_step(g); ASSERT_EQ(g->cpu.a, 0x60); ASSERT_EQ(FLAGS(g), 0x00); /* SRL */
        gb_free(g);
    }
    {   /* BIT 7,H: Z reflects complement of bit; H always set, C preserved */
        GB *g = gb_with((uint8_t[]){0x26, 0x80, 0xCB, 0x7C, 0xCB, 0x6C}, 6);
        g->cpu.f = 0x10;
        gb_step(g);
        ASSERT_EQ(gb_step(g), 8);
        ASSERT_EQ(FLAGS(g), 0x30);          /* bit set: Z=0; H=1; C preserved */
        gb_step(g);                          /* BIT 5,H: bit clear */
        ASSERT_EQ(FLAGS(g), 0xB0);          /* Z=1 H=1 C=1 */
        gb_free(g);
    }
    {   /* SET/RES on (HL): 16 cycles */
        GB *g = gb_with((uint8_t[]){
            0x21, 0x00, 0xC0,
            0xCB, 0xC6,        /* SET 0,(HL) */
            0xCB, 0x86,        /* RES 0,(HL) */
        }, 7);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 16); ASSERT_EQ(gb_read8(g, 0xC000), 0x01);
        ASSERT_EQ(gb_step(g), 16); ASSERT_EQ(gb_read8(g, 0xC000), 0x00);
        gb_free(g);
    }
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Write failing test `tests/test_cpu_misc.c`**

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static uint8_t rom[0x8000];
static GB *gb_with(const uint8_t *code, size_t n) {
    memset(rom, 0, sizeof rom);
    memcpy(rom + 0x100, code, n);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}
#define FLAGS(g) ((g)->cpu.f & 0xF0)

int main(void) {
    {   /* DAA after BCD add: 0x45 + 0x38 = 0x7D -> DAA -> 0x83 */
        GB *g = gb_with((uint8_t[]){0x3E, 0x45, 0xC6, 0x38, 0x27}, 5);
        gb_step(g); gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x83);
        ASSERT_EQ(FLAGS(g) & 0x10, 0);      /* no carry */
        gb_free(g);
    }
    {   /* DAA after BCD sub: 0x83 - 0x38 = 0x4B -> DAA -> 0x45 */
        GB *g = gb_with((uint8_t[]){0x3E, 0x83, 0xD6, 0x38, 0x27}, 5);
        gb_step(g); gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x45);
        gb_free(g);
    }
    {   /* CPL: N H set, others preserved */
        GB *g = gb_with((uint8_t[]){0x3E, 0x35, 0x2F}, 3);
        gb_step(g); gb_step(g);
        ASSERT_EQ(g->cpu.a, 0xCA);
        ASSERT_TRUE((g->cpu.f & 0x60) == 0x60);
        gb_free(g);
    }
    {   /* CCF flips carry, clears N/H */
        GB *g = gb_with((uint8_t[]){0x37, 0x3F, 0x3F}, 3);
        gb_step(g);
        gb_step(g); ASSERT_EQ(FLAGS(g) & 0x10, 0x00);
        gb_step(g); ASSERT_EQ(FLAGS(g) & 0x10, 0x10);
        gb_free(g);
    }
    {   /* EI is delayed one instruction; DI immediate */
        GB *g = gb_with((uint8_t[]){0xFB, 0x00, 0x00, 0xF3}, 4);
        gb_step(g);                       /* EI */
        ASSERT_EQ(g->cpu.ime, false);     /* not yet */
        gb_step(g);                       /* NOP — IME becomes true before this */
        ASSERT_EQ(g->cpu.ime, true);
        gb_step(g);
        gb_step(g);                       /* DI */
        ASSERT_EQ(g->cpu.ime, false);
        gb_free(g);
    }
    {   /* HALT with pending+enabled interrupt and IME=0: halt bug (PC fails to advance) */
        GB *g = gb_with((uint8_t[]){0x76, 0x3C, 0x00}, 3); /* HALT; INC A */
        g->ie = INT_TIMER; g->iflag |= INT_TIMER;          /* pending immediately */
        g->cpu.ime = false;
        gb_step(g);                       /* HALT: does not halt, sets halt_bug */
        uint8_t a0 = g->cpu.a;
        gb_step(g);                       /* INC A executes... */
        gb_step(g);                       /* ...and executes AGAIN (PC stuck once) */
        ASSERT_EQ(g->cpu.a, (uint8_t)(a0 + 2));
        ASSERT_EQ(g->cpu.pc, 0x0102);     /* both INCs came from 0x0101 */
        gb_free(g);
    }
    TEST_MAIN_END();
}
```

- [ ] **Step 3: Run, verify failures** — `make test`.

- [ ] **Step 4: Implement**

CB handler, above `exec`:

```c
static void exec_cb(GB *g) {
    CPU *c = &g->cpu;
    uint8_t op = fetch8(g);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v = get_r(g, z);

    if (x == 1) {                                   /* BIT y,r — read only */
        set_flag(c, FZ, !(v & (1 << y)));
        set_flag(c, FN, false); set_flag(c, FH, true);
        return;
    }
    if (x == 2) { set_r(g, z, v & ~(1 << y)); return; }   /* RES */
    if (x == 3) { set_r(g, z, v | (1 << y)); return; }    /* SET */

    /* x == 0: rotate/shift family, y selects op */
    bool carry = get_flag(c, FC);
    uint8_t r = 0; bool nc = false;
    switch (y) {
    case 0: nc = v >> 7; r = (uint8_t)(v << 1 | v >> 7); break;        /* RLC */
    case 1: nc = v & 1;  r = (uint8_t)(v >> 1 | v << 7); break;        /* RRC */
    case 2: nc = v >> 7; r = (uint8_t)(v << 1 | (carry ? 1 : 0)); break; /* RL */
    case 3: nc = v & 1;  r = (uint8_t)(v >> 1 | (carry ? 0x80 : 0)); break; /* RR */
    case 4: nc = v >> 7; r = (uint8_t)(v << 1); break;                 /* SLA */
    case 5: nc = v & 1;  r = (uint8_t)((v >> 1) | (v & 0x80)); break;  /* SRA */
    case 6: nc = false;  r = (uint8_t)(v << 4 | v >> 4); break;        /* SWAP */
    case 7: nc = v & 1;  r = (uint8_t)(v >> 1); break;                 /* SRL */
    }
    c->f = 0;
    set_flag(c, FZ, r == 0);
    set_flag(c, FC, nc);
    set_r(g, z, r);
}
```

Replace the `halt` stub with real behavior:

```c
static void halt(GB *g) {
    CPU *c = &g->cpu;
    uint8_t pending = g->ie & g->iflag & 0x1F;
    if (!c->ime && pending)
        c->halt_bug = true;       /* HALT bug: next opcode byte read twice */
    else
        c->halted = true;
}
```

Switch cases in `exec`:

```c
    case 0xCB: exec_cb(g); break;
    case 0x07: { bool b = c->a >> 7; c->a = (uint8_t)(c->a << 1 | b);
                 c->f = 0; set_flag(c, FC, b); break; }                  /* RLCA */
    case 0x0F: { bool b = c->a & 1; c->a = (uint8_t)(c->a >> 1 | b << 7);
                 c->f = 0; set_flag(c, FC, b); break; }                  /* RRCA */
    case 0x17: { bool b = c->a >> 7;
                 c->a = (uint8_t)(c->a << 1 | (get_flag(c, FC) ? 1 : 0));
                 c->f = 0; set_flag(c, FC, b); break; }                  /* RLA */
    case 0x1F: { bool b = c->a & 1;
                 c->a = (uint8_t)(c->a >> 1 | (get_flag(c, FC) ? 0x80 : 0));
                 c->f = 0; set_flag(c, FC, b); break; }                  /* RRA */
    case 0x27: {                                                          /* DAA */
        uint8_t a = c->a;
        if (!get_flag(c, FN)) {
            if (get_flag(c, FC) || a > 0x99) { a += 0x60; set_flag(c, FC, true); }
            if (get_flag(c, FH) || (a & 0x0F) > 0x09) a += 0x06;
        } else {
            if (get_flag(c, FC)) a -= 0x60;
            if (get_flag(c, FH)) a -= 0x06;
        }
        c->a = a;
        set_flag(c, FZ, a == 0); set_flag(c, FH, false);
        break;
    }
    case 0x2F: c->a = ~c->a; set_flag(c, FN, true); set_flag(c, FH, true); break;  /* CPL */
    case 0x3F: set_flag(c, FN, false); set_flag(c, FH, false);
               set_flag(c, FC, !get_flag(c, FC)); break;                  /* CCF */
    case 0xF3: c->ime = false; c->ime_pending = 0; break;                 /* DI */
    case 0xFB: if (!c->ime) c->ime_pending = 2; break;                    /* EI */
    case 0x10: fetch8(g); break;                                          /* STOP: skip byte */
```

(EI delay model: `ime_pending = 2` decremented once at the top of each `gb_step`, so IME
turns on at the start of the *second* step after EI — i.e. after the instruction following
EI begins. The blargg/mooneye ROMs in Task 10 validate this; if `ei_timing` style cases
fail, adjust to decrement after `exec` instead.)

- [ ] **Step 5: Run, verify pass** — `make test`.

- [ ] **Step 6: Commit**

```bash
git add src/gb/cpu.c tests/test_cpu_bits.c tests/test_cpu_misc.c
git commit -m "feat: rotates, CB prefix, DAA/CPL/CCF, HALT bug, EI delay, STOP"
```

---

### Task 9: Control flow + interrupt dispatch

**Files:**
- Modify: `src/gb/cpu.c`
- Create: `tests/test_cpu_flow.c`, `tests/test_interrupts.c`

- [ ] **Step 1: Write failing test `tests/test_cpu_flow.c`**

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static uint8_t rom[0x8000];
static GB *gb_with(const uint8_t *code, size_t n) {
    memset(rom, 0, sizeof rom);
    memcpy(rom + 0x100, code, n);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}

int main(void) {
    {   /* JR forward/backward; taken 12, not-taken 8 */
        GB *g = gb_with((uint8_t[]){0x18, 0x02, 0x00, 0x00, 0x18, 0xFC}, 6);
        ASSERT_EQ(gb_step(g), 12); ASSERT_EQ(g->cpu.pc, 0x0104);
        ASSERT_EQ(gb_step(g), 12); ASSERT_EQ(g->cpu.pc, 0x0102);
        gb_free(g);
    }
    {   /* JR NZ not taken after Z set */
        GB *g = gb_with((uint8_t[]){0xAF, 0x20, 0x10}, 3);  /* XOR A sets Z */
        gb_step(g);
        ASSERT_EQ(gb_step(g), 8); ASSERT_EQ(g->cpu.pc, 0x0103);
        gb_free(g);
    }
    {   /* CALL/RET: 24 + 16 cycles; stack correct */
        GB *g = gb_with((uint8_t[]){
            0xCD, 0x10, 0x01,   /* 0100: CALL 0x0110 */
            0x00,               /* 0103 */
        }, 4);
        rom[0x110] = 0xC9;      /* RET */
        gb_load_rom(g, rom, sizeof rom); gb_reset(g);
        ASSERT_EQ(gb_step(g), 24);
        ASSERT_EQ(g->cpu.pc, 0x0110);
        ASSERT_EQ(g->cpu.sp, 0xFFFC);
        ASSERT_EQ(gb_read8(g, 0xFFFC), 0x03);
        ASSERT_EQ(gb_read8(g, 0xFFFD), 0x01);
        ASSERT_EQ(gb_step(g), 16);
        ASSERT_EQ(g->cpu.pc, 0x0103);
        gb_free(g);
    }
    {   /* conditional RET timing: taken 20, not taken 8 */
        GB *g = gb_with((uint8_t[]){0xAF, 0xC8}, 2);  /* XOR A; RET Z (taken) */
        rom[0x100] = 0xAF; rom[0x101] = 0xC8;
        gb_step(g);
        g->cpu.sp = 0xFFFC;
        gb_write8(g, 0xFFFC, 0x00); gb_write8(g, 0xFFFD, 0x02);
        ASSERT_EQ(gb_step(g), 20);
        ASSERT_EQ(g->cpu.pc, 0x0200);
        gb_free(g);
    }
    {   /* RST 0x28: 16 cycles */
        GB *g = gb_with((uint8_t[]){0xEF}, 1);
        ASSERT_EQ(gb_step(g), 16);
        ASSERT_EQ(g->cpu.pc, 0x0028);
        gb_free(g);
    }
    {   /* JP HL: 4 cycles */
        GB *g = gb_with((uint8_t[]){0x21, 0x00, 0x40, 0xE9}, 4);
        gb_step(g);
        ASSERT_EQ(gb_step(g), 4);
        ASSERT_EQ(g->cpu.pc, 0x4000);
        gb_free(g);
    }
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Write failing test `tests/test_interrupts.c`**

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static uint8_t rom[0x8000];
static GB *gb_with(const uint8_t *code, size_t n) {
    memset(rom, 0, sizeof rom);
    memcpy(rom + 0x100, code, n);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}

int main(void) {
    {   /* dispatch: 20 cycles, IME off, IF bit cleared, PC = vector, old PC pushed */
        GB *g = gb_with((uint8_t[]){0x00, 0x00}, 2);
        g->cpu.ime = true;
        g->ie = INT_TIMER; g->iflag |= INT_TIMER;
        ASSERT_EQ(gb_step(g), 20);
        ASSERT_EQ(g->cpu.pc, 0x0050);              /* timer vector */
        ASSERT_EQ(g->cpu.ime, false);
        ASSERT_EQ(g->iflag & INT_TIMER, 0);
        ASSERT_EQ(gb_read8(g, g->cpu.sp), 0x00);   /* pushed PC lo */
        ASSERT_EQ(gb_read8(g, g->cpu.sp + 1), 0x01);
        gb_free(g);
    }
    {   /* priority: VBlank (bit 0) wins over timer */
        GB *g = gb_with((uint8_t[]){0x00}, 1);
        g->cpu.ime = true;
        g->ie = INT_VBLANK | INT_TIMER;
        g->iflag |= INT_VBLANK | INT_TIMER;
        gb_step(g);
        ASSERT_EQ(g->cpu.pc, 0x0040);
        ASSERT_EQ(g->iflag & INT_VBLANK, 0);
        ASSERT_EQ(g->iflag & INT_TIMER, INT_TIMER); /* still pending */
        gb_free(g);
    }
    {   /* HALT wakes on pending interrupt even with IME=0, no dispatch */
        GB *g = gb_with((uint8_t[]){0x76, 0x3C}, 2);   /* HALT; INC A */
        g->cpu.ime = false;
        g->ie = INT_TIMER;
        gb_step(g);                  /* halts (no pending yet) */
        ASSERT_TRUE(g->cpu.halted);
        gb_step(g);                  /* still halted, 4cy idle */
        g->iflag |= INT_TIMER;
        gb_step(g);                  /* wakes */
        ASSERT_TRUE(!g->cpu.halted);
        gb_step(g);
        ASSERT_EQ(g->cpu.a, 0x02);   /* INC A ran (A was 0x01 at reset) */
        gb_free(g);
    }
    {   /* RETI re-enables IME immediately */
        GB *g = gb_with((uint8_t[]){0xD9}, 1);
        g->cpu.sp = 0xFFFC;
        gb_write8(g, 0xFFFC, 0x00); gb_write8(g, 0xFFFD, 0x05);
        ASSERT_EQ(gb_step(g), 16);
        ASSERT_EQ(g->cpu.pc, 0x0500);
        ASSERT_EQ(g->cpu.ime, true);
        gb_free(g);
    }
    TEST_MAIN_END();
}
```

- [ ] **Step 3: Run, verify failures** — `make test`.

- [ ] **Step 4: Implement**

Interrupt service, above `gb_step`:

```c
static const uint16_t int_vector[5] = {0x40, 0x48, 0x50, 0x58, 0x60};

static bool service_interrupts(GB *g) {
    CPU *c = &g->cpu;
    uint8_t pending = g->ie & g->iflag & 0x1F;
    if (!pending) return false;
    c->halted = false;                 /* pending interrupt always wakes HALT */
    if (!c->ime) return false;
    for (int i = 0; i < 5; i++) {
        if (pending & (1 << i)) {
            c->ime = false;
            c->ime_pending = 0;
            g->iflag &= (uint8_t)~(1 << i);
            internal(g); internal(g);  /* 2 wait M-cycles */
            push16(g, c->pc);          /* 3 M-cycles (1 internal + 2 writes) */
            c->pc = int_vector[i];
            return true;
        }
    }
    return false;
}
```

In `gb_step`, replace the placeholder comment with:

```c
    if (service_interrupts(g))
        return (int)(g->cycles - start);
```

Control-flow cases in `exec`'s switch:

```c
    case 0x18: { int8_t e = (int8_t)fetch8(g); internal(g); c->pc += e; break; }
    case 0x20: case 0x28: case 0x30: case 0x38: {        /* JR cc */
        int8_t e = (int8_t)fetch8(g);
        if (cond(c, (op >> 3) & 3)) { internal(g); c->pc += e; }
        break;
    }
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: {        /* JP cc */
        uint16_t t = fetch16(g);
        if (cond(c, (op >> 3) & 3)) { internal(g); c->pc = t; }
        break;
    }
    case 0xCD: { uint16_t t = fetch16(g); push16(g, c->pc); c->pc = t; break; }
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: {        /* CALL cc */
        uint16_t t = fetch16(g);
        if (cond(c, (op >> 3) & 3)) { push16(g, c->pc); c->pc = t; }
        break;
    }
    case 0xC9: c->pc = pop16(g); internal(g); break;     /* RET */
    case 0xD9: c->pc = pop16(g); internal(g); c->ime = true; break;  /* RETI */
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:          /* RET cc */
        internal(g);
        if (cond(c, (op >> 3) & 3)) { c->pc = pop16(g); internal(g); }
        break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:          /* RST */
        push16(g, c->pc); c->pc = (uint16_t)(op & 0x38); break;
    case 0xE9: c->pc = HL(c); break;                     /* JP HL */
```

- [ ] **Step 5: Run, verify pass** — `make test`.

- [ ] **Step 6: Commit**

```bash
git add src/gb/cpu.c tests/test_cpu_flow.c tests/test_interrupts.c
git commit -m "feat: control flow, interrupt dispatch, HALT wake"
```

---

### Task 10: Timer

**Files:**
- Rewrite: `src/gb/timer.c`
- Create: `tests/test_timer.c`

- [ ] **Step 1: Write failing test `tests/test_timer.c`**

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

int main(void) {
    uint8_t rom[0x8000]; memset(rom, 0, sizeof rom);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);

    {   /* DIV: increments every 256 T-cycles; write resets to 0 */
        gb_reset(g);
        g->div16 = 0;
        gb_tick(g, 255);
        ASSERT_EQ(gb_read8(g, 0xFF04), 0x00);
        gb_tick(g, 1);
        ASSERT_EQ(gb_read8(g, 0xFF04), 0x01);
        gb_write8(g, 0xFF04, 0x55);             /* any write clears */
        ASSERT_EQ(gb_read8(g, 0xFF04), 0x00);
    }
    {   /* TIMA at 262144 Hz (TAC=0b101 -> every 16 T-cycles) */
        gb_reset(g);
        g->div16 = 0;
        gb_write8(g, 0xFF07, 0x05);
        gb_write8(g, 0xFF05, 0x00);
        gb_tick(g, 16 * 10);
        ASSERT_EQ(gb_read8(g, 0xFF05), 10);
    }
    {   /* overflow: TIMA reloads from TMA and raises INT_TIMER */
        gb_reset(g);
        g->div16 = 0;
        gb_write8(g, 0xFF07, 0x05);
        gb_write8(g, 0xFF06, 0xAB);             /* TMA */
        gb_write8(g, 0xFF05, 0xFF);
        g->iflag = 0;
        gb_tick(g, 16);
        ASSERT_EQ(gb_read8(g, 0xFF05), 0xAB);
        ASSERT_EQ(g->iflag & INT_TIMER, INT_TIMER);
    }
    {   /* disabled timer doesn't count */
        gb_reset(g);
        gb_write8(g, 0xFF07, 0x01);             /* freq bits set but enable bit clear */
        gb_write8(g, 0xFF05, 0x00);
        gb_tick(g, 4096);
        ASSERT_EQ(gb_read8(g, 0xFF05), 0x00);
    }
    gb_free(g);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run, verify failure** — `make test`.

- [ ] **Step 3: Implement `src/gb/timer.c`** (replace stub)

```c
#include "gb.h"

/* TAC bits 0-1 select which div16 bit's falling edge clocks TIMA:
   00 -> bit 9 (4096 Hz), 01 -> bit 3 (262144 Hz),
   10 -> bit 5 (65536 Hz), 11 -> bit 7 (16384 Hz). */
static int tac_bit(uint8_t tac) {
    static const int bits[4] = {9, 3, 5, 7};
    return bits[tac & 3];
}

static bool timer_signal(const GB *g) {
    return (g->tac & 0x04) && ((g->div16 >> tac_bit(g->tac)) & 1);
}

static void tima_inc(GB *g) {
    if (++g->tima == 0) {
        g->tima = g->tma;
        g->iflag |= INT_TIMER;
    }
}

void gb_timer_tick(GB *gb, int tcycles) {
    for (int i = 0; i < tcycles; i++) {
        bool before = timer_signal(gb);
        gb->div16++;
        if (before && !timer_signal(gb))   /* falling edge */
            tima_inc(gb);
    }
}

uint8_t gb_timer_read(GB *gb, uint16_t addr) {
    switch (addr) {
    case 0xFF04: return (uint8_t)(gb->div16 >> 8);
    case 0xFF05: return gb->tima;
    case 0xFF06: return gb->tma;
    case 0xFF07: return gb->tac | 0xF8;
    default:     return 0xFF;
    }
}

void gb_timer_write(GB *gb, uint16_t addr, uint8_t v) {
    switch (addr) {
    case 0xFF04: {
        bool before = timer_signal(gb);
        gb->div16 = 0;
        if (before && !timer_signal(gb)) tima_inc(gb);   /* DIV-write edge quirk */
        break;
    }
    case 0xFF05: gb->tima = v; break;
    case 0xFF06: gb->tma = v; break;
    case 0xFF07: {
        bool before = timer_signal(gb);
        gb->tac = v & 0x07;
        if (before && !timer_signal(gb)) tima_inc(gb);
        break;
    }
    }
}
```

- [ ] **Step 4: Run, verify pass** — `make test`.

- [ ] **Step 5: Commit**

```bash
git add src/gb/timer.c tests/test_timer.c
git commit -m "feat: falling-edge timer (DIV/TIMA/TMA/TAC)"
```

---

### Task 11: Blargg acceptance runner

**Files:**
- Create: `tests/blargg.c`

- [ ] **Step 1: Fetch test ROMs**

```bash
git clone --depth 1 https://github.com/retrio/gb-test-roms roms/gb-test-roms
ls roms/gb-test-roms/cpu_instrs/cpu_instrs.gb roms/gb-test-roms/instr_timing/instr_timing.gb
```

Expected: both files exist. (`roms/` is gitignored.)

- [ ] **Step 2: Write `tests/blargg.c`**

```c
/* Runs a blargg test ROM headless; result arrives via serial.
   Exit 0 on "Passed", 1 on "Failed", 2 on timeout. */
#include "../src/gb/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: blargg <rom.gb>\n"); return 2; }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (fread(data, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); return 2; }
    fclose(fp);

    GB *g = gb_new();
    if (!gb_load_rom(g, data, (size_t)size)) { fprintf(stderr, "bad rom\n"); return 2; }
    free(data);
    gb_reset(g);

    /* cpu_instrs takes ~55 emulated seconds; cap at 120 */
    const uint64_t limit = 4194304ULL * 120;
    while (g->cycles < limit) {
        gb_step(g);
        g->serial_buf[g->serial_len] = 0;
        if (strstr(g->serial_buf, "Passed")) {
            printf("PASS %s\n", argv[1]);
            gb_free(g); return 0;
        }
        if (strstr(g->serial_buf, "Failed")) {
            printf("FAIL %s\n----\n%s\n", argv[1], g->serial_buf);
            gb_free(g); return 1;
        }
    }
    g->serial_buf[g->serial_len] = 0;
    printf("TIMEOUT %s\n----\n%s\n", argv[1], g->serial_buf);
    gb_free(g);
    return 2;
}
```

Performance note: `strstr` over the whole buffer every instruction is O(n²)-ish but the
buffer is 8 KB and this is a test tool — fine. If it's annoyingly slow, check only every
65536 steps.

- [ ] **Step 3: Run cpu_instrs**

Run: `make blargg` (or directly: `./build/blargg roms/gb-test-roms/cpu_instrs/cpu_instrs.gb`)
Expected: `PASS .../cpu_instrs.gb`

**If it fails:** the serial output names the failing sub-test (e.g. `03 op sp,hl`).
Run the corresponding individual ROM from `roms/gb-test-roms/cpu_instrs/individual/`
to isolate. Debug with the superpowers:systematic-debugging skill — typical culprits are
flag edge cases in ADC/SBC/DAA, `LD HL,SP+e8` flags, and the HALT bug. Do not guess:
add a unit test reproducing the exact failing case, fix, then re-run.

- [ ] **Step 4: Run instr_timing**

Run: `./build/blargg roms/gb-test-roms/instr_timing/instr_timing.gb`
Expected: `PASS`. Failures here mean a missing/extra `internal(g)` in some instruction —
the ROM prints the failing opcode.

- [ ] **Step 5: Commit**

```bash
git add tests/blargg.c
git commit -m "test: blargg acceptance runner; cpu_instrs + instr_timing pass"
```

---

### Task 12: CI + milestone wrap-up

**Files:**
- Create: `.github/workflows/ci.yml`, `README.md`

- [ ] **Step 1: Create `.github/workflows/ci.yml`**

```yaml
name: ci
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: unit tests
        run: make test
      - name: fetch test roms
        run: git clone --depth 1 https://github.com/retrio/gb-test-roms roms/gb-test-roms
      - name: blargg acceptance
        run: make blargg
```

- [ ] **Step 2: Create `README.md`**

```markdown
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
```

- [ ] **Step 3: Full verification**

Run: `make clean && make test && make blargg`
Expected: all unit tests 0 failures, both ROMs PASS.

- [ ] **Step 4: Commit**

```bash
git add .github README.md
git commit -m "chore: CI workflow + README"
```

---

## Self-review notes (already applied)

- Spec coverage: this plan implements spec §3 CPU/bus/timer/serial-capture and the
  milestone-1 acceptance bar (blargg cpu_instrs + instr_timing). PPU/APU/MBC are
  milestone 2+ by design; `bus.c` keeps all traffic in one dispatch point per the
  provenance-hook requirement. `mem_timing` and mooneye need the PPU's concept of
  time to be meaningful alongside — they join the acceptance bar in milestone 2.
- Type consistency: helper names (`rd`, `wr`, `internal`, `get_r`, `set_r`, `get_rp`,
  `set_rp`, `cond`, `push16`, `pop16`, `alu`, `exec_cb`, `service_interrupts`,
  `gb_timer_*`) are defined once in Task 3/6/8/9/10 and referenced consistently.
- Known soft spots called out inline rather than hidden: EI-delay model (Task 8 note),
  DIV-write edge quirk (Task 10), and the Task 11 debugging protocol if blargg fails.
```
