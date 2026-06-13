# Milestone 2a: Pixel-FIFO PPU (headless) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A headless, SDL-free DMG PPU using a pixel-FIFO pipeline that renders background, window, and sprites correctly into a 160×144 framebuffer, drives the LCD STAT/VBlank interrupts and mode timing, blocks VRAM/OAM during the right modes, implements OAM DMA, and passes the **dmg-acid2** rendering test ROM.

**Architecture:** A new `src/gb/ppu.c` owns all PPU state (added to the `GB` struct) and the LCD registers FF40–FF4B + FF46. It is T-cycle stepped from `gb_tick` (alongside the timer). The scanline is a 456-dot state machine: Mode 2 (OAM scan, 80 dots) → Mode 3 (pixel transfer, variable) → Mode 0 (H-Blank) → next line; lines 144–153 are Mode 1 (V-Blank). Mode 3 runs a real pixel FIFO: a background/window fetcher (8-dot fetch cycle) feeds a BG FIFO, sprites are fetched on demand into a sprite FIFO, and one pixel per dot is mixed (sprite-over-BG priority + palettes) and written to the framebuffer. The framebuffer holds 2-bit color indices already mapped through the palette into shade 0–3; an accessor exposes it for the future SDL shell.

**Tech Stack:** C11, no new dependencies. Existing Makefile/test harness. dmg-acid2 ROM + its reference PNG fetched (gitignored).

**Spec:** `docs/superpowers/specs/2026-06-12-live-gameboy-design.md` §3 (PPU), §9. This is Milestone 2, part A (PPU). Part B (SDL3 shell + APU) is a separate later plan.

**Depends on:** Milestone 1 (merged): CPU, bus, timer, interrupts. The bus currently stubs `LY` (FF44) to 0x90 and stores FF40–FF4B in `io[]`; this plan removes that stub and routes those registers to the PPU.

---

## Background the implementer needs (DMG PPU, from Pan Docs)

**Registers (all in the FF40–FF4B block, plus FF46 DMA):**
- `FF40 LCDC`: bit7 LCD enable, bit6 window tilemap (0=9800,1=9C00), bit5 window enable, bit4 BG/win tile data (1=8000 unsigned, 0=8800 signed base 9000), bit3 BG tilemap (0=9800,1=9C00), bit2 OBJ size (0=8×8,1=8×16), bit1 OBJ enable, bit0 BG/window enable (DMG: when 0, BG and window are blank/white).
- `FF41 STAT`: bit6 LYC int enable, bit5 mode2 int, bit4 mode1 int, bit3 mode0 int, bit2 LYC==LY coincidence (read-only), bits1-0 mode (read-only). Bit 7 reads 1. Writing STAT sets only bits 3–6.
- `FF42 SCY`, `FF43 SCX`, `FF44 LY` (read-only), `FF45 LYC`, `FF47 BGP`, `FF48 OBP0`, `FF49 OBP1`, `FF4A WY`, `FF4B WX`.
- `FF46 DMA`: writing XX copies 0xXX00–0xXX9F → OAM (FE00–FE9F).

**Tile data:** 16 bytes/tile, 2 bytes/row. For a row, byte0 = low bits, byte1 = high bits; bit 7 is leftmost pixel. Pixel color = (high_bit<<1)|low_bit, range 0–3.

**Tilemaps:** 32×32 bytes at 9800 or 9C00. Tile index → tile data address: if LCDC.4 set, base 0x8000 + index*16 (index unsigned 0–255); else base 0x9000 + (int8_t)index*16 (signed).

**Palettes:** BGP/OBP0/OBP1 map color index 0–3 → shade via 2 bits each (bits 1-0 = shade for color 0, etc.). For sprites, color 0 is transparent (OBP bits 1-0 unused).

**OAM:** 40 entries × 4 bytes at FE00. Byte0 Y (screen Y + 16), Byte1 X (screen X + 8), Byte2 tile (8×16: bit0 ignored, top = tile&0xFE, bottom = tile|1), Byte3 flags: bit7 BG-over-OBJ priority (1 = OBJ behind BG colors 1-3), bit6 Y-flip, bit5 X-flip, bit4 palette (0=OBP0,1=OBP1).

**Per-line sprite selection (Mode 2):** scan OAM in order; a sprite is on this line if `LY+16 >= Y && LY+16 < Y + height` (height 8 or 16). Take at most the first 10 in OAM order.

**DMG sprite priority:** among selected sprites, the one with smaller X wins; ties broken by OAM order (lower address wins). BG/OBJ priority: OBJ flag bit7=0 → OBJ on top unless OBJ color is 0 (transparent); bit7=1 → OBJ only shows where BG color is 0.

**Mode timing per line:** 456 dots total. Mode 2 = 80 dots. Mode 3 ≈ 172 + penalties. Mode 0 = the rest. dmg-acid2 does NOT require cycle-exact Mode 3 length — correct *pixels* are what matter. We still run a real FIFO; Mode 3 ends when 160 pixels have been pushed to the screen, and Mode 0 fills the remainder of the 456-dot line.

**Interrupts:** VBlank (INT_VBLANK) at the start of line 144. STAT (INT_STAT) on the rising edge of the OR of: (LYC int enable && LYC==LY), (mode0 int && mode0), (mode1 int && mode1), (mode2 int && mode2). Use a single "stat line" rising-edge detector to avoid duplicate requests.

---

## File structure

```
src/gb/gb.h        — add PPU state to GB struct; declare ppu API + framebuffer accessor
src/gb/ppu.c       — NEW: all PPU logic (registers, timing, FIFO, rendering, DMA)
src/gb/bus.c       — route FF40-FF4B + FF46 to ppu; block VRAM/OAM by mode; drop LY stub
src/gb/gb.c        — gb_tick calls gb_ppu_tick; gb_reset inits PPU
tests/test_ppu_timing.c     — mode state machine, LY/LYC, interrupts
tests/test_ppu_access.c     — VRAM/OAM blocking, OAM DMA
tests/test_ppu_render.c     — BG, window, sprite pixel output
tests/dmg_acid2.c           — acceptance: render a frame, compare to reference
roms/  (gitignored)         — dmg-acid2.gb + reference image
```

Responsibility: `ppu.c` is the only place that knows pixel/tile/sprite layout. `bus.c` only *delegates* the FF40-block and *queries* `gb_ppu_vram_blocked()`/`gb_ppu_oam_blocked()`. Keep `ppu.c` focused; if it grows past ~600 lines, that's expected for a PPU — do not split mid-plan.

---

### Task 1: PPU state, registers, mode timing, interrupts (no rendering)

**Files:**
- Modify: `src/gb/gb.h`, `src/gb/bus.c`, `src/gb/gb.c`
- Create: `src/gb/ppu.c`, `tests/test_ppu_timing.c`

- [ ] **Step 1: Add PPU state + API to `src/gb/gb.h`**

Add these fields inside `struct GB` (after the `io[]`/`ie`/`iflag` block, before the timer block is fine):

