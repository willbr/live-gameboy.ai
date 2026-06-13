#include "live.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Internal session state
 * --------------------------------------------------------------------- */

struct LiveSession {
    GB              *gb;
    AsmResult        result;    /* current assembled ROM + build db            */
    AsmPlacementMem  mem;       /* retained placement memory across reloads    */
    char            *filename;  /* used in diagnostics (heap copy)             */
    char            *src;       /* last assembled source (heap copy)           */
};

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

/* Append one PatchEntry to a PatchReport (grows items array). */
static bool report_add(PatchReport *rpt, const char *name,
                       PatchKind kind, const char *reason)
{
    PatchEntry *items = realloc(rpt->items,
                                (size_t)(rpt->count + 1) * sizeof(PatchEntry));
    if (!items) return false;
    rpt->items = items;
    PatchEntry *e = &items[rpt->count++];
    snprintf(e->func, sizeof e->func, "%s", name);
    e->kind = kind;
    snprintf(e->reason, sizeof e->reason, "%s", reason ? reason : "");
    if (kind == PATCH_REFUSED) rpt->any_refused = true;
    return true;
}

/* Linear ROM offset for a bank-0 or ROMX address in bank `bank`. */
static uint32_t linear_off(int bank, uint16_t addr)
{
    if (bank == 0) return (uint32_t)addr;
    /* ROMX: bank N mapped at 0x4000-0x7FFF */
    return (uint32_t)bank * 0x4000u + (addr - 0x4000u);
}

/*
 * Safe-point check: is `pc` within [addr, addr+len)?
 * Bank-aware: for bank-0 functions, pc must be in [addr, addr+len) with no
 * bank switching concern.  For ROMX (bank>0), we also check the live
 * rom_bank.  (For this task's tests only bank-0 programs are used.)
 */
static bool pc_in_range(const GB *gb, int bank, uint16_t addr, int len)
{
    uint16_t pc = gb->cpu.pc;
    if (bank == 0) {
        /* bank-0 is always mapped at 0x0000-0x3FFF */
        return pc >= addr && pc < (uint16_t)(addr + len);
    }
    /* ROMX: only relevant if the same bank is currently mapped */
    if (gb->rom_bank != (uint8_t)bank) return false;
    return pc >= addr && pc < (uint16_t)(addr + len);
}

/*
 * Step the CPU until PC exits [addr, addr+len) or the step limit is hit.
 * Returns true if PC is now outside the range.
 */
static bool step_to_safe(GB *gb, int bank, uint16_t addr, int len, int limit)
{
    for (int i = 0; i < limit && pc_in_range(gb, bank, addr, len); i++) {
        gb_step(gb);
    }
    return !pc_in_range(gb, bank, addr, len);
}

/* -----------------------------------------------------------------------
 * Entry-point patch (0x0100..0x0102 = JP Main)
 *
 * After assembling, if the source defines a global symbol "Main", we write
 * a JP instruction at rom[0x100] so gb_reset()'s PC=0x100 jumps straight
 * to Main.  Without this, the GB would execute whatever bytes happen to be
 * at 0x100 (NOP + header area) and likely hang.
 *
 * The JP opcode (0xC3) is 3 bytes: C3 lo hi (little-endian address).
 * This only fires if Main's address is in ROM0 (addr < 0x4000).
 * --------------------------------------------------------------------- */
