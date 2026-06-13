#ifndef GB_DISASM_H
#define GB_DISASM_H

#include <stdint.h>
struct GB;

/* Decode one SM83 instruction located at CPU address `addr` (read through the
   bus view: bank 0 region direct, 0x4000-0x7FFF uses `bank`). Writes a
   human-readable string into out[0..out_sz-1] and returns the instruction's
   length in bytes (1..3). Never reads more than 3 bytes. */
int gb_disasm(struct GB *gb, uint8_t bank, uint16_t addr, char *out, int out_sz);

#endif /* GB_DISASM_H */
