# Milestone 2b: SDL3 Shell + Joypad Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A runnable desktop binary (`live-gameboy`) that loads a `.gb` ROM and runs it in an SDL3 window at ~59.7 fps, integer-scaled, with the keyboard mapped to the Game Boy joypad. Plus a headless `--shot` mode that runs N frames and writes a PNG, so the render+input path is verifiable in CI without a display.

**Architecture:** The headless core (`libgb`) is unchanged in spirit; this milestone adds (1) the joypad hardware register FF00 + a `gb_set_buttons()` API + the joypad interrupt to the core, and (2) a thin SDL3 front-end in `src/shell/` that owns the window, an audio-free main loop, palette mapping (GB shade 0–3 → an RGBA green LCD palette), and keyboard handling. The core stays SDL-free and headless-testable; the shell depends on SDL3 (via pkg-config) and on `libgb` only through its public API (`gb_step`, `gb_framebuffer`, `gb_set_buttons`, `frame_ready`).

**Tech Stack:** C11, SDL3 (Homebrew `sdl3` 3.4.x, linked via `pkg-config --cflags --libs sdl3`). No other new deps; PNG output reuses a tiny zlib-based writer (zlib ships with the system). Existing Makefile/test harness for the core.

**Spec:** `docs/superpowers/specs/2026-06-12-live-gameboy-design.md` §3 (joypad), §7 (game screen, joypad mapping). This is Milestone 2, part B (shell). The APU is a separate later milestone (2c).

**Depends on:** Milestones 1 + 2a (merged): CPU, bus, timer, interrupts, PPU with `gb_framebuffer()`/`frame_ready`.

---

## Background the implementer needs

**Joypad register FF00 (P1/JOYP), from Pan Docs:**
```
  Bit 7-6  unused (read 1)
  Bit 5    P15 Select Action buttons   (0=Select)
  Bit 4    P14 Select Direction buttons(0=Select)
  Bit 3    P13 Input Down  or Start    (0=Pressed) (Read Only)
  Bit 2    P12 Input Up    or Select   (0=Pressed) (Read Only)
  Bit 1    P11 Input Left  or B        (0=Pressed) (Read Only)
  Bit 0    P10 Input Right or A        (0=Pressed) (Read Only)
```
The CPU writes bits 4–5 to select which nibble (directions or actions) is read on bits 0–3. **A pressed button reads as 0.** If neither group is selected (both bits 4,5 = 1), the low nibble reads 1111. If both are selected, the low nibble is the AND of both groups. A joypad interrupt (`INT_JOYPAD`, bit 4 of IF) is requested on a high→low transition of any selected input line (i.e., a button press while its group is selected); a simple, widely-used approximation is to request it whenever a selected line goes low.

**Button state:** We track 8 buttons. Convention for `gb_set_buttons(GB*, uint8_t)` bitmask (1 = pressed):
```
  bit0 A     bit1 B     bit2 Select  bit3 Start
  bit4 Right bit5 Left  bit6 Up      bit7 Down
```

**Keyboard mapping (shell default):** Z→A, X→B, Enter→Start, Right-Shift→Select, Arrow keys→D-pad.

**Frame cadence:** A DMG frame is 70224 T-cycles; the PPU sets `frame_ready` at LY=144. Run `gb_step` until `frame_ready`, then render. Target wall-clock 1/59.7273 s per frame (~16.74 ms).

**Palette:** classic DMG-green LCD, shade 0 (lightest) → 3 (darkest):
```
  0: 0xE0F8D0  1: 0x88C070  2: 0x346856  3: 0x081820   (RGB)
```

---

## File structure

```
src/gb/joypad.c          — NEW: FF00 register, button state, joypad interrupt (part of libgb)
src/gb/gb.h              — add joypad state + gb_set_buttons/gb_joypad_* API
src/gb/bus.c             — route FF00 to joypad
src/gb/gb.c              — reset joypad
tests/test_joypad.c      — headless joypad register + interrupt tests
src/shell/main.c         — SDL3 front-end: window, main loop, input; and --shot mode
src/shell/png.c          — tiny PNG writer (used by --shot); zlib-backed
src/shell/png.h
Makefile                 — add `live-gameboy` (SDL) + `shell-shot` targets; keep core tests SDL-free
```