```c
    /* ppu */
    uint8_t lcdc, stat, scy, scx, ly, lyc, bgp, obp0, obp1, wy, wx;
    int      ppu_mode;        /* 0 HBlank, 1 VBlank, 2 OAM, 3 Draw */
    int      ppu_dot;         /* dot counter within the current scanline (0..455) */
    bool     stat_line;       /* previous STAT interrupt line level (for edge detect) */
    uint8_t  framebuffer[160 * 144];   /* shade 0..3 per pixel */
    int      win_line;        /* window internal line counter */
    bool     frame_ready;     /* set true when a full frame finishes (line 144 reached) */
    /* mode-3 fifo working state (declared here so the whole struct stays serializable) */
    int      fx;              /* current screen X being produced (0..159) in mode 3 */
    int      fetch_step;      /* bg fetcher step 0..7 */
    int      fetch_x;         /* bg fetcher tile-column counter for this line */
    uint8_t  bg_fifo_c[8];    /* bg fifo colors */
    int      bg_fifo_n;       /* bg fifo count */
    bool     window_active;   /* window currently driving the fetcher this line */
    uint8_t  oam_dma_src;     /* high byte of last DMA source */
```

Add the PPU API near the timer prototypes:

```c
/* internal: ppu (ppu.c) */
void    gb_ppu_tick(GB *gb, int tcycles);
uint8_t gb_ppu_read(GB *gb, uint16_t addr);   /* FF40-FF4B, FF46 */
void    gb_ppu_write(GB *gb, uint16_t addr, uint8_t v);
bool    gb_ppu_vram_blocked(const GB *gb);     /* true => CPU VRAM access denied */
bool    gb_ppu_oam_blocked(const GB *gb);      /* true => CPU OAM access denied */
void    gb_ppu_reset(GB *gb);
const uint8_t *gb_framebuffer(const GB *gb);    /* 160*144 shades, for the shell */
```

- [ ] **Step 2: Write the failing test `tests/test_ppu_timing.c`**

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static GB *fresh(void) {
    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    g->lcdc = 0x91;          /* LCD on, BG on, tiledata 8000 */
    return g;
}

int main(void) {
    {   /* line 0 starts in mode 2, LY=0 */
        GB *g = fresh();
        ASSERT_EQ(g->ly, 0);
        ASSERT_EQ(g->ppu_mode, 2);
        gb_free(g);
    }
    {   /* mode 2 lasts 80 dots, then mode 3 */
        GB *g = fresh();
        gb_ppu_tick(g, 79);
        ASSERT_EQ(g->ppu_mode, 2);
        gb_ppu_tick(g, 1);
        ASSERT_EQ(g->ppu_mode, 3);
        gb_free(g);
    }
    {   /* a full line is 456 dots; after 456, LY=1 and back to mode 2 */
        GB *g = fresh();
        gb_ppu_tick(g, 456);
        ASSERT_EQ(g->ly, 1);
        ASSERT_EQ(g->ppu_mode, 2);
        gb_free(g);
    }
    {   /* VBlank begins at LY=144: mode 1 and INT_VBLANK requested */
        GB *g = fresh();
        g->iflag = 0;
        gb_ppu_tick(g, 456 * 144);
        ASSERT_EQ(g->ly, 144);
        ASSERT_EQ(g->ppu_mode, 1);
        ASSERT_EQ(g->iflag & INT_VBLANK, INT_VBLANK);
        ASSERT_TRUE(g->frame_ready);
        gb_free(g);
    }
    {   /* frame wraps at 154 lines: 456*154 dots returns to LY=0 mode 2 */
        GB *g = fresh();
        gb_ppu_tick(g, 456 * 154);
        ASSERT_EQ(g->ly, 0);
        ASSERT_EQ(g->ppu_mode, 2);
        gb_free(g);
    }
    {   /* LYC coincidence sets STAT bit 2 and, if enabled, requests STAT int */
        GB *g = fresh();
        g->lyc = 2;
        g->stat = 0x40;           /* enable LYC interrupt (bit 6) */
        g->iflag = 0;
        gb_ppu_tick(g, 456 * 2);  /* advance to LY=2 */
        ASSERT_EQ(g->ly, 2);
        ASSERT_EQ(g->stat & 0x04, 0x04);          /* coincidence flag */
        ASSERT_EQ(g->iflag & INT_STAT, INT_STAT);
        gb_free(g);
    }
    {   /* LCD disabled (LCDC.7=0): LY=0, mode 0, no progress */
        GB *g = fresh();
        g->lcdc = 0x11;           /* LCD off */
        gb_ppu_reset(g);          /* reset re-reads lcdc state */
        g->lcdc = 0x11;
        gb_ppu_tick(g, 456 * 10);
        ASSERT_EQ(g->ly, 0);
        ASSERT_EQ(g->ppu_mode, 0);
        gb_free(g);
    }
    {   /* register read-back: STAT bit7=1, mode in low bits; LY read */
        GB *g = fresh();
        gb_ppu_tick(g, 80);                 /* now mode 3 */
        uint8_t st = gb_ppu_read(g, 0xFF41);
        ASSERT_EQ(st & 0x80, 0x80);
        ASSERT_EQ(st & 0x03, 3);
        ASSERT_EQ(gb_ppu_read(g, 0xFF44), g->ly);
        gb_free(g);
    }
    TEST_MAIN_END();
}
```

- [ ] **Step 3: Run, verify failure**

Run: `make test`
Expected: link failure (no `gb_ppu_*`) or assertion failures. (ppu.c doesn't exist yet.)

- [ ] **Step 4: Create `src/gb/ppu.c` (timing + registers + interrupts; rendering stubbed)**

```c
#include "gb.h"
#include <string.h>

#define MODE_HBLANK 0
#define MODE_VBLANK 1
#define MODE_OAM    2
#define MODE_DRAW   3

#define LCD_ON(g)   ((g)->lcdc & 0x80)

/* forward decls for the rendering pipeline (filled in later tasks) */
static void render_begin_line(GB *g);  /* set up mode-3 fifo state for current LY */
static void render_step(GB *g);        /* produce up to one pixel; advances g->fx */

void gb_ppu_reset(GB *g) {
    g->lcdc = 0x91; g->stat = 0x85; g->scy = 0; g->scx = 0;
    g->ly = 0; g->lyc = 0; g->bgp = 0xFC; g->obp0 = 0xFF; g->obp1 = 0xFF;
    g->wy = 0; g->wx = 0;
    g->ppu_mode = MODE_OAM; g->ppu_dot = 0; g->stat_line = false;
    g->win_line = 0; g->frame_ready = false;
    g->fx = 0; g->fetch_step = 0; g->fetch_x = 0; g->bg_fifo_n = 0;
    g->window_active = false; g->oam_dma_src = 0;
    memset(g->framebuffer, 0, sizeof g->framebuffer);
}

/* ---- STAT interrupt line (rising-edge detector) ---- */
static void update_stat_line(GB *g) {
    bool coin = (g->ly == g->lyc);
    if (coin) g->stat |= 0x04; else g->stat &= ~0x04;

    bool line = false;
    if ((g->stat & 0x40) && coin)                 line = true;
    if ((g->stat & 0x20) && g->ppu_mode == MODE_OAM)    line = true;
    if ((g->stat & 0x10) && g->ppu_mode == MODE_VBLANK) line = true;
    if ((g->stat & 0x08) && g->ppu_mode == MODE_HBLANK) line = true;

    if (line && !g->stat_line) g->iflag |= INT_STAT;   /* rising edge */
    g->stat_line = line;
}

