/*
 * layout.c — Function-aware layout + placement memory (Milestone 4, Task 1)
 *
 * A "function" is a global-label-delimited region: from a global label to
 * the next global label in the same bank/section.  Local labels (.foo)
 * belong to their enclosing function and are not treated as function
 * boundaries.
 *
 * slot_size = round_up(function_bytes, GRANULE) + GRANULE
 * where GRANULE = 16 bytes.  (One granule of slack lets small edits stay
 * in-place without relocation.)
 *
 * Layout runs BETWEEN pass 1 (which gives function byte sizes via symbol
 * sizes) and pass 2 (which emits at assigned addresses).
 *
 * Fresh layout (inout_mem NULL or empty):
 *   Place functions sequentially per bank from the section base, each in
 *   its padded slot.  Gaps between slots are left as 0x00 (the ROM image
 *   is already zeroed by calloc).
 *
 * Retained layout (inout_mem has prior placements):
 *   For each function, if placement memory has it AND
 *   function_bytes <= remembered slot_size, reuse that addr+slot_size
 *   (STABLE — same address).  Otherwise allocate a fresh slot at the
 *   bank's current high-water free area (relocation).
 */

#include "asm.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define LAYOUT_GRANULE  16          /* slot alignment / slack granule */
#define MAX_BANKS       256         /* enough for the largest MBC ROMs */
#define HDR_START       0x0100u     /* cartridge header: entry/logo/checksums */
#define HDR_END         0x0150u     /* first byte of code space after header  */

/* -------------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/* Round `n` up to the nearest multiple of `g` (g must be a power of 2). */
static int round_up(int n, int g)
{
    return (n + g - 1) & ~(g - 1);
}

/* Look up a placement by name in `mem`; returns NULL if not found. */
static AsmPlacement *mem_find(AsmPlacementMem *mem, const char *name)
{
    if (!mem) return NULL;
    for (int i = 0; i < mem->count; i++) {
        if (strcmp(mem->items[i].name, name) == 0)
            return &mem->items[i];
    }
    return NULL;
}

/* Add or update a placement in `mem`. */
static bool mem_upsert(AsmPlacementMem *mem,
                        const char *name, int bank, uint16_t addr, int slot_size)
{
    /* Update existing entry if present */
    for (int i = 0; i < mem->count; i++) {
        if (strcmp(mem->items[i].name, name) == 0) {
            mem->items[i].bank      = bank;
            mem->items[i].addr      = addr;
            mem->items[i].slot_size = slot_size;
            return true;
        }
    }
    /* Append new entry */
    if (mem->count >= mem->cap) {
        int nc = mem->cap ? mem->cap * 2 : 16;
        AsmPlacement *np = realloc(mem->items, (size_t)nc * sizeof(AsmPlacement));
        if (!np) return false;
        mem->items = np;
        mem->cap   = nc;
    }
    AsmPlacement *p = &mem->items[mem->count++];
    memset(p, 0, sizeof(*p));
    size_t nl = strlen(name);
    if (nl >= sizeof(p->name)) nl = sizeof(p->name) - 1;
    memcpy(p->name, name, nl);
    p->name[nl]  = '\0';
    p->bank      = bank;
    p->addr      = addr;
    p->slot_size = slot_size;
    return true;
}

/* -------------------------------------------------------------------------
 * Public: layout_plan
 *
 * Given:
 *   syms       — symbol table from pass 1 (with sizes computed)
 *   nsyms      — number of symbols
 *   sec_base   — base CPU address of the default section (DEFAULT_ORG = 0x0150)
 *   inout_mem  — retained placement memory (may be NULL)
 *
 * Returns a heap-allocated array of AsmPlacement (caller frees), or NULL
 * on allocation failure.  *out_count receives the number of placements.
 *
 * Also updates *inout_mem with the placements from this build.
 *
 * The function identifies "functions" = global labels with size > 0 (or
 * the last global label even if size is 0).  EQU constants (bank < 0) are
 * skipped.  Local labels are absorbed into their enclosing function's size
 * and are not treated as separate functions.
 *
 * After this call, the symbol table entries for global labels are updated:
 *   sym.addr = assigned slot start address
 *   sym.off  = linear ROM offset of slot start
 *   sym.value = sym.addr (kept consistent)
 * Local labels that fall inside a function get their addresses adjusted by
 * the same delta as their enclosing function.
 * ----------------------------------------------------------------------- */

/*
 * Identify global labels (functions) from the symbol table.
 * We define a "function" as a symbol that:
 *   - has bank >= 0 (not a constant)
 *   - is NOT a local label (name does not contain '.' after the first char
 *     in the expanded form "Global.local")
 *   - size >= 0 (we include size-0 functions for completeness)
 *
 * Returns a heap-allocated array of pointers into syms[], sorted by
 * (bank, off).  *out_n receives the count.  Caller frees the pointer array
 * (not the AsmSymbol objects themselves).
 */
