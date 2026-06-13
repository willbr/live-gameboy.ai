#include "gb.h"
#include <string.h>

/* Read-back OR masks for FF10-FF2F (indexed by addr - 0xFF10).
   Wave RAM FF30-FF3F => index 0x20-0x2F => mask 0x00 (fully readable).
   Unused/reserved ranges FF27-FF2F => 0xFF. */
static const uint8_t APU_MASK[0x30] = {
    /* FF10 */ 0x80,
    /* FF11 */ 0x3F,
    /* FF12 */ 0x00,
    /* FF13 */ 0xFF,
    /* FF14 */ 0xBF,
    /* FF15 (unused CH2 sweep) */ 0xFF,
    /* FF16 */ 0x3F,
    /* FF17 */ 0x00,
    /* FF18 */ 0xFF,
    /* FF19 */ 0xBF,
    /* FF1A */ 0x7F,
    /* FF1B */ 0xFF,
    /* FF1C */ 0x9F,
    /* FF1D */ 0xFF,
    /* FF1E */ 0xBF,
    /* FF1F (unused) */ 0xFF,
    /* FF20 */ 0xFF,
    /* FF21 */ 0x00,
    /* FF22 */ 0x00,
    /* FF23 */ 0xBF,
    /* FF24 */ 0x00,
    /* FF25 */ 0x00,
    /* FF26 */ 0x70,
    /* FF27 */ 0xFF,
    /* FF28 */ 0xFF,
    /* FF29 */ 0xFF,
    /* FF2A */ 0xFF,
    /* FF2B */ 0xFF,
    /* FF2C */ 0xFF,
    /* FF2D */ 0xFF,
    /* FF2E */ 0xFF,
    /* FF2F */ 0xFF,
    /* FF30-FF3F (wave RAM, 16 bytes) — mask 0x00 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* ---- Frame sequencer stubs (Tasks 2-5 will fill these) ---- */
static void clock_length(GB *g) { (void)g; }
static void clock_sweep(GB *g)  { (void)g; }
static void clock_env(GB *g)    { (void)g; }

/* ---- Public API ---- */

void gb_apu_reset(GB *gb) {
    memset(gb->apu_reg, 0, sizeof gb->apu_reg);
    /* DMG post-boot: NR52 = 0xF1 -> power on, CH1 on */
    gb->apu_power = true;
    gb->apu_reg[0x16] = 0xF1;  /* index 0x16 = FF26 - FF10 */
    gb->fs_step = 0;
    gb->apu_div_prev = gb->div16;
    /* CH4 LFSR */
    gb->ch4.lfsr = 0x7FFF;
    /* ring buffer */
    gb->audio_head = 0;
    gb->audio_tail = 0;
    gb->audio_accum = 0.0;
    gb->audio_sample_rate = 48000;
}

void gb_apu_set_sample_rate(GB *gb, int hz) {
    gb->audio_sample_rate = hz;
}

uint8_t gb_apu_read(GB *gb, uint16_t addr) {
    if (addr < 0xFF10 || addr > 0xFF3F) return 0xFF;
    int idx = addr - 0xFF10;

    /* NR52 (FF26) is special: always readable */
    if (addr == 0xFF26) {
        uint8_t nr52 = (gb->apu_power ? 0x80 : 0x00) | 0x70;
        /* channel status bits 0-3 */
        nr52 |= (gb->ch1.enabled ? 0x01 : 0x00);
        nr52 |= (gb->ch2.enabled ? 0x02 : 0x00);
        nr52 |= (gb->ch3.enabled ? 0x04 : 0x00);
        nr52 |= (gb->ch4.enabled ? 0x08 : 0x00);
        return nr52;
    }

    /* Wave RAM (FF30-FF3F): always accessible */
    if (addr >= 0xFF30) {
        return gb->apu_reg[idx];  /* mask 0x00, value fully readable */
    }

    /* Powered off: return mask (value is forced 0) */
    if (!gb->apu_power) {
        return APU_MASK[idx];
    }

    return gb->apu_reg[idx] | APU_MASK[idx];
}

void gb_apu_write(GB *gb, uint16_t addr, uint8_t v) {
    if (addr < 0xFF10 || addr > 0xFF3F) return;
    int idx = addr - 0xFF10;

    /* NR52 (FF26): always writable */
    if (addr == 0xFF26) {
        bool was_on = gb->apu_power;
        gb->apu_power = (v & 0x80) != 0;
        if (was_on && !gb->apu_power) {
            /* Power off: zero FF10-FF25 (indices 0x00-0x15) */
            memset(gb->apu_reg, 0, 0x16);
            /* clear channel enabled states */
            gb->ch1.enabled = false;
            gb->ch2.enabled = false;
            gb->ch3.enabled = false;
            gb->ch4.enabled = false;
        }
        /* Store power bit in backing (bit7); low bits are read-only channel status */
        gb->apu_reg[idx] = v & 0x80;
        return;
    }

    /* Wave RAM (FF30-FF3F): always writable */
    if (addr >= 0xFF30) {
        gb->apu_reg[idx] = v;
        return;
    }

    /* FF10-FF25: ignore when powered off */
    if (!gb->apu_power) return;

    gb->apu_reg[idx] = v;
}

void gb_apu_tick(GB *gb, int tcycles) {
    /* Frame sequencer: clocked by falling edge of (div16 >> 4) & 1 (512 Hz on DMG) */
    for (int i = 0; i < tcycles; i++) {
        /* We advance one T-cycle at a time for edge detection accuracy */
        uint16_t cur_div = gb->div16; /* already updated by timer_tick before apu_tick */
        uint8_t cur_bit  = (cur_div >> 4) & 1;
        uint8_t prev_bit = (gb->apu_div_prev >> 4) & 1;

        if (prev_bit && !cur_bit) {
            /* Falling edge: advance frame sequencer */
            switch (gb->fs_step) {
            case 0: clock_length(gb); break;
            case 1: break;
            case 2: clock_length(gb); clock_sweep(gb); break;
            case 3: break;
            case 4: clock_length(gb); break;
            case 5: break;
            case 6: clock_length(gb); clock_sweep(gb); break;
            case 7: clock_env(gb); break;
            }
            gb->fs_step = (gb->fs_step + 1) & 7;
        }
        gb->apu_div_prev = cur_div;

        /* Sample output: accumulate cycles and push silence at sample rate */
        gb->audio_accum += 1.0;
        double cycles_per_sample = 4194304.0 / (double)gb->audio_sample_rate;
        while (gb->audio_accum >= cycles_per_sample) {
            gb->audio_accum -= cycles_per_sample;
            /* Push stereo pair (L, R) — silence for now */
            int next_head = (gb->audio_head + 2) & (8192 - 1);
            if (next_head != gb->audio_tail) {
                /* ring not full: push silence */
                gb->audio_ring[gb->audio_head]     = 0.0f;
                gb->audio_ring[gb->audio_head + 1] = 0.0f;
                gb->audio_head = next_head;
            }
            /* if full, drop sample (oldest is not overwritten) */
        }
    }
}

int gb_audio_read(GB *gb, float *out, int max_samples) {
    int count = 0;
    while (count < max_samples && gb->audio_tail != gb->audio_head) {
        out[count++] = gb->audio_ring[gb->audio_tail];
        gb->audio_tail = (gb->audio_tail + 1) & (8192 - 1);
    }
    return count;
}
