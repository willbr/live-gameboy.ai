# Debug Panels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the live-gameboy IDE into a real debugger — pause/step execution, bank-aware breakpoints, read/write watchpoints, a disassembly panel, and OAM/tilemap/palette viewers — without losing the live-coding (edit/paint while running) thesis.

**Architecture:** Breakpoint/watchpoint state and the inline checks live in `libgb` (pure C, SDL-free, headless-testable) because the bus is the only choke point watchpoints can observe. The execution-control state machine lives in the IDE layer. All panel rendering moves to a new `src/ide/panels.c`. A new SM83 decoder (`src/gb/disasm.c`) feeds the disassembly panel.

**Tech Stack:** C11, SDL3 (interactive shell only), the project's existing `ui.c` software canvas, the project's hand-rolled test harness (`tests/test.h`). The Makefile globs `src/gb/*.c`, `src/ide/*.c`, and `tests/test_*.c`, so new source/test files are picked up automatically.

---

## File Structure

**New files:**
- `src/gb/debug.h` — `GbDebug` struct, hit kinds, public debug API. Owns breakpoint/watchpoint types.
- `src/gb/debug.c` — attach/detach, add/clear/toggle, and the `gb_debug_check_bp` / `gb_debug_check_wp` hooks.
- `src/gb/disasm.h` / `src/gb/disasm.c` — `gb_disasm()`, one-instruction SM83 decoder.
- `src/ide/panels.h` / `src/ide/panels.c` — all panel rendering (existing panels moved here from `ide.c`, plus the new ones).
- `tests/test_dbg.c` — breakpoint + watchpoint unit tests.
- `tests/test_disasm.c` — decoder unit tests.
- `tests/test_exec.c` — IDE execution-control state-machine tests.

**Modified files:**
- `src/gb/gb.h` — add `struct GbDebug *dbg;` field (forward-declared) to `GB`.
- `src/gb/cpu.c` — breakpoint check at the top of `gb_step`.
- `src/gb/bus.c` — watchpoint checks in `gb_read8` / `gb_write8`.
- `src/gb/gb.c` — NULL-init `dbg` in `gb_new`; detach in `gb_free`.
- `src/ide/ide.h` — `ExecMode`, exec accessors, new `IdePanel` enum values, `GbDebug` accessor, address-field accessors, new canvas dimensions.
- `src/ide/ide.c` — exec state machine, `ide_run_slice`, attach debugger in `ide_new`, address-field state, grown canvas, delegate rendering to `panels.c`.
- `src/ide/ui.h` / `src/ide/ui.c` — single-line text-input widget.
- `src/ide/main.c` — wire new keys (Space/F6/F7/F9/backtick), call `ide_run_slice`, route clicks to disasm/hex panels.
- `Makefile` — wire `mem_timing` into the `blargg` target.
- `tests/test_ui.c` — text-input widget tests.

---

## Phase A — Core debug engine (no UI)

### Task 1: `GbDebug` struct + attach/detach + breakpoint/watchpoint management

**Files:**
- Create: `src/gb/debug.h`
- Create: `src/gb/debug.c`
- Modify: `src/gb/gb.h` (add `dbg` field)
- Modify: `src/gb/gb.c` (init/free)
- Test: `tests/test_dbg.c`

- [ ] **Step 1: Write `src/gb/debug.h`**

```c
#ifndef GB_DEBUG_H
#define GB_DEBUG_H

#include <stdint.h>
#include <stdbool.h>

struct GB;  /* forward decl; debug.c includes gb.h */

#define DBG_MAX_BP 32
#define DBG_MAX_WP 32

typedef struct { uint8_t bank; uint16_t addr; bool enabled; } Breakpoint;
typedef struct { uint16_t addr; bool on_read, on_write, enabled; } Watchpoint;

typedef enum {
    DBG_NONE = 0,
    DBG_BREAKPOINT,
    DBG_WATCH_READ,
    DBG_WATCH_WRITE
} DbgHitKind;

typedef struct GbDebug {
    Breakpoint bp[DBG_MAX_BP]; int bp_count;
    Watchpoint wp[DBG_MAX_WP]; int wp_count;

    bool       hit;          /* set by a check; cleared by gb_debug_resume */
    DbgHitKind hit_kind;
    uint16_t   hit_addr;     /* breakpoint addr or watched addr */
    uint16_t   hit_pc;       /* PC at the moment of the hit */

    bool       skip_next_bp; /* one-shot: don't break on the bp at the current PC */
} GbDebug;

/* Lifecycle: attach allocates and wires gb->dbg; detach frees and NULLs it. */
GbDebug *gb_debug_attach(struct GB *gb);
void     gb_debug_detach(struct GB *gb);

/* Breakpoints. toggle adds if absent (returns index >= 0) or removes if present
   (returns -1). add returns -1 if the table is full. bank is ignored for
   addresses outside 0x4000-0x7FFF. */
int  gb_debug_toggle_bp(struct GB *gb, uint8_t bank, uint16_t addr);
int  gb_debug_find_bp(struct GB *gb, uint8_t bank, uint16_t addr); /* index or -1 */

/* Watchpoints. add returns index or -1 if full. */
int  gb_debug_add_wp(struct GB *gb, uint16_t addr, bool on_read, bool on_write);
void gb_debug_clear_wp(struct GB *gb, int index);

/* Clear a pending hit and arm a one-shot skip so a resume/step from a bp
   address executes that instruction instead of immediately re-breaking. */
void gb_debug_resume(struct GB *gb);

/* Hooks called from the hot path (cpu.c / bus.c). Return true from check_bp to
   tell gb_step to pause WITHOUT executing the current instruction. */
bool gb_debug_check_bp(struct GB *gb);
void gb_debug_check_wp(struct GB *gb, uint16_t addr, bool is_write);

#endif /* GB_DEBUG_H */
```

- [ ] **Step 2: Add the `dbg` field to `GB` in `src/gb/gb.h`**

Find the end of the `GB` struct (the line `bool fetching_opcode;` just before the closing `} GB;`). Add a forward-declared pointer field immediately before the closing brace:

```c
    bool     fetching_opcode;   /* true while cpu.c is fetching the opcode byte (not an operand) */

    struct GbDebug *dbg;        /* optional debugger; NULL = no debugging (zero cost) */
} GB;
```

- [ ] **Step 3: Init/free `dbg` in `src/gb/gb.c`**

In `gb_new`, after the GB is zero-allocated (calloc) `dbg` is already NULL — no change needed there, but add an explicit `g->dbg = NULL;` next to the other field inits to be unambiguous. In `gb_free`, before freeing the GB, add:

```c
    if (gb->dbg) gb_debug_detach(gb);
```

Add `#include "debug.h"` near the top of `src/gb/gb.c`.

- [ ] **Step 4: Write `src/gb/debug.c`**

```c
#include "debug.h"
#include "gb.h"
#include <stdlib.h>
#include <string.h>

GbDebug *gb_debug_attach(GB *gb) {
    if (gb->dbg) return gb->dbg;
    GbDebug *d = (GbDebug *)calloc(1, sizeof(GbDebug));
    gb->dbg = d;
    return d;
}

void gb_debug_detach(GB *gb) {
    free(gb->dbg);
    gb->dbg = NULL;
}

int gb_debug_find_bp(GB *gb, uint8_t bank, uint16_t addr) {
    GbDebug *d = gb->dbg;
    if (!d) return -1;
    bool banked = (addr >= 0x4000 && addr < 0x8000);
    for (int i = 0; i < d->bp_count; i++) {
        if (!d->bp[i].enabled) continue;
        if (d->bp[i].addr != addr) continue;
        if (banked && d->bp[i].bank != bank) continue;
        return i;
    }
    return -1;
}

int gb_debug_toggle_bp(GB *gb, uint8_t bank, uint16_t addr) {
    GbDebug *d = gb->dbg;
    if (!d) return -1;
    int existing = gb_debug_find_bp(gb, bank, addr);
    if (existing >= 0) {
        /* remove by compaction */
        for (int i = existing; i < d->bp_count - 1; i++) d->bp[i] = d->bp[i + 1];
        d->bp_count--;
        return -1;
    }
    if (d->bp_count >= DBG_MAX_BP) return -1;
    d->bp[d->bp_count] = (Breakpoint){ bank, addr, true };
    return d->bp_count++;
}

int gb_debug_add_wp(GB *gb, uint16_t addr, bool on_read, bool on_write) {
    GbDebug *d = gb->dbg;
    if (!d || d->wp_count >= DBG_MAX_WP) return -1;
    d->wp[d->wp_count] = (Watchpoint){ addr, on_read, on_write, true };
    return d->wp_count++;
}

void gb_debug_clear_wp(GB *gb, int index) {
    GbDebug *d = gb->dbg;
    if (!d || index < 0 || index >= d->wp_count) return;
    for (int i = index; i < d->wp_count - 1; i++) d->wp[i] = d->wp[i + 1];
    d->wp_count--;
}

void gb_debug_resume(GB *gb) {
    GbDebug *d = gb->dbg;
    if (!d) return;
    d->hit = false;
    d->hit_kind = DBG_NONE;
    d->skip_next_bp = true;
}

bool gb_debug_check_bp(GB *gb) {
    GbDebug *d = gb->dbg;
    bool skip = d->skip_next_bp;
    d->skip_next_bp = false;
    if (skip || d->bp_count == 0) return false;
    uint16_t pc = gb->cpu.pc;
    int idx = gb_debug_find_bp(gb, gb->rom_bank, pc);
    if (idx < 0) return false;
    d->hit = true;
    d->hit_kind = DBG_BREAKPOINT;
    d->hit_addr = pc;
    d->hit_pc = pc;
    return true;
}

void gb_debug_check_wp(GB *gb, uint16_t addr, bool is_write) {
    GbDebug *d = gb->dbg;
    for (int i = 0; i < d->wp_count; i++) {
        Watchpoint *w = &d->wp[i];
        if (!w->enabled || w->addr != addr) continue;
        if (is_write ? !w->on_write : !w->on_read) continue;
        d->hit = true;
        d->hit_kind = is_write ? DBG_WATCH_WRITE : DBG_WATCH_READ;
        d->hit_addr = addr;
        d->hit_pc = gb->cpu.pc;
        return;
    }
}
```

