#ifndef ASM_H
#define ASM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * Diagnostic
 * --------------------------------------------------------------------- */

typedef struct {
    const char *filename;
    int         line;
    int         col;
    char        msg[160];
} AsmDiag;

/* -----------------------------------------------------------------------
 * Token kinds
 * --------------------------------------------------------------------- */

typedef enum {
    TOK_IDENT,    /* identifier, mnemonic, register name, directive */
    TOK_NUMBER,   /* $hex, 0xhex, %bin, decimal integer             */
    TOK_STRING,   /* "..." — text/len point at contents (no quotes) */
    TOK_CHAR,     /* 'c'  — single character, value holds codepoint */
    TOK_PUNCT,    /* , : ( ) + - * / & | ~ << >>                    */
    TOK_NEWLINE,  /* physical newline; significant for stmt parsing  */
    TOK_EOF       /* sentinel                                        */
} AsmTokenKind;

typedef struct {
    AsmTokenKind kind;
    const char  *text;   /* pointer into original source (NOT NUL-terminated) */
    size_t       len;    /* byte length of lexeme (for strings: excludes quotes) */
    long         value;  /* numeric value for TOK_NUMBER / TOK_CHAR; 0 otherwise */
    int          line;   /* 1-based line where token starts */
    int          col;    /* 1-based column where token starts */
} AsmToken;

/* -----------------------------------------------------------------------
 * Lexer API
 *
 * asm_lex() tokenises `src` (NUL-terminated source text) and returns a
 * malloc'd array of tokens terminated by a TOK_EOF entry.  The caller is
 * responsible for free()ing the returned array.
 *
 * *out_count receives the number of tokens INCLUDING the TOK_EOF sentinel.
 *
 * Lexical errors are appended to *diags (realloc'd); *ndiags is updated.
 * On entry *diags may be NULL and *ndiags may be 0; the function will
 * malloc/realloc as needed.  The caller frees *diags.
 *
 * Returns NULL only on an allocation failure.
 * --------------------------------------------------------------------- */

AsmToken *asm_lex(const char *src,
                  const char *filename,
                  int        *out_count,
                  AsmDiag   **diags,
                  int        *ndiags);

/* -----------------------------------------------------------------------
 * Symbol table
 *
 * AsmSymbol holds a resolved name->value mapping.  The symbol table is a
 * simple flat array used by the expression evaluator.
 * --------------------------------------------------------------------- */

typedef struct {
    char     name[128];  /* symbol name (NUL-terminated, case-sensitive) */
    int      bank;       /* ROM bank (0 for ROM0, -1 for constants)      */
    uint16_t addr;       /* CPU address                                   */
    uint32_t off;        /* linear ROM offset                             */
    long     value;      /* for EQU constants (also set addr/off = 0)     */
    int      size;       /* byte size (0 if unknown)                      */
    bool     defined;    /* false = forward-reference placeholder         */
} AsmSymbol;

typedef struct {
    AsmSymbol *syms;
    int        count;
    int        cap;
} AsmSymbolTable;

/* Look up a symbol by name; returns NULL if not found. */
const AsmSymbol *asm_sym_lookup(const AsmSymbolTable *st, const char *name);

/* -----------------------------------------------------------------------
 * Expression evaluator
 *
 * Evaluates an expression given as a sub-range [start, start+count) of the
 * token array `toks`.  Resolves symbol names via `syms` (may be NULL for
 * symbol-free expressions).  `cur_addr` is the value of the special `$`/@
 * token.
 *
 * Returns true and stores the result in *out on success.
 * Returns false and fills *err on failure (undefined symbol, div by zero,
 * syntax error).  err may be NULL if the caller does not need details.
 * --------------------------------------------------------------------- */
bool asm_eval_expr(const AsmToken     *toks,
                   int                 start,
                   int                 count,
                   const AsmSymbolTable *syms,
                   long                cur_addr,
                   long               *out,
                   AsmDiag            *err);

/* -----------------------------------------------------------------------
 * Statement IR
 *
 * The parser produces a flat array of AsmStmt, one per logical line.
 * Expressions are captured as token spans (start index + count) into the
 * original token array so they can be re-evaluated in pass 2 after all
 * labels are known (enabling forward references).
 * --------------------------------------------------------------------- */

