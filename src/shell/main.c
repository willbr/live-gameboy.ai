#include "../gb/gb.h"
#include "png.h"
#include <SDL3/SDL.h>
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

static void run_one_frame(GB *g) {
    g->frame_ready = false;
    while (!g->frame_ready) gb_step(g);
}

/* keyboard -> button mask (1=pressed) */
static uint8_t poll_buttons(const bool *ks) {
    uint8_t m = 0;
    if (ks[SDL_SCANCODE_Z])      m |= 0x01;  /* A */
    if (ks[SDL_SCANCODE_X])      m |= 0x02;  /* B */
    if (ks[SDL_SCANCODE_RSHIFT]) m |= 0x04;  /* Select */
    if (ks[SDL_SCANCODE_RETURN]) m |= 0x08;  /* Start */
    if (ks[SDL_SCANCODE_RIGHT])  m |= 0x10;
    if (ks[SDL_SCANCODE_LEFT])   m |= 0x20;
    if (ks[SDL_SCANCODE_UP])     m |= 0x40;
    if (ks[SDL_SCANCODE_DOWN])   m |= 0x80;
    return m;
}

int run_window(GB *g, int scale) {
    gb_apu_set_sample_rate(g, 48000);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_AudioSpec spec;
    spec.freq = 48000;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!audio) {
        fprintf(stderr, "SDL_OpenAudioDeviceStream: %s\n", SDL_GetError());
    } else {
        SDL_ResumeAudioStreamDevice(audio);
    }

    SDL_Window *win = SDL_CreateWindow("live-gameboy", 160 * scale, 144 * scale, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_STREAMING, 160, 144);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    static uint8_t rgba[160 * 144 * 4];
    bool running = true;
    const double frame_ms = 1000.0 / 59.7273;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_ESCAPE)
                running = false;
        }
        int nkeys;
        const bool *ks = SDL_GetKeyboardState(&nkeys);
        gb_set_buttons(g, poll_buttons(ks));

        run_one_frame(g);

        if (audio) {
            static float abuf[4096];
            int n;
            while ((n = gb_audio_read(g, abuf, 4096)) > 0)
                SDL_PutAudioStreamData(audio, abuf, n * (int)sizeof(float));
        }

        framebuffer_to_rgba(gb_framebuffer(g), rgba, 1);   /* texture is 160x144; scale via renderer */
        SDL_UpdateTexture(tex, NULL, rgba, 160 * 4);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        SDL_Delay((Uint32)frame_ms);    /* coarse pacing; good enough without audio sync */
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    if (audio) SDL_DestroyAudioStream(audio);
    SDL_Quit();
    return 0;
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
    /* interactive window mode */
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
