# Milestone 3: In-process SM83 Assembler + Build Database Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** An in-process SM83 assembler (`libasm`) with RGBDS-inspired syntax that assembles real Game Boy assembly into a valid, runnable `.gb` ROM, and emits a **build database** (symbol table with sizes + reference sites, line↔address map, per-byte provenance). Acceptance: (a) the encoder produces externally-correct opcode bytes for the full SM83 instruction set (unit tests assert known hex), (b) assembled test programs run on our own emulator and produce the expected behavior + framebuffer, (c) generated ROMs have a valid header (logo + checksums) and pass our emulator's load.

**Architecture:** `libasm` is a pure C library in `src/asm/`, SDL-free and headless-testable, depending only on `libgb` types where useful (it does not depend on the emulator runtime). Pipeline: **lex** (source → tokens) → **parse** (tokens → a flat statement IR: labels, instructions, directives) → **two-pass assemble** (pass 1: assign addresses, build symbol table + section layout; pass 2: encode bytes, resolve references) → **emit** (ROM image + cartridge header + build database). The build database is the deliverable that Milestone 4 (live patching) and Milestone 5 (live tiles) consume; it keys everything by **linear ROM offset** and records symbol `(bank, cpu_addr, size)`, every static reference site, a bidirectional line↔address map, and per-byte provenance (which source span or INCBIN'd asset byte produced each ROM byte).

**Tech Stack:** C11, no new deps. Existing Makefile/test harness. A new `gbasm` CLI for standalone assembly.

**Spec:** design §4 (assembler), §9. This is Milestone 3.

**Depends on:** Milestones 1–2 (merged): the emulator runs ROMs, so the end-to-end gate can assemble→run→assert.

---

## Syntax (RGBDS-inspired subset)

```
; comment to end of line
SECTION "name", ROM0[, BANK[n]]        ; ROM0 (bank 0) or ROMX (banked)
Label:                                  ; global label
.local:                                 ; local label (scoped to last global)
NAME EQU $1234                          ; or: DEF NAME EQU $1234
    ld a, $42
    ld hl, Label
    jr .local
    db $01, $02, "txt"                  ; bytes
    dw $1234, Label                     ; little-endian words
    ds 16                               ; 16 zero bytes; ds 16, $FF fills
    incbin "assets/tiles.2bpp"          ; raw file include
    include "other.asm"
```

Numbers: `$hex`, `%binary`, decimal, `'c'` char. Expressions: `+ - * / & | << >>` and parentheses, label arithmetic (e.g. `Label + 4`), `LOW()`/`HIGH()`. Case-insensitive mnemonics and registers; labels case-sensitive.

## SM83 encoding reference

The encoder is the inverse of `src/gb/cpu.c`'s decoder. Use the x/y/z structure: registers r = {b,c,d,e,h,l,(hl),a} → 0..7; rp = {bc,de,hl,sp}; rp2 = {bc,de,hl,af}; cc = {nz,z,nc,c}. Examples (externally verifiable from any SM83 opcode table / Pan Docs):
```
nop            -> 00
ld b, c        -> 41          (0x40 | b<<3 | c)
ld a, $42      -> 3E 42
ld (hl), $12   -> 36 12
ld hl, $1234   -> 21 34 12
ld a, (hl)     -> 7E
add a, b       -> 80
add a, $10     -> C6 10
inc bc         -> 03
jr $+2 (e=0)   -> 18 00
jr nz, target  -> 20 xx       (xx = target - (addr+2))
jp $1234       -> C3 34 12
call $1234     -> CD 34 12
ret            -> C9
rst $28        -> EF
push bc        -> C5
ldh (a8), a    -> E0 a8       ; ld ($FF00+a8),a
ld (c), a      -> E2
ld ($1234), a  -> EA 34 12
bit 7, h       -> CB 7C
swap a         -> CB 37
```
CB-prefixed: `CB (x<<6 | y<<3 | z)` where x: 0=rot/shift family (y selects rlc/rrc/rl/rr/sla/sra/swap/srl), 1=bit, 2=res, 3=set.

---

## File structure
```
src/asm/asm.h        — public API + token/symbol/section/builddb/diagnostic types
src/asm/lexer.c      — source -> token stream
src/asm/parser.c     — tokens -> statement IR
src/asm/encode.c     — mnemonic+operands -> opcode bytes (the big table)
src/asm/expr.c       — expression parsing + evaluation (symbols, arithmetic)
src/asm/assemble.c   — two-pass driver, sections, symbols, build db, ROM + header emit
src/asm/gbasm.c      — CLI: gbasm in.asm -o out.gb [--sym out.sym]
tests/test_lexer.c
tests/test_encode.c          — the comprehensive encoding gate
tests/test_asm_directives.c
tests/test_asm_e2e.c         — assemble -> run on emulator -> assert
```

Keep each file focused; `encode.c` will be the largest (the opcode table) — that's expected.

---

### Task 1: Lexer + types

**Files:** create `src/asm/asm.h`, `src/asm/lexer.c`, `tests/test_lexer.c`; add Makefile asm objects + `gbasm` target (stub).

- [ ] **Step 1: asm.h core types**

Define: `AsmTokenKind` (TOK_IDENT, TOK_NUMBER, TOK_STRING, TOK_CHAR, TOK_PUNCT, TOK_NEWLINE, TOK_EOF), `AsmToken { kind; const char *text; size_t len; long value; int line; int col; }`, a lexer that produces a dynamic array of tokens, and a diagnostics list `AsmDiag { int line; int col; char msg[160]; }`. Provide:
```c
typedef struct AsmLexer AsmLexer;
AsmToken *asm_lex(const char *src, const char *filename, int *out_count,
                  AsmDiag **diags, int *ndiags);   /* caller frees */
```
Numbers: `$`/`0x` hex, `%` binary, decimal; `'c'` char literal → TOK_CHAR with value. Strings `"..."` → TOK_STRING. Comments `;` to EOL discarded. Newlines significant (TOK_NEWLINE) since statements are line-based. Punctuation single/multi-char: `, : ( ) + - * / & | ~ << >>` and `.` (for local labels).

- [ ] **Step 2: tests/test_lexer.c** — assert token kinds/values for a snippet covering hex/dec/bin/char/string, identifiers, a label `Foo:`, a local `.bar`, punctuation incl `<<`, and comment stripping. Assert line numbers track newlines.

- [ ] **Step 3: implement lexer; build.**

- [ ] **Step 4: Makefile** — add an `ASM_OBJ` set compiled from `src/asm/*.c`, and a `gbasm` target linking asm objects (gbasm.c is a stub printing usage for now). Core test binaries do NOT link asm. The new test_*.c for asm link `$(ASM_OBJ)`.

- [ ] **Step 5: run `make test` (lexer passes); commit** (message + Co-Authored-By trailer; this convention applies to every task below).

---

### Task 2: Expression evaluator + parser (statement IR)

**Files:** create `src/asm/expr.c`, `src/asm/parser.c`; extend `asm.h`; create `tests/test_asm_directives.c` (parser portion first).

- [ ] **Step 1: asm.h — statement IR + symbol table types**

```c
typedef enum { ST_LABEL, ST_INSTR, ST_DIRECTIVE } AsmStmtKind;
typedef struct { /* parsed operand: register name, immediate expr, (mem), condition, etc. */ } AsmOperand;
typedef struct {
    AsmStmtKind kind;
    int line;
    /* ST_LABEL: name (+is_local); ST_INSTR: mnemonic + up to 2 operands (raw token spans
       for pass-2 expr eval); ST_DIRECTIVE: which directive + args */
    ...
} AsmStmt;
typedef struct { char name[64]; int bank; uint16_t addr; uint32_t off; int size;
                 bool defined; } AsmSymbol;
```

(The implementer designs the exact operand representation; keep operand expressions as token spans evaluated in pass 2 so forward references work.)

- [ ] **Step 2: expr.c** — `bool asm_eval_expr(const AsmToken *toks, int n, const AsmSymbolTable *syms, int cur_bank, long *out, AsmDiag *err);` supporting numbers, symbols, `+ - * / & | << >> ~`, parens, `LOW()`/`HIGH()`, and the `$` current-address symbol (pass current addr in). Unit-test expression eval (e.g. `(2+3)*4` → 20, `HIGH($1234)` → 0x12, `Label+4` with a symbol).

- [ ] **Step 3: parser.c** — `AsmStmt *asm_parse(const AsmToken*, int count, int *out_n, AsmDiag**, int*);` Recognize labels (`ident:` and `.ident:`), instructions (mnemonic + comma-separated operands), and directives (`SECTION`, `EQU`/`DEF`, `DB`/`DW`/`DS`, `INCBIN`, `INCLUDE`). Operands captured as token spans + a classification (register / condition / `(...)` memory / immediate-expr).

- [ ] **Step 4: parser tests** in test_asm_directives.c: parse a multi-line program, assert the statement kinds/mnemonics/operand classifications and that `SECTION`/`DB`/`EQU` parse with correct args.

- [ ] **Step 5: build, `make test` pass, commit.**

---

### Task 3: Encoder — full SM83 instruction set (the gate-critical correctness task)

**Files:** create `src/asm/encode.c`; create `tests/test_encode.c`.

- [ ] **Step 1: tests/test_encode.c — comprehensive externally-verified encoding tests**

Write a table `{ const char *src; uint8_t bytes[3]; int n; }` and assert `asm_encode` produces exactly those bytes. COVER THE WHOLE INSTRUCTION SET — at minimum one case per opcode family and every irregular opcode, with bytes taken from a standard SM83 opcode table (NOT from our CPU, to avoid mirroring a decode bug). Include: all `ld` forms (r,r / r,d8 / r,(hl) / (hl),r / (hl),d8 / a,(bc/de/hl+/hl-) / (bc/de/hl+/hl-),a / a,(a16) / (a16),a / ldh / (c) / hl,sp+e8 / sp,hl / (a16),sp / rr,d16); all ALU (add/adc/sub/sbc/and/xor/or/cp, r and d8); inc/dec r and rr; add hl,rr; add sp,e8; all rotates (rlca/rrca/rla/rra) and the CB family (rlc/rrc/rl/rr/sla/sra/swap/srl × 8 regs, bit/res/set × 8 bits × 8 regs — at least a representative sweep + all 8 ops on one reg + bit 0..7); jp/jp cc/jp hl/jr/jr cc/call/call cc/ret/ret cc/reti/rst (all 8); push/pop (bc/de/hl/af); di/ei/halt/stop/nop/scf/ccf/cpl/daa. Also test that `jr` to a label computes the right signed displacement and that an out-of-range jr errors.

- [ ] **Step 2: run, fail.**

- [ ] **Step 3: encode.c** — `int asm_encode(const char *mnemonic, const AsmOperand *ops, int nops, uint16_t cur_addr, const AsmSymbolTable *syms, uint8_t *out, AsmDiag *err);` returns byte count or -1. Implement using the x/y/z structure for regular families and a switch for irregular opcodes (mirror cpu.c's exec inversely). Resolve immediate/label operands via expr eval; `jr` computes `target - (cur_addr + 2)` and range-checks to [-128,127].

- [ ] **Step 4: run until the full table passes.** This is the milestone's correctness backbone — debug any mismatch against a real opcode table; do not weaken. Commit.

---

### Task 4: Two-pass assembler — sections, symbols, DB/DW/DS, label resolution, build database

**Files:** create `src/asm/assemble.c`; extend `asm.h` (build database types); create `tests/test_asm_e2e.c` (single-section programs first).

- [ ] **Step 1: asm.h — build database**

```c
typedef struct {
    uint8_t *rom; size_t rom_size;
    AsmSymbol *syms; int nsyms;
    /* line<->address: parallel arrays */
    struct { int line; uint32_t off; } *linemap; int nlines;
    /* per-byte provenance: for each rom offset, the source line that produced it
       (or -1), and for INCBIN bytes, an asset id + asset offset */
    int32_t *prov_line;        /* rom_size entries */
    AsmDiag *diags; int ndiags;
    bool ok;
} AsmResult;

AsmResult asm_assemble(const char *src, const char *filename);
void asm_free(AsmResult *r);
```

- [ ] **Step 2: tests/test_asm_e2e.c — first cases (single ROM0 section)**

Assemble a small program into ROM0, assert: (a) `result.ok`; (b) specific bytes at specific offsets; (c) a label's symbol address; (d) the line↔address map maps a known line to the right offset; (e) a forward `jr`/`jp` to a later label resolves correctly. Example program: a loop that writes a value to WRAM and halts.

- [ ] **Step 3: assemble.c — two passes**

Pass 1: walk statements; track current section (bank + cur addr); assign label addresses; size each instruction (via a length-only encode) and DB/DW/DS; record symbol sizes (next-label distance or explicit). Handle EQU (constant symbols). Pass 2: encode bytes into the ROM image at each statement's offset; resolve labels; fill DB/DW/DS; build linemap + provenance. Default a single ROM0 section at 0x0000 if none declared (but reserve 0x0100-0x014F for the header — see Task 5; for now place code at 0x0150 if no section given, or honor SECTION).

- [ ] **Step 4: run, pass, commit.**

---

### Task 5: Cartridge header, multi-bank sections, INCBIN/INCLUDE, provenance

**Files:** modify `assemble.c`; extend tests in `test_asm_e2e.c`.

- [ ] **Step 1: tests** — (a) assemble a program with `SECTION "x", ROMX, BANK[2]` and assert the bytes land at linear offset 0x8000 and the symbol records bank 2; (b) `incbin "<tmpfile>"` includes the raw bytes and provenance for those offsets points at the asset (assert prov marks them as asset bytes, not a source line); (c) generated ROM has a valid header: Nintendo logo bytes at 0x104-0x133, header checksum at 0x14D correct, global checksum at 0x14E-0x14F correct; (d) the emulator accepts it (gb_load_rom + run a few frames without crashing).

- [ ] **Step 2: implement** — write the 48-byte Nintendo logo at 0x104; title at 0x134; cartridge-type/ROM-size/RAM-size bytes; compute header checksum (0x134..0x14C: `x = x - byte - 1`) at 0x14D and the 16-bit global checksum (sum of all bytes except 0x14E-0x14F) at 0x14E-0x14F. Multi-bank: linear offset = bank*0x4000 + (addr-0x4000) for ROMX; grow rom_size to cover the highest bank (round to a valid ROM size). INCBIN reads the file and marks provenance as asset bytes; INCLUDE lexes+parses the included file inline (track filename per line for diagnostics).

- [ ] **Step 3: run, pass, commit.**

---

### Task 6: End-to-end gate (assemble → run → assert) + gbasm CLI + CI/README

**Files:** finish `src/asm/gbasm.c`; extend `tests/test_asm_e2e.c`; modify Makefile, CI, README.

- [ ] **Step 1: e2e gate test** — assemble a real, non-trivial program from source text in the test, load it into a `GB`, run N frames, and assert observable behavior through the emulator. Two concrete programs:
  1. **Serial "Passed"**: a program that writes specific bytes out the serial port (like blargg) — assert `g->serial_buf` contains the expected string. (Tests CPU-visible execution of assembled code.)
  2. **Draw a tile**: a program that sets up the palette, writes a tile + tilemap entry, enables the LCD, and loops; run enough frames, then assert `gb_framebuffer` has the expected shade at a known pixel. (Tests a realistic graphics program end-to-end.)
  These prove the assembler output actually runs correctly on real hardware semantics.

- [ ] **Step 2: gbasm CLI** — `gbasm in.asm -o out.gb [--sym out.sym]`: assemble, write the ROM, optionally write a `.sym` file (`BB:AAAA Name` lines) from the symbol table; print diagnostics with file:line on error and exit nonzero.

- [ ] **Step 3: Makefile** — `gbasm` target (real), and an `asm-test` convenience target. Add asm tests to the default `make test` (they're picked up by the `tests/test_*.c` wildcard automatically — ensure they link `$(ASM_OBJ)`; if the wildcard rule can't distinguish, give asm tests their own rule or link ASM_OBJ into all test binaries — simplest: link both GB_OBJ and ASM_OBJ into every test binary, since they don't conflict).

- [ ] **Step 4: CI/README** — add an "assembler" CI step running the asm tests (already in `make test`). README: add Milestone 3 as done with a one-line on `gbasm`, and a `## Assemble` usage section.

- [ ] **Step 5: full verification** — `make clean && make test && make blargg && make acid2 && make sound && make gbasm && ./gbasm <a sample>` ; confirm everything green. Commit.

---

## Self-review notes
- **Spec coverage:** implements design §4 (RGBDS-inspired assembler, two-pass, build database with symbols/line-map/provenance, header generation, multi-bank sections, INCBIN/INCLUDE). The layout manager's *placement memory + padded slots* (also §4) is introduced in Milestone 4 (live patching), which is its first consumer; Milestone 3 provides the symbol sizes + reference info that layout needs.
- **Anti-mirror-bug measure:** the encoder gate (Task 3) asserts opcode bytes from an external SM83 table, not by round-tripping through our own CPU decoder, so an encode bug can't be masked by a matching decode bug. The e2e gate (Task 6) then runs assembled code on the emulator for realistic validation.
- **Core stays clean:** libasm is separate from libgb and SDL; it's headless and unit-tested. gbasm is a standalone CLI.
- **Build-db is offset-keyed** per the spec's banking requirement, ready for Milestone 4's patch engine and Milestone 5's tile provenance.
- **Regression:** the gate set (blargg/acid2/sound) is re-run at the end; the assembler doesn't touch the emulator core.
```