`joypad.c` is core (hardware) and stays SDL-free + unit-tested. The SDL dependency is isolated to `src/shell/`. The core test binaries do NOT link SDL.

---

### Task 1: Joypad register (FF00) + interrupt + API

**Files:**
- Modify: `src/gb/gb.h`, `src/gb/bus.c`, `src/gb/gb.c`
- Create: `src/gb/joypad.c`, `tests/test_joypad.c`

- [ ] **Step 1: Add joypad state + API to `gb.h`**

Add to `struct GB` (near the ppu block):

```c
    /* joypad */
    uint8_t buttons;     /* 1=pressed; bit0 A,1 B,2 Sel,3 Start,4 R,5 L,6 Up,7 Down */
    uint8_t joyp_sel;    /* last-written P1 select bits (bit4 dirs, bit5 actions) */
```

Add API near the other module prototypes:

```c
/* joypad (joypad.c) */
void    gb_set_buttons(GB *gb, uint8_t mask);   /* 1=pressed; see bit layout in gb.h */
uint8_t gb_joypad_read(GB *gb);                  /* FF00 */
void    gb_joypad_write(GB *gb, uint8_t v);      /* FF00 (select bits) */
void    gb_joypad_reset(GB *gb);
```

- [ ] **Step 2: Write failing test `tests/test_joypad.c`**

```c
#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

#define BTN_A 0x01
#define BTN_B 0x02
#define BTN_SEL 0x04
#define BTN_START 0x08
#define BTN_RIGHT 0x10
#define BTN_LEFT 0x20
#define BTN_UP 0x40
#define BTN_DOWN 0x80

static GB *fresh(void) {
    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}

int main(void) {
    {   /* nothing selected -> low nibble reads 1111; top bits read 1 */
        GB *g = fresh();
        gb_write8(g, 0xFF00, 0x30);          /* deselect both groups (bits4,5=1) */
        ASSERT_EQ(gb_read8(g, 0xFF00) & 0x0F, 0x0F);
        ASSERT_EQ(gb_read8(g, 0xFF00) & 0xC0, 0xC0);
        gb_free(g);
    }
    {   /* select directions (bit4=0), press Right -> bit0 reads 0 */
        GB *g = fresh();
        gb_set_buttons(g, BTN_RIGHT);
        gb_write8(g, 0xFF00, 0x20);          /* bit5=1 (actions off), bit4=0 (dirs on) */
        ASSERT_EQ(gb_read8(g, 0xFF00) & 0x01, 0x00);   /* Right pressed */
        ASSERT_EQ(gb_read8(g, 0xFF00) & 0x02, 0x02);   /* Left not pressed */
        gb_free(g);
    }
    {   /* select actions (bit5=0), press A -> bit0 reads 0; dirs ignored */
        GB *g = fresh();
        gb_set_buttons(g, BTN_A | BTN_RIGHT);
        gb_write8(g, 0xFF00, 0x10);          /* bit4=1 (dirs off), bit5=0 (actions on) */
        ASSERT_EQ(gb_read8(g, 0xFF00) & 0x01, 0x00);   /* A pressed */
        gb_free(g);
    }
    {   /* direction/action mapping: Down=bit3, Up=bit2, Left=bit1, Right=bit0 */
        GB *g = fresh();
        gb_set_buttons(g, BTN_DOWN | BTN_UP | BTN_LEFT | BTN_RIGHT);
        gb_write8(g, 0xFF00, 0x20);
        ASSERT_EQ(gb_read8(g, 0xFF00) & 0x0F, 0x00);   /* all four dirs pressed */
        gb_free(g);
    }
    {   /* action mapping: Start=bit3, Select=bit2, B=bit1, A=bit0 */
        GB *g = fresh();
        gb_set_buttons(g, BTN_START | BTN_SEL | BTN_B | BTN_A);
        gb_write8(g, 0xFF00, 0x10);
        ASSERT_EQ(gb_read8(g, 0xFF00) & 0x0F, 0x00);
        gb_free(g);
    }
    {   /* pressing a button while its group is selected requests INT_JOYPAD */
        GB *g = fresh();
        gb_write8(g, 0xFF00, 0x20);          /* directions selected */
        g->iflag = 0;
        gb_set_buttons(g, BTN_RIGHT);        /* press while selected */
        ASSERT_EQ(g->iflag & INT_JOYPAD, INT_JOYPAD);
        gb_free(g);
    }
    TEST_MAIN_END();
}
```

