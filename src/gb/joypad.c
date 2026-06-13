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
