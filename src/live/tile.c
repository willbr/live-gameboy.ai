/*
 * tile.c — 2bpp Game Boy tile codec + minimal PNG read/write + live painting.
 *
 * PNG support is self-contained (uses only zlib); does NOT depend on
 * src/shell/png.c so that liblive remains shell-independent.
 *
 * PNG write: 8-bit grayscale (color type 0), filter 0 rows, single IDAT.
 * PNG read:  restricted to 8-bit grayscale, filter 0 rows (what we write).
 *            Multiple IDATs are concatenated before inflate.
 */

#include "tile.h"
#include "../gb/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* -----------------------------------------------------------------------
 * 2bpp get / set
 * --------------------------------------------------------------------- */

uint8_t tile2bpp_get(const uint8_t *tile16, int x, int y) {
    uint8_t lo = tile16[y * 2];
    uint8_t hi = tile16[y * 2 + 1];
    int bit = 7 - x;
    return (uint8_t)(((hi >> bit) & 1) << 1 | ((lo >> bit) & 1));
}

void tile2bpp_set(uint8_t *tile16, int x, int y, uint8_t color) {
    int bit = 7 - x;
    uint8_t mask = (uint8_t)(1 << bit);
    /* clear both planes for this pixel */
    tile16[y * 2]     &= (uint8_t)~mask;
    tile16[y * 2 + 1] &= (uint8_t)~mask;
    /* set according to color bits */
    if (color & 1) tile16[y * 2]     |= mask;
    if (color & 2) tile16[y * 2 + 1] |= mask;
}

/* -----------------------------------------------------------------------
 * Minimal PNG helpers
 * --------------------------------------------------------------------- */

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) | (uint32_t)p[3];
}

static int write_chunk(FILE *f, const char tag[4], const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, tag, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return 1;
    if (len && fwrite(data, 1, len, f) != (size_t)len) return 1;
    uLong crc = crc32(0L, (const Bytef *)tag, 4);
    if (len) crc = crc32(crc, (const Bytef *)data, len);
    uint8_t crcb[4];
    put_be32(crcb, (uint32_t)crc);
    return fwrite(crcb, 1, 4, f) != 4;
}

/* color 0..3 → grayscale byte (monotonic: 0=white, 3=black) */
static uint8_t color_to_gray(uint8_t c) {
    static const uint8_t lut[4] = {255, 170, 85, 0};
    return lut[c & 3];
}

/* grayscale byte → nearest color 0..3 */
static uint8_t gray_to_color(uint8_t g) {
    /* thresholds midway between 255/170/85/0 */
    if (g >= 213) return 0;   /* nearest to 255 */
    if (g >= 128) return 1;   /* nearest to 170 */
    if (g >= 43)  return 2;   /* nearest to 85  */
    return 3;                 /* nearest to 0   */
}

/* -----------------------------------------------------------------------
 * tile_sheet_to_png
 * --------------------------------------------------------------------- */

int tile_sheet_to_png(const char *path, const uint8_t *tiles, int ntiles, int tiles_per_row) {
    if (!path || !tiles || ntiles <= 0 || tiles_per_row <= 0) return 1;

    int rows = (ntiles + tiles_per_row - 1) / tiles_per_row;
    int img_w = tiles_per_row * 8;
    int img_h = rows * 8;

    /* build raw scanlines: 1 filter byte + img_w gray bytes per row */
    size_t raw_len = (size_t)img_h * (1 + (size_t)img_w);
    uint8_t *raw = (uint8_t *)calloc(raw_len, 1);
    if (!raw) return 1;

    for (int ty = 0; ty < rows; ty++) {
        for (int tx = 0; tx < tiles_per_row; tx++) {
            int tile_idx = ty * tiles_per_row + tx;
            for (int py = 0; py < 8; py++) {
                int img_y = ty * 8 + py;
                uint8_t *scanline = raw + (size_t)img_y * (1 + (size_t)img_w) + 1; /* skip filter byte */
                for (int px = 0; px < 8; px++) {
                    int img_x = tx * 8 + px;
                    uint8_t c = 0;
                    if (tile_idx < ntiles) {
                        c = tile2bpp_get(tiles + (size_t)tile_idx * 16, px, py);
                    }
                    scanline[img_x] = color_to_gray(c);
                }
            }
        }
    }
    /* filter bytes are already 0 (calloc) */

    uLongf clen = compressBound((uLong)raw_len);
    uint8_t *comp = (uint8_t *)malloc(clen);
    if (!comp) { free(raw); return 1; }

    if (compress2(comp, &clen, raw, (uLong)raw_len, 9) != Z_OK) {
        free(raw); free(comp); return 1;
    }
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) { free(comp); return 1; }

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    put_be32(ihdr,     (uint32_t)img_w);
    put_be32(ihdr + 4, (uint32_t)img_h);
    ihdr[8]  = 8;  /* bit depth */
    ihdr[9]  = 0;  /* color type: grayscale */
    ihdr[10] = 0;  /* compression method */
    ihdr[11] = 0;  /* filter method */
    ihdr[12] = 0;  /* interlace method */
    if (write_chunk(f, "IHDR", ihdr, 13)) { free(comp); fclose(f); return 1; }
    if (write_chunk(f, "IDAT", comp, (uint32_t)clen)) { free(comp); fclose(f); return 1; }
    free(comp);
    if (write_chunk(f, "IEND", NULL, 0)) { fclose(f); return 1; }
    fclose(f);
    return 0;
}

