/*
 * test_layout.c — Tests for function-aware layout + placement memory (Task 1)
 *
 * Verifies:
 *   1. Fresh layout: 3 functions get granule-aligned sequential slots.
 *   2. Stable layout: editing FuncB to same/smaller size preserves all addresses.
 *   3. Relocation: growing FuncB beyond slot relocates it; FuncA+FuncC stay.
 *
 * Slot formula: slot_size = round_up(func_bytes, 16) + 16
 * GRANULE = 16 bytes.
 */

#include "test.h"
#include "../src/asm/asm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static const AsmPlacement *find_placement(const AsmResult *r, const char *name)
{
    for (int i = 0; i < r->nplacements; i++) {
        if (strcmp(r->placements[i].name, name) == 0)
            return &r->placements[i];
    }
    return NULL;
}

static const AsmSymbol *find_sym(const AsmResult *r, const char *name)
{
    for (int i = 0; i < r->nsyms; i++) {
        if (strcmp(r->syms[i].name, name) == 0)
            return &r->syms[i];
    }
    return NULL;
}

/* Round up n to nearest multiple of g (g must be power of 2) */
static int round_up(int n, int g) { return (n + g - 1) & ~(g - 1); }
#define GRANULE 16

/* =========================================================================
 * Test 1: Fresh layout — 3 functions, check granule alignment + sequential
 * slots.
 *
 * Program:
 *   FuncA: 2 nops (2 bytes)  -> slot = round_up(2,16)+16 = 32
 *   FuncB: 3 nops (3 bytes)  -> slot = round_up(3,16)+16 = 32
 *   FuncC: 1 nop  (1 byte)   -> slot = round_up(1,16)+16 = 32
 *
 * Sequential placement from DEFAULT_ORG (0x0150):
 *   FuncA.addr = 0x0150, FuncA.slot_size = 32
 *   FuncB.addr = 0x0170, FuncB.slot_size = 32
 *   FuncC.addr = 0x0190, FuncC.slot_size = 32
 * ======================================================================= */
