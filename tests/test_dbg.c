#include "test.h"
#include "../src/gb/gb.h"
#include "../src/gb/debug.h"
#include <string.h>

int main(void) {
    GB *g = gb_new();
    GbDebug *d = gb_debug_attach(g);
    ASSERT_TRUE(d != NULL);
    ASSERT_TRUE(g->dbg == d);

    /* toggle adds then removes */
    int i = gb_debug_toggle_bp(g, 0, 0x0150);
    ASSERT_EQ(i, 0);
    ASSERT_EQ(d->bp_count, 1);
    ASSERT_EQ(gb_debug_find_bp(g, 0, 0x0150), 0);
    ASSERT_EQ(gb_debug_toggle_bp(g, 0, 0x0150), -1);  /* removed */
    ASSERT_EQ(d->bp_count, 0);

    /* bank-gated find: a banked bp only matches its bank */
    gb_debug_toggle_bp(g, 2, 0x4000);
    ASSERT_EQ(gb_debug_find_bp(g, 2, 0x4000), 0);
    ASSERT_EQ(gb_debug_find_bp(g, 3, 0x4000), -1);

    /* watchpoint add/clear */
    int w = gb_debug_add_wp(g, 0xC000, true, true);
    ASSERT_TRUE(w == 0 && d->wp_count == 1);
    gb_debug_clear_wp(g, 0);
    ASSERT_EQ(d->wp_count, 0);

    gb_free(g);

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
        ASSERT_TRUE(d2->hit);
        ASSERT_EQ(d2->hit_kind, DBG_BREAKPOINT);
        ASSERT_EQ(d2->hit_pc, 0x0151);
        ASSERT_EQ(g2->cpu.pc, 0x0151);   /* paused AT the bp */
        ASSERT_EQ(g2->cpu.a, 1);         /* only the INC A ran, once */

        /* resume arms a one-shot skip so we step past the bp instead of re-breaking */
        gb_debug_resume(g2);
        ASSERT_TRUE(!d2->hit);
        gb_step(g2);                   /* executes the JP, lands back at 0150 */
        ASSERT_TRUE(!d2->hit);
        ASSERT_EQ(g2->cpu.pc, 0x0150);
        gb_free(g2);
    }

    TEST_MAIN_END();
}