static AsmSymbol **collect_globals(AsmSymbol *syms, int nsyms, int *out_n)
{
    int cap = 16, n = 0;
    AsmSymbol **arr = malloc((size_t)cap * sizeof(AsmSymbol *));
    if (!arr) { *out_n = 0; return NULL; }

    for (int i = 0; i < nsyms; i++) {
        AsmSymbol *s = &syms[i];
        if (s->bank < 0) continue;   /* constant / EQU */
        /* Local labels in expanded form contain a dot NOT at position 0 */
        bool is_local = false;
        for (size_t k = 1; s->name[k]; k++) {
            if (s->name[k] == '.') { is_local = true; break; }
        }
        if (is_local) continue;

        if (n >= cap) {
            int nc = cap * 2;
            AsmSymbol **np = realloc(arr, (size_t)nc * sizeof(AsmSymbol *));
            if (!np) { free(arr); *out_n = 0; return NULL; }
            arr = np; cap = nc;
        }
        arr[n++] = s;
    }

    /* Sort by (bank, off) — insertion sort (usually small) */
    for (int i = 1; i < n; i++) {
        AsmSymbol *key = arr[i];
        int j = i - 1;
        while (j >= 0 &&
               (arr[j]->bank > key->bank ||
                (arr[j]->bank == key->bank && arr[j]->off > key->off))) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }

    *out_n = n;
    return arr;
}

/* Linear ROM offset from bank + cpu_addr (matches assemble.c) */
static uint32_t layout_linear_off(int bank, uint16_t addr)
{
    if (bank == 0) return (uint32_t)addr;
    return (uint32_t)bank * 0x4000u + ((uint32_t)addr - 0x4000u);
}

/*
 * layout_plan: the main entry point.
 *
 * `syms` / `nsyms` are passed by pointer so we can update sym.addr, sym.off,
 * sym.value in place (the assembler uses the same symbol table in pass 2).
 */