- [ ] **Step 5: Write the failing test `tests/test_dbg.c` (management only, for now)**

```c
#include "test.h"
#include "../src/gb/gb.h"
#include "../src/gb/debug.h"

int main(void) {
    GB *g = gb_new();
    GbDebug *d = gb_debug_attach(g);
    CHECK(d != NULL);
    CHECK(g->dbg == d);

    /* toggle adds then removes */
    int i = gb_debug_toggle_bp(g, 0, 0x0150);
    CHECK(i == 0);
    CHECK(d->bp_count == 1);
    CHECK(gb_debug_find_bp(g, 0, 0x0150) == 0);
    CHECK(gb_debug_toggle_bp(g, 0, 0x0150) == -1);  /* removed */
    CHECK(d->bp_count == 0);

    /* bank-gated find: a banked bp only matches its bank */
    gb_debug_toggle_bp(g, 2, 0x4000);
    CHECK(gb_debug_find_bp(g, 2, 0x4000) == 0);
    CHECK(gb_debug_find_bp(g, 3, 0x4000) == -1);

    /* watchpoint add/clear */
    int w = gb_debug_add_wp(g, 0xC000, true, true);
    CHECK(w == 0 && d->wp_count == 1);
    gb_debug_clear_wp(g, 0);
    CHECK(d->wp_count == 0);

    gb_free(g);
    DONE();
}
```

