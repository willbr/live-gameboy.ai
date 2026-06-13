#ifndef GB_H
#define GB_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    bool ime;
    uint8_t ime_pending;   /* EI takes effect after the next instruction */
    bool halted;
    bool halt_bug;
} CPU;

typedef struct GB {
    CPU cpu;

    uint8_t *rom;
    size_t rom_size;
    uint8_t vram[0x2000];
    uint8_t wram[0x2000];
    uint8_t oam[0xA0];
    uint8_t hram[0x7F];
    uint8_t io[0x80];      /* raw backing for not-yet-modeled IO regs */
    uint8_t ie, iflag;     /* FFFF, FF0F (iflag upper 3 bits read as 1) */

    /* timer */
    uint16_t div16;        /* internal divider; DIV (FF04) is its high byte */
    uint8_t tima, tma, tac;

    /* serial (test-ROM result channel) */
    char serial_buf[8192];
    size_t serial_len;

    uint64_t cycles;       /* T-cycles since reset */
} GB;

GB  *gb_new(void);
void gb_free(GB *gb);
bool gb_load_rom(GB *gb, const uint8_t *data, size_t size); /* copies data */
void gb_reset(GB *gb);   /* DMG post-boot-ROM register state */
int  gb_step(GB *gb);    /* one instruction or interrupt dispatch; returns T-cycles */
void gb_tick(GB *gb, int tcycles);  /* advance subsystems (timer) */

/* Untimed bus access (tests, future debugger). CPU wraps these with ticks. */
uint8_t gb_read8(GB *gb, uint16_t addr);
void    gb_write8(GB *gb, uint16_t addr, uint8_t v);

/* interrupt bits in IE/IF */
#define INT_VBLANK 0x01
#define INT_STAT   0x02
#define INT_TIMER  0x04
#define INT_SERIAL 0x08
#define INT_JOYPAD 0x10

#endif
