#ifndef EXAMPLE_RUN_H
#define EXAMPLE_RUN_H
#include <stdio.h>
#include <stdlib.h>
#include "../src/asm/asm.h"
#include "../src/gb/gb.h"

/* Read a whole file into a NUL-terminated heap buffer (caller frees). */
static char *ex_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "example_run: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f); fclose(f);
    b[got] = '\0';
    return b;
}

/* Assemble an examples .asm file. Returns an AsmResult (caller asm_free's).
 * On failure, r.ok is false and diagnostics are printed. */
static AsmResult ex_assemble(const char *path) {
    char *src = ex_read_file(path);
    AsmResult r = {0};
    if (!src) return r;
    r = asm_assemble(src, path);
    free(src);
    if (!r.ok) {
        for (int i = 0; i < r.ndiags; i++)
            fprintf(stderr, "  %s:%d: %s\n",
                    r.diags[i].filename ? r.diags[i].filename : path,
                    r.diags[i].line, r.diags[i].msg);
    }
    return r;
}

/* Step the emulator until `frames` VBlanks elapse or `max_steps` instructions run. */
static void ex_run(GB *gb, int frames, int max_steps) {
    int f = 0, s = 0;
    while (f < frames && s < max_steps) {
        int tc = gb_step(gb);
        gb_tick(gb, tc);
        gb_ppu_tick(gb, tc);
        if (gb->frame_ready) { gb->frame_ready = false; f++; }
        s++;
    }
}

/* True if any framebuffer pixel is non-zero (screen is not blank). */
static int ex_fb_nonblank(GB *gb) {
    const uint8_t *fb = gb_framebuffer(gb);
    for (int i = 0; i < 160 * 144; i++) if (fb[i] != 0) return 1;
    return 0;
}
#endif
