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
    uint8_t op  = rd(gb, bank, addr);
    uint8_t d8  = rd(gb, bank, (uint16_t)(addr + 1));
    uint8_t d8b = rd(gb, bank, (uint16_t)(addr + 2));
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
