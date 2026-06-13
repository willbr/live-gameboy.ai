#include "test.h"
#include "../src/gb/gb.h"
#include "../src/gb/debug.h"

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
    TEST_MAIN_END();
}