- [ ] **Step 3: Run, verify failure** — `make test` (link error / no joypad).

- [ ] **Step 4: Create `src/gb/joypad.c`**

```c
#include "gb.h"

void gb_joypad_reset(GB *g) {
    g->buttons = 0;          /* none pressed */
    g->joyp_sel = 0x30;      /* both groups deselected */
}

/* compute the live low nibble for the current selection (0=pressed) */
static uint8_t joyp_low_nibble(const GB *g) {
    uint8_t low = 0x0F;
    if (!(g->joyp_sel & 0x10)) {        /* directions selected (bit4=0) */
        /* buttons bit4 R,5 L,6 Up,7 Down -> P1 bit0 R,1 L,2 Up,3 Down */
        uint8_t dirs = (uint8_t)(g->buttons >> 4) & 0x0F;
        low &= (uint8_t)~dirs;
    }
    if (!(g->joyp_sel & 0x20)) {        /* actions selected (bit5=0) */
        uint8_t acts = g->buttons & 0x0F;     /* bit0 A,1 B,2 Sel,3 Start */
        low &= (uint8_t)~acts;
    }
    return low & 0x0F;
}

uint8_t gb_joypad_read(GB *g) {
    return (uint8_t)(0xC0 | (g->joyp_sel & 0x30) | joyp_low_nibble(g));
}

void gb_joypad_write(GB *g, uint8_t v) {
    g->joyp_sel = v & 0x30;
}

void gb_set_buttons(GB *g, uint8_t mask) {
    uint8_t before = joyp_low_nibble(g);
    g->buttons = mask;
    uint8_t after = joyp_low_nibble(g);
    /* any selected line going high->low (press) requests the joypad interrupt */
    if (before & ~after) g->iflag |= INT_JOYPAD;
}
```

- [ ] **Step 5: Wire into `bus.c` and `gb.c`**

In `bus.c` `io_read`, add `case 0x00: return gb_joypad_read(gb);` and in `io_write` add `case 0x00: gb_joypad_write(gb, v); break;`.

In `gb.c` `gb_reset`, add `gb_joypad_reset(gb);` (near the `gb_ppu_reset` call).

- [ ] **Step 6: Run, verify pass** — `make test` (all pass incl. test_joypad). Re-run `make blargg` + `make acid2` (still PASS — those ROMs don't use the joypad meaningfully, but confirm no regression).

- [ ] **Step 7: Commit**

```bash
git add src/gb/joypad.c src/gb/gb.h src/gb/bus.c src/gb/gb.c tests/test_joypad.c
git commit -m "feat: joypad register (FF00) with select nibbles and joypad interrupt

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: PNG writer + headless `--shot` mode

We build the verifiable headless path first (no window needed), so the render pipeline through the shell is testable before touching SDL window/event code.

**Files:**
- Create: `src/shell/png.h`, `src/shell/png.c`, `src/shell/main.c`
- Modify: `Makefile`

- [ ] **Step 1: Create `src/shell/png.h`**

```c
#ifndef SHELL_PNG_H
#define SHELL_PNG_H
#include <stdint.h>
#include <stddef.h>
/* Write an RGBA8888 buffer (w*h*4 bytes, row-major) as a PNG file.
   Returns 0 on success, nonzero on error. */
int png_write_rgba(const char *path, const uint8_t *rgba, int w, int h);
#endif
```

- [ ] **Step 2: Create `src/shell/png.c`**

```c
#include "png.h"
#include <stdio.h>
#include <string.h>
#include <zlib.h>

static void put_be32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}

static int write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return 1;
    if (len && fwrite(data, 1, len, f) != len) return 1;
    uLong crc = crc32(0L, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, (const Bytef *)data, len);
    uint8_t crcb[4]; put_be32(crcb, (uint32_t)crc);
    return fwrite(crcb, 1, 4, f) != 4;
}

