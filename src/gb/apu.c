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

/* Duty cycle waveform table: 4 patterns, 8 steps each.
   0x01 = 00000001 (12.5%), 0x81 = 10000001 (25%),
   0x87 = 10000111 (50% inverted ≡ 12.5% inverted), 0x7E = 01111110 (75%) */
static const uint8_t DUTY_TABLE[4] = { 0x01, 0x81, 0x87, 0x7E };

/* Helper: get duty bit for a channel (duty_pattern index, duty_pos 0..7) */
static int duty_bit(uint8_t pattern, uint8_t pos) {
    return (DUTY_TABLE[pattern] >> pos) & 1;
}

/* Helper: compute freq11 from NRx3 (lo) and NRx4 (hi, bits 2-0) */
static uint16_t pulse_freq(uint8_t nrx3, uint8_t nrx4) {
    return ((uint16_t)(nrx4 & 0x07) << 8) | nrx3;
}

/* Helper: timer reload value from freq11 */
static uint16_t freq_timer_reload(uint16_t freq11) {
    return (2048 - freq11) * 4;
}

/* ---- Frame sequencer clock functions ---- */

static void clock_length(GB *g) {
    /* CH1 */
    if (g->ch1.enabled && (g->apu_reg[0x04] & 0x40)) {  /* NR14 bit6 length-enable */
        if (g->ch1.len > 0) {
            g->ch1.len--;
            if (g->ch1.len == 0) g->ch1.enabled = false;
        }
    }
    /* CH2 */
    if (g->ch2.enabled && (g->apu_reg[0x09] & 0x40)) {  /* NR24 bit6 length-enable */
        if (g->ch2.len > 0) {
            g->ch2.len--;
            if (g->ch2.len == 0) g->ch2.enabled = false;
        }
    }
    /* CH3 */
    if (g->ch3.enabled && (g->apu_reg[0x0E] & 0x40)) {  /* NR34 bit6 length-enable */
        if (g->ch3.len > 0) {
            g->ch3.len--;
            if (g->ch3.len == 0) g->ch3.enabled = false;
        }
    }
    /* CH4 */
    if (g->ch4.enabled && (g->apu_reg[0x13] & 0x40)) {  /* NR44 bit6 length-enable */
        if (g->ch4.len > 0) {
            g->ch4.len--;
            if (g->ch4.len == 0) g->ch4.enabled = false;
        }
    }
}

static void clock_sweep(GB *g) {
    /* CH1 sweep only */
    if (g->ch1.sweep_timer > 0) g->ch1.sweep_timer--;
    if (g->ch1.sweep_timer != 0) return;

    uint8_t nr10 = g->apu_reg[0x00];  /* NR10 = FF10 */
    uint8_t period = (nr10 >> 4) & 0x07;
    uint8_t shift  = nr10 & 0x07;

    /* Reload timer: if period is 0 treat as 8 */
    g->ch1.sweep_timer = period ? period : 8;

    if (!g->ch1.sweep_enabled || period == 0) return;

    /* Compute new frequency */
    uint16_t shadow = g->ch1.sweep_shadow;
    uint16_t delta  = shadow >> shift;
    uint16_t new_freq;
    if (nr10 & 0x08) {
        /* Direction: decrease */
        new_freq = shadow - delta;
    } else {
        new_freq = shadow + delta;
    }

    /* Overflow check: disable if > 2047 */
    if (new_freq > 2047) {
        g->ch1.enabled = false;
        return;
    }

    /* Only write back if shift != 0 */
    if (shift != 0) {
        g->ch1.sweep_shadow = new_freq;
        /* Write back to NR13 / NR14 frequency registers */
        g->apu_reg[0x03] = (uint8_t)(new_freq & 0xFF);       /* NR13 lo */
        g->apu_reg[0x04] = (g->apu_reg[0x04] & 0xF8) | ((new_freq >> 8) & 0x07);  /* NR14 hi */

        /* Second overflow check */
        if (new_freq > 2047) {
            g->ch1.enabled = false;
        }
    }
}