(Note: `CHECK` and `DONE` are this project's macros from `tests/test.h`; mirror the style of an existing test such as `tests/test_timer.c` if signatures differ — open it first and match its harness exactly.)

- [ ] **Step 6: Run the test — expect a build/link, then PASS**

Run: `make build/test_dbg && ./build/test_dbg`
Expected: `tests/test_dbg.c  N assertions, 0 failures`

- [ ] **Step 7: Run the full core test suite to confirm no regressions (the new `dbg` field must not perturb anything)**

Run: `make test 2>&1 | tail -5`
Expected: all test files report `0 failures`.

- [ ] **Step 8: Commit**

```bash
git add src/gb/debug.h src/gb/debug.c src/gb/gb.h src/gb/gb.c tests/test_dbg.c
git commit -m "feat(dbg): GbDebug struct + breakpoint/watchpoint management"
```

---

### Task 2: Breakpoint check wired into `gb_step`

**Files:**
- Modify: `src/gb/cpu.c:194-213` (`gb_step`)
- Test: `tests/test_dbg.c` (extend)

- [ ] **Step 1: Add the failing test to `tests/test_dbg.c`**

Insert before `gb_free(g)` in `main`. This builds a tiny ROM that spins in a loop and asserts a breakpoint halts execution exactly at its address, with the instruction NOT yet executed:

```c
    /* --- breakpoint halts at the right PC, instruction not yet run --- */
    {
        GB *g2 = gb_new();
        /* Minimal ROM: at 0x0150  INC A (3C); 0x0151 JP 0x0150 (C3 50 01). */
        static uint8_t rom[0x8000];
        memset(rom, 0, sizeof rom);
        rom[0x0150] = 0x3C;                       /* INC A      */
        rom[0x0151] = 0xC3; rom[0x0152] = 0x50; rom[0x0153] = 0x01; /* JP 0150 */
        gb_load_rom(g2, rom, sizeof rom);
        gb_reset(g2);
        g2->cpu.pc = 0x0150;
        g2->cpu.a = 0;

        GbDebug *d2 = gb_debug_attach(g2);
        gb_debug_toggle_bp(g2, 0, 0x0151);  /* break on the JP */

        int steps = 0;
        while (!d2->hit && steps < 100) { gb_step(g2); steps++; }
        CHECK(d2->hit);
        CHECK(d2->hit_kind == DBG_BREAKPOINT);
        CHECK(d2->hit_pc == 0x0151);
        CHECK(g2->cpu.pc == 0x0151);   /* paused AT the bp */
        CHECK(g2->cpu.a == 1);         /* only the INC A ran, once */

        /* resume arms a one-shot skip so we step past the bp instead of re-breaking */
        gb_debug_resume(g2);
        CHECK(!d2->hit);
        gb_step(g2);                   /* executes the JP, lands back at 0150 */
        CHECK(!d2->hit);
        CHECK(g2->cpu.pc == 0x0150);
        gb_free(g2);
    }
```

- [ ] **Step 2: Run the test — expect FAIL (breakpoint never fires)**

Run: `make build/test_dbg && ./build/test_dbg`
Expected: FAIL at `CHECK(d2->hit)` — the hook isn't wired yet.

- [ ] **Step 3: Wire the breakpoint check into `gb_step`**

In `src/gb/cpu.c`, at the very top of `gb_step` (right after `CPU *c = &g->cpu;`), add the hook. It must run before interrupt servicing and fetch so we pause on the instruction boundary:

```c
int gb_step(GB *g) {
    uint64_t start = g->cycles;
    CPU *c = &g->cpu;

    if (g->dbg && gb_debug_check_bp(g))
        return 0;   /* paused at a breakpoint; instruction not executed */

    if (service_interrupts(g))
        return (int)(g->cycles - start);
    /* ... rest unchanged ... */
```

Add `#include "debug.h"` near the top of `src/gb/cpu.c` if not already pulled in via `gb.h`.

- [ ] **Step 4: Run the test — expect PASS**

Run: `make build/test_dbg && ./build/test_dbg`
Expected: `0 failures`.

- [ ] **Step 5: Confirm zero-cost path — blargg still passes (no debugger attached there)**

Run: `make blargg`
Expected: `PASS` for cpu_instrs and instr_timing.

- [ ] **Step 6: Commit**

```bash
git add src/gb/cpu.c tests/test_dbg.c
git commit -m "feat(dbg): pause at bank-aware PC breakpoints in gb_step"
```

---

### Task 3: Watchpoint checks wired into the bus

**Files:**
- Modify: `src/gb/bus.c` (`gb_read8`, `gb_write8`)
- Test: `tests/test_dbg.c` (extend)

- [ ] **Step 1: Add the failing test to `tests/test_dbg.c`**

```c
    /* --- write watchpoint fires; read watchpoint fires --- */
    {
        GB *g3 = gb_new();
        static uint8_t rom3[0x8000];
        memset(rom3, 0, sizeof rom3);
        /* 0150: LD A,$AA (3E AA); 0152: LD ($C000),A (EA 00 C0); 0155: LD A,($C000) (FA 00 C0); 0158: JR 0158 (18 FE) */
        rom3[0x0150]=0x3E; rom3[0x0151]=0xAA;
        rom3[0x0152]=0xEA; rom3[0x0153]=0x00; rom3[0x0154]=0xC0;
        rom3[0x0155]=0xFA; rom3[0x0156]=0x00; rom3[0x0157]=0xC0;
        rom3[0x0158]=0x18; rom3[0x0159]=0xFE;
        gb_load_rom(g3, rom3, sizeof rom3);
        gb_reset(g3);
        g3->cpu.pc = 0x0150;
        GbDebug *d3 = gb_debug_attach(g3);

        gb_debug_add_wp(g3, 0xC000, false, true);  /* write-only */
        int s = 0; while (!d3->hit && s < 50) { gb_step(g3); s++; }
        CHECK(d3->hit);
        CHECK(d3->hit_kind == DBG_WATCH_WRITE);
        CHECK(d3->hit_addr == 0xC000);

        /* swap to a read watchpoint and continue */
        d3->wp_count = 0;
        gb_debug_add_wp(g3, 0xC000, true, false);  /* read-only */
        gb_debug_resume(g3);
        s = 0; while (!d3->hit && s < 50) { gb_step(g3); s++; }
        CHECK(d3->hit);
        CHECK(d3->hit_kind == DBG_WATCH_READ);
        gb_free(g3);
    }
```

- [ ] **Step 2: Run the test — expect FAIL (watchpoints never fire)**

Run: `make build/test_dbg && ./build/test_dbg`
Expected: FAIL at the first `CHECK(d3->hit)`.

- [ ] **Step 3: Wire watchpoint checks into the bus**

In `src/gb/bus.c`, add `#include "debug.h"` near the top if not already present. At the **top** of `gb_read8`, before any dispatch:

```c
uint8_t gb_read8(GB *gb, uint16_t a) {
    if (gb->dbg && gb->dbg->wp_count > 0 && !gb->fetching_opcode)
        gb_debug_check_wp(gb, a, false);
    if (a < 0x4000) {
        /* ... existing body unchanged ... */
```

(The `!fetching_opcode` guard keeps opcode fetches from tripping read watchpoints — a watchpoint means "data access," matching the project's existing provenance convention.)

At the **top** of `gb_write8`, before any dispatch:

```c
void gb_write8(GB *gb, uint16_t a, uint8_t v) {
    if (gb->dbg && gb->dbg->wp_count > 0)
        gb_debug_check_wp(gb, a, true);
    /* MBC1 register writes */
    if (a < 0x8000) {
        /* ... existing body unchanged ... */
```

- [ ] **Step 4: Run the test — expect PASS**

Run: `make build/test_dbg && ./build/test_dbg`
Expected: `0 failures`.

- [ ] **Step 5: Confirm no regressions across the full suite (the bus is on every memory path)**

Run: `make test 2>&1 | grep -i fail | tail; make blargg`
Expected: no `failures`; blargg `PASS`.

- [ ] **Step 6: Commit**

```bash
git add src/gb/bus.c tests/test_dbg.c
git commit -m "feat(dbg): read/write watchpoints in the bus"
```

---

## Phase B — Disassembler

### Task 4: `gb_disasm` one-instruction SM83 decoder

**Files:**
- Create: `src/gb/disasm.h`, `src/gb/disasm.c`
- Test: `tests/test_disasm.c`

- [ ] **Step 1: Write `src/gb/disasm.h`**

```c
#ifndef GB_DISASM_H
#define GB_DISASM_H

#include <stdint.h>
struct GB;

/* Decode one SM83 instruction located at CPU address `addr` (read through the
   bus view: bank 0 region direct, 0x4000-0x7FFF uses `bank`). Writes a
   human-readable string into out[0..out_sz-1] and returns the instruction's
   length in bytes (1..3). Never reads more than 3 bytes. */
int gb_disasm(struct GB *gb, uint8_t bank, uint16_t addr, char *out, int out_sz);

#endif /* GB_DISASM_H */
```

- [ ] **Step 2: Write the failing test `tests/test_disasm.c`**

```c
#include "test.h"
#include "../src/gb/gb.h"
#include "../src/gb/disasm.h"
#include <string.h>

static GB *with_rom(const uint8_t *bytes, int n) {
    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    memcpy(rom + 0x0150, bytes, (size_t)n);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}

static void expect(const uint8_t *bytes, int n, const char *text, int len) {
    GB *g = with_rom(bytes, n);
    char out[32];
    int l = gb_disasm(g, 0, 0x0150, out, sizeof out);
    if (strcmp(out, text) != 0) { fprintf(stderr, "got '%s' want '%s'\n", out, text); }
    CHECK(strcmp(out, text) == 0);
    CHECK(l == len);
    gb_free(g);
}

int main(void) {
    expect((uint8_t[]){0x00}, 1, "NOP", 1);
    expect((uint8_t[]){0x3C}, 1, "INC A", 1);
    expect((uint8_t[]){0x76}, 1, "HALT", 1);
    expect((uint8_t[]){0x47}, 1, "LD B,A", 1);
    expect((uint8_t[]){0x3E,0xAA}, 2, "LD A,$AA", 2);
    expect((uint8_t[]){0x01,0x34,0x12}, 3, "LD BC,$1234", 3);
    expect((uint8_t[]){0xC3,0x50,0x01}, 3, "JP $0150", 3);
    expect((uint8_t[]){0x18,0xFE}, 2, "JR $0150", 2);   /* -2 from end of insn (0x0152) */
    expect((uint8_t[]){0xCA,0x00,0x40}, 3, "JP Z,$4000", 3);
    expect((uint8_t[]){0xEA,0x00,0xC0}, 3, "LD ($C000),A", 3);
    expect((uint8_t[]){0xF0,0x44}, 2, "LDH A,($FF44)", 2);
    expect((uint8_t[]){0xCB,0x11}, 2, "RL C", 2);
    expect((uint8_t[]){0xCB,0x7E}, 2, "BIT 7,(HL)", 2);
    expect((uint8_t[]){0xC7}, 1, "RST $00", 1);
    DONE();
}
```

- [ ] **Step 3: Run the test — expect FAIL (link error: `gb_disasm` undefined)**

Run: `make build/test_disasm && ./build/test_disasm`
Expected: link/compile failure.

- [ ] **Step 4: Write `src/gb/disasm.c`**

```c
#include "disasm.h"
#include "gb.h"
#include <stdio.h>
#include <string.h>

static const char *R[8]   = {"B","C","D","E","H","L","(HL)","A"};
static const char *RP[4]  = {"BC","DE","HL","SP"};
static const char *RP2[4] = {"BC","DE","HL","AF"};
static const char *CC[4]  = {"NZ","Z","NC","C"};
static const char *ALU[8] = {"ADD A,","ADC A,","SUB ","SBC A,","AND ","XOR ","OR ","CP "};
static const char *ROT[8] = {"RLC","RRC","RL","RR","SLA","SRA","SWAP","SRL"};

/* Read a byte at addr through the (bank, addr) view. */
static uint8_t rd(GB *gb, uint8_t bank, uint16_t addr) {
    if (addr < 0x4000) return gb->rom[addr];
    if (addr < 0x8000) {
        uint32_t off = (uint32_t)bank * 0x4000u + (addr - 0x4000u);
        return off < gb->rom_size ? gb->rom[off] : 0xFF;
    }
    return gb_read8(gb, addr);  /* RAM views: best-effort, no side effects we care about */
}

int gb_disasm(GB *gb, uint8_t bank, uint16_t addr, char *out, int out_sz) {
    uint8_t op = rd(gb, bank, addr);
    uint8_t d8 = rd(gb, bank, addr + 1);
    uint8_t d8b = rd(gb, bank, addr + 2);
    uint16_t d16 = (uint16_t)(d8 | (d8b << 8));
    int8_t e = (int8_t)d8;
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

    char b[32];

    if (op == 0xCB) {
        uint8_t cb = d8;
        int cx = cb >> 6, cy = (cb >> 3) & 7, cz = cb & 7;
        if (cx == 0)      snprintf(b, sizeof b, "%s %s", ROT[cy], R[cz]);
        else if (cx == 1) snprintf(b, sizeof b, "BIT %d,%s", cy, R[cz]);
        else if (cx == 2) snprintf(b, sizeof b, "RES %d,%s", cy, R[cz]);
        else              snprintf(b, sizeof b, "SET %d,%s", cy, R[cz]);
        snprintf(out, (size_t)out_sz, "%s", b);
        return 2;
    }

    if (x == 1) {
        if (op == 0x76) { snprintf(out, (size_t)out_sz, "HALT"); return 1; }
        snprintf(out, (size_t)out_sz, "LD %s,%s", R[y], R[z]); return 1;
    }
    if (x == 2) { snprintf(out, (size_t)out_sz, "%s%s", ALU[y], R[z]); return 1; }

    if (x == 0) {
        switch (z) {
        case 0:
            if (y == 0) { snprintf(out, (size_t)out_sz, "NOP"); return 1; }
            if (y == 1) { snprintf(out, (size_t)out_sz, "LD ($%04X),SP", d16); return 3; }
            if (y == 2) { snprintf(out, (size_t)out_sz, "STOP"); return 2; }
            if (y == 3) { snprintf(out, (size_t)out_sz, "JR $%04X", (uint16_t)(addr + 2 + e)); return 2; }
            snprintf(out, (size_t)out_sz, "JR %s,$%04X", CC[y - 4], (uint16_t)(addr + 2 + e)); return 2;
        case 1:
            if (q == 0) { snprintf(out, (size_t)out_sz, "LD %s,$%04X", RP[p], d16); return 3; }
            snprintf(out, (size_t)out_sz, "ADD HL,%s", RP[p]); return 1;
        case 2: {
            const char *m[8] = {"LD (BC),A","LD (DE),A","LD (HL+),A","LD (HL-),A",
                                "LD A,(BC)","LD A,(DE)","LD A,(HL+)","LD A,(HL-)"};
            snprintf(out, (size_t)out_sz, "%s", m[(q << 2) | p]); return 1;
        }
        case 3:
            snprintf(out, (size_t)out_sz, q ? "DEC %s" : "INC %s", RP[p]); return 1;
        case 4: snprintf(out, (size_t)out_sz, "INC %s", R[y]); return 1;
        case 5: snprintf(out, (size_t)out_sz, "DEC %s", R[y]); return 1;
        case 6: snprintf(out, (size_t)out_sz, "LD %s,$%02X", R[y], d8); return 2;
        case 7: {
            const char *m[8] = {"RLCA","RRCA","RLA","RRA","DAA","CPL","SCF","CCF"};
            snprintf(out, (size_t)out_sz, "%s", m[y]); return 1;
        }
        }
    }

    /* x == 3 */
    switch (z) {
    case 0:
        if (y < 4) { snprintf(out, (size_t)out_sz, "RET %s", CC[y]); return 1; }
        if (y == 4) { snprintf(out, (size_t)out_sz, "LDH ($FF%02X),A", d8); return 2; }
        if (y == 5) { snprintf(out, (size_t)out_sz, "ADD SP,%d", e); return 2; }
        if (y == 6) { snprintf(out, (size_t)out_sz, "LDH A,($FF%02X)", d8); return 2; }
        snprintf(out, (size_t)out_sz, "LD HL,SP+%d", e); return 2;
    case 1:
        if (q == 0) { snprintf(out, (size_t)out_sz, "POP %s", RP2[p]); return 1; }
        switch (p) {
        case 0: snprintf(out, (size_t)out_sz, "RET"); return 1;
        case 1: snprintf(out, (size_t)out_sz, "RETI"); return 1;
        case 2: snprintf(out, (size_t)out_sz, "JP HL"); return 1;
        default: snprintf(out, (size_t)out_sz, "LD SP,HL"); return 1;
        }
    case 2:
        if (y < 4) { snprintf(out, (size_t)out_sz, "JP %s,$%04X", CC[y], d16); return 3; }
        if (y == 4) { snprintf(out, (size_t)out_sz, "LDH (C),A"); return 1; }
        if (y == 5) { snprintf(out, (size_t)out_sz, "LD ($%04X),A", d16); return 3; }
        if (y == 6) { snprintf(out, (size_t)out_sz, "LDH A,(C)"); return 1; }
        snprintf(out, (size_t)out_sz, "LD A,($%04X)", d16); return 3;
    case 3:
        if (y == 0) { snprintf(out, (size_t)out_sz, "JP $%04X", d16); return 3; }
        if (y == 6) { snprintf(out, (size_t)out_sz, "DI"); return 1; }
        if (y == 7) { snprintf(out, (size_t)out_sz, "EI"); return 1; }
        snprintf(out, (size_t)out_sz, "DB $%02X", op); return 1;  /* removed/illegal */
    case 4:
        if (y < 4) { snprintf(out, (size_t)out_sz, "CALL %s,$%04X", CC[y], d16); return 3; }
        snprintf(out, (size_t)out_sz, "DB $%02X", op); return 1;
    case 5:
        if (q == 0) { snprintf(out, (size_t)out_sz, "PUSH %s", RP2[p]); return 1; }
        if (p == 0) { snprintf(out, (size_t)out_sz, "CALL $%04X", d16); return 3; }
        snprintf(out, (size_t)out_sz, "DB $%02X", op); return 1;
    case 6: snprintf(out, (size_t)out_sz, "%s$%02X", ALU[y], d8); return 2;
    case 7: snprintf(out, (size_t)out_sz, "RST $%02X", y * 8); return 1;
    }

    snprintf(out, (size_t)out_sz, "DB $%02X", op);
    return 1;
}
```

- [ ] **Step 5: Run the test — expect PASS**

Run: `make build/test_disasm && ./build/test_disasm`
Expected: `0 failures`. If any mnemonic mismatches, fix the format string to match the test's expected text exactly (the test is the source of truth for formatting).

- [ ] **Step 6: Commit**

```bash
git add src/gb/disasm.h src/gb/disasm.c tests/test_disasm.c
git commit -m "feat(dbg): SM83 one-instruction disassembler"
```

---

## Phase C — Execution control in the IDE

### Task 5: `ExecMode` state machine + `ide_run_slice`

**Files:**
- Modify: `src/ide/ide.h` (ExecMode + accessors)
- Modify: `src/ide/ide.c` (state field, attach debugger, run_slice, accessors)
- Test: `tests/test_exec.c`

- [ ] **Step 1: Add the exec API to `src/ide/ide.h`**

Add after the existing `#include` lines and the `IdeState` typedef:

```c
typedef enum {
    EXEC_RUNNING = 0,
    EXEC_PAUSED,
    EXEC_STEP_INSN,
    EXEC_STEP_LINE,
    EXEC_STEP_FRAME
} ExecMode;

/* Advance execution by one host-frame "slice", honoring the current ExecMode.
 * RUNNING: run until frame_ready or a debug hit. STEP_*: do the step then PAUSE.
 * PAUSED: no-op. A breakpoint/watchpoint hit transitions to PAUSED. */
void     ide_run_slice(IdeState *s);

ExecMode ide_exec_mode(IdeState *s);
void     ide_pause(IdeState *s);
void     ide_resume(IdeState *s);              /* clears any hit, runs */
void     ide_step_insn(IdeState *s);
void     ide_step_line(IdeState *s);
void     ide_step_frame_once(IdeState *s);

/* The debugger attached to the session's GB (never NULL after ide_new). */
struct GbDebug *ide_debug(IdeState *s);
```

Add `struct GbDebug;` forward declaration near the top of `ide.h` (after the includes).

- [ ] **Step 2: Write the failing test `tests/test_exec.c`**

```c
#include "test.h"
#include "../src/ide/ide.h"
#include "../src/gb/gb.h"
#include "../src/gb/debug.h"

int main(void) {
    IdeState *s = ide_new("examples/hello.asm");
    CHECK(s != NULL);
    GB *g = ide_gb(s);
    CHECK(g != NULL);
    CHECK(ide_debug(s) != NULL);       /* ide_new attaches a debugger */
    CHECK(ide_exec_mode(s) == EXEC_RUNNING);

    /* pause, then single-step one instruction: PC must change, mode returns to PAUSED */
    ide_pause(s);
    CHECK(ide_exec_mode(s) == EXEC_PAUSED);
    uint16_t pc0 = g->cpu.pc;
    uint64_t cyc0 = g->cycles;
    ide_step_insn(s);
    ide_run_slice(s);
    CHECK(g->cpu.pc != pc0);
    CHECK(g->cycles > cyc0);
    CHECK(ide_exec_mode(s) == EXEC_PAUSED);

    /* step one scanline: ly advances */
    uint8_t ly0 = g->ly;
    ide_step_line(s);
    ide_run_slice(s);
    CHECK(g->ly != ly0 || g->frame_ready);

    /* a write watchpoint pauses a RUNNING session: hello.asm writes serial FF01 */
    gb_debug_add_wp(g, 0xFF01, false, true);
    ide_resume(s);
    CHECK(ide_exec_mode(s) == EXEC_RUNNING);
    int slices = 0;
    while (ide_exec_mode(s) == EXEC_RUNNING && slices < 600) { ide_run_slice(s); slices++; }
    CHECK(ide_exec_mode(s) == EXEC_PAUSED);
    CHECK(ide_debug(s)->hit_kind == DBG_WATCH_WRITE);
    CHECK(ide_debug(s)->hit_addr == 0xFF01);

    ide_free(s);
    DONE();
}
```

- [ ] **Step 3: Run the test — expect FAIL (functions undefined)**

Run: `make build/test_exec && ./build/test_exec`
Expected: link failure / undefined `ide_run_slice`.

- [ ] **Step 4: Implement in `src/ide/ide.c`**

Add `#include "../gb/debug.h"` near the top. Add an `ExecMode exec_mode;` field to `struct IdeState`. In `ide_new`, after the `GB` exists, attach a debugger and default to running:

```c
    gb_debug_attach(s->gb);
    s->exec_mode = EXEC_RUNNING;
```

(Place this where the rest of `IdeState` is initialized; if `ide_new` has both `.asm` and `.gb` branches, do it after both converge on a valid `s->gb`.)

Then add the implementations near `ide_step_frame`:

```c
ExecMode ide_exec_mode(IdeState *s) { return s ? s->exec_mode : EXEC_PAUSED; }
struct GbDebug *ide_debug(IdeState *s) { return s ? s->gb->dbg : NULL; }

void ide_pause(IdeState *s)            { if (s) s->exec_mode = EXEC_PAUSED; }
void ide_resume(IdeState *s)           { if (s) { gb_debug_resume(s->gb); s->exec_mode = EXEC_RUNNING; } }
void ide_step_insn(IdeState *s)        { if (s) { gb_debug_resume(s->gb); s->exec_mode = EXEC_STEP_INSN; } }
void ide_step_line(IdeState *s)        { if (s) { gb_debug_resume(s->gb); s->exec_mode = EXEC_STEP_LINE; } }
void ide_step_frame_once(IdeState *s)  { if (s) { gb_debug_resume(s->gb); s->exec_mode = EXEC_STEP_FRAME; } }

static bool dbg_hit(GB *g) { return g->dbg && g->dbg->hit; }

void ide_run_slice(IdeState *s) {
    if (!s) return;
    GB *g = s->gb;
    switch (s->exec_mode) {
    case EXEC_PAUSED:
        return;
    case EXEC_RUNNING:
        g->frame_ready = false;
        while (!g->frame_ready) {
            gb_step(g);
            if (dbg_hit(g)) { s->exec_mode = EXEC_PAUSED; return; }
        }
        s->frame_counter++;
        return;
    case EXEC_STEP_INSN:
        gb_step(g);
        s->exec_mode = EXEC_PAUSED;
        return;
    case EXEC_STEP_LINE: {
        uint8_t ly0 = g->ly;
        do { gb_step(g); } while (g->ly == ly0 && !g->frame_ready && !dbg_hit(g));
        s->exec_mode = EXEC_PAUSED;
        return;
    }
    case EXEC_STEP_FRAME:
        g->frame_ready = false;
        while (!g->frame_ready && !dbg_hit(g)) gb_step(g);
        s->exec_mode = EXEC_PAUSED;
        return;
    }
}
```

Note: `ide_step_frame` (the existing always-run-a-frame helper used by `ide_shot` and `test_ide_render`) stays unchanged so headless screenshots remain deterministic.

- [ ] **Step 5: Run the test — expect PASS**

Run: `make build/test_exec && ./build/test_exec`
Expected: `0 failures`.

- [ ] **Step 6: Full suite green**

Run: `make test 2>&1 | grep -i fail | tail`
Expected: no `failures`.

- [ ] **Step 7: Commit**

```bash
git add src/ide/ide.h src/ide/ide.c tests/test_exec.c
git commit -m "feat(ide): execution-control state machine (pause/step/run-slice)"
```

---

## Phase D — Text-input widget

### Task 6: Single-line text-input field in `ui.c`

**Files:**
- Modify: `src/ide/ui.h`, `src/ide/ui.c`
- Test: `tests/test_ui.c`

- [ ] **Step 1: Add the widget API to `src/ide/ui.h`**

```c
/* Single-line text input. Fixed-capacity, caret at the end (no mid-line edit
   in v1). Reused for the debugger address field and later the code editor. */
#define TEXTFIELD_CAP 32
typedef struct {
    char text[TEXTFIELD_CAP];
    int  len;
    bool active;
} TextField;

void textfield_clear(TextField *t);
void textfield_putc(TextField *t, char ch);   /* append a printable char if room */
void textfield_backspace(TextField *t);
/* Render at (x,y): the text plus a caret block when active. */
void textfield_render(Canvas *c, int x, int y, const TextField *t,
                      uint32_t fg, uint32_t bg);
```

Add `#include <stdbool.h>` to `ui.h` if not already present.

- [ ] **Step 2: Add failing tests to `tests/test_ui.c`**

Add inside `main` (before the existing `DONE()`):

```c
    {
        TextField t; textfield_clear(&t);
        CHECK(t.len == 0);
        textfield_putc(&t, 'C'); textfield_putc(&t, '0');
        textfield_putc(&t, '0'); textfield_putc(&t, '0');
        CHECK(t.len == 4);
        CHECK(strcmp(t.text, "C000") == 0);
        textfield_backspace(&t);
        CHECK(t.len == 3);
        CHECK(strcmp(t.text, "C00") == 0);
        /* non-printable rejected */
        textfield_putc(&t, '\n');
        CHECK(t.len == 3);
        /* capacity guard */
        textfield_clear(&t);
        for (int i = 0; i < 100; i++) textfield_putc(&t, 'A');
        CHECK(t.len == TEXTFIELD_CAP - 1);
    }
```

Ensure `tests/test_ui.c` has `#include <string.h>`.

- [ ] **Step 3: Run the test — expect FAIL**

Run: `make build/test_ui && ./build/test_ui`
Expected: FAIL (undefined `textfield_*`).

- [ ] **Step 4: Implement in `src/ide/ui.c`**

```c
void textfield_clear(TextField *t) { t->text[0] = '\0'; t->len = 0; t->active = false; }

void textfield_putc(TextField *t, char ch) {
    if (ch < 0x20 || ch > 0x7E) return;          /* printable ASCII only */
    if (t->len >= TEXTFIELD_CAP - 1) return;
    t->text[t->len++] = ch;
    t->text[t->len] = '\0';
}

void textfield_backspace(TextField *t) {
    if (t->len > 0) t->text[--t->len] = '\0';
}

void textfield_render(Canvas *c, int x, int y, const TextField *t,
                      uint32_t fg, uint32_t bg) {
    ui_text_bg(c, x, y, t->text, fg, bg);
    if (t->active) ui_fill_rect(c, x + t->len * 8, y, 8, 8, fg);  /* caret block */
}
```

- [ ] **Step 5: Run the test — expect PASS**

Run: `make build/test_ui && ./build/test_ui`
Expected: `0 failures`.

- [ ] **Step 6: Commit**

```bash
git add src/ide/ui.h src/ide/ui.c tests/test_ui.c
git commit -m "feat(ide): single-line text-input widget"
```

---

## Phase E — Panels + layout

### Task 7: Grow the canvas, define the new layout, split rendering into `panels.c`

**Files:**
- Modify: `src/ide/ide.h` (canvas dims, IdePanel enum, panel-rect table)
- Modify: `src/ide/ide.c` (panel rects, delegate to panels.c)
- Create: `src/ide/panels.h`, `src/ide/panels.c`
- Test: `tests/test_ide_render.c` (re-bake), plus `make ide-shot` visual check

- [ ] **Step 1: Update canvas dimensions and the `IdePanel` enum in `src/ide/ide.h`**

Change the documented canvas size to `1024 x 720`, and extend the enum (keep the existing values' order so old code keeps working; append the new ones):

```c
typedef enum {
    PANEL_GAME        = 0,
    PANEL_REGISTERS   = 1,
    PANEL_VRAM_TILES  = 2,
    PANEL_CODE        = 3,
    PANEL_TILE_EDITOR = 4,
    PANEL_MEM_HEX     = 5,
    PANEL_STATUS      = 6,
    PANEL_EXEC        = 7,
    PANEL_DISASM      = 8,
    PANEL_PALETTE     = 9,
    PANEL_OAM         = 10,
    PANEL_TILEMAP     = 11,
    PANEL_ADDR_INPUT  = 12,
    PANEL_COUNT
} IdePanel;

#define IDE_CANVAS_W 1024
#define IDE_CANVAS_H 720
```

- [ ] **Step 2: Update the panel-rect table in `src/ide/ide.c`**

Replace the existing `rects[]` table (the `static const struct {int x,y,w,h;} rects[...]` near `ide_panel_rect`) with the full new layout. Match this exact set of rectangles (used to re-bake the screenshot hash):

```c
static const struct { int x, y, w, h; } PANEL_RECTS[PANEL_COUNT] = {
    [PANEL_GAME]        = {   8,   8, 336, 304 },
    [PANEL_REGISTERS]   = { 352,   8, 320, 120 },
    [PANEL_EXEC]        = { 352, 136, 320,  62 },
    [PANEL_DISASM]      = { 352, 206, 320, 392 },
    [PANEL_VRAM_TILES]  = { 680,   8, 336, 148 },
    [PANEL_PALETTE]     = { 680, 164, 336,  36 },
    [PANEL_OAM]         = { 680, 208, 336, 192 },
    [PANEL_TILEMAP]     = { 680, 408, 336, 190 },
    [PANEL_CODE]        = {   8, 320, 336, 150 },
    [PANEL_TILE_EDITOR] = {   8, 478, 336, 120 },
    [PANEL_MEM_HEX]     = {   8, 606,1008,  80 },
    [PANEL_ADDR_INPUT]  = {   8, 690,1008,  12 },
    [PANEL_STATUS]      = {   8, 706,1008,   8 },
};
```

Update `ide_panel_rect` to index `PANEL_RECTS`. Update the `ide.c` header-comment layout table to match.

- [ ] **Step 3: Create `src/ide/panels.h`**

```c
#ifndef IDE_PANELS_H
#define IDE_PANELS_H
#include "ui.h"
#include "ide.h"
struct GB;

/* Each draws one panel into the canvas at its PANEL_RECTS rectangle. The IDE's
   ide_render() calls these in order. Read-only over GB except where noted. */
void panel_registers(Canvas *c, struct GB *gb);
void panel_exec(Canvas *c, IdeState *s);
void panel_disasm(Canvas *c, IdeState *s);
void panel_palette(Canvas *c, struct GB *gb);
void panel_oam(Canvas *c, struct GB *gb);
void panel_tilemap(Canvas *c, struct GB *gb);

#endif /* IDE_PANELS_H */
```

- [ ] **Step 4: Move existing render helpers into `src/ide/panels.c`**

Create `src/ide/panels.c`. Move the existing per-panel rendering blocks (registers, VRAM tiles, code pane, tile editor, mem hex, status) out of `ide_render` in `ide.c` into functions in `panels.c`, preserving their current pixel output exactly. Keep the shared palette/color constants by moving them to `panels.c` (or a small shared header). `ide_render` becomes a sequence of `panel_*` calls. **Do not change any existing panel's appearance in this step** — only relocate code — so the only hash change comes from the larger canvas and repositioned rects.

Add `#include "panels.h"` to `ide.c`.

- [ ] **Step 5: Update `tests/test_ide_render.c` for the new canvas size**

Open `tests/test_ide_render.c`. Wherever it allocates a `640x432` canvas, change to `IDE_CANVAS_W x IDE_CANVAS_H`. If it asserts a baked FNV hash, set the expected hash to `0` (print-only) for now; we re-bake after the new panels exist (Task 11). If it asserts panel rects, update expected values to the new table.

- [ ] **Step 6: Build, run the render test, and produce a screenshot for eyeball check**

Run: `make build/test_ide_render && ./build/test_ide_render && make ide-shot`
Expected: test passes (hash print-only); `build/ide.png` opens to a 1024x720 canvas with existing panels repositioned and empty space where the new panels will go.

- [ ] **Step 7: Commit**

```bash
git add src/ide/ide.h src/ide/ide.c src/ide/panels.h src/ide/panels.c tests/test_ide_render.c
git commit -m "refactor(ide): grow canvas to 1024x720, split rendering into panels.c"
```

---

### Task 8: Disassembly panel (render + PC highlight + breakpoint gutter)

**Files:**
- Modify: `src/ide/panels.c` (`panel_disasm`)
- Modify: `src/ide/ide.c` (call it in `ide_render`)
- Test: `make ide-shot` visual check (rendering verified via the re-baked hash in Task 11)

- [ ] **Step 1: Implement `panel_disasm` in `src/ide/panels.c`**

```c
#include "../gb/disasm.h"
#include "../gb/debug.h"

void panel_disasm(Canvas *c, IdeState *s) {
    int x, y, w, h;
    ide_panel_rect(PANEL_DISASM, &x, &y, &w, &h);
    GB *gb = ide_gb(s);
    ui_rect(c, x, y, w, h, 0x60A060FF);
    ui_text(c, x + 4, y + 2, "DISASM", 0xA0FFA0FF);

    uint8_t bank = gb->rom_bank;
    /* Start a few instructions before PC by stepping back a fixed window. SM83 is
       variable-length, so we approximate: begin 8 bytes before PC, then resync
       forward to a clean boundary by decoding until we pass (PC - margin). */
    uint16_t pc = gb->cpu.pc;
    uint16_t addr = (pc > 0x000C) ? (uint16_t)(pc - 0x000C) : 0;

    int line_h = 8, top = y + 14, rows = (h - 16) / line_h;
    for (int r = 0; r < rows; r++) {
        char text[40];
        int len = gb_disasm(gb, bank, addr, text, sizeof text);
        bool is_pc = (addr == pc);
        bool has_bp = (gb_debug_find_bp(gb, bank, addr) >= 0);
        int ly = top + r * line_h;

        if (is_pc) ui_fill_rect(c, x + 1, ly, w - 2, line_h, 0x303820FF);
        if (has_bp) ui_fill_rect(c, x + 2, ly + 1, 6, 6, 0xFF4040FF);  /* gutter dot */

        char row[64];
        snprintf(row, sizeof row, "%c%04X %s", is_pc ? '>' : ' ', addr, text);
        ui_text(c, x + 10, ly, row, is_pc ? 0xFFD700FF : 0xC0E0C0FF);

        addr = (uint16_t)(addr + (len > 0 ? len : 1));
        if (addr < pc - 0x000C) { r--; continue; }  /* skip rows before window start */
    }
}
```

(If the resync `continue` causes an awkward first row, simplify by removing the back-step: start `addr = pc` and list forward only. The test in Task 11 bakes whatever is rendered; PC-at-top is acceptable for v1.)

- [ ] **Step 2: Call it from `ide_render` in `ide.c`**

Add `panel_disasm(c, s);` to the `ide_render` sequence (after `panel_exec`).

- [ ] **Step 3: Build + screenshot**

Run: `make ide-shot` and open `build/ide.png`.
Expected: the disasm panel lists instructions around PC with the PC line highlighted gold.

- [ ] **Step 4: Commit**

```bash
git add src/ide/panels.c src/ide/panels.h src/ide/ide.c
git commit -m "feat(ide): disassembly panel with PC highlight + breakpoint gutter"
```

---

### Task 9: OAM / sprite viewer panel

**Files:**
- Modify: `src/ide/panels.c` (`panel_oam`), `src/ide/ide.c` (call it)
- Test: `make ide-shot` visual check

- [ ] **Step 1: Implement `panel_oam`**

```c
/* decode one shade (0..3) of a tile pixel from VRAM 2bpp at tile index t. */
static uint8_t tile_pixel(GB *gb, int t, int px, int py) {
    int base = t * 16 + py * 2;
    uint8_t lo = gb->vram[base], hi = gb->vram[base + 1];
    int bit = 7 - px;
    return (uint8_t)(((hi >> bit) & 1) << 1 | ((lo >> bit) & 1));
}

void panel_oam(Canvas *c, GB *gb) {
    int x, y, w, h;
    ide_panel_rect(PANEL_OAM, &x, &y, &w, &h);
    ui_rect(c, x, y, w, h, 0x60A060FF);
    ui_text(c, x + 4, y + 2, "OAM", 0xA0FFA0FF);

    static const uint32_t GRAY[4] = {0xFFFFFFFF,0xAAAAAAFF,0x555555FF,0x000000FF};
    int top = y + 14, row_h = 9, rows = (h - 16) / row_h;
    for (int i = 0; i < 40 && i < rows; i++) {
        uint8_t oy = gb->oam[i*4+0], ox = gb->oam[i*4+1];
        uint8_t tile = gb->oam[i*4+2], attr = gb->oam[i*4+3];
        int ly = top + i * row_h;
        /* 8x8 sprite preview at 1x */
        for (int py = 0; py < 8; py++)
            for (int px = 0; px < 8; px++)
                ui_fill_rect(c, x + 4 + px, ly + (8 - 8) + py, 1, 1,
                             GRAY[tile_pixel(gb, tile, px, py)]);
        char row[40];
        snprintf(row, sizeof row, "%02d Y%3d X%3d T%02X %02X", i, oy, ox, tile, attr);
        ui_text(c, x + 16, ly, row, 0xC0E0C0FF);
    }
}
```

- [ ] **Step 2: Call it from `ide_render`; build + screenshot**

Add `panel_oam(c, ide_gb(s));` to `ide_render`.
Run: `make ide-shot` and open `build/ide.png`.
Expected: 40 OAM rows with tiny sprite previews. (For demo.asm most entries are zeroed — that's fine.)

- [ ] **Step 3: Commit**

```bash
git add src/ide/panels.c src/ide/ide.c
git commit -m "feat(ide): OAM/sprite viewer panel"
```

---

### Task 10: Background tilemap viewer panel

**Files:**
- Modify: `src/ide/panels.c` (`panel_tilemap`), `src/ide/ide.c`
- Test: `make ide-shot` visual check

- [ ] **Step 1: Implement `panel_tilemap`**

```c
void panel_tilemap(Canvas *c, GB *gb) {
    int x, y, w, h;
    ide_panel_rect(PANEL_TILEMAP, &x, &y, &w, &h);
    ui_rect(c, x, y, w, h, 0x60A060FF);
    ui_text(c, x + 4, y + 2, "BG MAP", 0xA0FFA0FF);

    static const uint32_t GRAY[4] = {0xFFFFFFFF,0xAAAAAAFF,0x555555FF,0x000000FF};
    /* LCDC bit3 selects 0x9800 vs 0x9C00; bit4 selects tile data base. */
    int map = (gb->lcdc & 0x08) ? 0x1C00 : 0x1800;     /* VRAM offset */
    bool signed_idx = !(gb->lcdc & 0x10);
    int ox = x + 4, oy = y + 12;
    /* 32x32 tiles, draw each as a 4x4 block (128x128 px), then SCX/SCY box. */
    int cell = 4;
    for (int ty = 0; ty < 32; ty++) {
        for (int tx = 0; tx < 32; tx++) {
            uint8_t idx = gb->vram[map + ty*32 + tx];
            /* tile-data base: 0x8000 (unsigned) or 0x9000 (signed) -> VRAM tile index */
            int t = signed_idx ? 256 + (int8_t)idx : idx;
            /* cheap thumbnail: shade of the tile's top-left pixel */
            uint8_t shade = tile_pixel(gb, t, 0, 0);
            ui_fill_rect(c, ox + tx*cell, oy + ty*cell, cell, cell, GRAY[shade]);
        }
    }
    /* viewport rectangle (SCX/SCY in tile-space, scaled by cell/8) */
    int vx = ox + (gb->scx / 8) * cell, vy = oy + (gb->scy / 8) * cell;
    ui_rect(c, vx, vy, 20 * cell, 18 * cell, 0xFFD700FF);
}
```

- [ ] **Step 2: Call it from `ide_render`; build + screenshot**

Add `panel_tilemap(c, ide_gb(s));`.
Run: `make ide-shot`; open `build/ide.png`.
Expected: a 32x32 thumbnail grid with a gold viewport rectangle. For demo.asm's scrolling background the grid should show structure.

- [ ] **Step 3: Commit**

```bash
git add src/ide/panels.c src/ide/ide.c
git commit -m "feat(ide): background tilemap viewer panel"
```

---

### Task 11: Palette view panel + re-bake the render hash

**Files:**
- Modify: `src/ide/panels.c` (`panel_palette`), `src/ide/ide.c`
- Modify: `tests/test_ide_render.c` (bake new hash)
- Test: `tests/test_ide_render.c`

- [ ] **Step 1: Implement `panel_palette`**

```c
void panel_palette(Canvas *c, GB *gb) {
    int x, y, w, h;
    ide_panel_rect(PANEL_PALETTE, &x, &y, &w, &h);
    ui_rect(c, x, y, w, h, 0x60A060FF);
    static const uint32_t DMG[4] = {0xE0F8D0FF,0x88C070FF,0x346856FF,0x081820FF};
    const char *names[3] = {"BGP","OB0","OB1"};
    uint8_t regs[3] = { gb->bgp, gb->obp0, gb->obp1 };
    int sw = 14;
    for (int p = 0; p < 3; p++) {
        int py = y + 2 + p * 11;
        ui_text(c, x + 4, py, names[p], 0xA0FFA0FF);
        for (int i = 0; i < 4; i++) {
            int shade = (regs[p] >> (i * 2)) & 3;
            ui_fill_rect(c, x + 36 + i * (sw + 2), py, sw, 9, DMG[shade]);
        }
    }
}
```

- [ ] **Step 2: Call it from `ide_render`; build + screenshot for manual verification**

Add `panel_palette(c, ide_gb(s));`.
Run: `make ide-shot`; open `build/ide.png`.
Expected: BGP/OB0/OB1 each show four DMG-green swatches in the palette panel. **Eyeball the whole canvas — all panels present and sane.**

- [ ] **Step 3: Print the current render hash**

In `tests/test_ide_render.c`, run with the expected hash set to print-only. Run:
`./build/test_ide_render` and copy the printed FNV hash.

- [ ] **Step 4: Bake the hash and re-enable the assertion**

Set the expected-hash constant in `tests/test_ide_render.c` to the printed value (mirror the `dmg_acid2.c` "bake after manual verification" pattern). Re-run:
`make build/test_ide_render && ./build/test_ide_render`
Expected: `0 failures`.

- [ ] **Step 5: Commit**

```bash
git add src/ide/panels.c src/ide/ide.c tests/test_ide_render.c
git commit -m "feat(ide): palette viewer panel; re-bake IDE render hash"
```

---

### Task 12: Memory-hex watchpoint markers + exec status panel

**Files:**
- Modify: `src/ide/panels.c` (mem-hex marker, `panel_exec`), `src/ide/ide.c`
- Test: `make ide-shot` + re-bake if hash assertion trips

- [ ] **Step 1: Implement `panel_exec` in `panels.c`**

```c
void panel_exec(Canvas *c, IdeState *s) {
    int x, y, w, h;
    ide_panel_rect(PANEL_EXEC, &x, &y, &w, &h);
    GB *gb = ide_gb(s);
    ui_rect(c, x, y, w, h, 0x60A060FF);

    const char *mode = ide_exec_mode(s) == EXEC_PAUSED ? "PAUSED" : "RUNNING";
    char line[64];
    snprintf(line, sizeof line, "EXEC: %s  PC=%04X", mode, gb->cpu.pc);
    ui_text(c, x + 4, y + 2, line, 0xFFD700FF);

    GbDebug *d = gb->dbg;
    if (d && d->hit) {
        const char *k = d->hit_kind == DBG_BREAKPOINT ? "BP"
                      : d->hit_kind == DBG_WATCH_READ  ? "WR"
                      : d->hit_kind == DBG_WATCH_WRITE ? "WW" : "?";
        snprintf(line, sizeof line, "BREAK %s @%04X (pc %04X)", k, d->hit_addr, d->hit_pc);
        ui_text(c, x + 4, y + 12, line, 0xFF8080FF);
    }
    snprintf(line, sizeof line, "BP:%d WP:%d", d ? d->bp_count : 0, d ? d->wp_count : 0);
    ui_text(c, x + 4, y + 22, line, 0xC0E0C0FF);
    ui_text(c, x + 4, y + 40, "SPC run/pause F7 ins F6 line F9 frm", 0x80A080FF);
}
```

- [ ] **Step 2: Add a watchpoint marker to the mem-hex panel**

In the mem-hex render function (moved to `panels.c` in Task 7), when drawing each byte cell, if a watchpoint covers that address, draw a 1px underline/marker. Add near where each byte is drawn:

```c
            if (gb->dbg) {
                for (int wi = 0; wi < gb->dbg->wp_count; wi++) {
                    if (gb->dbg->wp[wi].addr == cell_addr) {
                        ui_fill_rect(c, cell_x, cell_y + 8, 16, 1, 0xFF8080FF);
                        break;
                    }
                }
            }
```

(`cell_addr`, `cell_x`, `cell_y` are whatever the existing loop already computes per byte — reuse those locals; adjust the underline width to the cell's pixel width.)

- [ ] **Step 3: Call `panel_exec` from `ide_render`; build + screenshot**

Run: `make ide-shot`; open `build/ide.png`.
Expected: exec panel shows `EXEC: RUNNING PC=...` and the key legend.

- [ ] **Step 4: Re-bake the render hash (panels changed) and confirm the test**

Print the new hash, bake it into `tests/test_ide_render.c`, then:
`make build/test_ide_render && ./build/test_ide_render`
Expected: `0 failures`.

- [ ] **Step 5: Commit**

```bash
git add src/ide/panels.c src/ide/ide.c tests/test_ide_render.c
git commit -m "feat(ide): exec status panel + mem-hex watchpoint markers"
```

---

### Task 13: SDL wiring — keys, run-slice loop, panel clicks, address field

**Files:**
- Modify: `src/ide/main.c`
- Modify: `src/ide/ide.h` / `src/ide/ide.c` (click + address-field glue)
- Test: build the SDL binary + `make ide-shot`; manual interactive check

This task touches the SDL event loop, which is not unit-tested. Verify by building and by exercising the headless shot; the logic it calls (`ide_run_slice`, `gb_debug_toggle_bp`, etc.) is already covered by Tasks 1-5.

- [ ] **Step 1: Add IDE glue functions for panel clicks and the address field to `ide.h`**

```c
/* Toggle a breakpoint at the disasm line under canvas-space (mx,my). No-op if
   outside the disasm panel. Returns true if a line was hit. */
bool ide_disasm_click(IdeState *s, int mx, int my);

/* Toggle a read+write watchpoint at the mem-hex cell under (mx,my). Returns
   true if a cell was hit. */
bool ide_memhex_click(IdeState *s, int mx, int my);

/* Address-entry field (PANEL_ADDR_INPUT). */
void ide_addr_focus(IdeState *s, bool on);
bool ide_addr_focused(IdeState *s);
void ide_addr_putc(IdeState *s, char ch);
void ide_addr_backspace(IdeState *s);
/* Commit the typed hex address: toggles a PC breakpoint (current bank). */
void ide_addr_commit(IdeState *s);
/* Render the address field (called by ide_render). */
void ide_addr_render(IdeState *s, Canvas *c);
```

- [ ] **Step 2: Implement them in `ide.c`**

Add a `TextField addr;` field to `IdeState`. Implement using the same disasm-walk used by the panel so the clicked row maps to an address. For the mem-hex cell, reuse the panel's addressing math (`mem_base + row*16 + col`). Example:

```c
bool ide_disasm_click(IdeState *s, int mx, int my) {
    int x, y, w, h; ide_panel_rect(PANEL_DISASM, &x, &y, &w, &h);
    if (mx < x || mx >= x + w || my < y + 14 || my >= y + h) return false;
    int row = (my - (y + 14)) / 8;
    GB *gb = s->gb;
    uint8_t bank = gb->rom_bank;
    uint16_t pc = gb->cpu.pc;
    uint16_t addr = (pc > 0x000C) ? (uint16_t)(pc - 0x000C) : 0;  /* match panel_disasm start */
    char tmp[40];
    for (int r = 0; r < row; r++) addr += (uint16_t)gb_disasm(gb, bank, addr, tmp, sizeof tmp);
    gb_debug_toggle_bp(gb, bank, addr);
    return true;
}

bool ide_memhex_click(IdeState *s, int mx, int my) {
    int x, y, w, h; ide_panel_rect(PANEL_MEM_HEX, &x, &y, &w, &h);
    if (mx < x || mx >= x + w || my < y || my >= y + h) return false;
    /* Reuse the same row/col geometry the mem-hex renderer uses. */
    int col = (mx - (x + 56)) / 16;    /* 56 = label width; 16 = cell width */
    int rowi = (my - (y + 2)) / 8;
    if (col < 0 || col > 15 || rowi < 0) return false;
    uint16_t a = (uint16_t)(s->mem_base + rowi * 16 + col);
    int idx = -1;
    for (int i = 0; i < s->gb->dbg->wp_count; i++)
        if (s->gb->dbg->wp[i].addr == a) { idx = i; break; }
    if (idx >= 0) gb_debug_clear_wp(s->gb, idx);
    else gb_debug_add_wp(s->gb, a, true, true);
    return true;
}

void ide_addr_focus(IdeState *s, bool on) { s->addr.active = on; if (on) textfield_clear(&s->addr), s->addr.active = true; }
bool ide_addr_focused(IdeState *s) { return s->addr.active; }
void ide_addr_putc(IdeState *s, char ch) { textfield_putc(&s->addr, ch); }
void ide_addr_backspace(IdeState *s) { textfield_backspace(&s->addr); }

void ide_addr_commit(IdeState *s) {
    if (s->addr.len == 0) { s->addr.active = false; return; }
    unsigned v = (unsigned)strtol(s->addr.text, NULL, 16);
    gb_debug_toggle_bp(s->gb, s->gb->rom_bank, (uint16_t)v);
    textfield_clear(&s->addr);
}

void ide_addr_render(IdeState *s, Canvas *c) {
    int x, y, w, h; ide_panel_rect(PANEL_ADDR_INPUT, &x, &y, &w, &h); (void)w; (void)h;
    ui_text(c, x, y, "BP@", 0xA0FFA0FF);
    textfield_render(c, x + 24, y, &s->addr, 0xFFD700FF, 0x081820FF);
}
```

Call `ide_addr_render(s, c);` at the end of `ide_render`. (The geometry constants `56`/`16`/`24` must match whatever the mem-hex renderer actually uses — verify against the moved code and adjust.)

- [ ] **Step 3: Wire keys and the run loop in `src/ide/main.c`**

Replace the per-frame `ide_step_frame(s)` call in the main loop with `ide_run_slice(s)`. In the SDL keydown handler, add (guarding text entry when the address field is focused):

```c
if (ide_addr_focused(s)) {
    if (key == SDLK_RETURN)      ide_addr_commit(s);
    else if (key == SDLK_ESCAPE) ide_addr_focus(s, false);
    else if (key == SDLK_BACKSPACE) ide_addr_backspace(s);
    /* printable chars arrive via SDL_EVENT_TEXT_INPUT -> ide_addr_putc */
} else {
    switch (key) {
    case SDLK_SPACE:
        if (ide_exec_mode(s) == EXEC_PAUSED) ide_resume(s); else ide_pause(s);
        break;
    case SDLK_F7: ide_step_insn(s); break;
    case SDLK_F6: ide_step_line(s); break;
    case SDLK_F9: ide_step_frame_once(s); break;
    case SDLK_GRAVE: ide_addr_focus(s, true); break;
    /* existing F5/F8/0-3/joypad cases remain */
    }
}
```

Handle `SDL_EVENT_TEXT_INPUT` to feed `ide_addr_putc(s, ev.text.text[0])` when the field is focused (and call `SDL_StartTextInput`/`SDL_StopTextInput` on focus toggle). In the mouse-button handler, before the existing tile-paint routing, try `ide_disasm_click(s, mx, my)` and `ide_memhex_click(s, mx, my)` (both return false when the click is outside their panels, so ordering is safe).

- [ ] **Step 4: Build the SDL IDE and the headless shot**

Run: `make live-gameboy-ide && make ide-shot`
Expected: both build clean (`-Werror`); `build/ide.png` renders. If `make ide-shot`'s baked hash differs because `ide_addr_render` now draws, re-bake `tests/test_ide_render.c` as in Task 11.

- [ ] **Step 5: Manual interactive smoke test (document the result)**

Run: `./live-gameboy-ide examples/demo.asm`
Verify: Space pauses/resumes; F7 advances PC one instruction (watch the disasm highlight move); clicking a disasm line adds a red gutter dot and the game halts when PC reaches it; backtick + typing a hex address + Enter sets a breakpoint; clicking a mem-hex cell underlines it and breaks on access. Note any issues; fix before committing.

- [ ] **Step 6: Commit**

```bash
git add src/ide/main.c src/ide/ide.h src/ide/ide.c tests/test_ide_render.c
git commit -m "feat(ide): wire debugger keys, click-to-break, address field into SDL shell"
```

---

## Phase F — Incidental fix

### Task 14: Gate `mem_timing` in CI

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add `mem_timing` to the `blargg` target**

In `Makefile`, extend the `blargg` recipe:

```make
blargg: $(BUILD)/blargg
	./$(BUILD)/blargg roms/gb-test-roms/cpu_instrs/cpu_instrs.gb
	./$(BUILD)/blargg roms/gb-test-roms/instr_timing/instr_timing.gb
	./$(BUILD)/blargg roms/gb-test-roms/mem_timing/mem_timing.gb
```

- [ ] **Step 2: Run it**

Run: `make blargg`
Expected: three `PASS` lines including `mem_timing.gb`.

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "test: gate blargg mem_timing in CI"
```

---

## Final verification

- [ ] **Run the full suite:** `make test 2>&1 | tail -8` — every file `0 failures`.
- [ ] **Acceptance:** `make blargg && make acid2 && make sound` — all `PASS`.
- [ ] **IDE builds + shot:** `make live-gameboy-ide && make ide-shot` — clean build, `build/ide.png` shows all panels.
- [ ] **Update README:** add the new keys (Space/F6/F7/F9/backtick), the debugger panels, and breakpoint/watchpoint usage to the IDE section. Commit.

---

## Self-review notes (coverage against the spec)

- Spec §3 (GbDebug, bp/wp, fast path) → Tasks 1-3.
- Spec §4 (ExecMode, ide_run_slice, keys) → Task 5 + Task 13.
- Spec §5 (disassembler) → Task 4 + Task 8.
- Spec §6 (panels.c, disasm/OAM/tilemap/palette + mem-hex markers) → Tasks 7-12.
- Spec §7 (text-input widget) → Task 6 + Task 13.
- Spec §8 (canvas grow + layout) → Task 7.
- Spec §9 (testing: test_dbg, test_disasm, exec test, ide-shot hash, test_ui) → Tasks 1-6, 11.
- Spec §10 (mem_timing CI) → Task 14.
- Spec §11 out-of-scope items are not implemented (correct).
