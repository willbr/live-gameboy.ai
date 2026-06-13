/* Runs a blargg test ROM headless; result arrives via serial.
   Exit 0 on "Passed", 1 on "Failed", 2 on timeout. */
#include "../src/gb/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: blargg <rom.gb>\n"); return 2; }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (fread(data, 1, (size_t)size, fp) != (size_t)size) { fclose(fp); return 2; }
    fclose(fp);

    GB *g = gb_new();
    if (!gb_load_rom(g, data, (size_t)size)) { fprintf(stderr, "bad rom\n"); return 2; }
    free(data);
    gb_reset(g);

    /* cpu_instrs takes ~55 emulated seconds; cap at 120 */
    const uint64_t limit = 4194304ULL * 120;
    while (g->cycles < limit) {
        gb_step(g);
        g->serial_buf[g->serial_len] = 0;
        if (strstr(g->serial_buf, "Passed")) {
            printf("PASS %s\n", argv[1]);
            gb_free(g); return 0;
        }
        if (strstr(g->serial_buf, "Failed")) {
            printf("FAIL %s\n----\n%s\n", argv[1], g->serial_buf);
            gb_free(g); return 1;
        }
    }
    g->serial_buf[g->serial_len] = 0;
    printf("TIMEOUT %s\n----\n%s\n", argv[1], g->serial_buf);
    gb_free(g);
    return 2;
}