int png_write_rgba(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)w); put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8]=8; ihdr[9]=6; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;   /* 8-bit RGBA */
    if (write_chunk(f, "IHDR", ihdr, 13)) { fclose(f); return 1; }

    /* raw scanlines with filter byte 0 */
    size_t raw_len = (size_t)h * (1 + (size_t)w * 4);
    uint8_t *raw = malloc(raw_len);
    if (!raw) { fclose(f); return 1; }
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (1 + (size_t)w * 4);
        row[0] = 0;
        memcpy(row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    uLongf clen = compressBound((uLong)raw_len);
    uint8_t *comp = malloc(clen);
    if (!comp || compress2(comp, &clen, raw, (uLong)raw_len, 9) != Z_OK) {
        free(raw); free(comp); fclose(f); return 1;
    }
    free(raw);
    int rc = write_chunk(f, "IDAT", comp, (uint32_t)clen);
    free(comp);
    if (rc) { fclose(f); return 1; }
    if (write_chunk(f, "IEND", NULL, 0)) { fclose(f); return 1; }
    fclose(f);
    return 0;
}
```

Add `#include <stdlib.h>` at the top of png.c (for malloc/free).

- [ ] **Step 3: Create `src/shell/main.c` (args + shot mode only; SDL window added in Task 3)**

```c
#include "../gb/gb.h"
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t PALETTE[4] = {       /* shade 0..3 -> RGB */
    0xE0F8D0, 0x88C070, 0x346856, 0x081820
};

/* expand the 160x144 shade framebuffer into RGBA8888 (optionally integer-scaled) */
static void framebuffer_to_rgba(const uint8_t *fb, uint8_t *rgba, int scale) {
    int W = 160 * scale;
    for (int y = 0; y < 144 * scale; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t rgb = PALETTE[fb[(y / scale) * 160 + (x / scale)] & 3];
            uint8_t *p = rgba + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)(rgb >> 16); p[1] = (uint8_t)(rgb >> 8);
            p[2] = (uint8_t)rgb;         p[3] = 0xFF;
        }
    }
}

static uint8_t *load_file(const char *path, size_t *out) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f); *out = (size_t)n; return d;
}

int run_window(GB *g, int scale);   /* Task 3 (defined in main.c) */

static void run_one_frame(GB *g) {
    g->frame_ready = false;
    while (!g->frame_ready) gb_step(g);
}

static int run_shot(const char *rom, const char *out, int frames, int scale) {
    size_t n; uint8_t *data = load_file(rom, &n);
    if (!data) return 2;
    GB *g = gb_new();
    if (!gb_load_rom(g, data, n)) { fprintf(stderr, "bad rom\n"); free(data); return 2; }
    free(data);
    gb_reset(g);
    for (int i = 0; i < frames; i++) run_one_frame(g);
    int W = 160 * scale, H = 144 * scale;
    uint8_t *rgba = malloc((size_t)W * H * 4);
    framebuffer_to_rgba(gb_framebuffer(g), rgba, scale);
    int rc = png_write_rgba(out, rgba, W, H);
    free(rgba); gb_free(g);
    if (rc) { fprintf(stderr, "png write failed\n"); return 2; }
    printf("wrote %s (%dx%d, %d frames)\n", out, W, H, frames);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage:\n  %s <rom.gb> [scale]\n"
                        "  %s --shot <rom.gb> <out.png> [frames] [scale]\n",
                argv[0], argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "--shot")) {
        if (argc < 4) { fprintf(stderr, "--shot needs <rom> <out>\n"); return 2; }
        int frames = argc > 4 ? atoi(argv[4]) : 60;
        int scale  = argc > 5 ? atoi(argv[5]) : 1;
        return run_shot(argv[2], argv[3], frames, scale > 0 ? scale : 1);
    }
    /* interactive window mode (Task 3) */
    size_t n; uint8_t *data = load_file(argv[1], &n);
    if (!data) return 2;
    GB *g = gb_new();
    if (!gb_load_rom(g, data, n)) { fprintf(stderr, "bad rom\n"); free(data); return 2; }
    free(data);
    gb_reset(g);
    int scale = argc > 2 ? atoi(argv[2]) : 4;
    int rc = run_window(g, scale > 0 ? scale : 4);
    gb_free(g);
    return rc;
}
```

For now add a temporary stub so it links without SDL (Task 3 replaces it):

```c
int run_window(GB *g, int scale) {
    (void)g; (void)scale;
    fprintf(stderr, "window mode not built yet\n");
    return 1;
}
```

