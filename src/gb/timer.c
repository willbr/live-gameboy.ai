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