/* ---- Operand classification ----------------------------------------- */

/*
 * OperandForm describes the syntactic shape of an instruction operand.
 *
 * OPND_REG      – a bare identifier that names a register or condition
 *                 (a,b,c,d,e,h,l,af,bc,de,hl,sp,nz,z,nc,c).  The encoder
 *                 disambiguates `c` between register-C and condition-C.
 *
 * OPND_MEM      – a parenthesised expression: (expr).  tok_start/tok_count
 *                 span the OUTER parentheses (inclusive), so the encoder can
 *                 inspect the inner tokens to distinguish (hl), (bc), (de),
 *                 (hl+), (hl-), (c), and (immediate).
 *
 * OPND_IMM      – a bare expression (number, symbol, arithmetic, LOW/HIGH).
 *                 tok_start/tok_count span the expression tokens only.
 *
 * OPND_SP_DISP  – sp+e8 (used by `add sp,e8` and `ld hl,sp+e8`).
 *                 tok_start/tok_count cover the whole "sp+expr" span.
 */
typedef enum {
    OPND_REG,       /* register or condition name (case-insensitive ident) */
    OPND_MEM,       /* (expr) — parenthesised indirect                     */
    OPND_IMM,       /* bare immediate / expression                          */
    OPND_SP_DISP    /* sp+e8 signed displacement form                       */
} OperandForm;

typedef struct {
    OperandForm form;
    int         tok_start;   /* index into the original token array */
    int         tok_count;   /* number of tokens in this operand    */
} AsmOperand;

/* ---- Directive kinds ------------------------------------------------- */

typedef enum {
    DIR_SECTION,   /* SECTION "name", type[, BANK[n]]  */
    DIR_EQU,       /* NAME EQU expr  or  DEF NAME EQU expr */
    DIR_DB,        /* DB expr, expr, "str", ...         */
    DIR_DW,        /* DW expr, expr, ...                */
    DIR_DS,        /* DS count[, fill]                  */
    DIR_INCBIN,    /* INCBIN "file"                     */
    DIR_INCLUDE    /* INCLUDE "file"                    */
} AsmDirectiveKind;

/* ---- Statement kinds ------------------------------------------------- */

typedef enum {
    ST_LABEL,      /* label definition                 */
    ST_INSTR,      /* instruction: mnemonic + operands */
    ST_DIRECTIVE   /* assembler directive              */
} AsmStmtKind;

/*
 * A single parsed statement.
 *
 * ST_LABEL:
 *   label.name     – NUL-terminated label text (without the colon)
 *   label.is_local – true if the name starts with '.'
 *
 * ST_INSTR:
 *   instr.mnemonic – NUL-terminated mnemonic text (up to 15 chars)
 *   instr.ops      – operand array (0..4 operands)
 *   instr.nops     – operand count
 *
 * ST_DIRECTIVE:
 *   dir.kind       – which directive
 *   dir.name_start / dir.name_count  – for EQU: the symbol name token span
 *   dir.args_start / dir.args_count  – token span of the directive arguments
 *                                       (everything after the directive keyword,
 *                                        up to but not including NEWLINE/EOF).
 *                                       For EQU this is the expression after EQU.
 */
typedef struct {
    AsmStmtKind kind;
    int         line;          /* source line number of statement start */

    union {
        /* ST_LABEL */
        struct {
            char name[128];    /* label name, without leading '.' for locals */
            bool is_local;
        } label;

        /* ST_INSTR */
        struct {
            char       mnemonic[16];
            AsmOperand ops[4];
            int        nops;
        } instr;

        /* ST_DIRECTIVE */
        struct {
            AsmDirectiveKind kind;
            /* EQU: symbol name token */
            int name_start;    /* index of name token (EQU only) */
            int name_count;    /* always 1 for EQU               */
            /* all directives: argument token span */
            int args_start;    /* index of first arg token       */
            int args_count;    /* number of arg tokens           */
        } dir;
    };
} AsmStmt;