static void clock_env(GB *g) {
    /* CH1 envelope */
    {
        uint8_t nr12 = g->apu_reg[0x02];  /* NR12 */
        uint8_t period = nr12 & 0x07;
        if (period != 0) {
            if (g->ch1.env_timer > 0) g->ch1.env_timer--;
            if (g->ch1.env_timer == 0) {
                g->ch1.env_timer = period;
                if (nr12 & 0x08) {
                    /* Increase */
                    if (g->ch1.vol < 15) g->ch1.vol++;
                } else {
                    /* Decrease */
                    if (g->ch1.vol > 0) g->ch1.vol--;
                }
            }
        }
    }
    /* CH2 envelope */
    {
        uint8_t nr22 = g->apu_reg[0x07];  /* NR22 */
        uint8_t period = nr22 & 0x07;
        if (period != 0) {
            if (g->ch2.env_timer > 0) g->ch2.env_timer--;
            if (g->ch2.env_timer == 0) {
                g->ch2.env_timer = period;
                if (nr22 & 0x08) {
                    /* Increase */
                    if (g->ch2.vol < 15) g->ch2.vol++;
                } else {
                    /* Decrease */
                    if (g->ch2.vol > 0) g->ch2.vol--;
                }
            }
        }
    }
    /* CH4 envelope */
    {
        uint8_t nr42 = g->apu_reg[0x11];  /* NR42 = FF21 */
        uint8_t period = nr42 & 0x07;
        if (period != 0) {
            if (g->ch4.env_timer > 0) g->ch4.env_timer--;
            if (g->ch4.env_timer == 0) {
                g->ch4.env_timer = period;
                if (nr42 & 0x08) {
                    if (g->ch4.vol < 15) g->ch4.vol++;
                } else {
                    if (g->ch4.vol > 0) g->ch4.vol--;
                }
            }
        }
    }
}

/* ---- Trigger helpers ---- */

static void trigger_ch1(GB *g) {
    uint8_t nr12 = g->apu_reg[0x02];
    /* DAC on if NRx2 bits 7-3 != 0 */
    g->ch1.dac = (nr12 & 0xF8) != 0;
    if (!g->ch1.dac) { g->ch1.enabled = false; return; }

    g->ch1.enabled = true;
    /* Reload length if 0 */
    if (g->ch1.len == 0) g->ch1.len = 64;
    /* Reload freq timer */
    uint16_t freq11 = pulse_freq(g->apu_reg[0x03], g->apu_reg[0x04]);
    g->ch1.timer = freq_timer_reload(freq11);
    /* Reload envelope */
    g->ch1.vol = (nr12 >> 4) & 0x0F;
    g->ch1.env_timer = nr12 & 0x07;

    /* Sweep: copy freq to shadow, arm, do initial overflow check */
    g->ch1.sweep_shadow = freq11;
    uint8_t nr10 = g->apu_reg[0x00];
    uint8_t period = (nr10 >> 4) & 0x07;
    uint8_t shift  = nr10 & 0x07;
    g->ch1.sweep_timer = period ? period : 8;
    g->ch1.sweep_enabled = (period != 0) || (shift != 0);

    /* Initial overflow check */
    if (shift != 0) {
        uint16_t delta = freq11 >> shift;
        uint16_t new_freq = (nr10 & 0x08) ? (freq11 - delta) : (freq11 + delta);
        if (new_freq > 2047) g->ch1.enabled = false;
    }
}

static void trigger_ch2(GB *g) {
    uint8_t nr22 = g->apu_reg[0x07];
    /* DAC on if NRx2 bits 7-3 != 0 */
    g->ch2.dac = (nr22 & 0xF8) != 0;
    if (!g->ch2.dac) { g->ch2.enabled = false; return; }

    g->ch2.enabled = true;
    /* Reload length if 0 */
    if (g->ch2.len == 0) g->ch2.len = 64;
    /* Reload freq timer */
    uint16_t freq11 = pulse_freq(g->apu_reg[0x08], g->apu_reg[0x09]);
    g->ch2.timer = freq_timer_reload(freq11);
    /* Reload envelope */
    g->ch2.vol = (nr22 >> 4) & 0x0F;
    g->ch2.env_timer = nr22 & 0x07;
}

/* ---- Per-channel digital output (0..15) ---- */

static int ch1_output(GB *g) {
    if (!g->ch1.enabled || !g->ch1.dac) return 0;
    uint8_t nr11  = g->apu_reg[0x01];
    uint8_t duty  = (nr11 >> 6) & 0x03;
    return duty_bit(duty, g->ch1.duty_pos) ? g->ch1.vol : 0;
}

static int ch2_output(GB *g) {
    if (!g->ch2.enabled || !g->ch2.dac) return 0;
    uint8_t nr21  = g->apu_reg[0x06];
    uint8_t duty  = (nr21 >> 6) & 0x03;
    return duty_bit(duty, g->ch2.duty_pos) ? g->ch2.vol : 0;
}

/* Convert digital 0..15 sample to DAC float [-1, 1].
   Maps 0->-1.0, 15->+1.0 via (vol/7.5) - 1.0 */
