/*
 * test_refsites.c — Reference-site tracking (Task 2 of Milestone 4)
 *
 * Verifies that the assembler records AsmRefSite entries for every 16-bit
 * absolute address operand that resolves to exactly one symbol, and does NOT
 * record entries for relative (jr) operands or pure constants.
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

/* Find a refsite by symbol name; return NULL if not found. */
static const AsmRefSite *find_ref(const AsmResult *r, const char *sym)
{
    for (int i = 0; i < r->nrefs; i++) {
        if (strcmp(r->refs[i].sym, sym) == 0)
            return &r->refs[i];
    }
    return NULL;
}

/* Count refsites for a given symbol name. */
static int count_refs(const AsmResult *r, const char *sym)
{
    int n = 0;
    for (int i = 0; i < r->nrefs; i++) {
        if (strcmp(r->refs[i].sym, sym) == 0)
            n++;
    }
    return n;
}

/* Find a refsite by symbol name AND addend. */
static const AsmRefSite *find_ref_addend(const AsmResult *r,
                                          const char *sym, long addend)
{
    for (int i = 0; i < r->nrefs; i++) {
        if (strcmp(r->refs[i].sym, sym) == 0 &&
            r->refs[i].addend == addend)
            return &r->refs[i];
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

/* =========================================================================
 * Test 1: Basic absolute refs: call, jp, ld hl, dw; no jr ref.
 *
 * Program layout (with layout memory, all functions get padded slots):
 *
 *   Main:
 *       call Func        ; 3 bytes -> refsite at (cur_off+1), sym="Func", addend=0
 *       jp   Func        ; 3 bytes -> refsite at (cur_off+1), sym="Func", addend=0
 *       ld   hl, Data    ; 3 bytes -> refsite at (cur_off+1), sym="Data", addend=0
 *   .loop:
 *       jr   .loop       ; 2 bytes -> NO refsite (relative)
 *       ret
 *   Func:
 *       ret
 *   Data:
 *       dw   Func        ; 2 bytes -> refsite at (cur_off), sym="Func", addend=0
 *       dw   Func+2      ; 2 bytes -> refsite at (cur_off), sym="Func", addend=2
 * ======================================================================= */
static void test_basic_refsites(void)
{
    const char *src =
        "Main:\n"
        "    call Func\n"       /* absolute: refsite for Func */
        "    jp   Func\n"       /* absolute: refsite for Func */
        "    ld   hl, Data\n"   /* absolute: refsite for Data */
        ".loop:\n"
        "    jr   .loop\n"      /* relative: NO refsite */
        "    ret\n"
        "Func:\n"
        "    ret\n"
        "Data:\n"
        "    dw   Func\n"       /* absolute DW: refsite for Func, addend=0 */
        "    dw   Func+2\n";    /* absolute DW: refsite for Func, addend=2 */

    AsmResult r = asm_assemble(src, "test_refsites.asm");

    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.refs != NULL || r.nrefs == 0); /* array may be NULL only if nrefs==0 */

    /* We expect 5 refsites: call Func, jp Func, ld hl Data, dw Func, dw Func+2 */
    ASSERT_EQ(r.nrefs, 5);

    /* --- Verify call Func --- */
    const AsmRefSite *rs_call = NULL;
    /* There may be multiple refs to Func; find the one with addend=0 from call */
    /* call emits: CD lo hi  -> refsite at off+1 */
    for (int i = 0; i < r.nrefs; i++) {
        const AsmRefSite *rs = &r.refs[i];
        if (strcmp(rs->sym, "Func") == 0 && rs->addend == 0 && rs->size == 2) {
            /* Check the rom byte at off-1 is 0xCD (call opcode) */
            if (rs->off > 0 && r.rom[rs->off - 1] == 0xCD) {
                rs_call = rs;
                break;
            }
        }
    }
    ASSERT_TRUE(rs_call != NULL);
    if (rs_call) {
        ASSERT_EQ(rs_call->size, 2);
        ASSERT_EQ(rs_call->addend, 0);
        /* rom[off] and rom[off+1] should equal lo/hi of Func's address */
        const AsmSymbol *func_sym = find_sym(&r, "Func");
        ASSERT_TRUE(func_sym != NULL);
        if (func_sym) {
            uint16_t func_addr = func_sym->addr;
            ASSERT_EQ(r.rom[rs_call->off],     (func_addr & 0xFF));
            ASSERT_EQ(r.rom[rs_call->off + 1], ((func_addr >> 8) & 0xFF));
        }
    }

    /* --- Verify jp Func --- */
    const AsmRefSite *rs_jp = NULL;
    for (int i = 0; i < r.nrefs; i++) {
        const AsmRefSite *rs = &r.refs[i];
        if (strcmp(rs->sym, "Func") == 0 && rs->addend == 0 && rs->size == 2) {
            if (rs->off > 0 && r.rom[rs->off - 1] == 0xC3) { /* jp opcode */
                rs_jp = rs;
                break;
            }
        }
    }
    ASSERT_TRUE(rs_jp != NULL);
    if (rs_jp) {
        ASSERT_EQ(rs_jp->size, 2);
        ASSERT_EQ(rs_jp->addend, 0);
        const AsmSymbol *func_sym = find_sym(&r, "Func");
        if (func_sym) {
            uint16_t func_addr = func_sym->addr;
            ASSERT_EQ(r.rom[rs_jp->off],     (func_addr & 0xFF));
            ASSERT_EQ(r.rom[rs_jp->off + 1], ((func_addr >> 8) & 0xFF));
        }
    }

    /* --- Verify ld hl, Data --- */
    const AsmRefSite *rs_ld = find_ref(&r, "Data");
    ASSERT_TRUE(rs_ld != NULL);
    if (rs_ld) {
        ASSERT_EQ(rs_ld->size, 2);
        ASSERT_EQ(rs_ld->addend, 0);
        /* opcode before should be 0x21 (ld hl, d16) */
        ASSERT_TRUE(rs_ld->off > 0);
        if (rs_ld->off > 0)
            ASSERT_EQ(r.rom[rs_ld->off - 1], 0x21);
        const AsmSymbol *data_sym = find_sym(&r, "Data");
        ASSERT_TRUE(data_sym != NULL);
        if (data_sym) {
            uint16_t data_addr = data_sym->addr;
            ASSERT_EQ(r.rom[rs_ld->off],     (data_addr & 0xFF));
            ASSERT_EQ(r.rom[rs_ld->off + 1], ((data_addr >> 8) & 0xFF));
        }
    }

    /* --- Verify dw Func (addend=0) --- */
    /* There might be multiple Func,addend=0 refs (call and dw); find the one
     * where the preceding byte is NOT an opcode (i.e. in data region) */
    /* Actually: find the DW one by checking the Data symbol offset */
    {
        const AsmSymbol *data_sym = find_sym(&r, "Data");
        ASSERT_TRUE(data_sym != NULL);
        if (data_sym) {
            /* The dw Func should be at Data's ROM offset */
            const AsmRefSite *rs_dw = NULL;
            for (int i = 0; i < r.nrefs; i++) {
                if (strcmp(r.refs[i].sym, "Func") == 0 &&
                    r.refs[i].addend == 0 &&
                    r.refs[i].off == data_sym->off) {
                    rs_dw = &r.refs[i];
                    break;
                }
            }
            ASSERT_TRUE(rs_dw != NULL);
            if (rs_dw) {
                ASSERT_EQ(rs_dw->size, 2);
                const AsmSymbol *func_sym = find_sym(&r, "Func");
                if (func_sym) {
                    uint16_t func_addr = func_sym->addr;
                    ASSERT_EQ(r.rom[rs_dw->off],     (func_addr & 0xFF));
                    ASSERT_EQ(r.rom[rs_dw->off + 1], ((func_addr >> 8) & 0xFF));
                }
            }
        }
    }

    /* --- Verify dw Func+2 (addend=2) --- */
    const AsmRefSite *rs_dw2 = find_ref_addend(&r, "Func", 2);
    ASSERT_TRUE(rs_dw2 != NULL);
    if (rs_dw2) {
        ASSERT_EQ(rs_dw2->size, 2);
        ASSERT_EQ(rs_dw2->addend, 2);
        /* ROM bytes should be Func.addr + 2, little-endian */
        const AsmSymbol *func_sym = find_sym(&r, "Func");
        if (func_sym) {
            uint16_t expected = (uint16_t)(func_sym->addr + 2);
            ASSERT_EQ(r.rom[rs_dw2->off],     (expected & 0xFF));
            ASSERT_EQ(r.rom[rs_dw2->off + 1], ((expected >> 8) & 0xFF));
        }
    }

    /* --- Verify jr .loop produces NO absolute refsite --- */
    /* The jr instruction is 2 bytes (relative), not 3 — it can't produce a
     * refsite (our refsite code only fires for n==3 instructions).
     * Additionally check that no refsite has sym matching a local label. */
    for (int i = 0; i < r.nrefs; i++) {
        /* Local label expanded as "Main.loop" or similar; no ref to it expected */
        ASSERT_TRUE(strstr(r.refs[i].sym, "loop") == NULL);
    }

    asm_free(&r);
}

/* =========================================================================
 * Test 2: call cc (conditional call) and jp cc (conditional jump).
 * Both produce 3-byte absolute instructions and should record refsites.
 * ======================================================================= */
static void test_conditional_refs(void)
{
    const char *src =
        "Start:\n"
        "    call nz, Target\n"   /* 3 bytes, absolute -> refsite */
        "    jp   z,  Target\n"   /* 3 bytes, absolute -> refsite */
        "    ret\n"
        "Target:\n"
        "    ret\n";

    AsmResult r = asm_assemble(src, "test_cond.asm");
    ASSERT_TRUE(r.ok);

    /* Should have exactly 2 refsites (one per conditional instruction) */
    ASSERT_EQ(r.nrefs, 2);

    /* Both should reference "Target" with addend 0 */
    ASSERT_EQ(count_refs(&r, "Target"), 2);

    const AsmSymbol *tgt = find_sym(&r, "Target");
    ASSERT_TRUE(tgt != NULL);
    if (tgt) {
        for (int i = 0; i < r.nrefs; i++) {
            const AsmRefSite *rs = &r.refs[i];
            ASSERT_EQ(rs->size, 2);
            ASSERT_EQ(rs->addend, 0);
            ASSERT_EQ(r.rom[rs->off],     (tgt->addr & 0xFF));
            ASSERT_EQ(r.rom[rs->off + 1], ((tgt->addr >> 8) & 0xFF));
        }
    }

    asm_free(&r);
}

/* =========================================================================
 * Test 3: Pure constant expressions do NOT produce refsites.
 *         Multi-symbol expressions do NOT produce refsites.
 *         jr (relative) does NOT produce refsites.
 * ======================================================================= */
static void test_no_spurious_refs(void)
{
    const char *src =
        "Start:\n"
        "    ld   bc, $1234\n"    /* pure constant -> no refsite */
        "    jp   $0150\n"        /* pure constant -> no refsite */
        ".self:\n"
        "    jr   .self\n"        /* relative -> no refsite */
        "    ret\n";

    AsmResult r = asm_assemble(src, "test_noref.asm");
    ASSERT_TRUE(r.ok);

    /* No refsites: all operands are pure constants or relative */
    ASSERT_EQ(r.nrefs, 0);

    asm_free(&r);
}

/* =========================================================================
 * Test 4: ld rr, Symbol for all 16-bit register pairs.
 * ======================================================================= */
static void test_ld_rr_symbol(void)
{
    const char *src =
        "Main:\n"
        "    ld   bc, Data\n"   /* ld bc, d16 -> refsite */
        "    ld   de, Data\n"   /* ld de, d16 -> refsite */
        "    ld   hl, Data\n"   /* ld hl, d16 -> refsite */
        "    ld   sp, Data\n"   /* ld sp, d16 -> refsite */
        "    ret\n"
        "Data:\n"
        "    dw   $0000\n";

    AsmResult r = asm_assemble(src, "test_ldrr.asm");
    ASSERT_TRUE(r.ok);

    /* 4 ld rr, Data instructions -> 4 refsites */
    ASSERT_EQ(r.nrefs, 4);
    ASSERT_EQ(count_refs(&r, "Data"), 4);

    const AsmSymbol *data = find_sym(&r, "Data");
    ASSERT_TRUE(data != NULL);
    if (data) {
        for (int i = 0; i < r.nrefs; i++) {
            ASSERT_EQ(r.refs[i].size, 2);
            ASSERT_EQ(r.refs[i].addend, 0);
            ASSERT_EQ(r.rom[r.refs[i].off],     (data->addr & 0xFF));
            ASSERT_EQ(r.rom[r.refs[i].off + 1], ((data->addr >> 8) & 0xFF));
        }
    }

    asm_free(&r);
}

/* =========================================================================
 * Test 5: dw with addend cases.
 * ======================================================================= */
static void test_dw_addend(void)
{
    const char *src =
        "Label:\n"
        "    ret\n"
        "Table:\n"
        "    dw   Label\n"       /* addend = 0 */
        "    dw   Label+1\n"     /* addend = 1 */
        "    dw   Label+10\n"    /* addend = 10 */
        "    dw   $FFFF\n";      /* pure constant -> no refsite */

    AsmResult r = asm_assemble(src, "test_dw_addend.asm");
    ASSERT_TRUE(r.ok);

    /* 3 refsites (the $FFFF constant produces none) */
    ASSERT_EQ(r.nrefs, 3);
    ASSERT_EQ(count_refs(&r, "Label"), 3);

    ASSERT_TRUE(find_ref_addend(&r, "Label", 0)  != NULL);
    ASSERT_TRUE(find_ref_addend(&r, "Label", 1)  != NULL);
    ASSERT_TRUE(find_ref_addend(&r, "Label", 10) != NULL);

    const AsmSymbol *lbl = find_sym(&r, "Label");
    ASSERT_TRUE(lbl != NULL);
    if (lbl) {
        const AsmRefSite *rs10 = find_ref_addend(&r, "Label", 10);
        if (rs10) {
            uint16_t expected = (uint16_t)(lbl->addr + 10);
            ASSERT_EQ(r.rom[rs10->off],     (expected & 0xFF));
            ASSERT_EQ(r.rom[rs10->off + 1], ((expected >> 8) & 0xFF));
        }
    }

    asm_free(&r);
}

/* =========================================================================
 * main
 * ======================================================================= */
int main(void)
{
    test_basic_refsites();
    test_conditional_refs();
    test_no_spurious_refs();
    test_ld_rr_symbol();
    test_dw_addend();

    TEST_MAIN_END();
}
