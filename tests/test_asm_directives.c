/*
 * test_asm_directives.c — tests for expr.c (expression evaluator) and
 * parser.c (statement IR parser).
 */

#include "test.h"
#include "../src/asm/asm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

/* Build a small symbol table on the stack. */
static AsmSymbolTable make_symtab(AsmSymbol *syms, int n)
{
    AsmSymbolTable st;
    st.syms  = syms;
    st.count = n;
    st.cap   = n;
    return st;
}

/* Lex src, then parse; returns tokens (*ntoks set) and statements (*nstmts). */
static AsmStmt *lex_and_parse(const char *src,
                               AsmToken **toks_out, int *ntoks,
                               int *nstmts)
{
    AsmDiag *ldiags = NULL; int nldiags = 0;
    AsmToken *toks = asm_lex(src, "test.asm", ntoks, &ldiags, &nldiags);
    free(ldiags);
    if (!toks) { *nstmts = 0; return NULL; }
    *toks_out = toks;

    AsmDiag *pdiags = NULL; int npdiags = 0;
    AsmStmt *stmts = asm_parse(toks, *ntoks, nstmts, &pdiags, &npdiags);
    free(pdiags);
    return stmts;
}

/* Quick eval helper: lex expr_src, evaluate, return value (-999999 on fail). */
static long eval(const char *expr_src, const AsmSymbolTable *syms, long cur_addr)
{
    int n = 0;
    AsmDiag *d = NULL; int nd = 0;
    AsmToken *toks = asm_lex(expr_src, "expr", &n, &d, &nd);
    free(d);
    if (!toks) return -999999;

    /* tokens: all except final EOF */
    int count = n - 1; /* exclude EOF */
    long out = -999999;
    AsmDiag err; memset(&err, 0, sizeof(err));
    bool ok = asm_eval_expr(toks, 0, count, syms, cur_addr, &out, &err);
    free(toks);
    return ok ? out : -999999;
}

/* -----------------------------------------------------------------------
 * Expression evaluator tests
 * --------------------------------------------------------------------- */

static void test_expr_basic_arith(void)
{
    /* (2+3)*4 == 20 */
    long v = eval("(2+3)*4", NULL, 0);
    ASSERT_EQ(v, 20);
}

static void test_expr_high(void)
{
    /* HIGH($1234) == 0x12 */
    long v = eval("HIGH($1234)", NULL, 0);
    ASSERT_EQ(v, 0x12);
}

static void test_expr_low(void)
{
    /* LOW($1234) == 0x34 */
    long v = eval("LOW($1234)", NULL, 0);
    ASSERT_EQ(v, 0x34);
}

static void test_expr_shift(void)
{
    /* 1<<4 == 16 */
    long v = eval("1<<4", NULL, 0);
    ASSERT_EQ(v, 16);
}

static void test_expr_symbol_lookup(void)
{
    /* Label+4 with Label=$1000 -> $1004 */
    AsmSymbol sym;
    memset(&sym, 0, sizeof(sym));
    strncpy(sym.name, "Label", sizeof(sym.name)-1);
    sym.value   = 0x1000;
    sym.defined = true;

    AsmSymbolTable st = make_symtab(&sym, 1);
    long v = eval("Label+4", &st, 0);
    ASSERT_EQ(v, 0x1004);
}

static void test_expr_undefined_symbol(void)
{
    /* Undefined symbol -> returns false */
    int n = 0;
    AsmDiag *d = NULL; int nd = 0;
    AsmToken *toks = asm_lex("Undef+1", "expr", &n, &d, &nd);
    free(d);
    ASSERT_TRUE(toks != NULL);

    long out = 0;
    AsmDiag err; memset(&err, 0, sizeof(err));
    bool ok = asm_eval_expr(toks, 0, n - 1, NULL, 0, &out, &err);
    ASSERT_EQ(ok, 0);
    ASSERT_TRUE(err.msg[0] != '\0'); /* diagnostic was filled */
    free(toks);
}

static void test_expr_current_addr(void)
{
    /* $ == 0x150 when cur_addr = 0x150 */
    long v = eval("$", NULL, 0x150);
    ASSERT_EQ(v, 0x150);

    /* $+2 == 0x152 */
    v = eval("$+2", NULL, 0x150);
    ASSERT_EQ(v, 0x152);
}

static void test_expr_bitwise(void)
{
    /* $F0 & $0F == 0 */
    long v = eval("$F0&$0F", NULL, 0);
    ASSERT_EQ(v, 0);

    /* $F0 | $0F == 0xFF */
    v = eval("$F0|$0F", NULL, 0);
    ASSERT_EQ(v, 0xFF);
}

