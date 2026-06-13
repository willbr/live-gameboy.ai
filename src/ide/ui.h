/*
 * ui.h — Software UI canvas + 8x8 bitmap font.
 *
 * Color convention: RGBA8888 packed as 0xRRGGBBAA.
 *   r = (rgba >> 24) & 0xFF
 *   g = (rgba >> 16) & 0xFF
 *   b = (rgba >>  8) & 0xFF
 *   a =  rgba        & 0xFF
 *
 * The canvas pixel buffer stores bytes in R, G, B, A order (row-major).
 * Canvas size: w*h*4 bytes.
 */
#ifndef IDE_UI_H
#define IDE_UI_H

#include <stdint.h>

/* RGBA8888 canvas; px[y*w*4 + x*4 + 0] = R, +1 = G, +2 = B, +3 = A */
typedef struct { uint8_t *px; int w, h; } Canvas;

Canvas canvas_new(int w, int h);
void   canvas_free(Canvas *c);

/* Fill entire canvas with color */
void   ui_clear(Canvas *c, uint32_t rgba);

/* Filled rectangle, clipped to canvas */
void   ui_fill_rect(Canvas *c, int x, int y, int w, int h, uint32_t rgba);

/* Outline rectangle (1px border), clipped */
void   ui_rect(Canvas *c, int x, int y, int w, int h, uint32_t rgba);

/* Draw text with 8x8 bitmap font; handles '\n' (advance y+8, reset x) */
void   ui_text(Canvas *c, int x, int y, const char *s, uint32_t fg);
void   ui_text_bg(Canvas *c, int x, int y, const char *s, uint32_t fg, uint32_t bg);

/*
 * Blit a 160x144 Game Boy framebuffer (shade values 0..3) into the canvas at
 * (x,y) with integer scaling.  Each source pixel becomes a scale*scale block
 * coloured by pal[shade].
 */
void   ui_blit_gb(Canvas *c, const uint8_t *fb, int x, int y, int scale,
                  const uint32_t pal[4]);

/* Write an 8-bit RGBA PNG (color type 6) via zlib.  Returns 0 on success. */
int    ui_save_png(const Canvas *c, const char *path);

#endif /* IDE_UI_H */
