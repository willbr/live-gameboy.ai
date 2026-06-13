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

static void halt(GB *g) { g->cpu.halted = true; }

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

    /* Temporary references to silence -Werror=unused-function until later tasks consume these */
    (void)get_flag; (void)cond; (void)get_rp;

    return (int)(g->cycles - start);
}

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
    default:
        fprintf(stderr, "unimplemented opcode 0x%02X at PC=0x%04X\n", op, c->pc - 1);
        abort();
    }
}
