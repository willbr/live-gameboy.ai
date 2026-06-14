#include "test.h"
#include "example_run.h"

/* Boots and renders a non-blank 3D view into the framebuffer. Rendering is
 * heavy (20 columns x up to 18 tiles), so give it generous frames/steps. */
static void test_raycaster_boots(void) {
    AsmResult r = ex_assemble("examples/raycaster.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }
    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 60, 20000000);
    ASSERT_TRUE(ex_fb_nonblank(gb));   /* a 3D corridor was drawn */
    gb_free(gb); asm_free(&r);
}

/* Init powers on the APU and routes channels (NR51=$FF). */
static void test_raycaster_apu_on(void) {
    AsmResult r = ex_assemble("examples/raycaster.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);
    ASSERT_EQ(gb_read8(gb, 0xFF25), 0xFF);
    gb_free(gb); asm_free(&r);
}

/* Turning: holding Left changes the heading variable ($C0A2) over time.
 * Edge-detection means it cycles through directions; we just assert it differs
 * from the start value at some point. */
static void test_raycaster_turns(void) {
    AsmResult r = ex_assemble("examples/raycaster.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 10, 4000000);
    uint8_t d0 = gb_read8(gb, 0xC0A2);   /* heading */
    /* Tap Right repeatedly (release between taps so edge-detect fires). */
    int changed = 0;
    for (int i = 0; i < 6 && !changed; i++) {
        gb_set_buttons(gb, 0x10);        /* bit4 = Right pressed */
        ex_run(gb, 3, 2000000);
        gb_set_buttons(gb, 0x00);        /* release */
        ex_run(gb, 3, 2000000);
        if (gb_read8(gb, 0xC0A2) != d0) changed = 1;
    }
    ASSERT_TRUE(changed);                 /* heading changed by turning */
    gb_free(gb); asm_free(&r);
}

/* Moving: start facing North at cell (4,6); (4,5) is empty in the maze so a
 * forward step (Up) changes py ($C0A1) AND changes the rendered tilemap. */
static void test_raycaster_moves(void) {
    AsmResult r = ex_assemble("examples/raycaster.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 10, 4000000);
    uint8_t py0 = gb_read8(gb, 0xC0A1);
    /* snapshot the whole visible BG tilemap (18 rows x 20 cols) before moving.
     * The corridor's side walls are self-similar along its length, so only the
     * far wall near the centre changes on a step — a full-screen compare. */
    uint8_t before[18 * 20];
    for (int row = 0; row < 18; row++)
        for (int col = 0; col < 20; col++)
            before[row * 20 + col] = gb->vram[0x1800 + row * 32 + col];

    /* Tap Up (forward) a few times, releasing between taps. */
    uint8_t py1 = py0;
    for (int i = 0; i < 6; i++) {
        gb_set_buttons(gb, 0x40);        /* bit6 = Up pressed */
        ex_run(gb, 3, 2000000);
        gb_set_buttons(gb, 0x00);
        ex_run(gb, 3, 2000000);
        py1 = gb_read8(gb, 0xC0A1);
        if (py1 != py0) break;
    }
    ASSERT_TRUE(py1 != py0);             /* player advanced a cell */

    /* the rendered view changed too */
    int differ = 0;
    for (int row = 0; row < 18 && !differ; row++)
        for (int col = 0; col < 20; col++)
            if (gb->vram[0x1800 + row * 32 + col] != before[row * 20 + col]) { differ = 1; break; }
    ASSERT_TRUE(differ);                  /* tilemap re-cast after the move */
    gb_free(gb); asm_free(&r);
}

/* Turning fires a CH1 blip (SfxTurn): watch NR52 bit0 go active. */
static void test_raycaster_turn_sfx(void) {
    AsmResult r = ex_assemble("examples/raycaster.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 10, 4000000);
    gb_set_buttons(gb, 0x10);            /* Right pressed -> turn -> SfxTurn */
    uint8_t seen = ex_run_watch_nr52(gb, 20, 6000000);
    ASSERT_TRUE(seen & 0x01);            /* CH1 was triggered */
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_raycaster_boots();
    test_raycaster_apu_on();
    test_raycaster_turns();
    test_raycaster_moves();
    test_raycaster_turn_sfx();
    TEST_MAIN_END();
}
