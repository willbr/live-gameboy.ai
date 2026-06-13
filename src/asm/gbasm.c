/*
 * gbasm.c — SM83 assembler CLI
 *
 * Usage: gbasm <input.asm> -o <output.gb> [--sym <output.sym>]
 *
 * Reads the source file, assembles it via asm_assemble(), and:
 *  - On error: prints diagnostics (file:line: msg) to stderr, exits 1.
 *  - On success: writes the ROM to <output.gb>.
 *  - If --sym given: writes a symbol file with one line per symbol:
 *      BB:AAAA Name
 *    where BB is the bank (hex, 2 digits) and AAAA is the CPU address
 *    (hex, 4 digits).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asm.h"

static void usage(void)
{
    fprintf(stderr,
            "gbasm — SM83 assembler\n"
            "Usage: gbasm <input.asm> -o <output.gb> [--sym <output.sym>]\n");
}

/* Read entire file into a NUL-terminated heap buffer.  Returns NULL on error. */
static char *read_file(const char *path)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        fprintf(stderr, "gbasm: cannot open '%s'\n", path);
        return NULL;
    }
    if (fseek(fh, 0, SEEK_END) != 0) { fclose(fh); return NULL; }
    long sz = ftell(fh);
    if (sz < 0) { fclose(fh); return NULL; }
    rewind(fh);

    char *buf = malloc((size_t)(sz + 1));
    if (!buf) { fclose(fh); return NULL; }

    size_t got = fread(buf, 1, (size_t)sz, fh);
    fclose(fh);
    buf[got] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *output_path = NULL;
    const char *sym_path   = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) { usage(); return 1; }
            output_path = argv[i];
        } else if (strcmp(argv[i], "--sym") == 0) {
            if (++i >= argc) { usage(); return 1; }
            sym_path = argv[i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "gbasm: unknown option '%s'\n", argv[i]);
            usage();
            return 1;
        } else {
            if (input_path) {
                fprintf(stderr, "gbasm: multiple input files not supported\n");
                usage();
                return 1;
            }
            input_path = argv[i];
        }
    }

    if (!input_path || !output_path) {
        usage();
        return 1;
    }

    /* Read source */
    char *src = read_file(input_path);
    if (!src) return 1;

    /* Assemble */
    AsmResult r = asm_assemble(src, input_path);
    free(src);

    /* Print diagnostics */
    for (int i = 0; i < r.ndiags; i++) {
        const AsmDiag *d = &r.diags[i];
        const char *fname = d->filename ? d->filename : input_path;
        fprintf(stderr, "%s:%d: %s\n", fname, d->line, d->msg);
    }

    if (!r.ok) {
        asm_free(&r);
        return 1;
    }

    /* Write ROM */
    FILE *fout = fopen(output_path, "wb");
    if (!fout) {
        fprintf(stderr, "gbasm: cannot write '%s'\n", output_path);
        asm_free(&r);
        return 1;
    }
    size_t written = fwrite(r.rom, 1, r.rom_size, fout);
    fclose(fout);
    if (written != r.rom_size) {
        fprintf(stderr, "gbasm: write error for '%s'\n", output_path);
        asm_free(&r);
        return 1;
    }

    printf("gbasm: assembled %s -> %s (%zu bytes)\n",
           input_path, output_path, r.rom_size);

    /* Write symbol file */
    if (sym_path) {
        FILE *sf = fopen(sym_path, "w");
        if (!sf) {
            fprintf(stderr, "gbasm: cannot write sym file '%s'\n", sym_path);
            asm_free(&r);
            return 1;
        }
        for (int i = 0; i < r.nsyms; i++) {
            const AsmSymbol *s = &r.syms[i];
            /* Skip EQU constants (bank -1) */
            if (s->bank < 0) continue;
            fprintf(sf, "%02X:%04X %s\n", s->bank & 0xFF, s->addr, s->name);
        }
        fclose(sf);
        printf("gbasm: symbols -> %s (%d entries)\n", sym_path, r.nsyms);
    }

    asm_free(&r);
    return 0;
}
