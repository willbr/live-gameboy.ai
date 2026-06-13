#include "test.h"
#include "../src/gb/gb.h"
#include "../src/gb/disasm.h"
#include <string.h>
#include <stdio.h>

static GB *with_rom(const uint8_t *bytes, int n) {
    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof rom);
    memcpy(rom + 0x0150, bytes, (size_t)n);
    GB *g = gb_new();
    gb_load_rom(g, rom, sizeof rom);
    gb_reset(g);
    return g;
}

static void expect(const uint8_t *bytes, int n, const char *text, int len) {
    GB *g = with_rom(bytes, n);
    char out[32];
    int l = gb_disasm(g, 0, 0x0150, out, sizeof out);
    if (strcmp(out, text) != 0) { fprintf(stderr, "got '%s' want '%s'\n", out, text); }
    ASSERT_TRUE(strcmp(out, text) == 0);
    ASSERT_EQ(l, len);
    gb_free(g);
}

int main(void) {
    expect((uint8_t[]){0x00}, 1, "NOP", 1);
    expect((uint8_t[]){0x3C}, 1, "INC A", 1);
    expect((uint8_t[]){0x76}, 1, "HALT", 1);
    expect((uint8_t[]){0x47}, 1, "LD B,A", 1);
    expect((uint8_t[]){0x3E,0xAA}, 2, "LD A,$AA", 2);
    expect((uint8_t[]){0x01,0x34,0x12}, 3, "LD BC,$1234", 3);
    expect((uint8_t[]){0xC3,0x50,0x01}, 3, "JP $0150", 3);
    expect((uint8_t[]){0x18,0xFE}, 2, "JR $0150", 2);   /* -2 from end of insn (0x0152) */
    expect((uint8_t[]){0xCA,0x00,0x40}, 3, "JP Z,$4000", 3);
    expect((uint8_t[]){0xEA,0x00,0xC0}, 3, "LD ($C000),A", 3);
    expect((uint8_t[]){0xF0,0x44}, 2, "LDH A,($FF44)", 2);
    expect((uint8_t[]){0xCB,0x11}, 2, "RL C", 2);
    expect((uint8_t[]){0xCB,0x7E}, 2, "BIT 7,(HL)", 2);
    expect((uint8_t[]){0xC7}, 1, "RST $00", 1);
    TEST_MAIN_END();
}
