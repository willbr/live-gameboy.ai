#include "test.h"
#include "example_run.h"

/* Boot: pong.asm assembles, the LCD comes on, and the screen is not blank. */
static void test_pong_boots(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    if (!r.ok) { asm_free(&r); return; }

    GB *gb = gb_new();
    ASSERT_TRUE(gb_load_rom(gb, r.rom, r.rom_size));
    gb_reset(gb);
    ex_run(gb, 30, 6000000);          /* 30 frames */
    ASSERT_TRUE(ex_fb_nonblank(gb));  /* paddles + ball are drawn */

    gb_free(gb);
    asm_free(&r);
}

/* The ball moves on its own over a few frames. */
static void test_pong_ball_moves(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 3, 2000000);
    uint8_t x0 = gb_read8(gb, 0xC0A0), y0 = gb_read8(gb, 0xC0A1);
    ex_run(gb, 20, 6000000);
    uint8_t x1 = gb_read8(gb, 0xC0A0), y1 = gb_read8(gb, 0xC0A1);
    ASSERT_TRUE(x1 != x0 || y1 != y0);   /* it moved */
    gb_free(gb); asm_free(&r);
}

/* The ball stays on the playfield (never wraps off the top/bottom). */
static void test_pong_ball_in_bounds(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    for (int i = 0; i < 30; i++) {
        ex_run(gb, 5, 2000000);
        uint8_t y = gb_read8(gb, 0xC0A1);
        ASSERT_TRUE(y < 152);            /* 144 playfield + margin, no wrap to ~255 */
    }
    gb_free(gb); asm_free(&r);
}

/* Holding Up moves the left paddle up (lPadY decreases). */
static void test_pong_paddle_up(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    uint8_t y0 = gb_read8(gb, 0xC0A4);
    gb_set_buttons(gb, 0x40);            /* bit6 = Up held */
    ex_run(gb, 20, 6000000);
    uint8_t y1 = gb_read8(gb, 0xC0A4);
    ASSERT_TRUE(y1 < y0);                /* paddle moved up */
    gb_free(gb); asm_free(&r);
}

/* Init powers on the APU and routes all channels to both speakers.
 * NR51=$FF distinguishes our init from the DMG reset default ($F3), so this
 * actually exercises the added power-on block (not just the reset state). */
static void test_pong_apu_on(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    ex_run(gb, 5, 2000000);
    ASSERT_TRUE(gb_read8(gb, 0xFF26) & 0x80);   /* NR52 power bit */
    ASSERT_EQ(gb_read8(gb, 0xFF25), 0xFF);      /* NR51 set by init (reset=$F3) */
    gb_free(gb); asm_free(&r);
}

/* A CH1 blip fires during a rally (paddle/wall/score bounces). */
static void test_pong_sfx_fires(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    uint8_t seen = ex_run_watch_nr52(gb, 250, 9000000);
    ASSERT_TRUE(seen & 0x01);
    gb_free(gb); asm_free(&r);
}

/* The APU produces NON-SILENT PCM during gameplay, drained the way the IDE
 * does it: step with gb_step only (mirroring ide_run_slice) and pull samples
 * via gb_audio_read. Guards the full APU->PCM audibility path — a stronger
 * check than the NR52 status bit (which can be set while output is silent). */
static void test_pong_audio_nonsilent(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);                       /* APU sample rate defaults to 48000 */
    float buf[4096];
    long total = 0;
    float peak = 0.0f;
    for (int f = 0; f < 400; f++) {
        gb->frame_ready = false;
        int guard = 0;
        while (!gb->frame_ready && guard++ < 2000000) gb_step(gb);
        int n;
        while ((n = gb_audio_read(gb, buf, 4096)) > 0) {
            total += n;
            for (int i = 0; i < n; i++) {
                float a = buf[i] < 0 ? -buf[i] : buf[i];
                if (a > peak) peak = a;
            }
        }
    }
    ASSERT_TRUE(total > 0);             /* samples were produced and drained */
    ASSERT_TRUE(peak > 0.05f);          /* non-silent: an SFX moved the DAC */
    gb_free(gb); asm_free(&r);
}

/* After an SFX plays and its channel's length counter disables it, the output
 * must settle back to SILENCE — not stick at a DC offset (the bug that caused
 * a constant buzz in the IDE). Guards the mixer's DC-blocking high-pass filter:
 * a DAC-on-but-silent channel emits dac_float(0) = -1.0, which without the
 * filter would be a permanent audible offset. */
static void test_pong_audio_settles_silent(void) {
    AsmResult r = ex_assemble("examples/pong.asm");
    ASSERT_TRUE(r.ok);
    GB *gb = gb_new();
    gb_load_rom(gb, r.rom, r.rom_size);
    gb_reset(gb);
    float buf[4096];

    /* Phase 1: ~80 frames — a wall/paddle bounce fires a blip in this window. */
    float blip_peak = 0.0f;
    for (int f = 0; f < 80; f++) {
        gb->frame_ready = false; int guard = 0;
        while (!gb->frame_ready && guard++ < 2000000) gb_step(gb);
        int n;
        while ((n = gb_audio_read(gb, buf, 4096)) > 0)
            for (int i = 0; i < n; i++) {
                float a = buf[i] < 0 ? -buf[i] : buf[i];
                if (a > blip_peak) blip_peak = a;
            }
    }
    ASSERT_TRUE(blip_peak > 0.05f);    /* a blip actually played */

    /* Phase 2: a quiet window (no new bounce here) — output must decay to ~0. */
    float quiet_peak = 0.0f;
    for (int f = 0; f < 25; f++) {
        gb->frame_ready = false; int guard = 0;
        while (!gb->frame_ready && guard++ < 2000000) gb_step(gb);
        int n;
        while ((n = gb_audio_read(gb, buf, 4096)) > 0)
            for (int i = 0; i < n; i++) {
                float a = buf[i] < 0 ? -buf[i] : buf[i];
                if (a > quiet_peak) quiet_peak = a;
            }
    }
    ASSERT_TRUE(quiet_peak < 0.02f);   /* no DC offset / buzz after the blip */
    gb_free(gb); asm_free(&r);
}

int main(void) {
    test_pong_boots();
    test_pong_ball_moves();
    test_pong_ball_in_bounds();
    test_pong_paddle_up();
    test_pong_apu_on();
    test_pong_sfx_fires();
    test_pong_audio_nonsilent();
    test_pong_audio_settles_silent();
    TEST_MAIN_END();
}