static float dac_float(int vol) {
    return (float)vol / 7.5f - 1.0f;
}

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

    /* Handle writes that have side effects */
    switch (addr) {
    case 0xFF12:  /* NR12: CH1 envelope — update DAC status */
        gb->ch1.dac = (v & 0xF8) != 0;
        if (!gb->ch1.dac) gb->ch1.enabled = false;
        break;
    case 0xFF17:  /* NR22: CH2 envelope — update DAC status */
        gb->ch2.dac = (v & 0xF8) != 0;
        if (!gb->ch2.dac) gb->ch2.enabled = false;
        break;
    case 0xFF11:  /* NR11: CH1 length */
        gb->ch1.len = 64 - (v & 0x3F);
        break;
    case 0xFF16:  /* NR21: CH2 length */
        gb->ch2.len = 64 - (v & 0x3F);
        break;
    case 0xFF14:  /* NR14: CH1 trigger */
        if (v & 0x80) trigger_ch1(gb);
        break;
    case 0xFF19:  /* NR24: CH2 trigger */
        if (v & 0x80) trigger_ch2(gb);
        break;
    }
}

void gb_apu_tick(GB *gb, int tcycles) {
    /* Frame sequencer: clocked by falling edge of bit 12 of div16
       (= bit 4 of DIV register = 512 Hz on DMG).
       gb_timer_tick already advanced div16; reconstruct cycle-by-cycle
       from apu_div_prev to find falling edges in this window. */
    double cycles_per_sample = 4194304.0 / (double)gb->audio_sample_rate;

    for (int i = 0; i < tcycles; i++) {
        /* Reconstruct div16 at each step using apu_div_prev as the base.
           apu_div_prev holds the div16 value at the start of this tick window. */
        uint16_t d_before = (uint16_t)(gb->apu_div_prev + (uint16_t)i);
        uint16_t d_after  = (uint16_t)(d_before + 1u);

        uint8_t bit_before = (d_before >> 12) & 1;
        uint8_t bit_after  = (d_after  >> 12) & 1;

        if (bit_before && !bit_after) {
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

        /* Tick CH1 frequency timer */
        if (gb->ch1.enabled) {
            if (gb->ch1.timer > 0) gb->ch1.timer--;
            if (gb->ch1.timer == 0) {
                uint16_t freq11 = pulse_freq(gb->apu_reg[0x03], gb->apu_reg[0x04]);
                gb->ch1.timer = freq_timer_reload(freq11);
                gb->ch1.duty_pos = (gb->ch1.duty_pos + 1) & 7;
            }
        }

        /* Tick CH2 frequency timer */
        if (gb->ch2.enabled) {
            if (gb->ch2.timer > 0) gb->ch2.timer--;
            if (gb->ch2.timer == 0) {
                uint16_t freq11 = pulse_freq(gb->apu_reg[0x08], gb->apu_reg[0x09]);
                gb->ch2.timer = freq_timer_reload(freq11);
                gb->ch2.duty_pos = (gb->ch2.duty_pos + 1) & 7;
            }
        }

        /* Sample output: accumulate cycles and push mono-summed stereo at sample rate */
        gb->audio_accum += 1.0;
        while (gb->audio_accum >= cycles_per_sample) {
            gb->audio_accum -= cycles_per_sample;

            /* Mix channels: mono sum / 4 channels */
            float sample = 0.0f;
            if (gb->ch1.dac) {
                sample += dac_float(ch1_output(gb));
            }
            if (gb->ch2.dac) {
                sample += dac_float(ch2_output(gb));
            }
            /* CH3, CH4 not yet implemented: contribute 0 */

            /* Divide by 4 (total channels) for mono sum, output as stereo */
            float mono = sample / 4.0f;

            int next_head = (gb->audio_head + 2) & (8192 - 1);
            if (next_head != gb->audio_tail) {
                gb->audio_ring[gb->audio_head]     = mono;  /* L */
                gb->audio_ring[gb->audio_head + 1] = mono;  /* R */
                gb->audio_head = next_head;
            }
            /* if full, drop sample */
        }
    }

    /* Update apu_div_prev to match the final div16 after this tick window */
    gb->apu_div_prev = (uint16_t)(gb->apu_div_prev + (uint16_t)tcycles);
}

int gb_audio_read(GB *gb, float *out, int max_samples) {
    int count = 0;
    while (count < max_samples && gb->audio_tail != gb->audio_head) {
        out[count++] = gb->audio_ring[gb->audio_tail];
        gb->audio_tail = (gb->audio_tail + 1) & (8192 - 1);
    }
    return count;
}
