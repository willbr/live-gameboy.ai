/* Runs a blargg dmg_sound test ROM headless.
   These ROMs write results to cart RAM ($A000) rather than serial:
     $A000 = status ($80 = running, $00 = pass, other = fail code)
     $A001-$A003 = signature $DE $B0 $61
     $A004+ = NUL-terminated text output
   Exit 0 on pass, 1 on fail, 2 on timeout/error. */
#include "../src/gb/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: blargg_sound <rom.gb>\n"); return 2; }

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

    /* dmg_sound ROMs run for a few emulated seconds; cap at 120 */
    const uint64_t limit = 4194304ULL * 120;
    while (g->cycles < limit) {
        gb_step(g);

        /* Check cart RAM signature at $A001-$A003; $A000 = status */
        if (g->cart_ram[1] == 0xDE && g->cart_ram[2] == 0xB0 && g->cart_ram[3] == 0x61) {
            uint8_t status = g->cart_ram[0];
            if (status != 0x80) {  /* 0x80 = still running */
                /* Text output at $A004 as NUL-terminated string */
                const char *text = (const char *)&g->cart_ram[4];
                if (status == 0x00) {
                    printf("PASS %s\n", argv[1]);
                    gb_free(g); return 0;
                } else {
                    printf("FAIL %s (code %d)\n----\n%s\n", argv[1], status, text);
                    gb_free(g); return 1;
                }
            }
        }

        /* Also check serial for ROMs that use it */
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

    /* Print whatever text was accumulated */
    const char *text = (const char *)&g->cart_ram[4];
    printf("TIMEOUT %s\n----\n%s\n", argv[1], text);
    gb_free(g);
    return 2;
}