static void set_mode(GB *g, int mode) {
    g->ppu_mode = mode;
    g->stat = (uint8_t)((g->stat & ~0x03) | (mode & 0x03));
}

static void tick_one(GB *g) {
    if (!LCD_ON(g)) {
        /* LCD off: PPU idle, LY=0, mode 0, coincidence cleared */
        g->ly = 0; g->ppu_dot = 0; set_mode(g, MODE_HBLANK);
        g->stat &= ~0x04; g->stat_line = false; g->frame_ready = false;
        return;
    }

    g->ppu_dot++;

    if (g->ly < 144) {
        if (g->ppu_dot == 80 + 1 && g->ppu_mode == MODE_OAM) {
            set_mode(g, MODE_DRAW);
            render_begin_line(g);
        } else if (g->ppu_mode == MODE_DRAW) {
            render_step(g);
            if (g->fx >= 160) set_mode(g, MODE_HBLANK);
        }
    }

    if (g->ppu_dot >= 456) {
        g->ppu_dot = 0;
        g->ly++;
        if (g->ly == 144) {
            set_mode(g, MODE_VBLANK);
            g->iflag |= INT_VBLANK;
            g->frame_ready = true;
            g->win_line = 0;
        } else if (g->ly > 153) {
            g->ly = 0;
            set_mode(g, MODE_OAM);
            g->win_line = 0;
        } else if (g->ly < 144) {
            set_mode(g, MODE_OAM);
        }
    }

    update_stat_line(g);
}

void gb_ppu_tick(GB *g, int tcycles) {
    for (int i = 0; i < tcycles; i++) tick_one(g);
}

/* ---- registers ---- */
uint8_t gb_ppu_read(GB *g, uint16_t a) {
    switch (a) {
    case 0xFF40: return g->lcdc;
    case 0xFF41: return g->stat | 0x80;
    case 0xFF42: return g->scy;
    case 0xFF43: return g->scx;
    case 0xFF44: return g->ly;
    case 0xFF45: return g->lyc;
    case 0xFF46: return g->oam_dma_src;
    case 0xFF47: return g->bgp;
    case 0xFF48: return g->obp0;
    case 0xFF49: return g->obp1;
    case 0xFF4A: return g->wy;
    case 0xFF4B: return g->wx;
    default:     return 0xFF;
    }
}

static void oam_dma(GB *g, uint8_t hi);   /* Task 3 */

void gb_ppu_write(GB *g, uint16_t a, uint8_t v) {
    switch (a) {
    case 0xFF40: {
        bool was_on = LCD_ON(g);
        g->lcdc = v;
        if (was_on && !LCD_ON(g)) {      /* turning off resets the PPU */
            g->ly = 0; g->ppu_dot = 0; set_mode(g, MODE_HBLANK);
            g->stat_line = false;
        } else if (!was_on && LCD_ON(g)) {
            g->ppu_dot = 0; g->ly = 0; set_mode(g, MODE_OAM);
        }
        break;
    }
    case 0xFF41: g->stat = (uint8_t)((g->stat & 0x87) | (v & 0x78)); break;
    case 0xFF42: g->scy = v; break;
    case 0xFF43: g->scx = v; break;
    case 0xFF44: break;                  /* LY read-only */
    case 0xFF45: g->lyc = v; update_stat_line(g); break;
    case 0xFF46: oam_dma(g, v); break;
    case 0xFF47: g->bgp = v; break;
    case 0xFF48: g->obp0 = v; break;
    case 0xFF49: g->obp1 = v; break;
    case 0xFF4A: g->wy = v; break;
    case 0xFF4B: g->wx = v; break;
    }
}

bool gb_ppu_vram_blocked(const GB *g) {
    return LCD_ON(g) && g->ppu_mode == MODE_DRAW;
}
bool gb_ppu_oam_blocked(const GB *g) {
    return LCD_ON(g) && (g->ppu_mode == MODE_OAM || g->ppu_mode == MODE_DRAW);
}

const uint8_t *gb_framebuffer(const GB *g) { return g->framebuffer; }

/* ---- rendering stubs (replaced in Tasks 4-6) ---- */
static void render_begin_line(GB *g) { g->fx = 0; }
static void render_step(GB *g) { g->fx++; }   /* advance so mode 3 terminates */

/* ---- OAM DMA stub (replaced in Task 3) ---- */
static void oam_dma(GB *g, uint8_t hi) { g->oam_dma_src = hi; }
```

- [ ] **Step 5: Wire PPU into `gb.c` and `bus.c`**

In `src/gb/gb.c`, in `gb_tick`, call the PPU after the timer:

```c
void gb_tick(GB *gb, int tcycles) {
    gb->cycles += (uint64_t)tcycles;
    gb_timer_tick(gb, tcycles);
    gb_ppu_tick(gb, tcycles);
}
```

In `gb_reset` (gb.c), after the existing register/memory init, call `gb_ppu_reset(gb);` (replace the manual `io[]=0xFF` only if it conflicts — keep io[] init, just add the ppu reset call at the end of gb_reset).

In `src/gb/bus.c`, route the PPU registers in `io_read`/`io_write`. Replace the `case 0x44: return 0x90;` LY stub. Add to `io_read`:

```c
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
    case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:
        return gb_ppu_read(gb, 0xFF00 | r);
```

And to `io_write`:

```c
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
    case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:
        gb_ppu_write(gb, 0xFF00 | r, v); break;
```

Remove the now-dead `case 0x44: return 0x90;` line from `io_read`.

- [ ] **Step 6: Run, verify pass**

Run: `make test`
Expected: all prior tests still pass, `test_ppu_timing.c` passes. **Also re-run `make blargg`** — cpu_instrs/instr_timing must STILL pass (the LY stub is gone; real PPU now drives LY, which is fine since those ROMs only need LY to progress through VBlank, which the real PPU does). If cpu_instrs now hangs/fails, debug: the most likely cause is the PPU not advancing LY because LCD is off at reset — but `gb_ppu_reset` sets lcdc=0x91 (on). Confirm.

- [ ] **Step 7: Commit**

```bash
git add src/gb/ppu.c src/gb/gb.h src/gb/gb.c src/gb/bus.c tests/test_ppu_timing.c
git commit -m "feat: PPU mode timing, LCD registers, STAT/VBlank interrupts

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: VRAM/OAM access blocking by mode

**Files:**
- Modify: `src/gb/bus.c`
- Create: `tests/test_ppu_access.c`

- [ ] **Step 1: Write failing test `tests/test_ppu_access.c`**

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static GB *fresh(void) {
    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    g->lcdc = 0x91;
    return g;
}

