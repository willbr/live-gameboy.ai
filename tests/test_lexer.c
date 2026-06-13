#include "test.h"
#include "../src/asm/asm.h"
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

/* Find the nth token of a given kind (0-based) */
static const AsmToken *find_nth(const AsmToken *toks, int ntoks,
                                AsmTokenKind kind, int n)
{
    int found = 0;
    for (int i = 0; i < ntoks; i++) {
        if (toks[i].kind == kind) {
            if (found == n) return &toks[i];
            found++;
        }
    }
    return NULL;
}

static int tok_text_eq(const AsmToken *t, const char *want)
{
    if (!t) return 0;
    size_t wl = strlen(want);
    return t->len == wl && memcmp(t->text, want, wl) == 0;
}

/* -----------------------------------------------------------------------
 * Test 1: basic smoke test — hex, decimal, binary, string, identifier
 * --------------------------------------------------------------------- */

static void test_basic_numbers(void)
{
    const char *src = "$FF 255 %11001100 0xFF\n";
    int n = 0;
    AsmDiag *diags = NULL;
    int ndiags = 0;

    AsmToken *toks = asm_lex(src, "test", &n, &diags, &ndiags);
    ASSERT_TRUE(toks != NULL);
    ASSERT_EQ(ndiags, 0);

    /* 4 numbers + 1 NEWLINE + 1 EOF */
    ASSERT_EQ(n, 6);

    ASSERT_EQ(toks[0].kind,  TOK_NUMBER);
    ASSERT_EQ(toks[0].value, 0xFF);

    ASSERT_EQ(toks[1].kind,  TOK_NUMBER);
    ASSERT_EQ(toks[1].value, 255);

    ASSERT_EQ(toks[2].kind,  TOK_NUMBER);
    ASSERT_EQ(toks[2].value, 0xCC);

    ASSERT_EQ(toks[3].kind,  TOK_NUMBER);
    ASSERT_EQ(toks[3].value, 0xFF);

    ASSERT_EQ(toks[4].kind,  TOK_NEWLINE);
    ASSERT_EQ(toks[5].kind,  TOK_EOF);

    free(toks);
    free(diags);
}

/* -----------------------------------------------------------------------
 * Test 2: char literal
 * --------------------------------------------------------------------- */

static void test_char_literal(void)
{
    const char *src = "'A' 'z' ' '\n";
    int n = 0;
    AsmDiag *diags = NULL; int ndiags = 0;

    AsmToken *toks = asm_lex(src, "test", &n, &diags, &ndiags);
    ASSERT_TRUE(toks != NULL);
    ASSERT_EQ(ndiags, 0);

    ASSERT_EQ(toks[0].kind,  TOK_CHAR);
    ASSERT_EQ(toks[0].value, 'A');

    ASSERT_EQ(toks[1].kind,  TOK_CHAR);
    ASSERT_EQ(toks[1].value, 'z');

    ASSERT_EQ(toks[2].kind,  TOK_CHAR);
    ASSERT_EQ(toks[2].value, ' ');

    free(toks);
    free(diags);
}

/* -----------------------------------------------------------------------
 * Test 3: string literal contents and length
 * --------------------------------------------------------------------- */

static void test_string(void)
{
    const char *src = "\"hello\"\n";
    int n = 0;
    AsmDiag *diags = NULL; int ndiags = 0;

    AsmToken *toks = asm_lex(src, "test", &n, &diags, &ndiags);
    ASSERT_TRUE(toks != NULL);
    ASSERT_EQ(ndiags, 0);

    ASSERT_EQ(toks[0].kind, TOK_STRING);
    ASSERT_EQ((int)toks[0].len, 5); /* "hello" without quotes */
    ASSERT_TRUE(memcmp(toks[0].text, "hello", 5) == 0);

    free(toks);
    free(diags);
}

/* -----------------------------------------------------------------------
 * Test 4: identifier and label colon
 * --------------------------------------------------------------------- */

static void test_ident_label(void)
{
    const char *src = "Foo: bar\n";
    int n = 0;
    AsmDiag *diags = NULL; int ndiags = 0;

    AsmToken *toks = asm_lex(src, "test", &n, &diags, &ndiags);
    ASSERT_TRUE(toks != NULL);
    ASSERT_EQ(ndiags, 0);

    /* Foo  :  bar  NEWLINE  EOF */
    ASSERT_EQ(n, 5);

    ASSERT_EQ(toks[0].kind, TOK_IDENT);
    ASSERT_TRUE(tok_text_eq(&toks[0], "Foo"));

    ASSERT_EQ(toks[1].kind, TOK_PUNCT);
    ASSERT_TRUE(tok_text_eq(&toks[1], ":"));

    ASSERT_EQ(toks[2].kind, TOK_IDENT);
    ASSERT_TRUE(tok_text_eq(&toks[2], "bar"));

    free(toks);
    free(diags);
}

