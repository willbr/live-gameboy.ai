#ifndef LIVE_H
#define LIVE_H

#include <stdbool.h>
#include "../asm/asm.h"
#include "../gb/gb.h"

/*
 * live.h — Live session API for in-place ROM patching with state preservation.
 *
 * A LiveSession owns a GB emulator + the current assembled ROM + placement
 * memory.  live_reload() reassembles with the retained placement memory,
 * diffs per function, classifies each change, and applies in-place patches
 * to gb->rom at a safe point — leaving RAM/VRAM/CPU registers untouched.
 */

/* -----------------------------------------------------------------------
 * Patch classification
 * --------------------------------------------------------------------- */

typedef enum {
    PATCH_UNCHANGED,  /* Function bytes identical — no action needed        */
    PATCH_IN_PLACE,   /* Bytes changed, fit in slot, addr stable — patched  */
    PATCH_RELOCATED,  /* Layout assigned a new address (slot overflow)       */
    PATCH_REFUSED     /* Assembly failed or unsafe change — ROM not touched  */
} PatchKind;

typedef struct {
    char     func[64];    /* Function (placement) name                      */
    PatchKind kind;
    char     reason[120]; /* Human-readable reason (REFUSED / RELOCATED)    */
} PatchEntry;

typedef struct {
    PatchEntry *items;    /* Heap-allocated array (patch_report_free to free)*/
    int         count;
    bool        any_refused;
} PatchReport;

/* -----------------------------------------------------------------------
 * Live session (opaque)
 * --------------------------------------------------------------------- */

typedef struct LiveSession LiveSession;

/*
 * live_new — assemble `src` (RGBDS-style SM83 source) and load it into a
 * fresh GB emulator.  `filename` is used in diagnostics only (may be NULL).
 *
 * Returns NULL on assembly failure or OOM.  On success the GB is loaded and
 * reset (PC = 0x0100).  If the assembled source defines a global symbol
 * "Main", live_new patches rom[0x100..0x102] = JP Main so that execution
 * from 0x100 immediately jumps to the user's main loop entry point.
 */
LiveSession *live_new(const char *src, const char *filename);

/* live_gb — return the emulator owned by the session (never NULL after live_new). */
GB          *live_gb(LiveSession *s);

/*
 * live_reload — reassemble `new_src` with the session's retained placement
 * memory (so stable functions keep their addresses).  Then, for each
 * function placement:
 *
 *   UNCHANGED  — bytes identical, no action.
 *   IN_PLACE   — bytes differ, new code fits the slot, address unchanged:
 *                overwrite gb->rom at the function's linear offset (at a
 *                safe point: PC not in the patched range).
 *   RELOCATED  — layout assigned a new address (function outgrew its slot);
 *                logged but not fully implemented until Task 4.
 *   REFUSED    — assembly failed; ROM and state are not touched.
 *
 * Returns a PatchReport describing every function.  Call patch_report_free()
 * when done.
 */
PatchReport  live_reload(LiveSession *s, const char *new_src);

/*
 * live_soft_reload — stub (full implementation in Task 5).
 * For now: reassembles, reloads ROM, resets GB.  State is cleared.
 */
void         live_soft_reload(LiveSession *s, const char *new_src);

/* live_free — destroy session and all owned resources (GB, AsmResult, mem). */
void         live_free(LiveSession *s);

/* patch_report_free — release memory owned by a PatchReport. */
void         patch_report_free(PatchReport *r);

/*
 * live_result — return a pointer to the session's current AsmResult (build
 * database: assets, prov_asset, prov_asset_off, rom_size, etc.).
 * The pointer is valid until the next live_reload / live_soft_reload / live_free.
 * Never NULL after a successful live_new.
 */
AsmResult   *live_result(LiveSession *s);

#endif /* LIVE_H */