int main(void) {
    {   /* during mode 3: VRAM reads 0xFF, writes ignored */
        GB *g = fresh();
        gb_write8(g, 0x8000, 0x11);   /* mode 2 at dot 0: VRAM writable */
        gb_ppu_tick(g, 80);           /* enter mode 3 */
        ASSERT_EQ(g->ppu_mode, 3);
        ASSERT_EQ(gb_read8(g, 0x8000), 0xFF);   /* blocked read */
        gb_write8(g, 0x8000, 0x22);             /* blocked write */
        gb_ppu_tick(g, 376);          /* finish line -> back to mode 2 */
        ASSERT_EQ(gb_read8(g, 0x8000), 0x11);   /* original survived */
        gb_free(g);
    }
    {   /* during mode 2 and 3: OAM blocked; during HBlank/VBlank: OK */
        GB *g = fresh();
        ASSERT_EQ(g->ppu_mode, 2);
        ASSERT_EQ(gb_read8(g, 0xFE00), 0xFF);   /* mode 2 blocks OAM */
        gb_ppu_tick(g, 80);
        ASSERT_EQ(gb_read8(g, 0xFE00), 0xFF);   /* mode 3 blocks OAM */
        gb_free(g);
    }
    {   /* LCD off: VRAM/OAM always accessible regardless of stale mode */
        GB *g = fresh();
        gb_ppu_tick(g, 80);           /* mode 3 */
        g->lcdc = 0x11;               /* turn LCD off */
        gb_write8(g, 0x8000, 0x33);
        ASSERT_EQ(gb_read8(g, 0x8000), 0x33);
        gb_write8(g, 0xFE00, 0x44);
        ASSERT_EQ(gb_read8(g, 0xFE00), 0x44);
        gb_free(g);
    }
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run, verify failure** — `make test` (VRAM/OAM not yet blocked).

- [ ] **Step 3: Implement blocking in `bus.c`**

In `gb_read8`, guard the VRAM and OAM regions:

```c
    if (a < 0xA000)  return gb_ppu_vram_blocked(gb) ? 0xFF : gb->vram[a - 0x8000];
```

and

```c
    if (a < 0xFEA0)  return gb_ppu_oam_blocked(gb) ? 0xFF : gb->oam[a - 0xFE00];
```

In `gb_write8`, guard the same:

```c
    if (a < 0xA000)  { if (!gb_ppu_vram_blocked(gb)) gb->vram[a - 0x8000] = v; return; }
```

and

```c
    if (a < 0xFEA0)  { if (!gb_ppu_oam_blocked(gb)) gb->oam[a - 0xFE00] = v; return; }
```

- [ ] **Step 4: Run, verify pass** — `make test` (all pass). Re-run `make blargg` (still PASS — blargg writes VRAM during VBlank/disabled, which is allowed).

- [ ] **Step 5: Commit**

```bash
git add src/gb/bus.c tests/test_ppu_access.c
git commit -m "feat: block VRAM/OAM access during PPU modes 2/3

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: OAM DMA (FF46)

**Files:**
- Modify: `src/gb/ppu.c`
- Modify: `tests/test_ppu_access.c`

- [ ] **Step 1: Add failing test** (before `TEST_MAIN_END()` in `tests/test_ppu_access.c`)

```c
    {   /* OAM DMA copies 0xXX00-0xXX9F into OAM (FE00-FE9F) */
        GB *g = fresh();
        g->lcdc = 0x11;                   /* LCD off so OAM is readable for the check */
        for (int i = 0; i < 0xA0; i++) gb_write8(g, 0xC000 + i, (uint8_t)(i ^ 0x5A));
        gb_write8(g, 0xFF46, 0xC0);       /* DMA from 0xC000 */
        for (int i = 0; i < 0xA0; i++)
            ASSERT_EQ(gb_read8(g, 0xFE00 + i), (uint8_t)(i ^ 0x5A));
        ASSERT_EQ(gb_ppu_read(g, 0xFF46), 0xC0);   /* register reads back source hi */
        gb_free(g);
    }
```

- [ ] **Step 2: Run, verify failure** — `make test` (stub doesn't copy).

- [ ] **Step 3: Implement `oam_dma` in `ppu.c`** (replace the stub)

```c
static void oam_dma(GB *g, uint8_t hi) {
    g->oam_dma_src = hi;
    uint16_t src = (uint16_t)hi << 8;
    for (int i = 0; i < 0xA0; i++)
        g->oam[i] = gb_read8(g, (uint16_t)(src + i));   /* simplified: instantaneous copy */
}
```

(Note: real DMA takes 160 M-cycles and only HRAM is CPU-accessible meanwhile; instantaneous copy is correct enough for dmg-acid2 and all common games. A cycle-accurate DMA can come later. The copy uses `gb_read8` so it respects MBC banking; OAM is written directly to `g->oam` to bypass the mode-blocking guard.)

- [ ] **Step 4: Run, verify pass** — `make test`. Re-run `make blargg` (still PASS).

- [ ] **Step 5: Commit**

```bash
git add src/gb/ppu.c tests/test_ppu_access.c
git commit -m "feat: OAM DMA transfer (FF46)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Background rendering via pixel FIFO

**Files:**
- Modify: `src/gb/ppu.c`
- Create: `tests/test_ppu_render.c`

This task replaces the `render_begin_line`/`render_step` stubs with a real BG pixel FIFO. Window and sprites come in Tasks 5–6; structure the code so they slot in.

- [ ] **Step 1: Write failing test `tests/test_ppu_render.c`**

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static GB *fresh(void) {
    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}

/* write tile `idx` (8000 method) as 8 rows of (lo,hi) byte pairs */
static void set_tile(GB *g, int idx, const uint8_t rows[16]) {
    for (int i = 0; i < 16; i++) g->vram[idx * 16 + i] = rows[i];
}
/* run one whole frame so every visible line renders */
static void render_frame(GB *g) {
    g->frame_ready = false;
    gb_ppu_tick(g, 456 * 154);
}
static uint8_t px(GB *g, int x, int y) { return g->framebuffer[y * 160 + x]; }

int main(void) {
    {   /* solid color-3 tile across the whole BG, identity palette -> shade 3 */
        GB *g = fresh();
        g->lcdc = 0x91;                 /* LCD on, BG on, tiledata=8000, bgmap=9800 */
        g->bgp = 0xE4;                  /* identity: 3->3,2->2,1->1,0->0 */
        uint8_t solid3[16];
        for (int i = 0; i < 16; i++) solid3[i] = 0xFF;   /* lo=FF,hi=FF => color 3 */
        set_tile(g, 0, solid3);
        memset(g->vram + 0x1800, 0, 0x400);    /* 9800 map all -> tile 0 */
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);
        ASSERT_EQ(px(g, 159, 143), 3);
        gb_free(g);
    }
    {   /* palette remap: color 3 -> shade 1 */
        GB *g = fresh();
        g->lcdc = 0x91;
        g->bgp = 0x1B;          /* bits7-6=00(c3->0)? compute below */
        /* BGP bits: [7:6]=c3 [5:4]=c2 [3:2]=c1 [1:0]=c0. We want c3->1 => set bits7-6=01 */
        g->bgp = (1 << 6);      /* c3->1, others ->0 */
        uint8_t solid3[16]; for (int i=0;i<16;i++) solid3[i]=0xFF;
        set_tile(g, 0, solid3);
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        ASSERT_EQ(px(g, 5, 5), 1);
        gb_free(g);
    }
    {   /* horizontal pattern within a tile: lo=0xA0 hi=0x00 -> pixels: c1 0 c1 0 0 0 0 0 */
        GB *g = fresh();
        g->lcdc = 0x91; g->bgp = 0xE4;
        uint8_t t[16]; memset(t, 0, 16);
        t[0] = 0xA0;            /* row 0 low byte = 1010_0000 */
        set_tile(g, 0, t);
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 1);   /* bit7 set */
        ASSERT_EQ(px(g, 1, 0), 0);   /* bit6 clear */
        ASSERT_EQ(px(g, 2, 0), 1);   /* bit5 set */
        ASSERT_EQ(px(g, 3, 0), 0);
        gb_free(g);
    }
    {   /* SCX fine scroll by 2: the pattern shifts left by 2 */
        GB *g = fresh();
        g->lcdc = 0x91; g->bgp = 0xE4; g->scx = 2;
        uint8_t t[16]; memset(t, 0, 16);
        t[0] = 0xA0;
        set_tile(g, 0, t);
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        /* original col2 (c1) now at col0 */
        ASSERT_EQ(px(g, 0, 0), 1);
        gb_free(g);
    }
    {   /* LCDC.0=0 -> BG off -> all shade 0 */
        GB *g = fresh();
        g->lcdc = 0x90;                /* BG/win disable */
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;
        set_tile(g, 0, solid3);
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        ASSERT_EQ(px(g, 80, 72), 0);
        gb_free(g);
    }
    {   /* signed tile addressing (LCDC.4=0): tile index 0 at 0x9000 */
        GB *g = fresh();
        g->lcdc = 0x81;               /* LCD on, BG on, tiledata=8800/signed, bgmap=9800 */
        g->bgp = 0xE4;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;
        /* 0x9000 is vram offset 0x1000; tile index 0 signed => 0x9000 */
        for (int i=0;i<16;i++) g->vram[0x1000 + i] = solid3[i];
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);
        gb_free(g);
    }
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run, verify failure** — `make test` (stub renders all 0).

