#ifndef LIVE_TILE_H
#define LIVE_TILE_H

#include <stdint.h>
#include <stdbool.h>
#include "live.h"

/*
 * tile.h — 2bpp Game Boy tile codec + PNG import/export.
 *
 * A Game Boy tile is 16 bytes (8x8 pixels, 2 bits per pixel).
 * Row r uses bytes tile16[r*2] (low-bit plane) and tile16[r*2+1] (high-bit plane).
 * Pixel column x uses bit (7-x) of each plane.
 * color = (hi_bit << 1) | lo_bit  (0..3).
 */

/* tile2bpp_get — return color (0..3) of pixel (x,y) in a 16-byte tile */
uint8_t tile2bpp_get(const uint8_t *tile16, int x, int y);

/* tile2bpp_set — set pixel (x,y) in a 16-byte tile to color (0..3) */
void    tile2bpp_set(uint8_t *tile16, int x, int y, uint8_t color);

/*
 * tile_sheet_to_png — write ntiles tiles (each 16 bytes) to a grayscale PNG.
 * Layout: tiles_per_row columns; rows = ceil(ntiles / tiles_per_row).
 * Image size: (tiles_per_row * 8) x (rows * 8) pixels, 8-bit grayscale.
 * Color mapping: 0->255, 1->170, 2->85, 3->0.
 * Returns 0 on success, non-zero on error.
 */
int tile_sheet_to_png(const char *path, const uint8_t *tiles, int ntiles, int tiles_per_row);

/*
 * tile_sheet_from_png — read a grayscale PNG previously written by tile_sheet_to_png
 * (or any 8-bit grayscale PNG with pixel values matching the 4-shade palette).
 * Fills `tiles` (max_tiles * 16 bytes), sets *out_ntiles.
 * Restricted to: color type 0 (grayscale), 8-bit depth, filter 0 rows.
 * Returns 0 on success, non-zero on error.
 */
int tile_sheet_from_png(const char *path, uint8_t *tiles, int max_tiles, int *out_ntiles);

/*
 * tile_paint — paint one pixel into a live tile asset, propagating the change
 * through the build database (AsmResult), the emulator ROM (GB->rom), and live
 * VRAM (GB->vram / GB->vram_prov) — all without calling live_reload().
 *
 * Parameters:
 *   s           — live session (must be non-NULL)
 *   asset_path  — path of the asset as recorded in the build database; matched
 *                 by substring (strstr) so a basename or full path both work.
 *   tile_index  — 0-based index of the 16-byte tile within the asset.
 *   x, y        — pixel coordinates within the tile (0..7).
 *   color       — new color value (0..3).
 *   err         — buffer to receive a human-readable error message on failure.
 *   errlen      — capacity of err.
 *
 * On success: assets[ai].bytes is updated, every matching ROM offset is
 * patched in gb->rom, and every matching VRAM byte is patched in gb->vram.
 * Returns true.
 *
 * On failure: err is set, no mutation has occurred, returns false.
 */
bool tile_paint(LiveSession *s, const char *asset_path,
                int tile_index, int x, int y, uint8_t color,
                char *err, int errlen);

#endif /* LIVE_TILE_H */