static void test_fresh_layout(void)
{
    /* FuncA: 2 nops; FuncB: 3 nops; FuncC: 1 nop + ret */
    const char *src =
        "FuncA:\n"
        "    nop\n"
        "    nop\n"
        "FuncB:\n"
        "    nop\n"
        "    nop\n"
        "    nop\n"
        "FuncC:\n"
        "    nop\n"
        "    ret\n";

    AsmResult r = asm_assemble_mem(src, "layout_fresh.asm", NULL);

    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.placements != NULL);
    ASSERT_EQ(r.nplacements, 3);

    const AsmPlacement *pa = find_placement(&r, "FuncA");
    const AsmPlacement *pb = find_placement(&r, "FuncB");
    const AsmPlacement *pc = find_placement(&r, "FuncC");

    ASSERT_TRUE(pa != NULL);
    ASSERT_TRUE(pb != NULL);
    ASSERT_TRUE(pc != NULL);

    if (!pa || !pb || !pc) { asm_free(&r); return; }

    /* All functions should be in bank 0 */
    ASSERT_EQ(pa->bank, 0);
    ASSERT_EQ(pb->bank, 0);
    ASSERT_EQ(pc->bank, 0);

    /* All addresses should be granule-aligned */
    ASSERT_EQ(pa->addr % GRANULE, 0);
    ASSERT_EQ(pb->addr % GRANULE, 0);
    ASSERT_EQ(pc->addr % GRANULE, 0);

    /* Slot sizes: round_up(bytes, 16) + 16 */
    /* FuncA: 2 bytes -> round_up(2,16)+16 = 16+16 = 32 */
    ASSERT_EQ(pa->slot_size, 32);
    /* FuncB: 3 bytes -> round_up(3,16)+16 = 16+16 = 32 */
    ASSERT_EQ(pb->slot_size, 32);
    /* FuncC: 2 bytes (nop+ret) -> round_up(2,16)+16 = 16+16 = 32 */
    ASSERT_EQ(pc->slot_size, 32);

    /* FuncA starts at 0x0150 (DEFAULT_ORG) */
    ASSERT_EQ(pa->addr, 0x0150);

    /* Sequential: FuncB starts after FuncA's slot */
    ASSERT_EQ(pb->addr, (uint16_t)(pa->addr + pa->slot_size));

    /* Sequential: FuncC starts after FuncB's slot */
    ASSERT_EQ(pc->addr, (uint16_t)(pb->addr + pb->slot_size));

    /* Symbol table should reflect new addresses */
    const AsmSymbol *sa = find_sym(&r, "FuncA");
    const AsmSymbol *sb = find_sym(&r, "FuncB");
    const AsmSymbol *sc = find_sym(&r, "FuncC");
    ASSERT_TRUE(sa != NULL);
    ASSERT_TRUE(sb != NULL);
    ASSERT_TRUE(sc != NULL);
    if (sa) ASSERT_EQ(sa->addr, pa->addr);
    if (sb) ASSERT_EQ(sb->addr, pb->addr);
    if (sc) ASSERT_EQ(sc->addr, pc->addr);

    /* ROM bytes: FuncA code at pa->addr */
    ASSERT_EQ(r.rom[pa->addr],     0x00);  /* nop */
    ASSERT_EQ(r.rom[pa->addr + 1], 0x00);  /* nop */
    /* Padding bytes between FuncA and FuncB should be 0x00 */
    ASSERT_EQ(r.rom[pa->addr + 2], 0x00);  /* padding */

    /* FuncB code at pb->addr */
    ASSERT_EQ(r.rom[pb->addr],     0x00);  /* nop */
    ASSERT_EQ(r.rom[pb->addr + 1], 0x00);  /* nop */
    ASSERT_EQ(r.rom[pb->addr + 2], 0x00);  /* nop */

    /* FuncC code at pc->addr */
    ASSERT_EQ(r.rom[pc->addr],     0x00);  /* nop */
    ASSERT_EQ(r.rom[pc->addr + 1], 0xC9);  /* ret */

    asm_free(&r);
}

/* =========================================================================
 * Test 2: Stable layout — editing FuncB to same/smaller size keeps all
 * addresses identical.
 * ======================================================================= */