- [ ] **Step 3: Implement the BG pixel FIFO in `ppu.c`**

Replace the two render stubs. Add palette + tile helpers and the fetcher. This is the core of the milestone — transcribe carefully.

```c
/* apply a 2-bit palette: color index -> shade */
static uint8_t pal_shade(uint8_t pal, uint8_t color) {
    return (uint8_t)((pal >> (color * 2)) & 0x03);
}

/* read a BG/window tile row's two bytes for tile column `tx`, pixel row `py` */
static void fetch_bg_row(GB *g, int map_base, int tx, int ty, int py,
                         uint8_t *lo, uint8_t *hi) {
    int map_off = map_base + (ty & 31) * 32 + (tx & 31);
    uint8_t tile = g->vram[map_off];
    int addr;
    if (g->lcdc & 0x10) addr = tile * 16;                 /* 0x8000 unsigned */
    else                addr = 0x1000 + (int8_t)tile * 16; /* 0x9000 signed */
    *lo = g->vram[addr + py * 2];
    *hi = g->vram[addr + py * 2 + 1];
}

static void render_begin_line(GB *g) {
    g->fx = 0;
    g->bg_fifo_n = 0;
    g->fetch_step = 0;
    g->fetch_x = 0;
    g->window_active = false;
    g->discard = g->scx & 7;          /* fine-scroll discard counter (see note) */
}

/* push 8 BG pixels for the next tile column into the fifo */
static void bg_fetch_tile(GB *g) {
    int map_base = (g->lcdc & 0x08) ? 0x1C00 : 0x1800;    /* 9C00 / 9800 */
    int ty = ((g->scy + g->ly) / 8);
    int py = (g->scy + g->ly) % 8;
    int tx = (g->scx / 8) + g->fetch_x;
    uint8_t lo, hi;
    fetch_bg_row(g, map_base, tx, ty, py, &lo, &hi);
    for (int b = 7; b >= 0; b--) {
        uint8_t color = (uint8_t)(((hi >> b) & 1) << 1 | ((lo >> b) & 1));
        g->bg_fifo_c[g->bg_fifo_n++] = color;
    }
    g->fetch_x++;
}

static void render_step(GB *g) {
    if (g->fx >= 160) return;

    /* ensure the fifo has pixels; the BG fetcher pushes 8 at a time */
    if (g->bg_fifo_n == 0) bg_fetch_tile(g);

    /* discard SCX&7 pixels at the very start of the line */
    while (g->discard > 0 && g->bg_fifo_n > 0) {
        memmove(g->bg_fifo_c, g->bg_fifo_c + 1, (size_t)(--g->bg_fifo_n));
        g->discard--;
        if (g->bg_fifo_n == 0) bg_fetch_tile(g);
    }
    if (g->bg_fifo_n == 0) return;

    uint8_t color = g->bg_fifo_c[0];
    memmove(g->bg_fifo_c, g->bg_fifo_c + 1, (size_t)(--g->bg_fifo_n));

    if (!(g->lcdc & 0x01)) color = 0;     /* BG/window disabled -> color 0 */
    uint8_t shade = pal_shade(g->bgp, color);
    g->framebuffer[g->ly * 160 + g->fx] = shade;
    g->fx++;
}
```

Add a `discard` field to the PPU state in `gb.h` (next to `fx`): `int discard;`.

(Design note for the implementer: this is a *simplified* FIFO — it fetches 8 pixels on demand and emits one per dot, which yields correct pixels and correct SCX fine-scroll. It is not cycle-exact in Mode 3 length, which dmg-acid2 does not require. Tasks 5–6 extend `render_step`/`render_begin_line`; keep the BG color available for sprite mixing.)

- [ ] **Step 4: Run, verify pass** — `make test`. If a pixel assertion fails, print the offending tile/fifo values and compare to the Pan Docs bit layout (bit 7 = leftmost; color = hi<<1|lo). Re-run `make blargg` (still PASS).

- [ ] **Step 5: Commit**

```bash
git add src/gb/ppu.c src/gb/gb.h tests/test_ppu_render.c
git commit -m "feat: background rendering via pixel FIFO (scroll, palette, signed tiles)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Window rendering

**Files:**
- Modify: `src/gb/ppu.c`, `src/gb/gb.h`
- Modify: `tests/test_ppu_render.c`

- [ ] **Step 1: Add failing tests** (before `TEST_MAIN_END()`)

```c
    {   /* window covering whole screen (WX=7,WY=0) shows window tilemap */
        GB *g = fresh();
        g->lcdc = 0xB1;             /* LCD on, BG on, window enable(bit5), tiledata 8000, winmap 9800(bit6=0) */
        g->bgp = 0xE4; g->wy = 0; g->wx = 7;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;   /* tile 1 = solid c3 */
        set_tile(g, 1, solid3);
        memset(g->vram + 0x1800, 0, 0x400);   /* bg map -> tile 0 (blank) */
        memset(g->vram + 0x1800, 1, 0x400);   /* but window uses 9800 too here; set all to tile 1 */
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);
        ASSERT_EQ(px(g, 159, 143), 3);
        gb_free(g);
    }
    {   /* window offset: WX=87 (screen x=80), WY=72. left/top half = BG(blank,0), bottom-right = window */
        GB *g = fresh();
        g->lcdc = 0xB1; g->bgp = 0xE4; g->wx = 87; g->wy = 72;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;
        set_tile(g, 1, solid3);
        memset(g->vram + 0x1800, 1, 0x400);   /* both BG and window map entries -> tile 1; BG tile 0 stays blank */
        /* make BG map all tile 0 (blank) and rely on window for tile 1: */
        memset(g->vram + 0x1800, 0, 0x400);   /* 9800 used as BG map -> tile 0 blank */
        memset(g->vram + 0x1C00, 1, 0x400);   /* 9C00 used as window map -> tile 1 */
        g->lcdc = 0xF1;                        /* also set bit6=1 so window map=9C00, bit3=0 bg map=9800 */
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 0);             /* BG region blank */
        ASSERT_EQ(px(g, 159, 143), 3);         /* window region solid */
        ASSERT_EQ(px(g, 79, 71), 0);           /* just outside window */
        ASSERT_EQ(px(g, 80, 72), 3);           /* first window pixel */
        gb_free(g);
    }
    {   /* window disabled (LCDC.5=0): window map ignored */
        GB *g = fresh();
        g->lcdc = 0x91; g->bgp = 0xE4; g->wx = 7; g->wy = 0;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;
        set_tile(g, 1, solid3);
        memset(g->vram + 0x1C00, 1, 0x400);
        memset(g->vram + 0x1800, 0, 0x400);
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 0);
        gb_free(g);
    }