/* -----------------------------------------------------------------------
 * tile_sheet_from_png
 * --------------------------------------------------------------------- */

/* read exactly n bytes from file; return 0 on success */
static int fread_exact(FILE *f, void *buf, size_t n) {
    return fread(buf, 1, n, f) != n;
}

int tile_sheet_from_png(const char *path, uint8_t *tiles, int max_tiles, int *out_ntiles) {
    if (!path || !tiles || max_tiles <= 0 || !out_ntiles) return 1;
    *out_ntiles = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 1;

    /* verify PNG signature */
    uint8_t sig[8];
    if (fread_exact(f, sig, 8)) { fclose(f); return 1; }
    static const uint8_t expected_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (memcmp(sig, expected_sig, 8) != 0) { fclose(f); return 1; }

    int img_w = 0, img_h = 0;
    int color_type = -1, bit_depth = -1;
    int got_ihdr = 0;

    /* collect all IDAT data */
    size_t idat_cap = 0;
    size_t idat_len = 0;
    uint8_t *idat_buf = NULL;

    while (1) {
        uint8_t chunk_hdr[8];
        if (fread_exact(f, chunk_hdr, 8)) break; /* EOF or error */

        uint32_t chunk_len = get_be32(chunk_hdr);
        char tag[5];
        memcpy(tag, chunk_hdr + 4, 4);
        tag[4] = '\0';

        if (strcmp(tag, "IHDR") == 0) {
            if (chunk_len < 13) { free(idat_buf); fclose(f); return 1; }
            uint8_t ihdr[13];
            if (fread_exact(f, ihdr, 13)) { free(idat_buf); fclose(f); return 1; }
            img_w      = (int)get_be32(ihdr);
            img_h      = (int)get_be32(ihdr + 4);
            bit_depth  = ihdr[8];
            color_type = ihdr[9];
            got_ihdr   = 1;
            /* skip remaining bytes + CRC */
            uint32_t skip = chunk_len - 13 + 4;
            if (skip > 0) {
                uint8_t tmp[4];
                /* only need to skip CRC (4 bytes) since chunk_len==13 */
                if (fread_exact(f, tmp, 4)) { free(idat_buf); fclose(f); return 1; }
                (void)tmp;
            }
        } else if (strcmp(tag, "IDAT") == 0) {
            /* accumulate compressed data */
            if (idat_len + chunk_len > idat_cap) {
                size_t new_cap = idat_cap + chunk_len + 65536;
                uint8_t *nb = (uint8_t *)realloc(idat_buf, new_cap);
                if (!nb) { free(idat_buf); fclose(f); return 1; }
                idat_buf = nb;
                idat_cap = new_cap;
            }
            if (chunk_len > 0) {
                if (fread_exact(f, idat_buf + idat_len, chunk_len)) {
                    free(idat_buf); fclose(f); return 1;
                }
                idat_len += chunk_len;
            }
            /* skip CRC */
            uint8_t crc4[4];
            if (fread_exact(f, crc4, 4)) { free(idat_buf); fclose(f); return 1; }
            (void)crc4;
        } else if (strcmp(tag, "IEND") == 0) {
            /* skip CRC */
            uint8_t crc4[4];
            fread_exact(f, crc4, 4);
            break;
        } else {
            /* skip unknown chunk data + CRC */
            long skip = (long)chunk_len + 4;
            if (fseek(f, skip, SEEK_CUR) != 0) { free(idat_buf); fclose(f); return 1; }
        }
    }
    fclose(f);

    if (!got_ihdr || img_w <= 0 || img_h <= 0 || idat_len == 0) {
        free(idat_buf); return 1;
    }
    /* we only support 8-bit grayscale */
    if (bit_depth != 8 || color_type != 0) {
        free(idat_buf); return 1;
    }

    /* inflate: raw scanlines = img_h * (1 + img_w) bytes */
    size_t raw_len = (size_t)img_h * (1 + (size_t)img_w);
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) { free(idat_buf); return 1; }

    uLongf dest_len = (uLongf)raw_len;
    if (uncompress(raw, &dest_len, idat_buf, (uLong)idat_len) != Z_OK
        || dest_len != (uLongf)raw_len) {
        free(idat_buf); free(raw); return 1;
    }
    free(idat_buf);

    /* extract tiles from pixel data */
    int tiles_per_row = img_w / 8;
    int tile_rows     = img_h / 8;
    int ntiles_total  = tiles_per_row * tile_rows;
    if (ntiles_total > max_tiles) ntiles_total = max_tiles;

    memset(tiles, 0, (size_t)ntiles_total * 16);

    for (int ty = 0; ty < tile_rows; ty++) {
        for (int tx = 0; tx < tiles_per_row; tx++) {
            int tile_idx = ty * tiles_per_row + tx;
            if (tile_idx >= ntiles_total) break;
            uint8_t *tile16 = tiles + (size_t)tile_idx * 16;
            for (int py = 0; py < 8; py++) {
                int img_y = ty * 8 + py;
                /* scanline layout: 1 filter byte + img_w pixels */
                const uint8_t *scanline = raw + (size_t)img_y * (1 + (size_t)img_w) + 1;
                /* filter type byte is at offset 0; we only support filter 0 (None) */
                /* (with filter 0 the pixel bytes need no transformation) */
                for (int px = 0; px < 8; px++) {
                    int img_x = tx * 8 + px;
                    uint8_t gray = scanline[img_x];
                    uint8_t c = gray_to_color(gray);
                    tile2bpp_set(tile16, px, py, c);
                }
            }
        }
    }

    free(raw);
    *out_ntiles = ntiles_total;
    return 0;
}

