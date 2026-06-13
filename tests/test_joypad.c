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