/* -----------------------------------------------------------------------
 * Parser API
 *
 * asm_parse() converts a token array (from asm_lex) into a flat array of
 * statements.  `toks` must include the TOK_EOF sentinel.  `count` is the
 * total number of tokens including TOK_EOF.
 *
 * Returns a malloc'd array of *out_n statements on success (may be empty).
 * Parse errors are appended to *diags / *ndiags (same contract as asm_lex).
 * Individual bad lines are skipped; the function always returns an array
 * (may be NULL only on allocation failure).
 *
 * The returned statements contain token-span indices that are valid only
 * as long as the original `toks` array is alive.
 * --------------------------------------------------------------------- */
AsmStmt *asm_parse(const AsmToken *toks,
                   int             count,
                   int            *out_n,
                   AsmDiag       **diags,
                   int            *ndiags);

/* -----------------------------------------------------------------------
 * Encoder API
 *
 * asm_encode() assembles one instruction into `out` (caller provides at
 * least 3 bytes).  Returns the byte count (1–3) or -1 on error.
 *
 * mnemonic  — NUL-terminated mnemonic string (case-insensitive)
 * toks      — the full token array (for expression evaluation)
 * ops       — operand array (0..4 entries)
 * nops      — operand count
 * cur_addr  — current CPU address (value of the '$' symbol)
 * syms      — symbol table for expression evaluation (may be NULL)
 * out       — output buffer (at least 3 bytes)
 * err       — diagnostic filled on error (may be NULL)
 * --------------------------------------------------------------------- */
int asm_encode(const char          *mnemonic,
               const AsmToken      *toks,
               const AsmOperand    *ops,
               int                  nops,
               uint16_t             cur_addr,
               const AsmSymbolTable *syms,
               uint8_t             *out,
               AsmDiag             *err);

/* -----------------------------------------------------------------------
 * Layout types — function-aware placement memory (Task 1)
 * --------------------------------------------------------------------- */

/*
 * AsmPlacement records where a single function (global-label-delimited
 * region) was placed in the ROM.
 *
 *   name      — the global label name (NUL-terminated)
 *   bank      — ROM bank (0 for ROM0, >=1 for ROMX)
 *   addr      — CPU address of the function's first byte
 *   slot_size — total bytes reserved for this function (including padding):
 *               round_up(function_bytes, 16) + 16
 */
typedef struct {
    char     name[64];
    int      bank;
    uint16_t addr;
    int      slot_size;
} AsmPlacement;

/*
 * AsmPlacementMem is a growable array of AsmPlacement records retained
 * across builds.  Pass it to asm_assemble_mem() to get stable layout
 * across reloads.  NULL or empty = fresh (first-time) layout.
 */
typedef struct {
    AsmPlacement *items;
    int           count;
    int           cap;
} AsmPlacementMem;

/* -----------------------------------------------------------------------
 * Build database (output of two-pass assembly)
 * --------------------------------------------------------------------- */

/*
 * AsmRefSite records a 2-byte absolute address operand in the ROM that
 * references exactly one symbol.  Used by the live-patching engine to rebind
 * call/jump/data sites when a function relocates.
 *
 * Fields:
 *   off    — linear ROM offset of the first (low) byte of the 16-bit address
 *   sym    — name of the referenced symbol (NUL-terminated)
 *   addend — constant offset added to the symbol (e.g. 2 for Label+2)
 *   size   — always 2 (the number of bytes encoding the address)
 */
typedef struct {
    uint32_t off;
    char     sym[64];
    long     addend;
    int      size;   /* always 2 */
} AsmRefSite;

/*
 * AsmAsset records a binary asset file that was included via INCBIN.
 *
 * Fields:
 *   path  — resolved absolute (or relative) path of the asset file
 *   bytes — heap-allocated copy of the file's bytes
 *   size  — byte count of the asset
 */
typedef struct {
    char     path[256];
    uint8_t *bytes;
    size_t   size;
} AsmAsset;