AsmPlacement *layout_plan(AsmSymbol *syms, int nsyms,
                           uint16_t sec_base, int sec_bank,
                           AsmPlacementMem *inout_mem,
                           int *out_count)
{
    *out_count = 0;

    /* Collect global labels (potential functions) */
    int nglobals = 0;
    AsmSymbol **globals = collect_globals(syms, nsyms, &nglobals);
    if (!globals) return NULL;

    if (nglobals == 0) {
        free(globals);
        return NULL;  /* no functions to lay out */
    }

    /* Per-bank high-water mark for fresh / relocated allocation.
     *
     * For each bank, HWM starts at the MINIMUM original address of any
     * global label in that bank.  This preserves any preamble code that
     * appears before the first global label: the preamble lives in
     * [sec_base, first_label.addr) and is never clobbered.
     * If there is no preamble (first label IS at sec_base), HWM = sec_base.
     *
     * We use a small fixed array indexed by bank number.
     */
    uint16_t hwm[MAX_BANKS];
    memset(hwm, 0, sizeof(hwm));

    /* Initialise HWM to the section base as the default for banks with no
     * global labels. */
    if (sec_bank < MAX_BANKS)
        hwm[sec_bank] = sec_base;

    /* For banks with global labels, set HWM = min original label address.
     * (globals[] is sorted by (bank, off), so the first entry per bank
     *  gives the minimum.) */
    {
        int seen_bank = -1;
        for (int gi = 0; gi < nglobals; gi++) {
            AsmSymbol *sym = globals[gi];
            int b = sym->bank;
            if (b < 0 || b >= MAX_BANKS) continue;
            if (b != seen_bank) {
                /* First (minimum-off) label in this bank */
                seen_bank = b;
                if (sym->addr > hwm[b]) {
                    /* Preamble exists — preserve it by starting HWM after it */
                    hwm[b] = sym->addr;
                }
                /* If sym->addr == hwm[b] (first label IS at sec_base),
                 * HWM stays at sec_base — no preamble to preserve. */
            }
        }
    }

    /* If we have retained placement memory, also advance HWM past any
     * retained slots (for functions that already have stable addresses). */
    if (inout_mem) {
        for (int i = 0; i < inout_mem->count; i++) {
            AsmPlacement *p = &inout_mem->items[i];
            int b = p->bank;
            if (b < 0 || b >= MAX_BANKS) continue;
            uint16_t end_addr = (uint16_t)(p->addr + (uint16_t)p->slot_size);
            if (end_addr > hwm[b]) hwm[b] = end_addr;
        }
    }

    /* Allocate output placements array */
    AsmPlacement *out = malloc((size_t)nglobals * sizeof(AsmPlacement));
    if (!out) { free(globals); return NULL; }

    for (int gi = 0; gi < nglobals; gi++) {
        AsmSymbol *sym = globals[gi];
        int   bank      = sym->bank;
        int   func_bytes = sym->size;  /* computed by pass 1 */

        /* Clamp bank */
        if (bank < 0) bank = 0;
        if (bank >= MAX_BANKS) bank = MAX_BANKS - 1;

        /* Compute slot size: round_up(func_bytes, GRANULE) + GRANULE */
        int rounded = round_up(func_bytes > 0 ? func_bytes : 0, LAYOUT_GRANULE);
        int slot_sz  = rounded + LAYOUT_GRANULE;

        /* Determine base address for this bank */
        uint16_t bank_base = (bank == 0) ? 0x0000u : 0x4000u;

        /* Decide address: reuse retained or allocate fresh */
        uint16_t assigned_addr;
        int      assigned_slot;
        bool     reused = false;

        if (inout_mem) {
            AsmPlacement *prev = mem_find(inout_mem, sym->name);
            if (prev && prev->bank == bank && func_bytes <= prev->slot_size) {
                /* Reuse: STABLE address */
                assigned_addr = prev->addr;
                assigned_slot = prev->slot_size;
                reused = true;
            }
        }

        if (!reused) {
            /* Fresh or relocated allocation */
            /* Align HWM to GRANULE */
            uint16_t aligned = (uint16_t)round_up((int)hwm[bank], LAYOUT_GRANULE);
            /* For ROMX banks, don't go below 0x4000 */
            if (bank > 0 && aligned < bank_base) aligned = bank_base;
            /* For ROM0, respect sec_base if this is the same bank */
            if (bank == sec_bank && aligned < sec_base) aligned = sec_base;

            /* Don't place functions on top of the cartridge header
             * ($0100-$014F: entry point, Nintendo logo, title, checksums).
             * If a ROM0 slot would start in or extend into that region, bump
             * it up to $0150.  Small programs (slots ending at/below $0100)
             * are unaffected, preserving the ROM0-starts-at-$0000 contract. */
            if (bank == 0 &&
                aligned < HDR_END && (uint16_t)(aligned + slot_sz) > HDR_START) {
                aligned = HDR_END;
            }

            assigned_addr = aligned;
            assigned_slot = slot_sz;
        }

        /* Update HWM for subsequent fresh allocations */
        {
            uint16_t end = (uint16_t)(assigned_addr + (uint16_t)assigned_slot);
            if (bank < MAX_BANKS && end > hwm[bank]) hwm[bank] = end;
        }

        /* Record placement */
        AsmPlacement *pl = &out[gi];
        memset(pl, 0, sizeof(*pl));
        size_t nl = strlen(sym->name);
        if (nl >= sizeof(pl->name)) nl = sizeof(pl->name) - 1;
        memcpy(pl->name, sym->name, nl);
        pl->name[nl]  = '\0';
        pl->bank      = bank;
        pl->addr      = assigned_addr;
        pl->slot_size = assigned_slot;

        /* Update symbol address/offset in the table so pass 2 resolves
         * labels to slot addresses */
        int16_t delta = (int16_t)(assigned_addr - sym->addr);
        sym->addr  = assigned_addr;
        sym->off   = layout_linear_off(bank, assigned_addr);
        sym->value = (long)assigned_addr;

        /* Adjust local labels that belong to this function.
         * A local label "Func.local" has the function name as a prefix
         * followed by '.'.  We update its addr/off by the same delta. */
        if (delta != 0) {
            size_t prefix_len = strlen(sym->name);
            for (int si = 0; si < nsyms; si++) {
                AsmSymbol *ls = &syms[si];
                if (ls->bank != bank) continue;
                /* Check if ls->name starts with "sym->name." */
                if (strncmp(ls->name, sym->name, prefix_len) == 0 &&
                    ls->name[prefix_len] == '.') {
                    ls->addr  = (uint16_t)((int)ls->addr  + delta);
                    ls->off   = (uint32_t)((int32_t)ls->off + delta);
                    ls->value = (long)ls->addr;
                }
            }
        }
    }

    /* Update (or populate) inout_mem with this build's placements */
    if (inout_mem) {
        for (int gi = 0; gi < nglobals; gi++) {
            AsmPlacement *pl = &out[gi];
            mem_upsert(inout_mem, pl->name, pl->bank, pl->addr, pl->slot_size);
        }
    }

    free(globals);
    *out_count = nglobals;
    return out;
}