```

- [ ] **Step 2: Run, verify failure** — `make test`.

- [ ] **Step 3: Implement window in `ppu.c`**

The window replaces the BG fetcher mid-line once the screen X reaches WX-7 (and WY has been reached). Add a per-line "window has started" flag and use `win_line` as the window's own vertical counter.

Add to `gb.h` PPU state: `bool win_started;`.

In `render_begin_line`, add: `g->win_started = false;`.

Rewrite `bg_fetch_tile` to handle both BG and window source. Replace it with:

```c
static void bg_fetch_tile(GB *g) {
    int map_base, tx, ty, py;
    if (g->window_active) {
        map_base = (g->lcdc & 0x40) ? 0x1C00 : 0x1800;   /* window map: LCDC.6 */
        tx = g->fetch_x;
        ty = g->win_line / 8;
        py = g->win_line % 8;
    } else {
        map_base = (g->lcdc & 0x08) ? 0x1C00 : 0x1800;   /* BG map: LCDC.3 */
        tx = (g->scx / 8) + g->fetch_x;
        ty = ((g->scy + g->ly) / 8);
        py = (g->scy + g->ly) % 8;
    }
    uint8_t lo, hi;
    fetch_bg_row(g, map_base, tx, ty, py, &lo, &hi);
    for (int b = 7; b >= 0; b--) {
        uint8_t color = (uint8_t)(((hi >> b) & 1) << 1 | ((lo >> b) & 1));
        g->bg_fifo_c[g->bg_fifo_n++] = color;
    }
    g->fetch_x++;
}
```

In `render_step`, at the top (before producing a pixel), check whether the window should activate at this X:

```c
    /* window activation: when enabled, WY reached, and fx >= WX-7 */
    if (!g->window_active && (g->lcdc & 0x20) && (g->lcdc & 0x01)
        && g->ly >= g->wy && g->fx >= (g->wx - 7) && g->wx <= 166) {
        g->window_active = true;
        g->win_started = true;
        g->bg_fifo_n = 0;          /* flush BG fifo; window starts fresh */
        g->fetch_x = 0;
        g->discard = 0;            /* window has no fine X scroll */
    }
```

At the end of a visible line (when `g->fx` reaches 160 inside `tick_one`'s mode-3 termination), the window line counter must advance only if the window was shown. Handle this in `tick_one` where mode switches DRAW→HBLANK:

```c
        } else if (g->ppu_mode == MODE_DRAW) {
            render_step(g);
            if (g->fx >= 160) {
                if (g->win_started) g->win_line++;
                set_mode(g, MODE_HBLANK);
            }
        }
```

(Remove the simpler version from Task 1's `tick_one`.) Note `WX-7` can be negative if WX<7; guard with the `g->fx >= (g->wx - 7)` comparison using `int` (fx and wx are int / promoted) — when WX<7 the window effectively starts at fx=0.

- [ ] **Step 4: Run, verify pass** — `make test`. Debug pixel mismatches by checking WX/WY offset math (screen X = WX-7). Re-run `make blargg` (PASS).

- [ ] **Step 5: Commit**

```bash
git add src/gb/ppu.c src/gb/gb.h tests/test_ppu_render.c
git commit -m "feat: window rendering

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Sprite rendering (OAM scan, fetch, flips, 8×16, priority, palettes)

**Files:**
- Modify: `src/gb/ppu.c`, `src/gb/gb.h`
- Modify: `tests/test_ppu_render.c`

Approach: keep the BG/window FIFO producing the BG color per pixel. For sprites, at `render_begin_line` do the Mode-2 OAM scan (select ≤10 sprites for this line, in OAM order). In `render_step`, after computing the BG color for the current `fx`, find the highest-priority sprite covering `fx`, compute its pixel, and apply OBJ↔BG priority + palette.

- [ ] **Step 1: Add PPU state to `gb.h`**

```c
    /* per-line selected sprites (Mode 2 scan result) */
    uint8_t  spr_idx[10];     /* OAM entry indices, in scan order */
    int      spr_count;
    uint8_t  bg_color_at;     /* BG color (pre-palette) of the pixel being produced */
```

- [ ] **Step 2: Add failing tests** (before `TEST_MAIN_END()`)

