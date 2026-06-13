/*
 * test_asset_prov.c — tests for asset provenance in the build database.
 *
 * Verifies that:
 *   - INCBIN'd files appear in AsmResult.assets[] with the correct path,
 *     size, and bytes.
 *   - prov_asset / prov_asset_off correctly map each ROM byte back to the
 *     originating asset byte.
 *   - Non-asset bytes have prov_asset == -1.
 *   - The same asset included twice is deduped (nassets stays at 1).
 */

#include "test.h"
#include "../src/asm/asm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSET_PATH "build/test_asset_prov.bin"
#define ASSET_SIZE 32

static void write_asset(void)
{
    FILE *f = fopen(ASSET_PATH, "wb");
    if (!f) { fprintf(stderr, "Cannot create " ASSET_PATH "\n"); exit(1); }
    for (int i = 0; i < ASSET_SIZE; i++) {
        uint8_t b = (uint8_t)i;
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
}

int main(void)
{
    /* ------------------------------------------------------------------ */
    /* Setup: write asset file                                              */
    /* ------------------------------------------------------------------ */
    write_asset();

    /* ------------------------------------------------------------------ */
    /* Basic asset provenance test                                          */
    /* ------------------------------------------------------------------ */
    {
        /* The test source has a code byte (nop) followed by the INCBIN. */
        const char *src =
            "SECTION \"Code\", ROM0\n"
            "Main:\n"
            "  nop\n"             /* 1 code byte at DEFAULT_ORG */
            "Data:\n"
            "  incbin \"" ASSET_PATH "\"\n";

        AsmResult r = asm_assemble(src, "test_asset_prov.asm");

        ASSERT_TRUE(r.ok);

        /* Asset table */
        ASSERT_EQ(r.nassets, 1);
        ASSERT_TRUE(r.assets != NULL);

        /* Path must end with the filename */
        if (r.nassets >= 1) {
            const char *p = r.assets[0].path;
            size_t plen   = strlen(p);
            const char *tail = ASSET_PATH;
            size_t tlen      = strlen(tail);
            ASSERT_TRUE(plen >= tlen &&
                        strcmp(p + plen - tlen, tail) == 0);

            /* Size and byte content */
            ASSERT_EQ((long long)r.assets[0].size, ASSET_SIZE);
            ASSERT_TRUE(r.assets[0].bytes != NULL);
            for (int i = 0; i < ASSET_SIZE; i++)
                ASSERT_EQ(r.assets[0].bytes[i], (uint8_t)i);
        }

        /* Find "Data" symbol to locate where the asset landed in ROM */
        const AsmSymbol *data_sym = asm_sym_lookup(&(AsmSymbolTable){
            .syms  = r.syms,
            .count = r.nsyms,
            .cap   = r.nsyms
        }, "Data");
        ASSERT_TRUE(data_sym != NULL);

        if (data_sym) {
            uint32_t base = data_sym->off;

            /* Each asset byte: prov_asset == 0, prov_asset_off == i */
            for (int i = 0; i < ASSET_SIZE; i++) {
                uint32_t off = base + (uint32_t)i;
                ASSERT_EQ(r.prov_asset[off],     0);
                ASSERT_EQ(r.prov_asset_off[off], (uint32_t)i);
            }

            /* ROM content must match asset bytes */
            for (int i = 0; i < ASSET_SIZE; i++)
                ASSERT_EQ(r.rom[base + (uint32_t)i], (uint8_t)i);
        }

        /* The "Main" / "nop" byte is a code byte — prov_asset must be -1 */
        const AsmSymbol *main_sym = asm_sym_lookup(&(AsmSymbolTable){
            .syms  = r.syms,
            .count = r.nsyms,
            .cap   = r.nsyms
        }, "Main");
        ASSERT_TRUE(main_sym != NULL);
        if (main_sym) {
            ASSERT_EQ(r.prov_asset[main_sym->off], -1);
        }

        asm_free(&r);
    }

    /* ------------------------------------------------------------------ */
    /* Dedup test: same path incbin'd twice => nassets stays 1             */
    /* ------------------------------------------------------------------ */
    {
        const char *src =
            "SECTION \"D\", ROM0\n"
            "First:  incbin \"" ASSET_PATH "\"\n"
            "Second: incbin \"" ASSET_PATH "\"\n";

        AsmResult r = asm_assemble(src, "test_asset_prov_dedup.asm");

        ASSERT_TRUE(r.ok);
        ASSERT_EQ(r.nassets, 1);  /* same path => deduped */

        /* Both regions must have prov_asset == 0 */
        if (r.nassets >= 1) {
            const AsmSymbol *s1 = asm_sym_lookup(&(AsmSymbolTable){
                .syms = r.syms, .count = r.nsyms, .cap = r.nsyms
            }, "First");
            const AsmSymbol *s2 = asm_sym_lookup(&(AsmSymbolTable){
                .syms = r.syms, .count = r.nsyms, .cap = r.nsyms
            }, "Second");
            ASSERT_TRUE(s1 != NULL);
            ASSERT_TRUE(s2 != NULL);
            if (s1 && s2) {
                for (int i = 0; i < ASSET_SIZE; i++) {
                    ASSERT_EQ(r.prov_asset[s1->off + (uint32_t)i], 0);
                    ASSERT_EQ(r.prov_asset[s2->off + (uint32_t)i], 0);
                    ASSERT_EQ(r.prov_asset_off[s1->off + (uint32_t)i], (uint32_t)i);
                    ASSERT_EQ(r.prov_asset_off[s2->off + (uint32_t)i], (uint32_t)i);
                }
            }
        }

        asm_free(&r);
    }

    /* ------------------------------------------------------------------ */
    /* Cleanup                                                              */
    /* ------------------------------------------------------------------ */
    remove(ASSET_PATH);

    TEST_MAIN_END();
}
