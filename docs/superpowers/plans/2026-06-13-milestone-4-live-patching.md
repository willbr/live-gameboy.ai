# Milestone 4: Live Patching Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** The project's thesis — edit a function in a running game and have the change take effect **with all state (RAM/VRAM/CPU registers) preserved**. A `live` session wraps the assembler + emulator: it loads source, runs it, and on a source edit reassembles, diffs functions, and patches the live ROM in place — relocating with a trampoline when a function outgrows its slot, refusing unsafe edits with a soft-reload fallback. Acceptance (all headless): a scripted scenario boots a program that maintains state in WRAM, edits a function's body, reloads, and asserts both (a) the new behavior is live and (b) the prior state survived — across the in-place, relocation, and refusal cases.

**Architecture:** Three layers, all SDL-free and headless-testable:
1. **Layout manager** (in `libasm`): functions (global-label-delimited regions) are placed in **padded slots** at addresses that are **stable across rebuilds** via *placement memory* (a record of each function's bank/addr/slot-size). Unchanged functions keep their exact address; an edited function that still fits its slot keeps its address too — so the common edit needs **no relocation at all**, just a byte overwrite of the slot.
2. **Reference-site tracking** (in `libasm`'s build database): every ROM byte-pair that encodes an absolute address of a symbol is recorded as `(offset, symbol, addend)`, so call sites can be rebound when a function relocates.
3. **Live session** (`src/live/live.c`): owns a `GB` + the current `AsmResult` + placement memory. `live_reload(new_src)` reassembles with the retained placement memory, diffs per function, classifies each as UNCHANGED / IN_PLACE / RELOCATED / REFUSED, and applies patches to `gb->rom` at a safe point. Returns a patch report. The emulator keeps running with RAM/VRAM/registers intact because only ROM bytes change.

**Key correctness model (from spec §5):** patches apply at a *safe point* — between emulated instructions, with PC outside any patched range and no stack return address inside a relocated function's old body. Relocation keeps the old body as a *zombie* (never overwritten) so in-flight calls and stale return addresses stay valid; new calls go through a `JP new` trampoline written at the old entry. Everything is keyed by linear ROM offset and is bank-aware; relocation stays within the function's own bank (or bank 0).

**Tech Stack:** C11, no new deps. Builds on libgb + libasm. Headless tests drive the whole engine.

**Spec:** design §5 (live patch engine), §4 (layout manager). Milestone 4.

**Depends on:** Milestones 1–3 (merged): emulator runs ROMs; assembler produces ROM + build database (symbols with sizes, linemap, provenance).

---

## Concepts

- **Function**: a region from a global label to the next global label in the same section/bank. (Local labels belong to their enclosing function.) The build database already records global symbols with a `size` (distance to next label); a function's slot covers `[addr, addr+size)`.
- **Padded slot**: `slot_size = round_up(function_bytes + slack, GRANULE)` where GRANULE = 16 bytes and slack ≥ 0 (use slack so small growth fits without relocation; e.g. round function size up to the next 16-byte multiple plus one extra granule). The next function starts after the slot, not after the raw bytes.
- **Placement memory**: `{ name, bank, addr, slot_size }[]` retained across reloads. On reassembly, the layout manager places each function: if it exists in placement memory and still fits its remembered slot, reuse the same addr+slot; otherwise allocate a new slot (append in the bank's free area) — relocation.

---

## File structure
```
src/asm/layout.c     — NEW: function-aware layout + placement memory (in libasm)
src/asm/asm.h        — layout + reference-site types added to build db; asm_assemble takes optional placement memory
src/asm/assemble.c   — integrate layout pass; record reference sites
src/live/live.h      — live session API + patch report types
src/live/live.c      — NEW: session, reload/diff/classify/apply, safe-point, trampoline
tests/test_layout.c          — stable placement across rebuilds
tests/test_refsites.c        — reference-site recording
tests/test_live_inplace.c    — edit fits slot: state preserved, behavior changes
tests/test_live_reloc.c      — edit outgrows slot: trampoline + state preserved
tests/test_live_refuse.c     — unsafe edit refused + soft reload
tests/test_live_e2e.c        — full scripted scenario
```

---

### Task 1: Function-aware layout + placement memory

**Files:** create `src/asm/layout.c`; extend `asm.h`; modify `assemble.c`; create `tests/test_layout.c`.

- [ ] **Step 1: asm.h — layout types + API change**

```c
typedef struct { char name[64]; int bank; uint16_t addr; int slot_size; } AsmPlacement;
typedef struct { AsmPlacement *items; int count; int cap; } AsmPlacementMem;

/* assemble with optional retained placement memory (NULL = fresh).
   On return, *inout_mem (if non-NULL) is updated with this build's placements. */
AsmResult asm_assemble_mem(const char *src, const char *filename, AsmPlacementMem *inout_mem);
/* existing asm_assemble(src,file) = asm_assemble_mem(src,file,NULL) */
```

Add to `AsmResult`: `AsmPlacement *placements; int nplacements;` (the function placements used in this build) and per-symbol a `bool is_function` (global, code) flag if not already derivable.

- [ ] **Step 2: tests/test_layout.c**

```
1. Assemble a program with 3 functions (FuncA, FuncB, FuncC), capturing placement memory.
   Assert each function's addr is granule-aligned and FuncB starts at FuncA.addr+FuncA.slot_size, etc.
2. Edit FuncB's body to a DIFFERENT but same-or-smaller byte size; reassemble WITH the retained
   placement memory. Assert FuncA, FuncB, FuncC all keep their EXACT same addresses (stable layout),
   and FuncB still fits its slot.
3. Grow FuncB beyond its slot; reassemble with placement memory. Assert FuncA + FuncC keep their
   addresses, and FuncB gets a NEW addr (relocated) appended in the bank's free area; placement
   memory now records FuncB's new slot.
```

- [ ] **Step 3: implement layout.c + integrate**

`layout_plan(symbols, sizes, sections, placement_mem) -> placements`: for each function (in source order, per bank), if placement_mem has it and `function_bytes <= remembered slot_size`, reuse remembered addr+slot; else allocate a fresh slot after the current high-water mark of free space in that bank (granule-aligned, slot = round_up(bytes,16)+16). The assembler runs layout BETWEEN pass 1 (which determines function byte sizes) and pass 2 (which emits at the assigned addresses). Functions are emitted at their slot addr; gaps between slots are zero/0x00 padding (or 0xFF). Update placement_mem with the final placements.

NOTE: this changes addressing — labels now resolve to slot addresses. The default-org / SECTION handling from M3 still applies for the FIRST build; placement memory only constrains subsequent builds. Keep M3's e2e tests passing (they don't pass placement memory, so layout with NULL mem must reproduce sane sequential placement — i.e. fresh layout = pack functions with padding sequentially from the section base).

- [ ] **Step 4: run `make test` — test_layout passes AND all M3 asm tests + e2e still pass (fresh layout must keep them runnable). blargg/acid2/sound still pass. Commit.**

---

### Task 2: Reference-site tracking in the build database

**Files:** modify `asm.h`, `encode.c`, `assemble.c`; create `tests/test_refsites.c`.

- [ ] **Step 1: asm.h** — add to AsmResult:
```c
typedef struct { uint32_t off; char sym[64]; long addend; int size; /*2*/ } AsmRefSite;
AsmRefSite *refs; int nrefs;
```

- [ ] **Step 2: tests/test_refsites.c** — assemble a program with `call Func`, `jp Func`, `ld hl, Data`, `dw Func`; assert refs records the offset of each address operand, the symbol name, addend 0 (or correct for `Func+2`), size 2. Assert a `jr` (relative) is NOT recorded as an absolute ref (or recorded distinctly) — relative refs don't need rebinding the same way.

- [ ] **Step 3: implement** — in the encode/emit path, when a 16-bit absolute address operand resolves to an expression depending on exactly one symbol, record an AsmRefSite at the operand's ROM offset. (For `Func+2`, addend=2.) Capture during pass 2 where offsets are known.

- [ ] **Step 4: run, pass, gates pass, commit.**

---

### Task 3: Live session core — in-place patching with state preservation

**Files:** create `src/live/live.h`, `src/live/live.c`; create `tests/test_live_inplace.c`; Makefile (live objects, link into tests).

- [ ] **Step 1: live.h — API + patch report**

```c
typedef enum { PATCH_UNCHANGED, PATCH_IN_PLACE, PATCH_RELOCATED, PATCH_REFUSED } PatchKind;
typedef struct { char func[64]; PatchKind kind; char reason[120]; } PatchEntry;
typedef struct { PatchEntry *items; int count; bool any_refused; } PatchReport;

typedef struct LiveSession LiveSession;
LiveSession *live_new(const char *src, const char *filename);   /* assembles + loads a GB */
GB          *live_gb(LiveSession *s);
PatchReport  live_reload(LiveSession *s, const char *new_src);   /* reassemble + patch live */
void         live_soft_reload(LiveSession *s, const char *new_src); /* full re-run from reset */
void         live_free(LiveSession *s);
void         patch_report_free(PatchReport *r);
```

- [ ] **Step 2: tests/test_live_inplace.c — the thesis test (in-place case)**

```
Program v1: a "game loop" that, each iteration, calls Tick (which adds 1 to a counter at WRAM
$C000), then loops. Boot via live_new; run enough steps that the counter reaches, say, 50.
Capture counter value.
Edit ONLY Tick's body so it adds 2 instead of 1 (same byte size or within slot). live_reload.
Assert: report says Tick = PATCH_IN_PLACE; the counter at $C000 was NOT reset (>= 50, state
preserved); after running more steps, the counter increases by 2 per iteration (new behavior live).
Also assert an UNEDITED function reports PATCH_UNCHANGED and registers/SP are intact across reload.
```

(The test must construct programs where Tick fits its slot before and after. Use the layout's slack. The reload applies the patch at a safe point — in this single-threaded test, call live_reload while the CPU is paused between steps; live_reload internally verifies PC is not inside the patched range, which it won't be if the loop is in the main loop body, not inside Tick at the pause moment — arrange the pause accordingly, or have live_reload step to a safe point.)

- [ ] **Step 3: implement live.c (in-place path)**

`live_new`: assemble (fresh placement mem stored in session), gb_new + gb_load_rom(rom) + gb_reset; keep AsmResult + placement mem.
`live_reload`: `asm_assemble_mem(new_src, file, &session->mem)` (retained mem → stable layout). If !ok → return a report with a single REFUSED entry (assemble error) and do nothing. Else diff per function: for each function, compare the new ROM slot bytes to the live `gb->rom` slot bytes.
  - identical → UNCHANGED.
  - differ AND new function bytes ≤ slot AND addr unchanged → IN_PLACE: overwrite `gb->rom[addr_offset .. ]` with the new bytes (and clear the rest of the slot to padding) at a safe point. 
  - addr changed (relocated by layout) → RELOCATED (Task 4 implements; for this task, if layout relocated a function, mark RELOCATED but you may defer actual trampoline to Task 4 — to keep Task 3 self-contained, ensure the in-place test programs never trigger relocation).
Safe point: before applying, if `gb->cpu.pc` is within any patched offset's mapped range for the current bank, step the CPU until it isn't (bounded). Apply by writing to `gb->rom` (linear offset). Swap session's AsmResult to the new one.

- [ ] **Step 4: run test_live_inplace — must pass (state preserved + new behavior). Debug carefully. blargg/acid2/sound still pass. Commit.**

---

### Task 4: Trampoline relocation for outgrown functions

**Files:** modify `live.c`; create `tests/test_live_reloc.c`.

- [ ] **Step 1: tests/test_live_reloc.c**

```
Program v1 with Tick (small) and other functions; boot; advance state (counter to N).
Edit Tick to be LARGER than its slot (e.g. add many instructions). live_reload.
Assert: report says Tick = PATCH_RELOCATED; a JP trampoline (0xC3 + new addr little-endian) was
written at Tick's OLD entry address in gb->rom; calling Tick (which still happens via the old
address from unchanged callers) reaches the new code (counter changes per the new body); the
state counter was preserved (>= N). Run more steps and assert new behavior.
Also assert: a caller that referenced Tick via an absolute `call Tick` reference site was rebound
to the new address (check the bytes at the recorded ref site now equal the new addr) OR is covered
by the trampoline (either is acceptable; document which).
```

- [ ] **Step 2: implement relocation in live_reload**

When layout assigns a function a NEW address (outgrew its slot), the new ROM already has the function at the new addr (layout placed it there) and the OLD slot is now free/padding in the new ROM. To preserve running state we do NOT trust the new ROM's old-slot contents for the live image; instead:
  - Write the new function bytes at the new addr into `gb->rom` (they're already there in new ROM; copy that slot region into the live rom).
  - Write a `JP newaddr` (C3 lo hi) trampoline at the OLD entry address in `gb->rom` (the zombie body after the first 3 bytes stays as-is — old in-flight calls/returns remain valid).
  - Rebind static call sites: for each AsmRefSite in the new build referencing this function, also patch the live `gb->rom` at that offset to the new addr (so future calls go direct). The trampoline covers any site we miss.
  - All within the same bank; if the new addr is in a different bank than the old, refuse (REFUSED, soft reload) — relocation must stay in-bank (or bank 0).
Apply at a safe point (PC not in old or new range; no stack return addr in the *new* overwrite range — old body is never overwritten so stale returns are safe by construction).

- [ ] **Step 3: run, pass, gates pass, commit.**

---

### Task 5: Safe-point rigor + unsafe-edit refusal + soft reload

**Files:** modify `live.c`; create `tests/test_live_refuse.c`.

- [ ] **Step 1: tests/test_live_refuse.c**
```
1. Unsafe edit: change a SECTION/variable layout such that a function cannot be safely patched
   (e.g. the assemble fails, or a function would need cross-bank relocation, or the new build
   changes a WRAM variable address that running state depends on — simplest testable: a source
   that fails to assemble, and a source that forces cross-bank relocation). Assert live_reload
   returns REFUSED with a reason and does NOT alter gb->rom (verify ROM bytes unchanged) and does
   NOT corrupt running state.
2. live_soft_reload: assert it reassembles, reloads the ROM, and resets the GB (state cleared,
   PC=0x100), and runs the new program fresh.
3. Safe-point: construct a case where PC is inside a function being patched at reload time; assert
   live_reload advances to a safe point before applying (the patched function's bytes only change
   once PC has exited its range), and the program doesn't crash/execute a half-written instruction.
```

- [ ] **Step 2: implement** — REFUSED paths: assemble error, cross-bank relocation needed, slot/bank full. On refuse, make NO changes to gb->rom (apply patches to a scratch copy first, commit only if all entries are safe — or compute the full plan before mutating). Safe-point: a bounded loop stepping the CPU until PC (bank-aware) is outside all patch ranges and no stack return address lies in a range being *overwritten*; if it can't reach a safe point within a bound, refuse. `live_soft_reload`: asm_assemble_mem(fresh mem), gb_load_rom, gb_reset.

- [ ] **Step 3: run, pass, gates pass, commit.**

---

### Task 6: Full scripted scenario gate + integration + CI/README

**Files:** create `tests/test_live_e2e.c`; modify Makefile, CI, README.

- [ ] **Step 1: test_live_e2e.c — the milestone gate**

A single scripted scenario exercising the whole thesis: boot a small "game" (maintains a visible state — e.g. a counter rendered as a tile, or a value in WRAM, plus the framebuffer), then perform a SEQUENCE of live edits without ever resetting:
  1. in-place edit (change a constant in a function) → assert IN_PLACE, state preserved, behavior changed;
  2. an edit that grows a function past its slot → assert RELOCATED, state preserved, behavior changed;
  3. an unsafe edit → assert REFUSED, state + behavior unchanged;
  4. a follow-up valid in-place edit after the refusal → assert it still works (engine recovered).
Assert WRAM state monotonically reflects continuity (never reset) across all valid reloads. This is the proof of "edit a function in a live running game, state intact."

- [ ] **Step 2: integration** — expose a minimal stable API in live.h for the future IDE (already mostly there: live_new/reload/soft_reload/gb/free + PatchReport). No UI here (that's later milestones); milestone 4 is the engine.

- [ ] **Step 3: Makefile/CI/README** — live tests already picked up by the wildcard (link GB_OBJ+ASM_OBJ+LIVE_OBJ). Add a `LIVE_OBJ` set. CI: live tests run in `make test`. README: mark Milestone 4 done with a sentence on live patching being functional (headless engine; editor UI in a later milestone).

- [ ] **Step 4: full verification** — `make clean && make test && make blargg && make acid2 && make sound && make gbasm && make live-gameboy`. All green. Commit.

---

## Self-review notes
- **Thesis delivered, testably:** the engine is fully headless-testable — boot a program, edit source, reload, assert state preservation + new behavior — so the milestone gate proves "edit a live running game, state intact" without needing the editor UI (which comes in Milestone 6).
- **Stable layout is the key simplification:** slot-based placement memory means the common edit (function fits its slot) keeps every address fixed and needs only a byte overwrite — no relocation, no call-site rebinding. Relocation (trampoline + zombie + rebind) handles only the outgrow case, exactly per spec §5.
- **Safety is structural:** old function bodies are never overwritten on relocation (zombies), so in-flight calls and stale stack returns stay valid; patches compute a full plan and commit atomically (scratch-first) so a REFUSED reload never corrupts the running image; everything is bank-aware and refuses cross-bank relocation.
- **Builds on M3 cleanly:** layout + reference-sites extend libasm's build database; the live session is a thin new layer over libgb + libasm. Regression gates (blargg/acid2/sound) re-run each task since layout changes the assembler's output addressing.
- **Deferred:** cycle-accurate safe-point timing nuance and editor UI integration (Milestone 6). Data/variable-layout migration beyond refusal is out of scope (refuse + soft reload is the safe answer).
```