static void test_stable_layout(void)
{
    /* --- Build v1: FuncA=2nops, FuncB=3nops, FuncC=1nop+ret --- */
    const char *src_v1 =
        "FuncA:\n"
        "    nop\n"
        "    nop\n"
        "FuncB:\n"
        "    nop\n"
        "    nop\n"
        "    nop\n"
        "FuncC:\n"
        "    nop\n"
        "    ret\n";

    AsmPlacementMem mem;
    memset(&mem, 0, sizeof(mem));

    AsmResult r1 = asm_assemble_mem(src_v1, "stable_v1.asm", &mem);
    ASSERT_TRUE(r1.ok);
    ASSERT_TRUE(r1.nplacements == 3);

    /* Capture original addresses */
    uint16_t addr_a = 0, addr_b = 0, addr_c = 0;
    int slot_a = 0, slot_b = 0, slot_c = 0;

    const AsmPlacement *pa1 = find_placement(&r1, "FuncA");
    const AsmPlacement *pb1 = find_placement(&r1, "FuncB");
    const AsmPlacement *pc1 = find_placement(&r1, "FuncC");

    if (pa1) { addr_a = pa1->addr; slot_a = pa1->slot_size; }
    if (pb1) { addr_b = pb1->addr; slot_b = pb1->slot_size; }
    if (pc1) { addr_c = pc1->addr; slot_c = pc1->slot_size; }

    asm_free(&r1);

    /* --- Build v2: FuncB changed to 2 nops (smaller, fits in slot) --- */
    const char *src_v2 =
        "FuncA:\n"
        "    nop\n"
        "    nop\n"
        "FuncB:\n"
        "    nop\n"       /* 1 nop — smaller than original 3 */
        "    nop\n"       /* 2 nops total, still fits in 32-byte slot */
        "FuncC:\n"
        "    nop\n"
        "    ret\n";

    AsmResult r2 = asm_assemble_mem(src_v2, "stable_v2.asm", &mem);
    ASSERT_TRUE(r2.ok);
    ASSERT_TRUE(r2.nplacements == 3);

    const AsmPlacement *pa2 = find_placement(&r2, "FuncA");
    const AsmPlacement *pb2 = find_placement(&r2, "FuncB");
    const AsmPlacement *pc2 = find_placement(&r2, "FuncC");

    ASSERT_TRUE(pa2 != NULL);
    ASSERT_TRUE(pb2 != NULL);
    ASSERT_TRUE(pc2 != NULL);

    if (!pa2 || !pb2 || !pc2) {
        asm_free(&r2);
        free(mem.items);
        return;
    }

    /* All functions must keep EXACT same addresses (stable layout) */
    ASSERT_EQ(pa2->addr, addr_a);
    ASSERT_EQ(pb2->addr, addr_b);
    ASSERT_EQ(pc2->addr, addr_c);

    /* Slot sizes must be unchanged (reused from placement memory) */
    ASSERT_EQ(pa2->slot_size, slot_a);
    ASSERT_EQ(pb2->slot_size, slot_b);
    ASSERT_EQ(pc2->slot_size, slot_c);

    /* FuncB still fits in its slot (2 bytes <= 32) */
    ASSERT_TRUE(2 <= pb2->slot_size);

    /* Symbol addresses must match placement addresses */
    const AsmSymbol *sa = find_sym(&r2, "FuncA");
    const AsmSymbol *sb = find_sym(&r2, "FuncB");
    const AsmSymbol *sc = find_sym(&r2, "FuncC");
    if (sa) ASSERT_EQ(sa->addr, addr_a);
    if (sb) ASSERT_EQ(sb->addr, addr_b);
    if (sc) ASSERT_EQ(sc->addr, addr_c);

    asm_free(&r2);
    free(mem.items);
}

/* =========================================================================
 * Test 3: Relocation — FuncB grows beyond its slot.
 * FuncA and FuncC keep their exact addresses; FuncB gets a NEW address
 * appended in the bank's free area.
 * ======================================================================= */
