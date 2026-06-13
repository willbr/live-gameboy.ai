#include "gb.h"
#include <stdlib.h>
#include <string.h>

GB *gb_new(void) {
    GB *g = calloc(1, sizeof(GB));
    return g;
}

void gb_free(GB *gb) {
    if (!gb) return;
    free(gb->rom);
    free(gb);
}

bool gb_load_rom(GB *gb, const uint8_t *data, size_t size) {
    if (size < 0x8000) return false;
    free(gb->rom);
    gb->rom = malloc(size);
    if (!gb->rom) return false;
    memcpy(gb->rom, data, size);
    gb->rom_size = size;
    gb->mbc_type = data[0x147];
    gb->rom_bank = 1;
    return true;
}

void gb_reset(GB *gb) {
    CPU *c = &gb->cpu;
    memset(c, 0, sizeof *c);
    c->a = 0x01; c->f = 0xB0;
    c->b = 0x00; c->c = 0x13;
    c->d = 0x00; c->e = 0xD8;
    c->h = 0x01; c->l = 0x4D;
    c->sp = 0xFFFE; c->pc = 0x0100;

    memset(gb->vram, 0, sizeof gb->vram);
    memset(gb->wram, 0, sizeof gb->wram);
    memset(gb->oam, 0, sizeof gb->oam);
    memset(gb->hram, 0, sizeof gb->hram);
    memset(gb->io, 0xFF, sizeof gb->io);
    gb->ie = 0; gb->iflag = 0xE1;
    gb->div16 = 0xABCC;          /* DIV reads 0xAB at PC=0100 on DMG */
    gb->tima = 0; gb->tma = 0; gb->tac = 0xF8;
    gb->serial_len = 0;
    gb->cycles = 0;
    gb->rom_bank = 1;
    gb_ppu_reset(gb);
    gb_joypad_reset(gb);
    gb_apu_reset(gb);
}

void gb_tick(GB *gb, int tcycles) {
    gb->cycles += (uint64_t)tcycles;
    gb_timer_tick(gb, tcycles);
    gb_ppu_tick(gb, tcycles);
    gb_apu_tick(gb, tcycles);
}
