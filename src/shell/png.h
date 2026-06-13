#ifndef SHELL_PNG_H
#define SHELL_PNG_H
#include <stdint.h>
#include <stddef.h>
/* Write an RGBA8888 buffer (w*h*4 bytes, row-major) as a PNG file.
   Returns 0 on success, nonzero on error. */
int png_write_rgba(const char *path, const uint8_t *rgba, int w, int h);
#endif