static void patch_entry(AsmResult *r)
{
    /* Find "Main" symbol */
    for (int i = 0; i < r->nsyms; i++) {
        if (strcmp(r->syms[i].name, "Main") == 0 && r->syms[i].defined) {
            uint16_t main_addr = r->syms[i].addr;
            /* Only patch if Main is in ROM0 and the ROM is big enough */
            if (main_addr < 0x4000 && r->rom_size >= 0x103) {
                r->rom[0x100] = 0xC3;                    /* JP nn */
                r->rom[0x101] = (uint8_t)(main_addr & 0xFF);
                r->rom[0x102] = (uint8_t)((main_addr >> 8) & 0xFF);
            }
            return;
        }
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

LiveSession *live_new(const char *src, const char *filename)
{
    LiveSession *s = calloc(1, sizeof *s);
    if (!s) return NULL;

    s->filename = strdup(filename ? filename : "<live>");
    s->src      = strdup(src);
    if (!s->filename || !s->src) goto fail;

    /* Assemble with a fresh (empty) placement memory */
    memset(&s->mem, 0, sizeof s->mem);
    s->result = asm_assemble_mem(src, s->filename, &s->mem);
    if (!s->result.ok) {
        /* Print first diagnostic if available */
        if (s->result.ndiags > 0)
            fprintf(stderr, "live_new: assembly failed: %s\n",
                    s->result.diags[0].msg);
        asm_free(&s->result);
        goto fail;
    }

    /* Patch 0x100 -> JP Main so reset brings us to user code */
    patch_entry(&s->result);

    /* Boot the emulator */
    s->gb = gb_new();
    if (!s->gb) goto fail_after_asm;

    if (!gb_load_rom(s->gb, s->result.rom, s->result.rom_size)) {
        gb_free(s->gb);
        s->gb = NULL;
        goto fail_after_asm;
    }
    gb_reset(s->gb);
    return s;

fail_after_asm:
    asm_free(&s->result);
fail:
    free(s->filename);
    free(s->src);
    free(s->mem.items);
    free(s);
    return NULL;
}

GB *live_gb(LiveSession *s)
{
    return s ? s->gb : NULL;
}

PatchReport live_reload(LiveSession *s, const char *new_src)
{
    PatchReport rpt;
    memset(&rpt, 0, sizeof rpt);

    /* Reassemble with retained placement memory -> stable layout */
    AsmResult nr = asm_assemble_mem(new_src, s->filename, &s->mem);

    if (!nr.ok) {
        /* Assembly failed: refuse, leave gb->rom untouched */
        const char *reason = (nr.ndiags > 0) ? nr.diags[0].msg
                                              : "assembly failed";
        report_add(&rpt, "<source>", PATCH_REFUSED, reason);
        asm_free(&nr);
        return rpt;
    }

    /* Patch 0x100 -> JP Main in the new ROM */
    patch_entry(&nr);

    /* Diff each function placement from the NEW build against the LIVE gb->rom */
    for (int i = 0; i < nr.nplacements; i++) {
        const AsmPlacement *np = &nr.placements[i];

        /* Find the matching placement in the OLD (live) build */
        const AsmPlacement *op = NULL;
        for (int j = 0; j < s->result.nplacements; j++) {
            if (strcmp(s->result.placements[j].name, np->name) == 0) {
                op = &s->result.placements[j];
                break;
            }
        }

        if (!op) {
            /* New function not seen before — treat as in-place (new slot) */
            /* Copy new ROM bytes into live ROM */
            uint32_t off = linear_off(np->bank, np->addr);
            if (off + (uint32_t)np->slot_size <= (uint32_t)s->gb->rom_size &&
                off + (uint32_t)np->slot_size <= (uint32_t)nr.rom_size) {
                step_to_safe(s->gb, np->bank, np->addr, np->slot_size, 200000);
                memcpy(s->gb->rom + off, nr.rom + off,
                       (size_t)np->slot_size);
                report_add(&rpt, np->name, PATCH_IN_PLACE, "new function");
            }
            continue;
        }

        /* Check if address changed (relocation) */
        if (np->addr != op->addr || np->bank != op->bank) {
            /* RELOCATED: Task 4 will implement trampoline; log it for now */
            report_add(&rpt, np->name, PATCH_RELOCATED,
                       "function outgrew slot and was relocated");
            continue;
        }

        /* Addresses match: compare bytes in new ROM vs live gb->rom */
        uint32_t off      = linear_off(np->bank, np->addr);
        int      slot_sz  = np->slot_size;

        /* Bounds check */
        if (off + (uint32_t)slot_sz > (uint32_t)s->gb->rom_size ||
            off + (uint32_t)slot_sz > (uint32_t)nr.rom_size) {
            report_add(&rpt, np->name, PATCH_REFUSED, "out of bounds");
            continue;
        }

        bool same = (memcmp(s->gb->rom + off, nr.rom + off,
                            (size_t)slot_sz) == 0);
        if (same) {
            report_add(&rpt, np->name, PATCH_UNCHANGED, "");
            continue;
        }

        /* Bytes differ and addr is unchanged: IN_PLACE patch */
        /* Safe point: step CPU until PC exits the function's range */
        step_to_safe(s->gb, op->bank, op->addr, op->slot_size, 200000);

        /* Overwrite the full slot in gb->rom with the new ROM's bytes */
        memcpy(s->gb->rom + off, nr.rom + off, (size_t)slot_sz);
        report_add(&rpt, np->name, PATCH_IN_PLACE, "");
    }

    /* Check for functions present in old build but removed in new build */
    for (int j = 0; j < s->result.nplacements; j++) {
        const AsmPlacement *op = &s->result.placements[j];
        bool found = false;
        for (int i = 0; i < nr.nplacements; i++) {
            if (strcmp(nr.placements[i].name, op->name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            /* Function removed — report as unchanged (it's still in live ROM) */
            report_add(&rpt, op->name, PATCH_UNCHANGED, "removed from source");
        }
    }

    /* Replace session's result with the new build */
    asm_free(&s->result);
    s->result = nr;

    free(s->src);
    s->src = strdup(new_src);

    return rpt;
}

void live_soft_reload(LiveSession *s, const char *new_src)
{
    /* Stub: full implementation in Task 5.
     * For now: reassemble (fresh placement mem), reload ROM, reset GB. */
    AsmPlacementMem fresh;
    memset(&fresh, 0, sizeof fresh);

    AsmResult nr = asm_assemble_mem(new_src, s->filename, &fresh);
    if (!nr.ok) {
        asm_free(&nr);
        free(fresh.items);
        return;
    }

    patch_entry(&nr);

    asm_free(&s->result);
    s->result = nr;

    free(s->mem.items);
    s->mem = fresh;

    free(s->src);
    s->src = strdup(new_src);

    gb_load_rom(s->gb, s->result.rom, s->result.rom_size);
    gb_reset(s->gb);
}

void live_free(LiveSession *s)
{
    if (!s) return;
    gb_free(s->gb);
    asm_free(&s->result);
    free(s->mem.items);
    free(s->filename);
    free(s->src);
    free(s);
}

void patch_report_free(PatchReport *r)
{
    if (!r) return;
    free(r->items);
    r->items = NULL;
    r->count = 0;
    r->any_refused = false;
}