/* -----------------------------------------------------------------------
 * Test 5: local label starting with '.'
 * --------------------------------------------------------------------- */

static void test_local_label(void)
{
    const char *src = ".loop jr .loop\n";
    int n = 0;
    AsmDiag *diags = NULL; int ndiags = 0;

    AsmToken *toks = asm_lex(src, "test", &n, &diags, &ndiags);
    ASSERT_TRUE(toks != NULL);
    ASSERT_EQ(ndiags, 0);

    /* .loop  jr  .loop  NEWLINE  EOF */
    ASSERT_EQ(n, 5);

    ASSERT_EQ(toks[0].kind, TOK_IDENT);
    ASSERT_TRUE(tok_text_eq(&toks[0], ".loop"));

    ASSERT_EQ(toks[1].kind, TOK_IDENT);
    ASSERT_TRUE(tok_text_eq(&toks[1], "jr"));

    ASSERT_EQ(toks[2].kind, TOK_IDENT);
    ASSERT_TRUE(tok_text_eq(&toks[2], ".loop"));

    free(toks);
    free(diags);
}

/* -----------------------------------------------------------------------
 * Test 6: punctuation including << and >>
 * --------------------------------------------------------------------- */

static void test_punctuation(void)
{
    const char *src = ", : ( ) + - * / & | ~ << >>\n";
    int n = 0;
    AsmDiag *diags = NULL; int ndiags = 0;

    AsmToken *toks = asm_lex(src, "test", &n, &diags, &ndiags);
    ASSERT_TRUE(toks != NULL);
    ASSERT_EQ(ndiags, 0);

    /* 13 punct + NEWLINE + EOF = 15 */
    ASSERT_EQ(n, 15);

    /* check a few */
    ASSERT_EQ(toks[0].kind, TOK_PUNCT);
    ASSERT_TRUE(tok_text_eq(&toks[0], ","));

    ASSERT_EQ(toks[11].kind, TOK_PUNCT);
    ASSERT_TRUE(tok_text_eq(&toks[11], "<<"));

    ASSERT_EQ(toks[12].kind, TOK_PUNCT);
    ASSERT_TRUE(tok_text_eq(&toks[12], ">>"));

    free(toks);
    free(diags);
}

/* -----------------------------------------------------------------------
 * Test 7: comment stripping
 * --------------------------------------------------------------------- */

static void test_comments(void)
{
    const char *src =
        "ld a, $42 ; load 42 into A\n"
        "; full line comment\n"
        "nop\n";
    int n = 0;
    AsmDiag *diags = NULL; int ndiags = 0;

    AsmToken *toks = asm_lex(src, "test", &n, &diags, &ndiags);
    ASSERT_TRUE(toks != NULL);
    ASSERT_EQ(ndiags, 0);

    /* ld a , $42 NEWLINE nop NEWLINE EOF
     * (the full-line comment line produces no tokens, and its newline
     *  is collapsed into the one already emitted for line 1) */
    /* Tokens: ld(IDENT) a(IDENT) ,(PUNCT) $42(NUMBER) \n nop(IDENT) \n EOF */
    ASSERT_EQ(n, 8);

    ASSERT_EQ(toks[0].kind, TOK_IDENT);
    ASSERT_TRUE(tok_text_eq(&toks[0], "ld"));

    ASSERT_EQ(toks[3].kind, TOK_NUMBER);
    ASSERT_EQ(toks[3].value, 0x42);

    ASSERT_EQ(toks[4].kind, TOK_NEWLINE);
    ASSERT_EQ(toks[5].kind, TOK_IDENT);
    ASSERT_TRUE(tok_text_eq(&toks[5], "nop"));

    free(toks);
    free(diags);
}

/* -----------------------------------------------------------------------
 * Test 8: line tracking
 * --------------------------------------------------------------------- */

