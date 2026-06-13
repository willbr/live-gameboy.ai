/*
 * test_live_reloc.c — Trampoline relocation for outgrown functions.
 *
 * Scenario:
 *   v1: a "game loop" program — Main calls Tick each iteration.
 *       Tick is small (9 bytes, fits in 32-byte slot): reads counter at $C000,
 *       adds 1, writes back.
 *   Boot and run until counter >= 50. Record counter value.
 *
 *   v2: Tick is MUCH larger (>32 bytes) — we add enough NOPs and a different
 *       add so it no longer fits its original 32-byte slot.
 *       Tick now adds 3 to the counter (observable change).
 *   live_reload v2.
 *
 * Assertions:
 *   - PatchReport: Tick = PATCH_RELOCATED.
 *   - gb->rom at Tick's OLD entry addr == 0xC3 (JP opcode).
 *   - Next 2 bytes at old entry == new addr (lo, hi).
 *   - Counter state was preserved (>= 50 before reload).
 *   - After more steps, counter increases by 3 per Tick (new behavior).
 *   - (Optional) call site in Main was rebound to new addr OR trampoline works.
 *
 * Layout details:
 *   Tick v1: 9 bytes -> slot = round_up(9,16)+16 = 16+16 = 32 bytes
 *   Tick v2: 9+32 = 41+ bytes (we add 32+ NOPs) -> exceeds 32-byte slot ->
 *            layout assigns new address -> RELOCATED.
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
 * Program sources
 *
 * Main: infinite loop calling Tick
 * Tick v1: add 1 to counter at $C000 (9 bytes, fits 32-byte slot)
 * Tick v2: add 3 to counter, padded with many NOPs so it exceeds 32 bytes
 * ------------------------------------------------------------------------- */

static const char *SRC_V1 =
    "Main:\n"
    "    ld sp, $FFFE\n"     /* set stack pointer    */
    "    xor a\n"            /* a = 0                */
    "    ld ($C000), a\n"    /* counter = 0          */
    ".loop:\n"
    "    call Tick\n"
    "    jr .loop\n"
    "Tick:\n"
    "    ld a, ($C000)\n"   /* 3 bytes */
    "    add a, 1\n"         /* 2 bytes: +1 */
    "    ld ($C000), a\n"   /* 3 bytes */
    "    ret\n";             /* 1 byte  — total: 9 bytes */

/* Tick v2: adds 3, uses many NOPs to exceed the 32-byte slot.
 * We need > 32 bytes total.  Core: 9 bytes.  Add 30 NOPs = 39 bytes > 32. */
static const char *SRC_V2 =
    "Main:\n"
    "    ld sp, $FFFE\n"
    "    xor a\n"
    "    ld ($C000), a\n"
    ".loop:\n"
    "    call Tick\n"
    "    jr .loop\n"
    "Tick:\n"
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"  /* 5  */
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"  /* 10 */
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"  /* 15 */
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"  /* 20 */
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"  /* 25 */
    "    nop\n" "    nop\n" "    nop\n" "    nop\n" "    nop\n"  /* 30 */
    "    ld a, ($C000)\n"   /* 3 bytes */
    "    add a, 3\n"         /* 2 bytes: NOW +3 */
    "    ld ($C000), a\n"   /* 3 bytes */
    "    ret\n";             /* 1 byte  — total: 39 bytes > 32 -> RELOCATED */

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
 * Helper: read the counter value from WRAM $C000 -> wram[0].
 * ------------------------------------------------------------------------- */
static uint8_t read_counter(const GB *gb)
{
    return gb->wram[0];
}

/* =========================================================================
 * Test: trampoline relocation with state preservation
 * ======================================================================= */
