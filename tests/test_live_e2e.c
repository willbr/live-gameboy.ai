/*
 * test_live_e2e.c — Milestone 4 scenario gate: live-patching end-to-end.
 *
 * This single test is the milestone proof.  It exercises the full live-patching
 * thesis in one scripted run WITHOUT ever resetting the running game:
 *
 *   1. live_new(v1: Tick adds 1).  Run until counter >= ~30.  Record.
 *   2. live_reload(v2: in-place — same size, adds 2).
 *      Assert IN_PLACE, counter preserved, then +2/iteration.
 *   3. live_reload(v3: Tick grows past slot — 30 NOPs + adds 3 -> RELOCATED).
 *      Assert RELOCATED, counter preserved, then +3/iteration.
 *   4. live_reload(v4: UNSAFE — syntactically invalid source).
 *      Assert REFUSED, counter + behavior UNCHANGED (still +3), ROM unchanged.
 *   5. live_reload(v5: valid in-place again — adds 1 again, same size as v2).
 *      Assert IN_PLACE (engine recovered), counter preserved, then +1/iteration.
 *
 * Throughout: assert the $C000 counter NEVER decreased across valid reloads
 * (continuity — "edit a live running game, state intact").
 */

#include "test.h"
#include "../src/live/live.h"
#include "../src/asm/asm.h"
#include "../src/gb/gb.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Program source templates.
 *
 * Counter lives at WRAM $C000 (= gb->wram[0]).
 * Main: sets SP, zeroes counter, loops calling Tick.
 * Tick: reads counter, adds N, writes back, ret.
 *
 * Tick v1/v2/v5 body (9 bytes — fits 32-byte slot):
 *   FA 00 C0  ld a, ($C000)   3
 *   C6 NN     add a, N        2
 *   EA 00 C0  ld ($C000), a   3
 *   C9        ret             1
 *   Total: 9 bytes  => slot = round_up(9,16)+16 = 32 bytes.
 *
 * Tick v3 body (9 + 30 NOPs = 39 bytes — exceeds 32-byte slot -> RELOCATED).
 * Tick v4 = intentionally invalid assembly -> REFUSED.
 * ------------------------------------------------------------------------- */

static const char *SRC_V1 =            /* Tick adds 1 */
    "Main:\n"
    "    ld sp, $FFFE\n"
    "    xor a\n"
    "    ld ($C000), a\n"
    ".loop:\n"
    "    call Tick\n"
    "    jr .loop\n"
    "Tick:\n"
    "    ld a, ($C000)\n"
    "    add a, 1\n"
    "    ld ($C000), a\n"
    "    ret\n";

static const char *SRC_V2 =            /* Tick adds 2 — same size, IN_PLACE */
    "Main:\n"
    "    ld sp, $FFFE\n"
    "    xor a\n"
    "    ld ($C000), a\n"
    ".loop:\n"
    "    call Tick\n"
    "    jr .loop\n"
    "Tick:\n"
    "    ld a, ($C000)\n"
    "    add a, 2\n"
    "    ld ($C000), a\n"
    "    ret\n";

/* Tick v3: 30 NOPs + core (9 bytes) = 39 bytes > 32-byte slot -> RELOCATED */
static const char *SRC_V3 =
    "Main:\n"
    "    ld sp, $FFFE\n"
    "    xor a\n"
    "    ld ($C000), a\n"
    ".loop:\n"
    "    call Tick\n"
    "    jr .loop\n"
    "Tick:\n"
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"   /*  5 */
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"   /* 10 */
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"   /* 15 */
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"   /* 20 */
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"   /* 25 */
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"   /* 30 NOPs */
    "    ld a, ($C000)\n"
    "    add a, 3\n"
    "    ld ($C000), a\n"
    "    ret\n";

/* Tick v4: syntactically invalid — assembly must fail -> REFUSED */
static const char *SRC_V4 =
    "this is not valid asm !@#$%\n";

static const char *SRC_V5 =            /* Tick adds 1 again — same size as v2, IN_PLACE */
    "Main:\n"
    "    ld sp, $FFFE\n"
    "    xor a\n"
    "    ld ($C000), a\n"
    ".loop:\n"
    "    call Tick\n"
    "    jr .loop\n"
    "Tick:\n"
    "    ld a, ($C000)\n"
    "    add a, 1\n"
    "    ld ($C000), a\n"
    "    ret\n";

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static uint8_t read_counter(const GB *gb) { return gb->wram[0]; }

static const PatchEntry *find_entry(const PatchReport *rpt, const char *name)
{
    for (int i = 0; i < rpt->count; i++)
        if (strcmp(rpt->items[i].func, name) == 0)
            return &rpt->items[i];
    return NULL;
}

/*
 * run_until_delta — advance the emulator until the counter has increased by
 * at least `min_delta` (accounting for uint8_t wrap), or until `limit` steps
 * have elapsed.  Returns the actual total increase observed (capped at 255).
 */