static void test_line_tracking(void)
{
    const char *src =
        "a\n"       /* line 1 */
        "b\n"       /* line 2 */
        "\n"        /* line 3 (blank) */
        "c\n";      /* line 4 */
    int n = 0;
    AsmDiag *diags = NULL; int ndiags = 0;

    AsmToken *toks = asm_lex(src, "test", &n, &diags, &ndiags);
    ASSERT_TRUE(toks != NULL);
    ASSERT_EQ(ndiags, 0);

    /* a  \n  b  \n  c  \n  EOF  — blank line collapses */
    /* Find 'a', 'b', 'c' */
    const AsmToken *ta = find_nth(toks, n, TOK_IDENT, 0);
    const AsmToken *tb = find_nth(toks, n, TOK_IDENT, 1);
    const AsmToken *tc = find_nth(toks, n, TOK_IDENT, 2);

    ASSERT_TRUE(ta != NULL);
    ASSERT_TRUE(tb != NULL);
    ASSERT_TRUE(tc != NULL);

    ASSERT_EQ(ta->line, 1);
    ASSERT_EQ(tb->line, 2);
    ASSERT_EQ(tc->line, 4);

    free(toks);
    free(diags);
}

/* -----------------------------------------------------------------------
 * Test 9: multi-line program snippet
 * --------------------------------------------------------------------- */

static void test_program_snippet(void)
{
    const char *src =
        "SECTION \"ROM0\", ROM0\n"
        "Start:\n"
        "    ld hl, $C000\n"
        ".loop:\n"
        "    ld (hl), %10101010\n"
        "    jr .loop\n";

    int n = 0;
    AsmDiag *diags = NULL; int ndiags = 0;

    AsmToken *toks = asm_lex(src, "prog.asm", &n, &diags, &ndiags);
    ASSERT_TRUE(toks != NULL);
    ASSERT_EQ(ndiags, 0);

    /* Spot-check a few tokens */
    ASSERT_EQ(toks[0].kind, TOK_IDENT);
    ASSERT_TRUE(tok_text_eq(&toks[0], "SECTION"));

    /* "ROM0" string */
    const AsmToken *str = find_nth(toks, n, TOK_STRING, 0);
    ASSERT_TRUE(str != NULL);
    ASSERT_TRUE(memcmp(str->text, "ROM0", 4) == 0);

    /* $C000 value */
    const AsmToken *hex = find_nth(toks, n, TOK_NUMBER, 0);
    ASSERT_TRUE(hex != NULL);
    ASSERT_EQ(hex->value, 0xC000);

    /* %10101010 value */
    const AsmToken *bin = find_nth(toks, n, TOK_NUMBER, 1);
    ASSERT_TRUE(bin != NULL);
    ASSERT_EQ(bin->value, 0xAA);

    /* .loop ident */
    const AsmToken *local = find_nth(toks, n, TOK_IDENT, 0);
    /* first ident is SECTION, keep searching for .loop */
    for (int i = 0; i < n; i++) {
        if (toks[i].kind == TOK_IDENT &&
            toks[i].len == 5 &&
            memcmp(toks[i].text, ".loop", 5) == 0) {
            local = &toks[i];
            break;
        }
    }
    ASSERT_TRUE(local != NULL);
    ASSERT_TRUE(tok_text_eq(local, ".loop"));

    free(toks);
    free(diags);
}

/* -----------------------------------------------------------------------
 * Test 10: unterminated string → diagnostic
 * --------------------------------------------------------------------- */

static void test_unterminated_string(void)
{
    const char *src = "\"oops\n";
    int n = 0;
    AsmDiag *diags = NULL; int ndiags = 0;

    AsmToken *toks = asm_lex(src, "test", &n, &diags, &ndiags);
    ASSERT_TRUE(toks != NULL);
    ASSERT_EQ(ndiags, 1);  /* one diagnostic emitted */

    /* still get a TOK_STRING token (recovery) */
    ASSERT_TRUE(find_nth(toks, n, TOK_STRING, 0) != NULL);

    free(toks);
    free(diags);
}

/* -----------------------------------------------------------------------
 * Test 11: column tracking
 * --------------------------------------------------------------------- */

static void test_column_tracking(void)
{
    const char *src = "ld a, $42\n";
    int n = 0;
    AsmDiag *diags = NULL; int ndiags = 0;

    AsmToken *toks = asm_lex(src, "test", &n, &diags, &ndiags);
    ASSERT_TRUE(toks != NULL);

    ASSERT_EQ(toks[0].col, 1); /* ld  */
    ASSERT_EQ(toks[1].col, 4); /* a   */
    ASSERT_EQ(toks[2].col, 5); /* ,   */
    ASSERT_EQ(toks[3].col, 7); /* $42 */

    free(toks);
    free(diags);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

int main(void)
{
    test_basic_numbers();
    test_char_literal();
    test_string();
    test_ident_label();
    test_local_label();
    test_punctuation();
    test_comments();
    test_line_tracking();
    test_program_snippet();
    test_unterminated_string();
    test_column_tracking();

    TEST_MAIN_END();
}
