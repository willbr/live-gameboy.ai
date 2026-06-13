#include "gb.h"

/* timer.c owns FF04-FF07 */
uint8_t gb_timer_read(GB *gb, uint16_t addr);
void    gb_timer_write(GB *gb, uint16_t addr, uint8_t v);

static uint8_t io_read(GB *gb, uint8_t r) {
    switch (r) {
    case 0x04: case 0x05: case 0x06: case 0x07:
        return gb_timer_read(gb, 0xFF00 | r);
    case 0x0F: return gb->iflag | 0xE0;
    case 0x44: return 0x90;  /* TODO(milestone-2): real PPU LY. Stub reports VBlank so ROMs progress */
    default:   return gb->io[r];
    }
}

static void io_write(GB *gb, uint8_t r, uint8_t v) {
    switch (r) {
    case 0x01: gb->io[0x01] = v; break;                       /* SB */
    case 0x02:                                                 /* SC */
        if (v & 0x80) {  /* transfer start: capture for test ROMs */
            if (gb->serial_len < sizeof gb->serial_buf - 1)
                gb->serial_buf[gb->serial_len++] = (char)gb->io[0x01];
            gb->iflag |= INT_SERIAL;
            v &= 0x7F;
        }
        gb->io[0x02] = v;
        break;
    case 0x04: case 0x05: case 0x06: case 0x07:
        gb_timer_write(gb, 0xFF00 | r, v);
        break;
    case 0x0F: gb->iflag = v & 0x1F; break;
    default:   gb->io[r] = v; break;
    }
}

uint8_t gb_read8(GB *gb, uint16_t a) {
    if (a < 0x4000)  return gb->rom[a];
    if (a < 0x8000) {
        /* Banked ROM: rom_bank selects the 0x4000-0x7FFF window (MBC1 read-banking) */
        uint32_t off = (uint32_t)gb->rom_bank * 0x4000u + (a - 0x4000u);
        if (off < gb->rom_size) return gb->rom[off];
        return 0xFF;
    }
    if (a < 0xA000)  return gb->vram[a - 0x8000];
    if (a < 0xC000)  return 0xFF;                       /* cart RAM: none yet */
    if (a < 0xE000)  return gb->wram[a - 0xC000];
    if (a < 0xFE00)  return gb->wram[a - 0xE000];       /* echo */
    if (a < 0xFEA0)  return gb->oam[a - 0xFE00];
    if (a < 0xFF00)  return 0xFF;                       /* unusable */
    if (a < 0xFF80)  return io_read(gb, a & 0x7F);
    if (a < 0xFFFF)  return gb->hram[a - 0xFF80];
    return gb->ie;
}

void gb_write8(GB *gb, uint16_t a, uint8_t v) {
    /* MBC1 register writes */
    if (a < 0x8000) {
        if (gb->mbc_type >= 0x01 && gb->mbc_type <= 0x03) {   /* MBC1 */
            if (a >= 0x2000 && a < 0x4000) {
                /* ROM bank number (lower 5 bits); 0 -> 1 */
                uint8_t bank = v & 0x1F;
                if (bank == 0) bank = 1;
                gb->rom_bank = bank;
            }
            /* 0x0000-0x1FFF: RAM enable (ignored, no RAM)    */
            /* 0x4000-0x5FFF: upper bits / RAM bank (ignored) */
            /* 0x6000-0x7FFF: mode (ignored)                  */
        }
        return;
    }
    if (a < 0xA000)  { gb->vram[a - 0x8000] = v; return; }
    if (a < 0xC000)  return;
    if (a < 0xE000)  { gb->wram[a - 0xC000] = v; return; }
    if (a < 0xFE00)  { gb->wram[a - 0xE000] = v; return; }
    if (a < 0xFEA0)  { gb->oam[a - 0xFE00] = v; return; }
    if (a < 0xFF00)  return;
    if (a < 0xFF80)  { io_write(gb, a & 0x7F, v); return; }
    if (a < 0xFFFF)  { gb->hram[a - 0xFF80] = v; return; }
    gb->ie = v;
}