static void test_expr_unary(void)
{
    /* -5 == -5 */
    long v = eval("-5", NULL, 0);
    ASSERT_EQ(v, -5);

    /* ~0 == -1 (all bits) */
    v = eval("~0", NULL, 0);
    ASSERT_EQ(v, ~0L);
}

/* -----------------------------------------------------------------------
 * Parser tests
 * --------------------------------------------------------------------- */

/*
 * Multi-line program exercising:
 *   SECTION, global label, local label, EQU (DEF form), DB (numbers+string),
 *   DW, DS, instruction with 0 ops (nop), 1 op (jp), 2 ops (ld a,b),
 *   ldh, (hl) and (c) memory operands.
 */
static const char *TEST_PROG =
    "SECTION \"ROM0\", ROM0\n"           /* 1: SECTION directive          */
    "Start:\n"                           /* 2: global label               */
    ".local:\n"                          /* 3: local label                */
    "BUFSIZE EQU $0100\n"               /* 4: EQU directive              */
    "    db $01, $02, \"txt\"\n"         /* 5: DB directive               */
    "    dw $1234, Start\n"             /* 6: DW directive               */
    "    ds 16\n"                        /* 7: DS directive               */
    "    nop\n"                          /* 8: instruction 0 ops          */
    "    jp Start\n"                     /* 9: instruction 1 op           */
    "    ld a, b\n"                      /* 10: instruction 2 ops (reg,reg) */
    "    ldh (a8), a\n"                  /* 11: ldh with (mem), reg       */
    "    ld (hl), a\n"                   /* 12: ld (hl), a                */
    "    ld a, (hl)\n"                   /* 13: ld a, (hl)                */
    "    ld (c), a\n"                    /* 14: ld (c), a                 */
    ;

static void test_parser_statement_count(void)
{
    AsmToken *toks = NULL; int ntoks = 0, nstmts = 0;
    AsmStmt *stmts = lex_and_parse(TEST_PROG, &toks, &ntoks, &nstmts);
    ASSERT_TRUE(stmts != NULL);
    /* Expected statements (each line yields 1):
     * SECTION, Start(label), .local(label), BUFSIZE EQU, DB, DW, DS,
     * nop, jp, ld a,b, ldh, ld (hl) a, ld a (hl), ld (c) a
     * = 14 */
    ASSERT_EQ(nstmts, 14);
    free(stmts);
    free(toks);
}

static void test_parser_section(void)
{
    AsmToken *toks = NULL; int ntoks = 0, nstmts = 0;
    AsmStmt *stmts = lex_and_parse(TEST_PROG, &toks, &ntoks, &nstmts);
    ASSERT_TRUE(stmts != NULL && nstmts >= 1);

    /* First statement: SECTION directive */
    ASSERT_EQ(stmts[0].kind, ST_DIRECTIVE);
    ASSERT_EQ(stmts[0].dir.kind, DIR_SECTION);
    /* args_count > 0 */
    ASSERT_TRUE(stmts[0].dir.args_count > 0);
    /* First arg token is a string "ROM0" */
    const AsmToken *first_arg = &toks[stmts[0].dir.args_start];
    ASSERT_EQ(first_arg->kind, TOK_STRING);
    ASSERT_TRUE(first_arg->len == 4 && memcmp(first_arg->text, "ROM0", 4) == 0);

    free(stmts); free(toks);
}

static void test_parser_labels(void)
{
    AsmToken *toks = NULL; int ntoks = 0, nstmts = 0;
    AsmStmt *stmts = lex_and_parse(TEST_PROG, &toks, &ntoks, &nstmts);
    ASSERT_TRUE(stmts != NULL && nstmts >= 3);

    /* Statement 1: global label "Start" */
    ASSERT_EQ(stmts[1].kind, ST_LABEL);
    ASSERT_EQ(stmts[1].label.is_local, 0);
    ASSERT_TRUE(strcmp(stmts[1].label.name, "Start") == 0);

    /* Statement 2: local label ".local" */
    ASSERT_EQ(stmts[2].kind, ST_LABEL);
    ASSERT_EQ(stmts[2].label.is_local, 1);
    ASSERT_TRUE(strcmp(stmts[2].label.name, ".local") == 0);

    free(stmts); free(toks);
}