Put this stub at the bottom of main.c; Task 3 removes it and adds the real SDL window in a separate `src/shell/window.c` OR replaces this function.

- [ ] **Step 4: Add Makefile targets**

```makefile
# --- SDL shell (separate from the SDL-free core tests) ---
SDL_CFLAGS = $(shell pkg-config --cflags sdl3)
SDL_LIBS   = $(shell pkg-config --libs sdl3)
SHELL_SRC  = $(wildcard src/shell/*.c)

live-gameboy: $(SHELL_SRC) $(GB_OBJ)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(SHELL_SRC) $(GB_OBJ) $(SDL_LIBS) -lz -o $@

# headless screenshot for verification/CI (still links SDL but never opens a window)
shell-shot: live-gameboy
	./live-gameboy --shot roms/dmg-acid2.gb build/shell-acid2.png 60 2
```

Add `live-gameboy` to `.PHONY`? No — it's a real file output; instead add `shell-shot` and a `clean` rule update to remove `live-gameboy`. Add to the existing `clean`: `rm -rf $(BUILD) live-gameboy`.

(Note: Task 2's `main.c` references SDL only inside the not-yet-real `run_window`; since we compile the whole shell with SDL flags from the start, that's fine. The shot path doesn't call SDL.)

- [ ] **Step 5: Build + verify the shot path**

Run: `make live-gameboy && make shell-shot`
Expected: `live-gameboy` builds; `shell-shot` prints `wrote build/shell-acid2.png (320x288, 60 frames)`.

Then verify the PNG is the dmg-acid2 face (the controller will read the image). If the shell's pipeline is correct it matches what `make acid2` renders. If it's wrong (e.g. palette inverted, scaling garbled), debug the `framebuffer_to_rgba`/PNG code.

- [ ] **Step 6: Commit**

```bash
git add src/shell/ Makefile
git commit -m "feat: SDL shell skeleton with headless --shot PNG mode

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: SDL3 window, main loop, keyboard input

**Files:**
- Modify: `src/shell/main.c` (replace the `run_window` stub with a real SDL implementation)

- [ ] **Step 1: Replace the `run_window` stub in `main.c`**

Add the SDL include at the top of main.c (`#include <SDL3/SDL.h>`), and replace the stub with:

```c
/* keyboard -> button mask (1=pressed) */
static uint8_t poll_buttons(const bool *ks) {
    uint8_t m = 0;
    if (ks[SDL_SCANCODE_Z])      m |= 0x01;  /* A */
    if (ks[SDL_SCANCODE_X])      m |= 0x02;  /* B */
    if (ks[SDL_SCANCODE_RSHIFT]) m |= 0x04;  /* Select */
    if (ks[SDL_SCANCODE_RETURN]) m |= 0x08;  /* Start */
    if (ks[SDL_SCANCODE_RIGHT])  m |= 0x10;
    if (ks[SDL_SCANCODE_LEFT])   m |= 0x20;
    if (ks[SDL_SCANCODE_UP])     m |= 0x40;
    if (ks[SDL_SCANCODE_DOWN])   m |= 0x80;
    return m;
}

int run_window(GB *g, int scale) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("live-gameboy", 160 * scale, 144 * scale, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_STREAMING, 160, 144);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    static uint8_t rgba[160 * 144 * 4];
    bool running = true;
    const double frame_ms = 1000.0 / 59.7273;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_ESCAPE)
                running = false;
        }
        int nkeys;
        const bool *ks = SDL_GetKeyboardState(&nkeys);
        gb_set_buttons(g, poll_buttons(ks));

        run_one_frame(g);

        framebuffer_to_rgba(gb_framebuffer(g), rgba, 1);   /* texture is 160x144; scale via renderer */
        SDL_UpdateTexture(tex, NULL, rgba, 160 * 4);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        SDL_Delay((Uint32)frame_ms);    /* coarse pacing; good enough without audio sync */
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
```

Remove the temporary stub `run_window` from Task 2. `framebuffer_to_rgba` and `run_one_frame` already exist in main.c from Task 2 — reuse them (do not duplicate).

Note: SDL3 `SDL_GetKeyboardState` returns `const bool *` (SDL3 changed it from `Uint8*`); `SDL_PIXELFORMAT_RGBA8888` byte order matches the `framebuffer_to_rgba` writes (p[0]=R..p[3]=A). If colors look swapped on your platform, switch to `SDL_PIXELFORMAT_ABGR8888` — but verify via the window, not guesswork.

- [ ] **Step 2: Build**

Run: `make live-gameboy`
Expected: clean build, no warnings under the core CFLAGS + SDL flags.

- [ ] **Step 3: Smoke-run (manual / controller via run skill)**

Run: `./live-gameboy roms/dmg-acid2.gb 4` — a window should open showing the acid2 face; Esc/close quits. (In a headless CI this step is skipped; the `shell-shot` target is the automated check.) The controller may verify the window opens and renders by launching it briefly and screenshotting, or rely on `shell-shot`.

- [ ] **Step 4: Commit**

```bash
git add src/shell/main.c
git commit -m "feat: SDL3 window, 59.7fps main loop, keyboard joypad input

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Wire-up verification + README/CI

**Files:**
- Modify: `README.md`, `.github/workflows/ci.yml`, `Makefile`

- [ ] **Step 1: Add a `shell-shot` CI check + keep it robust**

In `.github/workflows/ci.yml`, add SDL3 to the build environment and run the headless shot. After the dmg-acid2 step add:

```yaml
      - name: install SDL3
        run: sudo apt-get update && sudo apt-get install -y libsdl3-dev zlib1g-dev || echo "SDL3 apt package unavailable; skipping shell build"
      - name: build shell + headless screenshot
        run: |
          if pkg-config --exists sdl3; then
            make live-gameboy && make shell-shot
          else
            echo "SDL3 not available on runner; skipping shell build"
          fi
```

(SDL3 may not be in the runner's apt yet; the guard keeps CI green. The shell still builds locally where SDL3 is installed.)

- [ ] **Step 2: Update `README.md`**

Change the Milestone 2 line to mark the shell done (APU still pending) and add a "Run" note:

```markdown
- [~] Milestone 2: SDL3 shell, pixel-FIFO PPU, APU — PPU + shell done; APU pending
```

Add under Build:

```markdown
## Run

    make live-gameboy            # build the SDL app (needs SDL3 via pkg-config)
    ./live-gameboy game.gb [scale]
    ./live-gameboy --shot game.gb out.png [frames] [scale]   # headless screenshot

Keys: Z=A, X=B, Enter=Start, RShift=Select, Arrows=D-pad, Esc=quit.
```

- [ ] **Step 3: Full verification**

Run: `make clean && make test && make blargg && make acid2 && make live-gameboy && make shell-shot`
Expected: all core unit tests 0 failures; blargg + acid2 PASS; `live-gameboy` builds; `shell-shot` writes the PNG. The controller verifies `build/shell-acid2.png` shows the acid2 face.

- [ ] **Step 4: Commit**

```bash
git add README.md .github/workflows/ci.yml Makefile
git commit -m "chore: shell CI screenshot check + run docs

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Self-review notes (already applied)

- **Spec coverage:** implements spec §3 joypad register + interrupt and §7 game-screen/joypad-mapping (SDL window, integer scale, keyboard binds). The core gains a unit-tested `joypad.c`; the SDL dependency is isolated to `src/shell/` and the core test binaries stay SDL-free.
- **Verifiability:** the `--shot` headless mode renders through the exact shell pipeline (palette + scaling) and dumps a PNG, so the front-end render path is checkable without a display (controller reads the PNG; CI runs `shell-shot` where SDL3 is present). Interactive window behavior is the one part that needs a real display — smoke-tested manually.
- **Type/name consistency:** `gb_set_buttons`/`gb_joypad_read`/`gb_joypad_write`/`gb_joypad_reset` declared in gb.h and used in joypad.c, bus.c, gb.c, and the shell. Button bit layout is documented once in gb.h and matched in joypad.c and the shell's `poll_buttons`. `framebuffer_to_rgba`/`run_one_frame` are defined once in main.c and reused by both shot and window paths.
- **Deferred (noted):** audio (Milestone 2c APU); precise frame pacing/audio-sync (coarse `SDL_Delay` for now); save-RAM persistence and gamepad input (later). The joypad interrupt uses the common press-edge approximation, sufficient for games and not exercised by an accuracy ROM in this milestone.
```