```c
    {   /* a single 8x8 sprite at (8,16)=>screen(0,0), solid color2, OBP0 identity */
        GB *g = fresh();
        g->lcdc = 0x93;            /* LCD on, BG on, OBJ enable(bit1), tiledata 8000 */
        g->bgp = 0xE4; g->obp0 = 0xE4;
        memset(g->vram + 0x1800, 0, 0x400);     /* blank BG */
        uint8_t solid2[16];
        for (int i=0;i<8;i++){ solid2[i*2]=0x00; solid2[i*2+1]=0xFF; } /* hi=FF lo=00 => color 2 */
        set_tile(g, 1, solid2);
        g->oam[0]=16; g->oam[1]=8; g->oam[2]=1; g->oam[3]=0x00;  /* y,x,tile,flags */
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 2);
        ASSERT_EQ(px(g, 7, 7), 2);
        ASSERT_EQ(px(g, 8, 0), 0);    /* outside sprite */
        gb_free(g);
    }
    {   /* sprite color 0 is transparent: BG shows through */
        GB *g = fresh();
        g->lcdc = 0x93; g->bgp = 0xE4; g->obp0 = 0xE4;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;  /* BG tile0 solid c3 */
        set_tile(g, 0, solid3);
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t t[16]; memset(t,0,16);  /* sprite tile1 all color 0 */
        set_tile(g, 1, t);
        g->oam[0]=16; g->oam[1]=8; g->oam[2]=1; g->oam[3]=0x00;
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);    /* BG shows; sprite transparent */
        gb_free(g);
    }
    {   /* OBJ-behind-BG priority (flags bit7=1): sprite hidden where BG color != 0 */
        GB *g = fresh();
        g->lcdc = 0x93; g->bgp = 0xE4; g->obp0 = 0xE4;
        uint8_t solid3[16]; for(int i=0;i<16;i++) solid3[i]=0xFF;
        set_tile(g, 0, solid3);            /* BG solid c3 */
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t solid2[16]; for(int i=0;i<8;i++){solid2[i*2]=0;solid2[i*2+1]=0xFF;}
        set_tile(g, 1, solid2);            /* sprite solid c2 */
        g->oam[0]=16; g->oam[1]=8; g->oam[2]=1; g->oam[3]=0x80;  /* bit7 set */
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);    /* BG wins because BG color != 0 */
        gb_free(g);
    }
    {   /* X-flip: pattern reversed */
        GB *g = fresh();
        g->lcdc = 0x93; g->bgp = 0xE4; g->obp0 = 0xE4;
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t t[16]; memset(t,0,16);
        t[1] = 0x80;     /* row0 hi byte bit7 -> color 2 at leftmost pixel */
        set_tile(g, 1, t);
        g->oam[0]=16; g->oam[1]=8; g->oam[2]=1; g->oam[3]=0x20;  /* X-flip */
        render_frame(g);
        ASSERT_EQ(px(g, 7, 0), 2);    /* leftmost pixel now at rightmost */
        ASSERT_EQ(px(g, 0, 0), 0);
        gb_free(g);
    }
    {   /* DMG X priority: lower X wins regardless of OAM order */
        GB *g = fresh();
        g->lcdc = 0x93; g->bgp = 0xE4; g->obp0 = 0xE4; g->obp1 = 0xE4;
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t s2[16]; for(int i=0;i<8;i++){s2[i*2]=0;s2[i*2+1]=0xFF;}   /* color2 */
        uint8_t s3[16]; for(int i=0;i<16;i++) s3[i]=0xFF;                  /* color3 */
        set_tile(g, 1, s2); set_tile(g, 2, s3);
        /* entry0: x=10 tile1(c2); entry1: x=8 tile2(c3). lower x (entry1) wins at overlap */
        g->oam[0]=16; g->oam[1]=10; g->oam[2]=1; g->oam[3]=0;
        g->oam[4]=16; g->oam[5]=8;  g->oam[6]=2; g->oam[7]=0;
        render_frame(g);
        ASSERT_EQ(px(g, 2, 0), 3);   /* overlap region: smaller-X sprite (c3) wins */
        gb_free(g);
    }
    {   /* 8x16 sprite: lower tile used for rows 8-15 */
        GB *g = fresh();
        g->lcdc = 0x97;             /* LCD on, BG on, OBJ enable, OBJ size 8x16(bit2), tiledata 8000 */
        g->bgp = 0xE4; g->obp0 = 0xE4;
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t top[16];    for(int i=0;i<8;i++){top[i*2]=0;top[i*2+1]=0xFF;}    /* c2 */
        uint8_t bot[16];    for(int i=0;i<16;i++) bot[i]=0xFF;                    /* c3 */
        set_tile(g, 2, top);   /* tile&0xFE = 2 */
        set_tile(g, 3, bot);   /* tile|1   = 3 */
        g->oam[0]=16; g->oam[1]=8; g->oam[2]=2; g->oam[3]=0;
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 2);    /* top half */
        ASSERT_EQ(px(g, 0, 8), 3);    /* bottom half */
        gb_free(g);
    }
    {   /* 10-sprites-per-line limit: 11th on the same line is dropped */
        GB *g = fresh();
        g->lcdc = 0x93; g->bgp = 0xE4; g->obp0 = 0xE4;
        memset(g->vram + 0x1800, 0, 0x400);
        uint8_t s3[16]; for(int i=0;i<16;i++) s3[i]=0xFF;
        set_tile(g, 1, s3);
        for (int i = 0; i < 11; i++) {     /* all on line 0, increasing X */
            g->oam[i*4+0]=16; g->oam[i*4+1]=(uint8_t)(8 + i*8);
            g->oam[i*4+2]=1;  g->oam[i*4+3]=0;
        }
        render_frame(g);
        ASSERT_EQ(px(g, 0, 0), 3);          /* sprite 0 visible */
        ASSERT_EQ(px(g, 8*9, 0), 3);        /* sprite 9 (10th) visible */
        ASSERT_EQ(px(g, 8*10, 0), 0);       /* sprite 10 (11th) dropped */
        gb_free(g);
    }
```

- [ ] **Step 3: Run, verify failure** — `make test`.

- [ ] **Step 4: Implement sprites in `ppu.c`**

Add the Mode-2 scan in `render_begin_line` (append before its end):

```c
    /* Mode 2 OAM scan: select up to 10 sprites covering this line, in OAM order */
    g->spr_count = 0;
    int height = (g->lcdc & 0x04) ? 16 : 8;
    for (int i = 0; i < 40 && g->spr_count < 10; i++) {
        int sy = g->oam[i * 4 + 0];
        int line = g->ly + 16 - sy;
        if (line >= 0 && line < height)
            g->spr_idx[g->spr_count++] = (uint8_t)i;
    }
```

In `render_step`, capture the BG color before palette and add sprite mixing. Replace the final pixel-write portion of `render_step` (the part after popping `color` from the fifo) with:

```c
    if (!(g->lcdc & 0x01)) color = 0;     /* BG/window disabled -> color 0 */
    g->bg_color_at = color;               /* pre-palette BG color for priority test */
    uint8_t shade = pal_shade(g->bgp, color);

    /* sprites */
    if ((g->lcdc & 0x02) && g->spr_count > 0) {
        int best = -1, best_x = 256;
        uint8_t best_color = 0, best_pal = 0, best_prio = 0;
        int height = (g->lcdc & 0x04) ? 16 : 8;
        for (int k = 0; k < g->spr_count; k++) {
            int i = g->spr_idx[k];
            int sx = g->oam[i*4+1];
            int screen_x = sx - 8;
            if (g->fx < screen_x || g->fx >= screen_x + 8) continue;
            int col_in = g->fx - screen_x;          /* 0..7 from left */
            uint8_t flags = g->oam[i*4+3];
            if (flags & 0x20) col_in = 7 - col_in;  /* X-flip */
            int line = g->ly + 16 - g->oam[i*4+0];  /* 0..height-1 */
            if (flags & 0x40) line = height - 1 - line;  /* Y-flip */
            int tile = g->oam[i*4+2];
            if (height == 16) { tile &= 0xFE; if (line >= 8) { tile |= 1; line -= 8; } }
            uint8_t lo = g->vram[tile*16 + line*2];
            uint8_t hi = g->vram[tile*16 + line*2 + 1];
            int b = 7 - col_in;
            uint8_t scolor = (uint8_t)(((hi >> b) & 1) << 1 | ((lo >> b) & 1));
            if (scolor == 0) continue;              /* transparent */
            /* DMG priority: smallest X wins; tie -> earliest OAM (k order) */
            if (sx < best_x) {
                best = i; best_x = sx; best_color = scolor;
                best_pal = (flags & 0x10) ? g->obp1 : g->obp0;
                best_prio = flags & 0x80;
            }
        }
        if (best >= 0) {
            bool behind = best_prio && (g->bg_color_at != 0);
            if (!behind) shade = pal_shade(best_pal, best_color);
        }
    }

    g->framebuffer[g->ly * 160 + g->fx] = shade;
    g->fx++;
```

