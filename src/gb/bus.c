#include "gb.h"

/* timer.c owns FF04-FF07 */
uint8_t gb_timer_read(GB *gb, uint16_t addr);
void    gb_timer_write(GB *gb, uint16_t addr, uint8_t v);

static uint8_t io_read(GB *gb, uint8_t r) {
    switch (r) {
    case 0x00: return gb_joypad_read(gb);
    case 0x04: case 0x05: case 0x06: case 0x07:
        return gb_timer_read(gb, 0xFF00 | r);
    case 0x0F: return gb->iflag | 0xE0;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
    case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
    case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26:
    case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2C: case 0x2D: case 0x2E: case 0x2F:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
    case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
    case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        return gb_apu_read(gb, 0xFF00 | r);
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
    case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:
        return gb_ppu_read(gb, 0xFF00 | r);
    default:   return gb->io[r];
    }
}

static void io_write(GB *gb, uint8_t r, uint8_t v) {
    switch (r) {
    case 0x00: gb_joypad_write(gb, v); break;
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
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
    case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
    case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26:
    case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2C: case 0x2D: case 0x2E: case 0x2F:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
    case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
    case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        gb_apu_write(gb, 0xFF00 | r, v); break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
    case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:
        gb_ppu_write(gb, 0xFF00 | r, v); break;
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
    if (a < 0xA000)  return gb_ppu_vram_blocked(gb) ? 0xFF : gb->vram[a - 0x8000];
    if (a < 0xC000)  return gb->cart_ram[a - 0xA000];   /* cart RAM (8KB) */
    if (a < 0xE000)  return gb->wram[a - 0xC000];
    if (a < 0xFE00)  return gb->wram[a - 0xE000];       /* echo */
    if (a < 0xFEA0)  return gb_ppu_oam_blocked(gb) ? 0xFF : gb->oam[a - 0xFE00];
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
    if (a < 0xA000)  { if (!gb_ppu_vram_blocked(gb)) gb->vram[a - 0x8000] = v; return; }
    if (a < 0xC000)  { gb->cart_ram[a - 0xA000] = v; return; }  /* cart RAM (8KB) */
    if (a < 0xE000)  { gb->wram[a - 0xC000] = v; return; }
    if (a < 0xFE00)  { gb->wram[a - 0xE000] = v; return; }
    if (a < 0xFEA0)  { if (!gb_ppu_oam_blocked(gb)) gb->oam[a - 0xFE00] = v; return; }
    if (a < 0xFF00)  return;
    if (a < 0xFF80)  { io_write(gb, a & 0x7F, v); return; }
    if (a < 0xFFFF)  { gb->hram[a - 0xFF80] = v; return; }
    gb->ie = v;
}