static void test_reloc_trampoline(void)
{
    /* --- Step 1: boot v1 --- */
    LiveSession *s = live_new(SRC_V1, "live_reloc_v1.asm");
    ASSERT_TRUE(s != NULL);
    if (!s) return;

    GB *gb = live_gb(s);
    ASSERT_TRUE(gb != NULL);
    if (!gb) { live_free(s); return; }

    /* Verify the 0x100 entry patch: JP (0xC3) */
    ASSERT_EQ(gb->rom[0x100], 0xC3);

    /* Find Tick's old address from the assembled ROM (via symbol lookup).
     * We'll capture it before live_reload. */
    /* Run v1 until counter >= 50 */
    const int LIMIT_V1 = 10000000;
    int steps = 0;
    while (read_counter(gb) < 50 && steps < LIMIT_V1) {
        gb_step(gb);
        steps++;
    }

    uint8_t counter_before = read_counter(gb);
    ASSERT_TRUE(counter_before >= 50);
    if (counter_before < 50) {
        fprintf(stderr, "test_reloc_trampoline: counter only reached %d after %d steps\n",
                counter_before, steps);
        live_free(s);
        return;
    }

    /* Capture Tick's old entry address from the live GB ROM's placement.
     * We know Tick is assembled; find its address from the current session's
     * AsmResult placements.  We need it to check the trampoline was written
     * at the right place after reload. */
    /* We can't access LiveSession internals directly (opaque), but we CAN:
     * 1. Assemble v1 fresh to find where Tick lives.
     * 2. Or inspect gb->rom: we know JP Main is at 0x100; Tick follows Main.
     *
     * Simplest: assemble v1 fresh and read the placement. */
    AsmResult r1 = asm_assemble(SRC_V1, "find_tick_v1");
    ASSERT_TRUE(r1.ok);
    uint16_t tick_old_addr = 0;
    int      tick_old_bank = 0;
    for (int i = 0; i < r1.nplacements; i++) {
        if (strcmp(r1.placements[i].name, "Tick") == 0) {
            tick_old_addr = r1.placements[i].addr;
            tick_old_bank = r1.placements[i].bank;
            break;
        }
    }
    asm_free(&r1);
    ASSERT_TRUE(tick_old_addr != 0);
    if (tick_old_addr == 0) {
        fprintf(stderr, "test_reloc_trampoline: could not find Tick placement in v1\n");
        live_free(s);
        return;
    }

    /* --- Step 2: live_reload v2 (Tick outgrows slot -> should relocate) --- */
    PatchReport rpt = live_reload(s, SRC_V2);

    /* Should have no assembly-level refusals */
    ASSERT_TRUE(!rpt.any_refused);

    /* Tick should be PATCH_RELOCATED */
    const PatchEntry *tick_entry = find_entry(&rpt, "Tick");
    ASSERT_TRUE(tick_entry != NULL);
    if (tick_entry) {
        if (tick_entry->kind != PATCH_RELOCATED) {
            fprintf(stderr,
                    "test_reloc_trampoline: Tick kind=%d reason='%s' (expected RELOCATED=%d)\n",
                    (int)tick_entry->kind, tick_entry->reason, (int)PATCH_RELOCATED);
        }
        ASSERT_EQ((int)tick_entry->kind, (int)PATCH_RELOCATED);
    }

    /* Find Tick's NEW address by assembling v2 fresh.
     * Because live_reload uses retained placement mem, Tick gets a new slot.
     * We can find the new addr from the report reason string, but it's easier
     * to assemble v2 with fresh placement mem to see where Tick would start
     * (not exactly the same as live_reload's retained layout, but we can
     * verify the trampoline by reading gb->rom directly). */

    /* --- Step 3: Check trampoline at Tick's OLD entry --- */
    uint32_t old_off = (tick_old_bank == 0) ? (uint32_t)tick_old_addr
                                             : (uint32_t)tick_old_bank * 0x4000u
                                               + (tick_old_addr - 0x4000u);

    /* The first byte at the old entry must be JP opcode (0xC3) */
    ASSERT_EQ(gb->rom[old_off], 0xC3);

    /* Read the new address from the trampoline (little-endian) */
    uint16_t trampoline_dest = (uint16_t)gb->rom[old_off + 1]
                             | ((uint16_t)gb->rom[old_off + 2] << 8);
    fprintf(stderr, "[info] Tick old addr=$%04X, trampoline -> $%04X\n",
            tick_old_addr, trampoline_dest);

    /* The trampoline destination should not be the old address (it changed) */
    ASSERT_TRUE(trampoline_dest != tick_old_addr);

    /* Verify the bytes at the new address look like valid code (first bytes
     * should be NOPs = 0x00, since v2 starts Tick with 30 NOPs). */
    uint32_t new_off = (uint32_t)trampoline_dest; /* bank 0, addr < 0x4000 */
    ASSERT_EQ(gb->rom[new_off], 0x00); /* first NOP */

    /* --- Step 4: assert state preserved --- */
    uint8_t counter_after_reload = read_counter(gb);
    ASSERT_TRUE(counter_after_reload >= 50);
    if (counter_after_reload < 50) {
        fprintf(stderr,
                "test_reloc_trampoline: state was reset! counter=%d (was %d)\n",
                counter_after_reload, counter_before);
    }

    patch_report_free(&rpt);

    /* --- Step 5: run more steps and verify +3 behavior ---
     * After relocation, calls to Tick (from Main) should eventually hit the
     * new code (+3) either via the rebound call site or the trampoline.
     * We measure: run until the counter advances, check delta == 3. */
    uint8_t prev = read_counter(gb);
    int increments_of_3 = 0;
    int increments_of_other = 0;
    const int LIMIT_V2 = 10000000;
    int steps2 = 0;

    for (steps2 = 0; steps2 < LIMIT_V2 && increments_of_3 < 20; steps2++) {
        gb_step(gb);
        uint8_t now = read_counter(gb);
        if (now != prev) {
            int delta = (int)now - (int)prev;
            if (delta < 0) delta += 256;  /* handle uint8_t wrap */
            if (delta == 3)
                increments_of_3++;
            else
                increments_of_other++;
            prev = now;
        }
    }

    ASSERT_TRUE(increments_of_3 >= 10);
    ASSERT_TRUE(increments_of_other == 0);

    if (increments_of_other > 0) {
        fprintf(stderr,
                "test_reloc_trampoline: saw %d non-3 increments (expected 0)\n",
                increments_of_other);
    }

    live_free(s);
}

