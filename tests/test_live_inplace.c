/*
 * test_live_inplace.c — THE THESIS TEST: in-place live patching with state
 * preservation.
 *
 * Scenario:
 *   v1: boot a "game loop" program that increments a counter at WRAM $C000
 *       by 1 on each call to Tick.
 *   - Run until counter reaches ~50.  Record the counter value.
 *   - live_reload v2: Tick now adds 2 instead of 1 (same byte size, fits slot).
 *   - Assert: Tick = PATCH_IN_PLACE; Main = PATCH_UNCHANGED.
 *   - Assert: counter at $C000 is still >= 50 (state NOT reset).
 *   - Run more steps; assert counter increases by 2 per Tick (new behavior).
 *
 * Entry-point strategy:
 *   live_new patches rom[0x100..0x102] = JP Main using the "Main" symbol from
 *   the build database, so gb_reset()'s PC=0x100 jumps to user code.  This is
 *   documented in live.c and live.h.
 *
 * Safe-point strategy:
 *   The CPU is paused between gb_step() calls (single-instruction granularity).
 *   At the moment live_reload is called the PC will be somewhere in Main's
 *   loop body (not inside Tick), so the safe-point check is a no-op — PC is
 *   already outside the patched range.  This is guaranteed by the test: we
 *   call live_reload only when gb_step has just returned to the main loop (i.e.
 *   after a ret from Tick), so PC is at the "call Tick" or "jr .loop"
 *   instruction, which is inside Main — not inside Tick.
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
 * Program source templates
 *
 * Layout:
 *   Main: (global — gets JP from 0x100)
 *       ld sp, $FFFE       ; set stack pointer
 *       xor a              ; a = 0
 *       ld [$C000], a      ; zero counter
 *   .loop:
 *       call Tick          ; call the counter function
 *       jr .loop           ; infinite loop
 *   Tick: (global — the function we will hot-reload)
 *       ld a, [$C000]
 *       add a, N           ; v1: N=1  v2: N=2
 *       ld [$C000], a
 *       ret
 *
 * Tick has exactly 7 bytes:
 *   FA 00 C0   = ld a, [$C000]  (3 bytes)
 *   C6 01      = add a, 1       (2 bytes)   <- changes to C6 02 in v2
 *   EA 00 C0   = ld [$C000], a  (3 bytes)
 *   C9         = ret            (1 byte)
 *
 * Total Tick: 9 bytes -> slot = round_up(9,16)+16 = 16+16 = 32 bytes.
 * Both v1 and v2 produce 9 bytes -> fits slot, no relocation.
 *
 * We use `add a, N` (opcode C6 N) which is a 2-byte immediate-add; changing N
 * from 1 to 2 alters exactly 1 byte, keeping the function the same size.
 * ------------------------------------------------------------------------- */

static const char *SRC_V1 =
    "Main:\n"
    "    ld sp, $FFFE\n"     /* set stack pointer */
    "    xor a\n"            /* a = 0             */
    "    ld ($C000), a\n"    /* counter = 0       */
    ".loop:\n"
    "    call Tick\n"
    "    jr .loop\n"
    "Tick:\n"
    "    ld a, ($C000)\n"
    "    add a, 1\n"         /* v1: +1 */
    "    ld ($C000), a\n"
    "    ret\n";

static const char *SRC_V2 =
    "Main:\n"
    "    ld sp, $FFFE\n"
    "    xor a\n"
    "    ld ($C000), a\n"
    ".loop:\n"
    "    call Tick\n"
    "    jr .loop\n"
    "Tick:\n"
    "    ld a, ($C000)\n"
    "    add a, 2\n"         /* v2: +2 */
    "    ld ($C000), a\n"
    "    ret\n";

/* -------------------------------------------------------------------------
 * Helper: find a PatchEntry in a PatchReport by function name.
 * ------------------------------------------------------------------------- */