static void test_parser_equ(void)
{
    AsmToken *toks = NULL; int ntoks = 0, nstmts = 0;
    AsmStmt *stmts = lex_and_parse(TEST_PROG, &toks, &ntoks, &nstmts);
    ASSERT_TRUE(stmts != NULL && nstmts >= 4);

    /* Statement 3: EQU */
    ASSERT_EQ(stmts[3].kind, ST_DIRECTIVE);
    ASSERT_EQ(stmts[3].dir.kind, DIR_EQU);
    /* name token is "BUFSIZE" */
    ASSERT_EQ(stmts[3].dir.name_count, 1);
    const AsmToken *name_tok = &toks[stmts[3].dir.name_start];
    ASSERT_EQ(name_tok->kind, TOK_IDENT);
    ASSERT_TRUE(name_tok->len == 7 && memcmp(name_tok->text, "BUFSIZE", 7) == 0);
    /* args: $0100 */
    ASSERT_TRUE(stmts[3].dir.args_count > 0);
    const AsmToken *val_tok = &toks[stmts[3].dir.args_start];
    ASSERT_EQ(val_tok->kind, TOK_NUMBER);
    ASSERT_EQ(val_tok->value, 0x0100);

    free(stmts); free(toks);
}

static void test_parser_db(void)
{
    AsmToken *toks = NULL; int ntoks = 0, nstmts = 0;
    AsmStmt *stmts = lex_and_parse(TEST_PROG, &toks, &ntoks, &nstmts);
    ASSERT_TRUE(stmts != NULL && nstmts >= 5);

    /* Statement 4: DB */
    ASSERT_EQ(stmts[4].kind, ST_DIRECTIVE);
    ASSERT_EQ(stmts[4].dir.kind, DIR_DB);
    /* args span includes numbers and string */
    ASSERT_TRUE(stmts[4].dir.args_count >= 5); /* $01 , $02 , "txt" */

    free(stmts); free(toks);
}

static void test_parser_dw(void)
{
    AsmToken *toks = NULL; int ntoks = 0, nstmts = 0;
    AsmStmt *stmts = lex_and_parse(TEST_PROG, &toks, &ntoks, &nstmts);
    ASSERT_TRUE(stmts != NULL && nstmts >= 6);

    /* Statement 5: DW */
    ASSERT_EQ(stmts[5].kind, ST_DIRECTIVE);
    ASSERT_EQ(stmts[5].dir.kind, DIR_DW);

    free(stmts); free(toks);
}

static void test_parser_ds(void)
{
    AsmToken *toks = NULL; int ntoks = 0, nstmts = 0;
    AsmStmt *stmts = lex_and_parse(TEST_PROG, &toks, &ntoks, &nstmts);
    ASSERT_TRUE(stmts != NULL && nstmts >= 7);

    /* Statement 6: DS */
    ASSERT_EQ(stmts[6].kind, ST_DIRECTIVE);
    ASSERT_EQ(stmts[6].dir.kind, DIR_DS);
    /* first arg is 16 */
    ASSERT_TRUE(stmts[6].dir.args_count >= 1);
    const AsmToken *a = &toks[stmts[6].dir.args_start];
    ASSERT_EQ(a->kind, TOK_NUMBER);
    ASSERT_EQ(a->value, 16);

    free(stmts); free(toks);
}

