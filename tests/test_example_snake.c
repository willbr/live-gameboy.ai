#include "test.h"
#include "example_run.h"

static void test_snake_boots(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }
    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);
    ASSERT_TRUE(ex_fb_nonblank(gb));   /* snake + food drawn into the tilemap */
    gb_free(gb); asm_free(&r);
}

/* The head advances on its own (default dir = right => headX increases). */
static void test_snake_moves(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t x0 = gb_read8(gb, 0xC000);
    ex_run(gb, 60, 8000000);            /* > throttle frames */
    uint8_t x1 = gb_read8(gb, 0xC000);
    ASSERT_TRUE(x1 != x0);              /* head moved */
    gb_free(gb); asm_free(&r);
}

/* Pressing Down changes heading so headY increases over time. */
static void test_snake_turns_down(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t y0 = gb_read8(gb, 0xC001);
    gb_set_buttons(gb, 0x80);           /* bit7 = Down */
    ex_run(gb, 80, 9000000);
    uint8_t y1 = gb_read8(gb, 0xC001);
    ASSERT_TRUE(y1 > y0);
    gb_free(gb); asm_free(&r);
}

/* Holding Left steers the snake left so headX decreases (would fail if Left
 * collapses to Down due to the register-clobber bug). */
static void test_snake_turns_left(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t x0 = gb_read8(gb, 0xC000);
    gb_set_buttons(gb, 0x20);            /* bit5 = Left */
    ex_run(gb, 80, 9000000);
    uint8_t x1 = gb_read8(gb, 0xC000);
    ASSERT_TRUE(x1 < x0);               /* moved left */
    gb_free(gb); asm_free(&r);
}

/* Init powers on the APU and routes channels (NR51=$FF vs reset $F3). */
static void test_snake_apu_on(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);
    ASSERT_EQ(gb_read8(gb, 0xFF25), 0xFF);
    gb_free(gb); asm_free(&r);
}

/* Eating food (default: head moves right into food at (15,9)) fires a CH1 chime. */
static void test_snake_eat_sfx(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    uint8_t seen = ex_run_watch_nr52(gb, 160, 9000000);
    ASSERT_TRUE(seen & 0x01);
    gb_free(gb); asm_free(&r);
}

/* Count BG tilemap cells ($9800..$9BFF) whose tile index is in [lo,hi]. */
static int snake_count_tiles(GB *gb, int lo, int hi) {
    int n = 0;
    for (int i = 0x1800; i < 0x1C00; i++)
        if (gb->vram[i] >= lo && gb->vram[i] <= hi) n++;
    return n;
}

/* Body tiles: straight horizontal (1), straight vertical (11), or a corner
 * bend (12-15). */
static int snake_body_cells(GB *gb) {
    return snake_count_tiles(gb, 1, 1) + snake_count_tiles(gb, 11, 15);
}

/* The snake is drawn as a tube: a directional head (3-6), straight/corner body
 * pieces (1, 11-15), and a directional tail (7-10), with the apple at tile 2.
 * Eating food GROWS it: drawn cells track $C003, head+tail stay unique. */
static void test_snake_grows(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_EQ(snake_count_tiles(gb, 3, 6), 1);   /* exactly one head */
    ASSERT_EQ(snake_count_tiles(gb, 7, 10), 1);  /* exactly one tail */
    ASSERT_EQ(snake_body_cells(gb), 1);          /* length 3 => 1 body */
    ASSERT_EQ(snake_count_tiles(gb, 2, 2), 1);   /* one apple */

    ex_run(gb, 160, 9000000);                    /* head moves right into apple */
    uint8_t len = gb_read8(gb, 0xC003);
    ASSERT_TRUE(len >= 4);                        /* ate at least once */
    ASSERT_EQ(snake_count_tiles(gb, 3, 6), 1);   /* still one head */
    ASSERT_EQ(snake_count_tiles(gb, 7, 10), 1);  /* still one tail */
    ASSERT_EQ(snake_body_cells(gb), len - 2);    /* body = length - head - tail */
    ASSERT_EQ(snake_count_tiles(gb, 2, 2), 1);   /* apple respawned */
    gb_free(gb); asm_free(&r);
}

/* Turning must produce a corner tile (12-15): drive the snake right then down
 * and confirm a bend appears where it changed direction. */
static void test_snake_corner(void) {
    AsmResult r = ex_assemble("examples/snake.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 40, 3000000);
    ASSERT_EQ(snake_count_tiles(gb, 12, 15), 0); /* straight so far: no corners */
    gb_set_buttons(gb, 0x80);                     /* Down */
    /* The bend exists only while it sits between head and tail; on a length-3
     * snake that is a single step, so poll a few frames at a time for it. */
    int saw_corner = 0;
    for (int i = 0; i < 8 && !saw_corner; i++) {
        ex_run(gb, 6, 1000000);
        if (snake_count_tiles(gb, 12, 15) >= 1) saw_corner = 1;
    }
    ASSERT_TRUE(saw_corner);                      /* a bend was drawn at the turn */
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_snake_boots();
    test_snake_moves();
    test_snake_turns_down();
    test_snake_turns_left();
    test_snake_apu_on();
    test_snake_eat_sfx();
    test_snake_grows();
    test_snake_corner();
    TEST_MAIN_END();
}