/* =========================================================================
 * Test: cross-bank relocation is refused
 *
 * We can't easily force a cross-bank relocation with the current test
 * infrastructure (it requires ROMX bank switching), but we verify that
 * a same-bank relocation succeeds (the positive case above covers this).
 * This test asserts the basic invariant: after PATCH_RELOCATED, the old
 * slot in gb->rom starts with 0xC3 (JP) and the zombie body after byte 2
 * is NOT zero (it still contains the old code's bytes).
 * ======================================================================= */
static void test_reloc_zombie_body(void)
{
    LiveSession *s = live_new(SRC_V1, "live_reloc_zombie.asm");
    ASSERT_TRUE(s != NULL);
    if (!s) return;

    GB *gb = live_gb(s);

    /* Find Tick's old address */
    AsmResult r1 = asm_assemble(SRC_V1, "find_tick_zombie");
    ASSERT_TRUE(r1.ok);
    uint16_t tick_old_addr = 0;
    int      tick_slot_size = 0;
    for (int i = 0; i < r1.nplacements; i++) {
        if (strcmp(r1.placements[i].name, "Tick") == 0) {
            tick_old_addr  = r1.placements[i].addr;
            tick_slot_size = r1.placements[i].slot_size;
            break;
        }
    }
    asm_free(&r1);
    ASSERT_TRUE(tick_old_addr != 0);

    /* Capture the OLD body bytes (bytes 3..slot_size-1) before reload */
    uint32_t old_off = (uint32_t)tick_old_addr;
    /* Save a few bytes of the old body beyond the trampoline */
    uint8_t old_body_byte3 = gb->rom[old_off + 3];  /* 4th byte of Tick */
    (void)tick_slot_size;

    /* Run a bit, then reload */
    for (int i = 0; i < 100000; i++) gb_step(gb);

    PatchReport rpt = live_reload(s, SRC_V2);
    const PatchEntry *tick_entry = find_entry(&rpt, "Tick");
    ASSERT_TRUE(tick_entry != NULL);
    if (tick_entry) {
        ASSERT_EQ((int)tick_entry->kind, (int)PATCH_RELOCATED);
    }

    /* After relocation: first 3 bytes = JP trampoline */
    ASSERT_EQ(gb->rom[old_off + 0], 0xC3);

    /* Zombie body: bytes at old_off+3 onwards must be UNCHANGED
     * (the old function's code, not overwritten).
     * old_body_byte3 was the 4th byte of the original Tick body.
     * After relocation, that byte should still be there (zombie). */
    ASSERT_EQ(gb->rom[old_off + 3], old_body_byte3);

    patch_report_free(&rpt);
    live_free(s);
}

/* =========================================================================
 * main
 * ======================================================================= */
int main(void)
{
    test_reloc_trampoline();
    test_reloc_zombie_body();

    TEST_MAIN_END();
}