static void test_relocation(void)
{
    /* --- Build v1: FuncA=2nops, FuncB=3nops, FuncC=1nop+ret --- */
    const char *src_v1 =
        "FuncA:\n"
        "    nop\n"
        "    nop\n"
        "FuncB:\n"
        "    nop\n"
        "    nop\n"
        "    nop\n"
        "FuncC:\n"
        "    nop\n"
        "    ret\n";

    AsmPlacementMem mem;
    memset(&mem, 0, sizeof(mem));

    AsmResult r1 = asm_assemble_mem(src_v1, "reloc_v1.asm", &mem);
    ASSERT_TRUE(r1.ok);
    ASSERT_TRUE(r1.nplacements == 3);

    uint16_t addr_a_v1 = 0, addr_b_v1 = 0, addr_c_v1 = 0;
    int slot_b_v1 = 0;

    const AsmPlacement *pa1 = find_placement(&r1, "FuncA");
    const AsmPlacement *pb1 = find_placement(&r1, "FuncB");
    const AsmPlacement *pc1 = find_placement(&r1, "FuncC");
    if (pa1) addr_a_v1 = pa1->addr;
    if (pb1) { addr_b_v1 = pb1->addr; slot_b_v1 = pb1->slot_size; }
    if (pc1) addr_c_v1 = pc1->addr;

    asm_free(&r1);

    /* --- Build v2: FuncB grown to 35 nops (more than 32-byte slot) ---
     * slot_b_v1 = 32. 35 > 32, so FuncB must relocate. */
    const char *src_v2 =
        "FuncA:\n"
        "    nop\n"
        "    nop\n"
        "FuncB:\n"
        "    nop\n nop\n nop\n nop\n nop\n"   /* 5 */
        "    nop\n nop\n nop\n nop\n nop\n"   /* 10 */
        "    nop\n nop\n nop\n nop\n nop\n"   /* 15 */
        "    nop\n nop\n nop\n nop\n nop\n"   /* 20 */
        "    nop\n nop\n nop\n nop\n nop\n"   /* 25 */
        "    nop\n nop\n nop\n nop\n nop\n"   /* 30 */
        "    nop\n nop\n nop\n nop\n nop\n"   /* 35 */
        "FuncC:\n"
        "    nop\n"
        "    ret\n";

    AsmResult r2 = asm_assemble_mem(src_v2, "reloc_v2.asm", &mem);
    ASSERT_TRUE(r2.ok);
    ASSERT_TRUE(r2.nplacements == 3);

    const AsmPlacement *pa2 = find_placement(&r2, "FuncA");
    const AsmPlacement *pb2 = find_placement(&r2, "FuncB");
    const AsmPlacement *pc2 = find_placement(&r2, "FuncC");

    ASSERT_TRUE(pa2 != NULL);
    ASSERT_TRUE(pb2 != NULL);
    ASSERT_TRUE(pc2 != NULL);

    if (!pa2 || !pb2 || !pc2) {
        asm_free(&r2);
        free(mem.items);
        return;
    }

    /* FuncA must keep its exact address */
    ASSERT_EQ(pa2->addr, addr_a_v1);

    /* FuncC must keep its exact address */
    ASSERT_EQ(pc2->addr, addr_c_v1);

    /* FuncB must have a NEW address (different from v1) */
    ASSERT_TRUE(pb2->addr != addr_b_v1);

    /* FuncB's new address must be after the end of the original layout
     * (i.e., in the bank's free area, past the last slot from v1) */
    uint16_t end_of_v1 = (uint16_t)(addr_c_v1 + 32); /* FuncC slot was 32 bytes */
    ASSERT_TRUE(pb2->addr >= end_of_v1);

    /* FuncB's new slot must fit 35 bytes: slot = round_up(35,16)+16 = 48+16 = 64 */
    ASSERT_TRUE(pb2->slot_size >= 35);
    ASSERT_EQ(pb2->slot_size, round_up(35, GRANULE) + GRANULE);  /* = 64 */

    /* FuncB's new addr must be granule-aligned */
    ASSERT_EQ(pb2->addr % GRANULE, 0);

    /* Placement memory must record FuncB's new slot */
    int found_b_in_mem = 0;
    for (int i = 0; i < mem.count; i++) {
        if (strcmp(mem.items[i].name, "FuncB") == 0) {
            ASSERT_EQ(mem.items[i].addr, pb2->addr);
            ASSERT_EQ(mem.items[i].slot_size, pb2->slot_size);
            found_b_in_mem = 1;
            break;
        }
    }
    ASSERT_TRUE(found_b_in_mem);

    /* Symbol addresses reflect new placements */
    const AsmSymbol *sa = find_sym(&r2, "FuncA");
    const AsmSymbol *sb = find_sym(&r2, "FuncB");
    const AsmSymbol *sc = find_sym(&r2, "FuncC");
    if (sa) ASSERT_EQ(sa->addr, addr_a_v1);
    if (sb) ASSERT_EQ(sb->addr, pb2->addr);
    if (sc) ASSERT_EQ(sc->addr, addr_c_v1);

    /* v2 slot_b_v1 recorded: the original slot was too small */
    ASSERT_TRUE(slot_b_v1 < 35);  /* sanity: original slot < new func size */

    asm_free(&r2);
    free(mem.items);
}

/* =========================================================================
 * Main
 * ======================================================================= */
int main(void)
{
    test_fresh_layout();
    test_stable_layout();
    test_relocation();

    TEST_MAIN_END();
}