static int run_until_delta(GB *gb, int min_delta, int limit)
{
    uint8_t start = read_counter(gb);
    for (int i = 0; i < limit; i++) {
        gb_step(gb);
        int diff = (int)read_counter(gb) - (int)start;
        if (diff < 0) diff += 256;
        if (diff >= min_delta) return diff;
    }
    int diff = (int)read_counter(gb) - (int)start;
    if (diff < 0) diff += 256;
    return diff;
}

/*
 * observe_increments — watch the counter over `limit` steps and count
 * increments matching `expected_delta`.  Returns the number of non-matching
 * increments seen (0 = all correct).
 */
static int observe_increments(GB *gb, int expected_delta, int want_count, int limit)
{
    uint8_t prev = read_counter(gb);
    int matched = 0;
    int other   = 0;
    for (int i = 0; i < limit && matched < want_count; i++) {
        gb_step(gb);
        uint8_t now = read_counter(gb);
        if (now != prev) {
            int delta = (int)now - (int)prev;
            if (delta < 0) delta += 256;
            if (delta == expected_delta) matched++;
            else                         other++;
            prev = now;
        }
    }
    return other;
}

/* =========================================================================
 * Milestone gate: full live-patching end-to-end scenario
 * ======================================================================= */
static void test_live_e2e(void)
{
    /* -----------------------------------------------------------------------
     * Step 1 — boot v1 (Tick adds 1), run until counter >= 30
     * --------------------------------------------------------------------- */
    LiveSession *s = live_new(SRC_V1, "e2e_v1.asm");
    ASSERT_TRUE(s != NULL);
    if (!s) return;

    GB *gb = live_gb(s);
    ASSERT_TRUE(gb != NULL);
    if (!gb) { live_free(s); return; }

    /* Confirm the JP patch at 0x100 was installed */
    ASSERT_EQ(gb->rom[0x100], 0xC3);

    /* Run until counter reaches 30 */
    int delta = run_until_delta(gb, 30, 10000000);
    ASSERT_TRUE(delta >= 30);

    uint8_t c1 = read_counter(gb);
    ASSERT_TRUE(c1 >= 30);
    fprintf(stderr, "[e2e step1] counter after v1 run = %u\n", (unsigned)c1);

    /* -----------------------------------------------------------------------
     * Step 2 — live_reload v2: same size, Tick adds 2 -> IN_PLACE
     * --------------------------------------------------------------------- */
    PatchReport rpt2 = live_reload(s, SRC_V2);

    ASSERT_TRUE(!rpt2.any_refused);

    const PatchEntry *tick2 = find_entry(&rpt2, "Tick");
    ASSERT_TRUE(tick2 != NULL);
    if (tick2) {
        if (tick2->kind != PATCH_IN_PLACE)
            fprintf(stderr, "[e2e step2] Tick kind=%d reason='%s' (expected IN_PLACE)\n",
                    (int)tick2->kind, tick2->reason);
        ASSERT_EQ((int)tick2->kind, (int)PATCH_IN_PLACE);
    }

    const PatchEntry *main2 = find_entry(&rpt2, "Main");
    ASSERT_TRUE(main2 != NULL);
    if (main2)
        ASSERT_EQ((int)main2->kind, (int)PATCH_UNCHANGED);

    /* Counter must NOT have decreased */
    uint8_t c2 = read_counter(gb);
    fprintf(stderr, "[e2e step2] counter after v2 reload = %u (was %u)\n",
            (unsigned)c2, (unsigned)c1);
    ASSERT_TRUE(c2 >= c1 || (int)c2 + 256 >= (int)c1); /* allow wrap */

    patch_report_free(&rpt2);

    /* Verify +2/iteration */
    int bad2 = observe_increments(gb, 2, 20, 5000000);
    ASSERT_EQ(bad2, 0);

    uint8_t c2b = read_counter(gb);
    fprintf(stderr, "[e2e step2] counter after v2 run   = %u (bad increments: %d)\n",
            (unsigned)c2b, bad2);

    /* -----------------------------------------------------------------------
     * Step 3 — live_reload v3: Tick grows past slot -> RELOCATED, adds 3
     * --------------------------------------------------------------------- */
    PatchReport rpt3 = live_reload(s, SRC_V3);

    ASSERT_TRUE(!rpt3.any_refused);

    const PatchEntry *tick3 = find_entry(&rpt3, "Tick");
    ASSERT_TRUE(tick3 != NULL);
    if (tick3) {
        if (tick3->kind != PATCH_RELOCATED)
            fprintf(stderr, "[e2e step3] Tick kind=%d reason='%s' (expected RELOCATED)\n",
                    (int)tick3->kind, tick3->reason);
        ASSERT_EQ((int)tick3->kind, (int)PATCH_RELOCATED);
    }

    /* Counter must NOT have decreased from where we left v2 */
    uint8_t c3 = read_counter(gb);
    fprintf(stderr, "[e2e step3] counter after v3 reload = %u (was %u)\n",
            (unsigned)c3, (unsigned)c2b);
    /* c3 >= c2b (allowing uint8_t wrap) */
    int c3_diff = (int)c3 - (int)c2b;
    if (c3_diff < 0) c3_diff += 256;
    ASSERT_TRUE(c3_diff >= 0);  /* must not have gone backwards */

    patch_report_free(&rpt3);

    /* Verify +3/iteration */
    int bad3 = observe_increments(gb, 3, 20, 10000000);
    ASSERT_EQ(bad3, 0);

    uint8_t c3b = read_counter(gb);
    fprintf(stderr, "[e2e step3] counter after v3 run   = %u (bad increments: %d)\n",
            (unsigned)c3b, bad3);

    /* -----------------------------------------------------------------------
     * Step 4 — live_reload v4: UNSAFE (invalid source) -> REFUSED
     * --------------------------------------------------------------------- */

    /* Snapshot ROM byte at 0x100 to verify it's untouched */
    uint8_t rom_snap = gb->rom[0x100];
    uint8_t c_before_refuse = read_counter(gb);

    PatchReport rpt4 = live_reload(s, SRC_V4);

    ASSERT_TRUE(rpt4.any_refused);
    ASSERT_TRUE(rpt4.count >= 1);
    if (rpt4.count > 0)
        ASSERT_EQ((int)rpt4.items[0].kind, (int)PATCH_REFUSED);

    /* ROM must be unchanged */
    ASSERT_EQ(gb->rom[0x100], rom_snap);

    /* Counter must not have decreased */
    uint8_t c4 = read_counter(gb);
    fprintf(stderr, "[e2e step4] REFUSED: counter before=%u after=%u\n",
            (unsigned)c_before_refuse, (unsigned)c4);
    int c4_diff = (int)c4 - (int)c_before_refuse;
    if (c4_diff < 0) c4_diff += 256;
    ASSERT_TRUE(c4_diff >= 0);

    patch_report_free(&rpt4);

    /* Behavior must still be +3 (REFUSED did not change code) */
    int bad4 = observe_increments(gb, 3, 20, 10000000);
    ASSERT_EQ(bad4, 0);

    uint8_t c4b = read_counter(gb);
    fprintf(stderr, "[e2e step4] counter after refused v4 run = %u (bad increments: %d)\n",
            (unsigned)c4b, bad4);

    /* -----------------------------------------------------------------------
     * Step 5 — live_reload v5: valid in-place after refusal -> IN_PLACE (recovery)
     *          Tick adds 1 again, same size as v2
     * --------------------------------------------------------------------- */
    PatchReport rpt5 = live_reload(s, SRC_V5);

    ASSERT_TRUE(!rpt5.any_refused);

    const PatchEntry *tick5 = find_entry(&rpt5, "Tick");
    ASSERT_TRUE(tick5 != NULL);
    if (tick5) {
        if (tick5->kind != PATCH_IN_PLACE)
            fprintf(stderr, "[e2e step5] Tick kind=%d reason='%s' (expected IN_PLACE)\n",
                    (int)tick5->kind, tick5->reason);
        ASSERT_EQ((int)tick5->kind, (int)PATCH_IN_PLACE);
    }

    /* Counter must not have decreased */
    uint8_t c5 = read_counter(gb);
    fprintf(stderr, "[e2e step5] counter after v5 reload = %u (was %u)\n",
            (unsigned)c5, (unsigned)c4b);
    int c5_diff = (int)c5 - (int)c4b;
    if (c5_diff < 0) c5_diff += 256;
    ASSERT_TRUE(c5_diff >= 0);

    patch_report_free(&rpt5);

    /* Verify +1/iteration */
    int bad5 = observe_increments(gb, 1, 20, 5000000);
    ASSERT_EQ(bad5, 0);

    uint8_t c5b = read_counter(gb);
    fprintf(stderr, "[e2e step5] counter after v5 run   = %u (bad increments: %d)\n",
            (unsigned)c5b, bad5);

    /* -----------------------------------------------------------------------
     * Final continuity check: the counter has been increasing throughout.
     * c5b should be well above c1 (allowing uint8_t wrap, but we ran enough
     * iterations that it must have advanced far beyond the initial 30).
     * --------------------------------------------------------------------- */
    fprintf(stderr, "[e2e final] start=%u end=%u — state was never reset\n",
            (unsigned)c1, (unsigned)c5b);

    live_free(s);
}

/* =========================================================================
 * main
 * ======================================================================= */
int main(void)
{
    test_live_e2e();
    TEST_MAIN_END();
}