/* -----------------------------------------------------------------------
 * tile_paint — paint a pixel into a live asset, ROM, and VRAM in-place.
 * --------------------------------------------------------------------- */

bool tile_paint(LiveSession *s, const char *asset_path,
                int tile_index, int x, int y, uint8_t color,
                char *err, int errlen)
{
    if (!s || !asset_path || x < 0 || x > 7 || y < 0 || y > 7
           || color > 3) {
        if (err && errlen > 0)
            snprintf(err, (size_t)errlen, "tile_paint: invalid arguments");
        return false;
    }

    AsmResult *r = live_result(s);
    GB        *gb = live_gb(s);
    if (!r || !gb) {
        if (err && errlen > 0)
            snprintf(err, (size_t)errlen, "tile_paint: null session internals");
        return false;
    }

    /* --- Step 2: find the asset by substring match on path --- */
    int ai = -1;
    for (int i = 0; i < r->nassets; i++) {
        if (strstr(r->assets[i].path, asset_path) != NULL ||
            strstr(asset_path, r->assets[i].path) != NULL) {
            ai = i;
            break;
        }
    }
    if (ai < 0) {
        if (err && errlen > 0)
            snprintf(err, (size_t)errlen,
                     "tile_paint: asset '%s' not found in build database",
                     asset_path);
        return false;
    }

    /* --- Step 3: compute affected byte offsets within the asset --- */
    uint32_t base   = (uint32_t)tile_index * 16;
    uint32_t lo_off = base + (uint32_t)y * 2;
    uint32_t hi_off = base + (uint32_t)y * 2 + 1;

    if (hi_off >= (uint32_t)r->assets[ai].size) {
        if (err && errlen > 0)
            snprintf(err, (size_t)errlen,
                     "tile_paint: tile_index %d y %d out of range for asset "
                     "'%s' (size %zu)",
                     tile_index, y, asset_path, r->assets[ai].size);
        return false;
    }

    /* --- Step 4: update the asset bytes via tile2bpp_set --- */
    /* Copy the 16-byte tile into a temp buffer, set the pixel, then write
     * back only the two changed bytes. */
    uint8_t tmp[16];
    memcpy(tmp, r->assets[ai].bytes + base, 16);
    tile2bpp_set(tmp, x, y, color);

    uint8_t new_lo = tmp[y * 2];
    uint8_t new_hi = tmp[y * 2 + 1];

    r->assets[ai].bytes[lo_off] = new_lo;
    r->assets[ai].bytes[hi_off] = new_hi;

    /* --- Steps 5 & 6: propagate to ROM then to VRAM --- */
    /* For each of the two changed asset bytes, scan prov_asset / prov_asset_off
     * over the full ROM to find the matching ROM offsets, then update gb->rom
     * and any VRAM bytes that originated from those ROM offsets. */

    uint32_t changed_asset_offs[2];
    uint8_t  changed_bytes[2];
    changed_asset_offs[0] = lo_off;  changed_bytes[0] = new_lo;
    changed_asset_offs[1] = hi_off;  changed_bytes[1] = new_hi;

    for (int ci = 0; ci < 2; ci++) {
        uint32_t asset_off  = changed_asset_offs[ci];
        uint8_t  new_byte   = changed_bytes[ci];

        /* Scan ROM provenance table for matching entries */
        for (size_t o = 0; o < r->rom_size; o++) {
            if (r->prov_asset[o] == ai &&
                r->prov_asset_off[o] == asset_off) {
                /* Step 5: patch the ROM byte */
                gb->rom[o] = new_byte;

                /* Step 6: patch any VRAM bytes that came from this ROM offset */
                for (int v = 0; v < 0x2000; v++) {
                    if (gb->vram_prov[v] == (uint32_t)o) {
                        gb->vram[v] = new_byte;
                    }
                }
            }
        }
    }

    return true;
}
