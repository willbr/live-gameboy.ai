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

static void exec(GB *g, uint8_t op);

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

static void halt(GB *g) {
    CPU *c = &g->cpu;
    uint8_t pending = g->ie & g->iflag & 0x1F;
    if (!c->ime && pending)
        c->halt_bug = true;       /* HALT bug: next opcode byte read twice */
    else
        c->halted = true;
}

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

int gb_step(GB *g) {
    uint64_t start = g->cycles;
    CPU *c = &g->cpu;

    if (service_interrupts(g))
        return (int)(g->cycles - start);

    if (c->halted) { gb_tick(g, 4); return (int)(g->cycles - start); }

    uint8_t op = fetch8(g);
    if (c->halt_bug) { c->pc--; c->halt_bug = false; }
    exec(g, op);

    if (c->ime_pending && --c->ime_pending == 0)
        c->ime = true;

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
    if (x == 2) { alu(g, y, get_r(g, z)); return; }          /* 0x80-0xBF */
    if (x == 3 && z == 6) { alu(g, y, fetch8(g)); return; }  /* d8 forms C6,CE,D6,DE,E6,EE,F6,FE */
    if (x == 0 && z == 4) { set_r(g, y, inc8(c, get_r(g, y))); return; }  /* INC r */
    if (x == 0 && z == 5) { set_r(g, y, dec8(c, get_r(g, y))); return; }  /* DEC r */
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
    case 0x37: set_flag(c, FN, false); set_flag(c, FH, false); set_flag(c, FC, true); break;
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
    case 0xC3: { uint16_t t = fetch16(g); internal(g); c->pc = t; break; }
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
    case 0xE8: { int8_t e = (int8_t)fetch8(g); internal(g); internal(g);
                 set_flag(c, FZ, false); set_flag(c, FN, false);
                 set_flag(c, FH, (c->sp & 0x0F) + (e & 0x0F) > 0x0F);
                 set_flag(c, FC, (c->sp & 0xFF) + (e & 0xFF) > 0xFF);
                 c->sp = (uint16_t)(c->sp + e); break; }
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
