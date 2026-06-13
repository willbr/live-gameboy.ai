/*
 * test_ide_tilepaint.c — regression test for the IDE tile-editor mapping bug.
 *
 * Painting in the tile editor must edit the SAME tile that is selected in the
 * VRAM viewer. The bug: ide_paint_at treated the selected VRAM-tile index as a
 * global INCBIN-asset tile index, so selecting VRAM tile 1 (the ball) painted
 * asset tile 1 (the paddle) instead.
 *
 * examples/pong.asm loads ball.2bpp -> VRAM tile 1 ($8010) and paddle.2bpp ->
 * VRAM tile 2 ($8020), so the VRAM index and asset index differ — the exact
 * condition that exposes the bug.
 */

#include "test.h"
#include "../src/ide/ide.h"
#include "../src/gb/gb.h"

#include <string.h>

static void run_frames(IdeState *s, int n) {
    for (int i = 0; i < n; i++) ide_step_frame(s);
}

/* Painting the selected VRAM tile edits THAT tile (not a different one). */
static void test_paint_edits_selected_vram_tile(void) {
    IdeState *s = ide_new("examples/pong.asm");
    ASSERT_TRUE(s != NULL);
    if (!s) return;

    run_frames(s, 10);                 /* init copies tiles to VRAM (+provenance) */
    GB *gb = ide_gb(s);

    /* Sanity: pong loads solid tiles to VRAM 1 (ball) and 2 (paddle). */
    ASSERT_EQ(gb->vram[1 * 16], 0xFF);
    ASSERT_EQ(gb->vram[2 * 16], 0xFF);

    uint8_t ball_before[16], paddle_before[16];
    memcpy(ball_before,   &gb->vram[1 * 16], 16);
    memcpy(paddle_before, &gb->vram[2 * 16], 16);

    /* Select the BALL (VRAM tile 1) and paint its top-left pixel a light shade. */
    ide_select_tile(s, 1);
    ide_set_paint_color(s, 0);

    /* Tile-editor zoomed pixel (0,0): panel origin (8,478), inner (+4,+12),
     * zoom 5 -> pixel (0,0) covers canvas x[12,17), y[490,495). */
    bool ok = ide_paint_at(s, 13, 491);
    ASSERT_TRUE(ok);

    /* The selected tile (ball) must have changed; the paddle must be untouched. */
    ASSERT_TRUE(memcmp(&gb->vram[1 * 16], ball_before,   16) != 0); /* ball edited */
    ASSERT_TRUE(memcmp(&gb->vram[2 * 16], paddle_before, 16) == 0); /* paddle intact */

    ide_free(s);
}

int main(void) {
    test_paint_edits_selected_vram_tile();
    TEST_MAIN_END();
}
