/*
 * main.c — SDL3 interactive shell for the live-gameboy IDE.
 *
 * Usage:
 *   live-gameboy-ide --ide-shot <in.asm|in.gb> <out.png> [frames]
 *       Headless: load, step `frames` frames, save PNG, exit.
 *
 *   live-gameboy-ide <file.asm|file.gb>
 *       Interactive 1024x720 window with joypad, F5 reload, debugger
 *       (pause/step, breakpoints, watchpoints), mouse tile paint.
 *
 * Key bindings:
 *   Esc / Q    — quit
 *   F5         — reload source from disk (asm mode only); sets status bar
 *   0-3        — set paint colour (0=lightest … 3=darkest)
 *   Z          — GB A
 *   X          — GB B
 *   RShift     — GB Select
 *   Enter      — GB Start
 *   Arrows     — GB D-pad
 *
 * Mouse:
 *   Left-click inside VRAM TILES panel → select tile
 *   Left-click inside TILE EDITOR panel → paint pixel with current colour
 */

#include "ide.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Canvas dimensions — must match ide.h convention */
#define CANVAS_W IDE_CANVAS_W
#define CANVAS_H IDE_CANVAS_H

/* SDL pixel format for the streaming texture.
 * Canvas stores RGBA bytes in R,G,B,A order (row-major), which corresponds
 * to SDL_PIXELFORMAT_RGBA8888 interpreted as big-endian, but SDL uses
 * host-endian layout.  We need SDL_PIXELFORMAT_ABGR8888 on little-endian
 * (the common case) so that the byte order R,G,B,A maps correctly.
 * Simplest: use SDL_PIXELFORMAT_RGBA32 which SDL defines as the format
 * that gives {R,G,B,A} bytes in memory regardless of host endianness. */
#define CANVAS_SDL_FORMAT SDL_PIXELFORMAT_RGBA32

/* -------------------------------------------------------------------------
 * Joypad mapping (same as src/shell/main.c)
 * ------------------------------------------------------------------------- */
static uint8_t poll_buttons(const bool *ks) {
    uint8_t m = 0;
    if (ks[SDL_SCANCODE_Z])      m |= 0x01;  /* A      */
    if (ks[SDL_SCANCODE_X])      m |= 0x02;  /* B      */
    if (ks[SDL_SCANCODE_RSHIFT]) m |= 0x04;  /* Select */
    if (ks[SDL_SCANCODE_RETURN]) m |= 0x08;  /* Start  */
    if (ks[SDL_SCANCODE_RIGHT])  m |= 0x10;
    if (ks[SDL_SCANCODE_LEFT])   m |= 0x20;
    if (ks[SDL_SCANCODE_UP])     m |= 0x40;
    if (ks[SDL_SCANCODE_DOWN])   m |= 0x80;
    return m;
}

/* -------------------------------------------------------------------------
 * Interactive window
 * ------------------------------------------------------------------------- */
