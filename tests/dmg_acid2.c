/* Runs dmg-acid2 for enough frames to settle, then prints an FNV-1a hash of the
   160x144 framebuffer. First run: record the hash, eyeball-verify the image is the
   acid2 face by dumping a PGM, then bake the hash in as EXPECTED. */
#include "../src/gb/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Baked after manual verification in Step 4. 0 means "print only". */
#define EXPECTED_HASH 0x6124ed564f3b3ad8ULL

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "roms/dmg-acid2.gb";
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 2; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)sz);
    if (fread(data, 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); return 2; }
    fclose(fp);

    GB *g = gb_new();
    if (!gb_load_rom(g, data, (size_t)sz)) { fprintf(stderr, "bad rom\n"); return 2; }
    free(data);
    gb_reset(g);

    /* run 60 frames (~1s) so the ROM finishes drawing */
    for (int f = 0; f < 60; f++) {
        g->frame_ready = false;
        while (!g->frame_ready) gb_step(g);
    }

    const uint8_t *fb = gb_framebuffer(g);
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < 160 * 144; i++) { h ^= fb[i]; h *= 1099511628211ULL; }

    /* dump a PGM for manual inspection */
    FILE *out = fopen("build/acid2.pgm", "wb");
    if (out) {
        fprintf(out, "P5\n160 144\n3\n");
        for (int i = 0; i < 160 * 144; i++) { uint8_t s = 3 - fb[i]; fputc(s, out); }
        fclose(out);
    }

    printf("dmg-acid2 framebuffer hash: 0x%016llx\n", (unsigned long long)h);
    if (EXPECTED_HASH == 0ULL) { printf("(no expected hash baked yet)\n"); gb_free(g); return 0; }
    if (h == EXPECTED_HASH) { printf("PASS dmg-acid2\n"); gb_free(g); return 0; }
    printf("FAIL dmg-acid2 (hash mismatch)\n"); gb_free(g); return 1;
}