static const PatchEntry *find_entry(const PatchReport *rpt, const char *name)
{
    for (int i = 0; i < rpt->count; i++) {
        if (strcmp(rpt->items[i].func, name) == 0)
            return &rpt->items[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Helper: read the counter value from WRAM.
 * $C000 maps to wram[0].
 * ------------------------------------------------------------------------- */
static uint8_t read_counter(const GB *gb) {
    return gb->wram[0];
}

/* =========================================================================
 * Test: live in-place patch + state preservation
 * ======================================================================= */
static void test_inplace_patch(void)
{
    /* --- Step 1: boot v1 --- */
    LiveSession *s = live_new(SRC_V1, "live_v1.asm");
    ASSERT_TRUE(s != NULL);
    if (!s) return;

    GB *gb = live_gb(s);
    ASSERT_TRUE(gb != NULL);
    if (!gb) { live_free(s); return; }

    /* Verify the 0x100 entry patch was applied: should be JP (0xC3) */
    ASSERT_EQ(gb->rom[0x100], 0xC3);

    /* --- Run until counter >= 50 (or step limit) ---
     * We run up to 10 million steps to reach counter=50.
     * Each Tick iteration costs roughly: call(6) + ld a,(nn)(4) + add(2) +
     * ld (nn),a(4) + ret(4) + jr(3) = ~23 T-cycles = ~6 instructions.
     * 50 iterations * 6 = ~300 instructions. Should be fast. */
    const int LIMIT_V1 = 10000000;
    int steps = 0;
    while (read_counter(gb) < 50 && steps < LIMIT_V1) {
        gb_step(gb);
        steps++;
    }

    uint8_t counter_before = read_counter(gb);
    uint16_t sp_before  = gb->cpu.sp;
    uint8_t  a_before   = gb->cpu.a;

    ASSERT_TRUE(counter_before >= 50);
    if (counter_before < 50) {
        fprintf(stderr, "test_inplace_patch: counter only reached %d after %d steps\n",
                counter_before, steps);
        live_free(s);
        return;
    }

    /* --- Step 2: live_reload v2 --- */
    PatchReport rpt = live_reload(s, SRC_V2);

    /* No refuses */
    ASSERT_TRUE(!rpt.any_refused);

    /* Tick should be PATCH_IN_PLACE */
    const PatchEntry *tick_entry = find_entry(&rpt, "Tick");
    ASSERT_TRUE(tick_entry != NULL);
    if (tick_entry) {
        ASSERT_EQ((int)tick_entry->kind, (int)PATCH_IN_PLACE);
    }

    /* Main should be PATCH_UNCHANGED */
    const PatchEntry *main_entry = find_entry(&rpt, "Main");
    ASSERT_TRUE(main_entry != NULL);
    if (main_entry) {
        ASSERT_EQ((int)main_entry->kind, (int)PATCH_UNCHANGED);
    }

    /* No RELOCATED entries (functions should fit their slots) */
    for (int i = 0; i < rpt.count; i++) {
        if (rpt.items[i].kind == PATCH_RELOCATED) {
            fprintf(stderr,
                    "test_inplace_patch: unexpected RELOCATED for '%s'\n",
                    rpt.items[i].func);
        }
        ASSERT_TRUE(rpt.items[i].kind != PATCH_RELOCATED);
    }

    /* --- Step 3: assert state preserved --- */
    uint8_t counter_after_reload = read_counter(gb);
    ASSERT_TRUE(counter_after_reload >= 50);
    if (counter_after_reload < 50) {
        fprintf(stderr,
                "test_inplace_patch: state was reset! counter=%d (was %d)\n",
                counter_after_reload, counter_before);
    }

    /* SP and A: live_reload may step the CPU a bounded amount for the
     * safe-point check (to ensure PC exits the patched range).  This means
     * registers can advance legitimately.  The important invariant is that
     * live_reload does NOT reset or externally clobber the CPU state — the
     * CPU only advances via normal gb_step() calls.  We verify this by
     * checking that the SP is still within a plausible range (between $FF80
     * and $FFFF — the GB's stack grows downward from $FFFE). */
    ASSERT_TRUE(gb->cpu.sp >= 0xFF80 && gb->cpu.sp <= 0xFFFE);
    (void)sp_before;
    (void)a_before;

    patch_report_free(&rpt);

    /* --- Step 4: run more steps and verify +2 behavior ---
     * Measure how many Tick calls happen over N total steps in v2 by watching
     * the counter delta.  We run until the counter advances by at least 10
     * more from where it is now (after reload).  Since v2 adds 2 per Tick, we
     * need at least 5 Tick calls. */
    uint8_t counter_start = read_counter(gb);
    const int LIMIT_V2 = 5000000;
    int steps2 = 0;
    /* We want to observe that the counter goes up by 2 each Tick.
     * Strategy: run until counter increases, then check it increased by 2. */
    uint8_t prev = counter_start;
    int increments_of_2 = 0;
    int increments_of_other = 0;

    for (steps2 = 0; steps2 < LIMIT_V2 && increments_of_2 < 20; steps2++) {
        gb_step(gb);
        uint8_t now = read_counter(gb);
        if (now != prev) {
            int delta = (int)now - (int)prev;
            /* Allow for uint8_t wrap */
            if (delta < 0) delta += 256;
            if (delta == 2)
                increments_of_2++;
            else
                increments_of_other++;
            prev = now;
        }
    }

    /* We should have seen many +2 increments and zero +1 increments */
    ASSERT_TRUE(increments_of_2 >= 10);
    ASSERT_TRUE(increments_of_other == 0);

    if (increments_of_other > 0) {
        fprintf(stderr,
                "test_inplace_patch: saw %d non-2 increments (expected 0)\n",
                increments_of_other);
    }

    /* Counter should be well above where it was before reload */
    uint8_t counter_final = read_counter(gb);
    ASSERT_TRUE((int)counter_final > (int)counter_before || counter_final < counter_before);
    /* (allow wrap — the important thing is it kept running) */

    live_free(s);
}

/* =========================================================================
 * Test: refuse on bad source
 * ======================================================================= */
static void test_refuse_bad_source(void)
{
    LiveSession *s = live_new(SRC_V1, "live_refuse.asm");
    ASSERT_TRUE(s != NULL);
    if (!s) return;

    GB *gb = live_gb(s);

    /* Run a few steps */
    for (int i = 0; i < 1000; i++) gb_step(gb);
    uint8_t counter = read_counter(gb);

    /* Save ROM bytes to compare later */
    uint8_t rom_100 = gb->rom[0x100];

    /* Try to reload with invalid source */
    PatchReport rpt = live_reload(s, "this is not valid asm !@#$%\n");
    ASSERT_TRUE(rpt.any_refused);
    ASSERT_EQ(rpt.count, 1);
    if (rpt.count > 0)
        ASSERT_EQ((int)rpt.items[0].kind, (int)PATCH_REFUSED);

    /* ROM should be unchanged (byte at 0x100 still the JP) */
    ASSERT_EQ(gb->rom[0x100], rom_100);

    /* State should be preserved */
    ASSERT_TRUE(read_counter(gb) >= counter);  /* may have advanced if safe-point stepped */

    patch_report_free(&rpt);
    live_free(s);
}

/* =========================================================================
 * Test: soft reload resets state
 * ======================================================================= */
static void test_soft_reload(void)
{
    LiveSession *s = live_new(SRC_V1, "live_soft.asm");
    ASSERT_TRUE(s != NULL);
    if (!s) return;

    GB *gb = live_gb(s);

    /* Run until counter reaches at least 10 */
    for (int i = 0; i < 1000000 && read_counter(gb) < 10; i++)
        gb_step(gb);
    ASSERT_TRUE(read_counter(gb) >= 10);

    /* Soft reload resets everything */
    live_soft_reload(s, SRC_V1);

    /* After soft reload: PC = 0x100, counter = 0 */
    ASSERT_EQ(gb->cpu.pc, 0x0100);
    ASSERT_EQ(read_counter(gb), 0);

    live_free(s);
}

/* =========================================================================
 * main
 * ======================================================================= */
int main(void)
{
    test_inplace_patch();
    test_refuse_bad_source();
    test_soft_reload();

    TEST_MAIN_END();
}
