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

#endif /* ASM_H */