static int run_interactive(const char *path) {
    IdeState *s = ide_new(path);
    if (!s) {
        fprintf(stderr, "ide_new failed for: %s\n", path);
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        ide_free(s);
        return 1;
    }

    /* Window at 2x with renderer logical size IDE_CANVAS_W x IDE_CANVAS_H
     * (1024x720) — the texture is drawn at its natural size so it fills the
     * logical viewport cleanly. */
    SDL_Window   *win = SDL_CreateWindow("live-gameboy IDE",
                                         CANVAS_W * 2, CANVAS_H * 2, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!win || !ren) {
        fprintf(stderr, "SDL window/renderer: %s\n", SDL_GetError());
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        ide_free(s);
        return 1;
    }

    /* Logical size so canvas coords map 1:1 regardless of window scale */
    SDL_SetRenderLogicalPresentation(ren, CANVAS_W, CANVAS_H,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    SDL_Texture *tex = SDL_CreateTexture(ren, CANVAS_SDL_FORMAT,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         CANVAS_W, CANVAS_H);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    /* Allocate a persistent canvas (reused each frame) */
    Canvas canvas = canvas_new(CANVAS_W, CANVAS_H);
    if (!canvas.px) {
        fprintf(stderr, "canvas_new failed\n");
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        ide_free(s);
        return 1;
    }

    /* Panel rects for mouse hit-testing */
    int vr_x, vr_y, vr_w, vr_h;  /* VRAM tiles */
    int te_x, te_y, te_w, te_h;  /* Tile editor */
    ide_panel_rect(PANEL_VRAM_TILES,  &vr_x, &vr_y, &vr_w, &vr_h);
    ide_panel_rect(PANEL_TILE_EDITOR, &te_x, &te_y, &te_w, &te_h);

    bool running = true;
    const double frame_ms = 1000.0 / 59.7273;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            /* Window is 2x the logical canvas with letterbox presentation, so
             * mouse events arrive in window pixels. Convert them to logical
             * (canvas) coordinates so they match the panel hit-test rects. */
            SDL_ConvertEventToRenderCoordinates(ren, &ev);

            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;

            case SDL_EVENT_KEY_DOWN: {
                SDL_Keycode key = ev.key.key;
                if (ide_addr_focused(s)) {
                    /* Address field is active: route keys to the text field. */
                    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                        ide_addr_commit(s);
                        SDL_StopTextInput(win);
                    } else if (key == SDLK_ESCAPE) {
                        ide_addr_focus(s, false);
                        SDL_StopTextInput(win);
                    } else if (key == SDLK_BACKSPACE) {
                        ide_addr_backspace(s);
                    }
                    /* Printable chars arrive via SDL_EVENT_TEXT_INPUT -> ide_addr_putc */
                } else {
                    switch (ev.key.scancode) {
                    case SDL_SCANCODE_ESCAPE:
                    case SDL_SCANCODE_Q:
                        running = false;
                        break;

                    case SDL_SCANCODE_F5:
                        /* Hot reload: patch running code, keep state (asm mode only) */
                        if (ide_is_asm(s))
                            ide_reload_from_file(s, path);
                        break;

                    case SDL_SCANCODE_F8:
                        /* Soft reset: re-run from Main, clearing RAM/VRAM. */
                        if (ide_is_asm(s))
                            ide_soft_reset_from_file(s, path);
                        break;

                    /* Debugger step / pause keys */
                    case SDL_SCANCODE_SPACE:
                        if (ide_exec_mode(s) == EXEC_PAUSED)
                            ide_resume(s);
                        else
                            ide_pause(s);
                        break;
                    case SDL_SCANCODE_F7:
                        ide_step_insn(s);
                        break;
                    case SDL_SCANCODE_F6:
                        ide_step_line(s);
                        break;
                    case SDL_SCANCODE_F9:
                        ide_step_frame_once(s);
                        break;
                    case SDL_SCANCODE_GRAVE:
                        /* Backtick — open address entry field */
                        ide_addr_focus(s, true);
                        SDL_StartTextInput(win);
                        break;

                    /* Paint colour 0-3 (top row or numpad) */
                    case SDL_SCANCODE_0: case SDL_SCANCODE_KP_0:
                        ide_set_paint_color(s, 0); break;
                    case SDL_SCANCODE_1: case SDL_SCANCODE_KP_1:
                        ide_set_paint_color(s, 1); break;
                    case SDL_SCANCODE_2: case SDL_SCANCODE_KP_2:
                        ide_set_paint_color(s, 2); break;
                    case SDL_SCANCODE_3: case SDL_SCANCODE_KP_3:
                        ide_set_paint_color(s, 3); break;

                    default: break;
                    }
                }
                break;
            }

            case SDL_EVENT_TEXT_INPUT:
                /* Feed printable chars to the address field when focused. */
                if (ide_addr_focused(s))
                    ide_addr_putc(s, ev.text.text[0]);
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    int mx = (int)ev.button.x;
                    int my = (int)ev.button.y;

                    /* Try debugger panel clicks first (return false when outside). */
                    if (ide_disasm_click(s, mx, my)) {
                        /* breakpoint toggled in disasm panel — done */
                    } else if (ide_memhex_click(s, mx, my)) {
                        /* watchpoint toggled in mem-hex panel — done */
                    }
                    /* VRAM tiles panel → select tile */
                    else if (mx >= vr_x && mx < vr_x + vr_w &&
                             my >= vr_y && my < vr_y + vr_h) {
                        ide_select_tile_at(s, mx, my);
                    }
                    /* Tile editor panel → select colour swatch / paint pixel */
                    else if (mx >= te_x && mx < te_x + te_w &&
                             my >= te_y && my < te_y + te_h) {
                        ide_mouse_paint(s, mx, my);
                    }
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                /* Brush: paint continuously while the left button is held and
                 * the cursor is over the tile editor. Uses ide_paint_at so a
                 * drag across the colour swatches doesn't change the colour. */
                if (ev.motion.state & SDL_BUTTON_LMASK) {
                    int mx = (int)ev.motion.x;
                    int my = (int)ev.motion.y;
                    if (mx >= te_x && mx < te_x + te_w &&
                        my >= te_y && my < te_y + te_h) {
                        ide_paint_at(s, mx, my);
                    }
                }
                break;

            default:
                break;
            }
        }

        /* Joypad from held keys */
        int nkeys = 0;
        const bool *ks = SDL_GetKeyboardState(&nkeys);
        gb_set_buttons(ide_gb(s), poll_buttons(ks));

        /* Step emulator (respects exec mode: running/paused/step) */
        ide_run_slice(s);

        /* Render IDE panels into canvas */
        ide_render(s, &canvas);

        /* Upload canvas pixels to GPU texture and present */
        SDL_UpdateTexture(tex, NULL, canvas.px, CANVAS_W * 4);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        SDL_Delay((Uint32)frame_ms);
    }

    canvas_free(&canvas);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    ide_free(s);
    return 0;
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage:\n"
                "  %s --ide-shot <in.asm|in.gb> <out.png> [frames]\n"
                "  %s <file.asm|file.gb>\n",
                argv[0], argv[0]);
        return 2;
    }

    /* Headless screenshot mode */
    if (strcmp(argv[1], "--ide-shot") == 0) {
        if (argc < 4) {
            fprintf(stderr, "--ide-shot needs <in> <out.png> [frames]\n");
            return 2;
        }
        const char *in_path  = argv[2];
        const char *out_png  = argv[3];
        int         frames   = (argc > 4) ? atoi(argv[4]) : 10;
        if (frames < 0) frames = 0;

        int rc = ide_shot(in_path, out_png, frames);
        if (rc == 0)
            printf("wrote %s (%d frames)\n", out_png, frames);
        else
            fprintf(stderr, "ide_shot failed (rc=%d)\n", rc);
        return rc;
    }

    /* Interactive mode */
    return run_interactive(argv[1]);
}
