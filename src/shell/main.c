#include "../gb/gb.h"
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t PALETTE[4] = {       /* shade 0..3 -> RGB */
    0xE0F8D0, 0x88C070, 0x346856, 0x081820
};

/* expand the 160x144 shade framebuffer into RGBA8888 (optionally integer-scaled) */
static void framebuffer_to_rgba(const uint8_t *fb, uint8_t *rgba, int scale) {
    int W = 160 * scale;
    for (int y = 0; y < 144 * scale; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t rgb = PALETTE[fb[(y / scale) * 160 + (x / scale)] & 3];
            uint8_t *p = rgba + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)(rgb >> 16); p[1] = (uint8_t)(rgb >> 8);
            p[2] = (uint8_t)rgb;         p[3] = 0xFF;
        }
    }
}

static uint8_t *load_file(const char *path, size_t *out) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f); *out = (size_t)n; return d;
}

int run_window(GB *g, int scale);   /* Task 3 (defined in main.c) */

static void run_one_frame(GB *g) {
    g->frame_ready = false;
    while (!g->frame_ready) gb_step(g);
}

static int run_shot(const char *rom, const char *out, int frames, int scale) {
    size_t n; uint8_t *data = load_file(rom, &n);
    if (!data) return 2;
    GB *g = gb_new();
    if (!gb_load_rom(g, data, n)) { fprintf(stderr, "bad rom\n"); free(data); return 2; }
    free(data);
    gb_reset(g);
    for (int i = 0; i < frames; i++) run_one_frame(g);
    int W = 160 * scale, H = 144 * scale;
    uint8_t *rgba = malloc((size_t)W * H * 4);
    framebuffer_to_rgba(gb_framebuffer(g), rgba, scale);
    int rc = png_write_rgba(out, rgba, W, H);
    free(rgba); gb_free(g);
    if (rc) { fprintf(stderr, "png write failed\n"); return 2; }
    printf("wrote %s (%dx%d, %d frames)\n", out, W, H, frames);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage:\n  %s <rom.gb> [scale]\n"
                        "  %s --shot <rom.gb> <out.png> [frames] [scale]\n",
                argv[0], argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "--shot")) {
        if (argc < 4) { fprintf(stderr, "--shot needs <rom> <out>\n"); return 2; }
        int frames = argc > 4 ? atoi(argv[4]) : 60;
        int scale  = argc > 5 ? atoi(argv[5]) : 1;
        return run_shot(argv[2], argv[3], frames, scale > 0 ? scale : 1);
    }
    /* interactive window mode (Task 3) */
    size_t n; uint8_t *data = load_file(argv[1], &n);
    if (!data) return 2;
    GB *g = gb_new();
    if (!gb_load_rom(g, data, n)) { fprintf(stderr, "bad rom\n"); free(data); return 2; }
    free(data);
    gb_reset(g);
    int scale = argc > 2 ? atoi(argv[2]) : 4;
    int rc = run_window(g, scale > 0 ? scale : 4);
    gb_free(g);
    return rc;
}

int run_window(GB *g, int scale) {
    (void)g; (void)scale;
    fprintf(stderr, "window mode not built yet\n");
    return 1;
}