/*
 * AsmResult is returned by asm_assemble().  All pointer fields are
 * heap-allocated; call asm_free() when done.
 *
 * Fields:
 *   rom         — the ROM image (rom_size bytes, zero-padded to >= 0x8000)
 *   rom_size    — byte count of rom (always >= 0x8000)
 *   syms        — resolved symbol table (nsyms entries)
 *   linemap     — parallel arrays: linemap[i].line -> linemap[i].off
 *                 One entry per emitting statement (instructions, DB/DW/DS).
 *                 Sorted by line number.
 *   nlines      — number of linemap entries
 *   prov_line   — per-byte source line: prov_line[off] = 1-based source line
 *                 that produced byte at linear ROM offset `off`, or -1.
 *                 INCBIN bytes are marked -2 (PROV_ASSET sentinel).
 *   prov_asset  — per-byte asset index: prov_asset[off] = index into assets[]
 *                 for INCBIN bytes, or -1 for non-asset bytes. (rom_size entries)
 *   prov_asset_off — per-byte offset within the asset: prov_asset_off[off] = byte
 *                 offset within assets[prov_asset[off]].bytes. (rom_size entries)
 *   assets      — table of INCBIN'd asset files (nassets entries)
 *   nassets     — number of distinct assets
 *   diags       — diagnostics list (ndiags entries)
 *   ok          — false if any error occurred; ROM content is partial.
 *   placements  — function placements used in this build (nplacements entries)
 *   nplacements — number of placements
 *   refs        — absolute address reference sites (nrefs entries)
 *   nrefs       — number of reference sites
 */
typedef struct {
    uint8_t  *rom;
    size_t    rom_size;

    AsmSymbol *syms;
    int        nsyms;

    struct { int line; uint32_t off; } *linemap;
    int nlines;

    int32_t  *prov_line;      /* rom_size entries; -2 = INCBIN byte */
    int32_t  *prov_asset;     /* rom_size entries; asset index or -1 */
    uint32_t *prov_asset_off; /* rom_size entries; byte offset within asset */

    AsmAsset *assets;  /* INCBIN'd asset files */
    int       nassets;

    AsmDiag  *diags;
    int       ndiags;

    bool ok;

    AsmPlacement *placements;  /* function placements (NULL if no layout was run) */
    int           nplacements;

    AsmRefSite   *refs;    /* absolute address reference sites */
    int           nrefs;
} AsmResult;

/*
 * layout_plan() — assign slot addresses to all global-label-delimited
 * functions in the symbol table.
 *
 * Called internally by asm_assemble_mem() between pass 1 and pass 2.
 * Exported so tests can exercise it directly.
 *
 * `syms` / `nsyms` — the symbol table (updated in-place with new addresses).
 * `sec_base`       — CPU address of the section start (DEFAULT_ORG = 0x0150
 *                    for the default ROM0 section; 0x4000 for ROMX).
 * `sec_bank`       — bank index of the section (0 for ROM0).
 * `inout_mem`      — retained placement memory (NULL = fresh).
 *
 * Returns a heap-allocated array of AsmPlacement (*out_count entries), or
 * NULL on allocation failure or if there are no functions.
 * The caller (asm_assemble_mem) owns the returned array.
 */
AsmPlacement *layout_plan(AsmSymbol *syms, int nsyms,
                           uint16_t sec_base, int sec_bank,
                           AsmPlacementMem *inout_mem,
                           int *out_count);

/*
 * Assemble `src` (NUL-terminated RGBDS-inspired SM83 assembly source).
 * `filename` is used only in diagnostic messages (may be NULL).
 * `inout_mem` is the retained placement memory from a prior build (may be
 * NULL for a fresh/first build).  On return, *inout_mem is updated with
 * this build's placements.  asm_assemble() is equivalent to calling this
 * with inout_mem = NULL.
 *
 * Always returns a fully initialised AsmResult; check result.ok.
 * Call asm_free() to release all heap memory when done.
 */
AsmResult asm_assemble_mem(const char *src, const char *filename,
                            AsmPlacementMem *inout_mem);

/*
 * Assemble `src` with no retained placement memory (fresh layout).
 * Equivalent to asm_assemble_mem(src, filename, NULL).
 */
AsmResult asm_assemble(const char *src, const char *filename);

/* Release all heap memory owned by an AsmResult. */
void asm_free(AsmResult *r);

#endif /* ASM_H */