static void test_parser_instructions(void)
{
    AsmToken *toks = NULL; int ntoks = 0, nstmts = 0;
    AsmStmt *stmts = lex_and_parse(TEST_PROG, &toks, &ntoks, &nstmts);
    ASSERT_TRUE(stmts != NULL && nstmts >= 14);

    /* Statement 7: nop — 0 operands */
    ASSERT_EQ(stmts[7].kind, ST_INSTR);
    ASSERT_TRUE(strcmp(stmts[7].instr.mnemonic, "nop") == 0);
    ASSERT_EQ(stmts[7].instr.nops, 0);

    /* Statement 8: jp Start — 1 operand (IMM) */
    ASSERT_EQ(stmts[8].kind, ST_INSTR);
    ASSERT_TRUE(strcmp(stmts[8].instr.mnemonic, "jp") == 0);
    ASSERT_EQ(stmts[8].instr.nops, 1);
    ASSERT_EQ(stmts[8].instr.ops[0].form, OPND_IMM);

    /* Statement 9: ld a, b — 2 operands (REG, REG) */
    ASSERT_EQ(stmts[9].kind, ST_INSTR);
    ASSERT_TRUE(strcmp(stmts[9].instr.mnemonic, "ld") == 0);
    ASSERT_EQ(stmts[9].instr.nops, 2);
    ASSERT_EQ(stmts[9].instr.ops[0].form, OPND_REG);
    ASSERT_EQ(stmts[9].instr.ops[1].form, OPND_REG);

    /* Statement 10: ldh (a8), a — 2 operands: first MEM, second REG */
    ASSERT_EQ(stmts[10].kind, ST_INSTR);
    ASSERT_TRUE(strcmp(stmts[10].instr.mnemonic, "ldh") == 0);
    ASSERT_EQ(stmts[10].instr.nops, 2);
    ASSERT_EQ(stmts[10].instr.ops[0].form, OPND_MEM);
    ASSERT_EQ(stmts[10].instr.ops[1].form, OPND_REG);

    /* Statement 11: ld (hl), a — first op is MEM */
    ASSERT_EQ(stmts[11].kind, ST_INSTR);
    ASSERT_TRUE(strcmp(stmts[11].instr.mnemonic, "ld") == 0);
    ASSERT_EQ(stmts[11].instr.nops, 2);
    ASSERT_EQ(stmts[11].instr.ops[0].form, OPND_MEM);
    ASSERT_EQ(stmts[11].instr.ops[1].form, OPND_REG);

    /* Statement 12: ld a, (hl) — second op is MEM */
    ASSERT_EQ(stmts[12].kind, ST_INSTR);
    ASSERT_EQ(stmts[12].instr.nops, 2);
    ASSERT_EQ(stmts[12].instr.ops[0].form, OPND_REG);
    ASSERT_EQ(stmts[12].instr.ops[1].form, OPND_MEM);

    /* Statement 13: ld (c), a — first op is MEM, (c) */
    ASSERT_EQ(stmts[13].kind, ST_INSTR);
    ASSERT_EQ(stmts[13].instr.nops, 2);
    ASSERT_EQ(stmts[13].instr.ops[0].form, OPND_MEM);
    ASSERT_EQ(stmts[13].instr.ops[1].form, OPND_REG);

    free(stmts); free(toks);
}

static void test_parser_def_equ(void)
{
    /* Test DEF NAME EQU expr form */
    const char *src = "DEF MYVAL EQU $FF\n";
    AsmToken *toks = NULL; int ntoks = 0, nstmts = 0;
    AsmStmt *stmts = lex_and_parse(src, &toks, &ntoks, &nstmts);
    ASSERT_TRUE(stmts != NULL);
    ASSERT_EQ(nstmts, 1);
    ASSERT_EQ(stmts[0].kind, ST_DIRECTIVE);
    ASSERT_EQ(stmts[0].dir.kind, DIR_EQU);
    /* name token is MYVAL */
    const AsmToken *name = &toks[stmts[0].dir.name_start];
    ASSERT_TRUE(name->len == 5 && memcmp(name->text, "MYVAL", 5) == 0);
    /* args token is $FF */
    const AsmToken *val = &toks[stmts[0].dir.args_start];
    ASSERT_EQ(val->kind, TOK_NUMBER);
    ASSERT_EQ(val->value, 0xFF);
    free(stmts); free(toks);
}

static void test_parser_include_incbin(void)
{
    const char *src =
        "INCLUDE \"other.asm\"\n"
        "INCBIN \"assets/tiles.2bpp\"\n";
    AsmToken *toks = NULL; int ntoks = 0, nstmts = 0;
    AsmStmt *stmts = lex_and_parse(src, &toks, &ntoks, &nstmts);
    ASSERT_TRUE(stmts != NULL);
    ASSERT_EQ(nstmts, 2);
    ASSERT_EQ(stmts[0].kind, ST_DIRECTIVE);
    ASSERT_EQ(stmts[0].dir.kind, DIR_INCLUDE);
    ASSERT_EQ(stmts[1].kind, ST_DIRECTIVE);
    ASSERT_EQ(stmts[1].dir.kind, DIR_INCBIN);
    free(stmts); free(toks);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

int main(void)
{
    /* Expression evaluator tests */
    test_expr_basic_arith();
    test_expr_high();
    test_expr_low();
    test_expr_shift();
    test_expr_symbol_lookup();
    test_expr_undefined_symbol();
    test_expr_current_addr();
    test_expr_bitwise();
    test_expr_unary();

    /* Parser tests */
    test_parser_statement_count();
    test_parser_section();
    test_parser_labels();
    test_parser_equ();
    test_parser_db();
    test_parser_dw();
    test_parser_ds();
    test_parser_instructions();
    test_parser_def_equ();
    test_parser_include_incbin();

    TEST_MAIN_END();
}