(Remove the old single-line framebuffer write + `g->fx++` that the BG-only version had, so it isn't duplicated.)

- [ ] **Step 5: Run, verify pass** — `make test`. Sprite bugs are usually flip math or the X-priority tiebreak; print sprite attrs vs expected. Re-run `make blargg` (PASS).

- [ ] **Step 6: Commit**

```bash
git add src/gb/ppu.c src/gb/gb.h tests/test_ppu_render.c
git commit -m "feat: sprite rendering (scan, flips, 8x16, DMG priority, palettes)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: dmg-acid2 acceptance gate

**Files:**
- Create: `tests/dmg_acid2.c`
- Modify: `Makefile`, `.github/workflows/ci.yml`, `README.md`

dmg-acid2 (by Matt Currie) renders a single reference image and writes "Passed"/"Failed" decisions visually; the standard automated check is to run until the test's VBlank handler signals completion and then compare the framebuffer to the known-good reference. The simplest robust automation: run a fixed number of frames, then compare the framebuffer against a checked-in expected hash/array generated from a reference emulator. We avoid image libs by hashing the framebuffer.

- [ ] **Step 1: Fetch the ROM**

```bash
cd /Users/wjbr/src/live-gameboy.ai
curl -L -o roms/dmg-acid2.gb https://github.com/mattcurrie/dmg-acid2/releases/download/v1.0/dmg-acid2.gb
ls -l roms/dmg-acid2.gb
```

Expected: the file downloads (it's ~32KB). `roms/` is gitignored.

- [ ] **Step 2: Write `tests/dmg_acid2.c`**

```c
/* Runs dmg-acid2 for enough frames to settle, then prints an FNV-1a hash of the
   160x144 framebuffer. First run: record the hash, eyeball-verify the image is the
   acid2 face by dumping a PGM, then bake the hash in as EXPECTED. */
#include "../src/gb/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Baked after manual verification in Step 4. 0 means "print only". */
#define EXPECTED_HASH 0ULL

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "roms/dmg-acid2.gb";
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 2; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)sz);
    if (fread(data, 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); return 2; }
    fclose(fp);

    GB *g = gb_new();
    if (!gb_load_rom(g, data, (size_t)sz)) { fprintf(stderr, "bad rom\n"); return 2; }
    free(data);
    gb_reset(g);

    /* run 60 frames (~1s) so the ROM finishes drawing */
    for (int f = 0; f < 60; f++) {
        g->frame_ready = false;
        while (!g->frame_ready) gb_step(g);
    }

    const uint8_t *fb = gb_framebuffer(g);
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < 160 * 144; i++) { h ^= fb[i]; h *= 1099511628211ULL; }

    /* dump a PGM for manual inspection */
    FILE *out = fopen("build/acid2.pgm", "wb");
    if (out) {
        fprintf(out, "P5\n160 144\n3\n");
        for (int i = 0; i < 160 * 144; i++) { uint8_t s = 3 - fb[i]; fputc(s, out); }
        fclose(out);
    }

    printf("dmg-acid2 framebuffer hash: 0x%016llx\n", (unsigned long long)h);
    if (EXPECTED_HASH == 0ULL) { printf("(no expected hash baked yet)\n"); gb_free(g); return 0; }
    if (h == EXPECTED_HASH) { printf("PASS dmg-acid2\n"); gb_free(g); return 0; }
    printf("FAIL dmg-acid2 (hash mismatch)\n"); gb_free(g); return 1;
}
```

- [ ] **Step 3: Add Makefile targets**

```makefile
$(BUILD)/dmg_acid2: tests/dmg_acid2.c $(GB_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< $(GB_OBJ) -o $@

acid2: $(BUILD)/dmg_acid2
	./$(BUILD)/dmg_acid2 roms/dmg-acid2.gb
```

Add `acid2` to `.PHONY`.

- [ ] **Step 4: First run + manual verification (CRITICAL)**

Run: `make acid2`
This prints a hash and writes `build/acid2.pgm`. Open the PGM (e.g. `open build/acid2.pgm` on macOS, or convert) and confirm it shows the dmg-acid2 reference: a smiling face with specific features. The official reference image is at https://github.com/mattcurrie/dmg-acid2 (README). Compare visually feature-by-feature (eyes, mouth, the "Hello World"-style elements).

**If the image is wrong:** there is a rendering bug. Use systematic debugging — dmg-acid2 is specifically designed so each face feature maps to a specific PPU behavior (the project README documents which feature tests which behavior, e.g. window, sprite priority, 8x16, palette). Identify the broken feature, write a focused unit test in `tests/test_ppu_render.c` reproducing it, fix `ppu.c`, re-run. Do not bake a hash for a wrong image.

**If the image is correct:** bake the printed hash into `EXPECTED_HASH` in `tests/dmg_acid2.c`, re-run `make acid2`, confirm it prints `PASS dmg-acid2`.

- [ ] **Step 5: Update CI + README**

In `.github/workflows/ci.yml`, add after the blargg step:

```yaml
      - name: dmg-acid2
        run: |
          mkdir -p roms
          curl -L -o roms/dmg-acid2.gb https://github.com/mattcurrie/dmg-acid2/releases/download/v1.0/dmg-acid2.gb
          make acid2
```

In `README.md`, check off Milestone 2 PPU progress: change the milestone-2 line to note the PPU is done (headless; SDL shell + APU pending). Use:

```markdown
- [~] Milestone 2: SDL3 shell, pixel-FIFO PPU, APU — PPU done (headless, passes dmg-acid2); shell + APU pending
```

- [ ] **Step 6: Full verification + commit**

Run: `make clean && make test && make blargg && make acid2`
Expected: all unit tests 0 failures; blargg both PASS; `PASS dmg-acid2`.

```bash
git add tests/dmg_acid2.c Makefile .github/workflows/ci.yml README.md
git commit -m "test: dmg-acid2 PPU acceptance gate

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Self-review notes (already applied)

- **Spec coverage:** implements spec §3 PPU (pixel-FIFO pipeline, BG/window/sprites, STAT/VBlank interrupts, mode timing, VRAM/OAM blocking, OAM DMA) and the dmg-acid2 acceptance bar. The pixel-FIFO is structurally real (on-demand 8-pixel fetch, per-dot emit, SCX discard, mid-line window switch) but not cycle-exact in Mode 3 *length* — dmg-acid2 is a rendering-correctness test and does not require exact Mode 3 timing; cycle-exact Mode 3 and cycle-accurate OAM DMA are deferred (noted inline). This is acceptable for the milestone and for the live-coding use case; mid-scanline raster effects that need exact timing are out of scope until a game needs them.
- **Type/field consistency:** new GB fields (`lcdc..wx`, `ppu_mode`, `ppu_dot`, `stat_line`, `framebuffer`, `win_line`, `frame_ready`, `fx`, `discard`, `fetch_step`, `fetch_x`, `bg_fifo_c`, `bg_fifo_n`, `window_active`, `win_started`, `oam_dma_src`, `spr_idx`, `spr_count`, `bg_color_at`) are declared in Task 1/4/5/6 before use. `gb_ppu_*` names are consistent across ppu.c, bus.c, gb.c, and gb.h.
- **Regression guard:** every task re-runs `make blargg` because removing the LY=0x90 stub hands LY to the real PPU; cpu_instrs/instr_timing must keep passing.
- **Known soft spots flagged inline:** simplified (instantaneous) OAM DMA, simplified Mode-3 timing, and the manual visual-verification step before baking the acid2 hash.
```
