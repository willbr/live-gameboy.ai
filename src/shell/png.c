#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void put_be32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}

static int write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return 1;
    if (len && fwrite(data, 1, len, f) != len) return 1;
    uLong crc = crc32(0L, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, (const Bytef *)data, len);
    uint8_t crcb[4]; put_be32(crcb, (uint32_t)crc);
    return fwrite(crcb, 1, 4, f) != 4;
}

int png_write_rgba(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)w); put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8]=8; ihdr[9]=6; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;   /* 8-bit RGBA */
    if (write_chunk(f, "IHDR", ihdr, 13)) { fclose(f); return 1; }

    /* raw scanlines with filter byte 0 */
    size_t raw_len = (size_t)h * (1 + (size_t)w * 4);
    uint8_t *raw = malloc(raw_len);
    if (!raw) { fclose(f); return 1; }
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (1 + (size_t)w * 4);
        row[0] = 0;
        memcpy(row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    uLongf clen = compressBound((uLong)raw_len);
    uint8_t *comp = malloc(clen);
    if (!comp || compress2(comp, &clen, raw, (uLong)raw_len, 9) != Z_OK) {
        free(raw); free(comp); fclose(f); return 1;
    }
    free(raw);
    int rc = write_chunk(f, "IDAT", comp, (uint32_t)clen);
    free(comp);
    if (rc) { fclose(f); return 1; }
    if (write_chunk(f, "IEND", NULL, 0)) { fclose(f); return 1; }
    fclose(f);
    return 0;
}
