#include "../src/gb/gb.h"
#include "test.h"
#include <string.h>

static GB *fresh(void) {
    static uint8_t r[0x8000];
    memset(r, 0, sizeof r);
    GB *g = gb_new();
    gb_load_rom(g, r, sizeof r);
    gb_reset(g);
    return g;
}

int main(void) {
    static const struct { uint16_t a; uint8_t mask; } M[] = {
        {0xFF10, 0x80}, {0xFF11, 0x3F}, {0xFF12, 0x00}, {0xFF13, 0xFF}, {0xFF14, 0xBF},
        {0xFF16, 0x3F}, {0xFF17, 0x00}, {0xFF18, 0xFF}, {0xFF19, 0xBF},
        {0xFF1A, 0x7F}, {0xFF1B, 0xFF}, {0xFF1C, 0x9F}, {0xFF1D, 0xFF}, {0xFF1E, 0xBF},
        {0xFF20, 0xFF}, {0xFF21, 0x00}, {0xFF22, 0x00}, {0xFF23, 0xBF},
        {0xFF24, 0x00}, {0xFF25, 0x00}, {0xFF26, 0x70}, {0xFF27, 0xFF}, {0xFF2F, 0xFF},
    };

    {   /* write 0x00 then read: result must have all mask bits set */
        GB *g = fresh();
        gb_write8(g, 0xFF26, 0x80);          /* power on so writes land */
        for (unsigned i = 0; i < sizeof M / sizeof M[0]; i++) {
            gb_write8(g, M[i].a, 0x00);
            uint8_t got = gb_read8(g, M[i].a);
            ASSERT_EQ(got & M[i].mask, M[i].mask);
        }
        gb_free(g);
    }

    {   /* NR52 power bit reads back; low bits are channel status */
        GB *g = fresh();
        gb_write8(g, 0xFF26, 0x80);
        ASSERT_EQ(gb_read8(g, 0xFF26) & 0x80, 0x80);
        gb_write8(g, 0xFF26, 0x00);                  /* power off */
        ASSERT_EQ(gb_read8(g, 0xFF26) & 0x80, 0x00);
        gb_free(g);
    }

    {   /* powered off: writes to FF10-FF25 ignored (read back as pure mask) */
        GB *g = fresh();
        gb_write8(g, 0xFF26, 0x00);    /* off */
        gb_write8(g, 0xFF12, 0xF0);
        ASSERT_EQ(gb_read8(g, 0xFF12), 0x00 | 0x00);   /* NR12 mask 0x00, value forced 0 */
        gb_free(g);
    }

    {   /* wave RAM is read/writable (mask 0x00) regardless of power */
        GB *g = fresh();
        gb_write8(g, 0xFF26, 0x00);
        gb_write8(g, 0xFF30, 0xA5);
        ASSERT_EQ(gb_read8(g, 0xFF30), 0xA5);
        gb_free(g);
    }

    {   /* gb_audio_read drains the ring: ticking produces samples */
        GB *g = fresh();
        gb_apu_set_sample_rate(g, 48000);
        /* tick enough cycles to produce at least one sample (4194304/48000 ~ 87 cycles) */
        gb_apu_tick(g, 200);
        float buf[64];
        int n = gb_audio_read(g, buf, 64);
        ASSERT_TRUE(n > 0);
        gb_free(g);
    }

    /* ---- Task 2: Pulse channel tests ---- */

    {   /* Test 1: CH2 square wave oscillates between high and low values */
        GB *g = fresh();
        gb_apu_set_sample_rate(g, 48000);
        /* Power on */
        gb_write8(g, 0xFF26, 0x80);
        /* NR21: duty=50% (bits 7-6 = 10 = 0x80), length=0 (don't care) */
        gb_write8(g, 0xFF16, 0x80);
        /* NR22: volume=15 (bits 7-4 = 1111), direction=up (bit3=0), period=0.
           Bits 7-3 non-zero => DAC on. 0xF0 = vol 15, dir 0, period 0. */
        gb_write8(g, 0xFF17, 0xF0);
        /* NR23: freq lo = 0x00 => freq11 low byte 0 */
        /* NR24: freq hi bits 2-0 = 0x7 => freq11 = 0x700 = 1792.
           Period = (2048-1792)*4 = 256*4 = 1024 T-cycles/step, 8 steps => 8192 T-cycles per period.
           At 48000 samples/sec and 4194304 cycles/sec: samples per period = 8192*(48000/4194304) ~= 94 samples/period.
           Trigger (bit7=1) + length-enable off (bit6=0) + freq hi=7 => 0x87 */
        gb_write8(g, 0xFF18, 0x00);
        gb_write8(g, 0xFF19, 0x87);  /* trigger + freq_hi=7 */
        /* Assert NR52 bit1 (CH2 status) is set after trigger */
        uint8_t nr52 = gb_read8(g, 0xFF26);
        ASSERT_TRUE(nr52 & 0x02);

        /* Tick 2 full periods worth of cycles to get enough samples */
        /* 2 * 8192 = 16384 T-cycles */
        gb_tick(g, 16384);

        /* Drain samples */
        float buf[2048];
        int n = gb_audio_read(g, buf, 2048);
        ASSERT_TRUE(n > 0);

        /* Find a clearly-high and clearly-low sample (mono = left channel = index 0,2,4...) */
        float max_val = -2.0f, min_val = 2.0f;
        for (int i = 0; i < n; i += 2) {  /* stereo interleaved, take left channel */
            if (buf[i] > max_val) max_val = buf[i];
            if (buf[i] < min_val) min_val = buf[i];
        }
        /* volume=15 with /4 mixer: high = ((15/7.5)-1)/4 = 0.25; low = ((0/7.5)-1)/4 = -0.25 */
        ASSERT_TRUE(max_val > 0.1f);   /* clearly high (at least 0.1 above zero) */
        ASSERT_TRUE(min_val < -0.1f);  /* clearly low (at least 0.1 below zero) */
        gb_free(g);
    }

    {   /* Test 2: Length counter disables CH2 after expiry */
        GB *g = fresh();
        gb_apu_set_sample_rate(g, 48000);
        gb_write8(g, 0xFF26, 0x80);
        /* NR21: duty=50% (0x80), length=63 => counter = 64-63 = 1 tick to expire */
        gb_write8(g, 0xFF16, 0x80 | 63);
        /* NR22: vol=15, DAC on */
        gb_write8(g, 0xFF17, 0xF0);
        gb_write8(g, 0xFF18, 0x00);
        /* NR24: trigger(bit7) + length-enable(bit6) + freq_hi=7 => 0xC7 */
        gb_write8(g, 0xFF19, 0xC7);
        /* Channel should be enabled now */
        ASSERT_TRUE(gb_read8(g, 0xFF26) & 0x02);

        /* Need 1 length clock = 1 frame-sequencer step at 256Hz = 8192 T-cycles (512Hz step * 2).
           Actually the frame sequencer runs at 512Hz, and length is clocked at 256Hz.
           Length is clocked at FS steps 0,2,4,6 — every other FS step.
           FS step occurs every 8192 T-cycles (4194304/512 = 8192).
           With length=1, one 256Hz clock (= one length clock event) will expire it.
           Advance enough cycles to guarantee at least one length clock fires.
           2 FS steps = 16384 T-cycles guarantees one length clock. */
        gb_tick(g, 16384 * 2);  /* 4 FS steps = 2 length clocks, definitely expires */
        ASSERT_EQ(gb_read8(g, 0xFF26) & 0x02, 0x00);  /* CH2 must be disabled */
        gb_free(g);
    }

    {   /* Test 3: DAC off (NR22 bits 7-3 all zero) => channel cannot enable */
        GB *g = fresh();
        gb_apu_set_sample_rate(g, 48000);
        gb_write8(g, 0xFF26, 0x80);
        gb_write8(g, 0xFF16, 0x80);
        /* NR22 = 0x00: vol=0, dir=decrease => DAC off */
        gb_write8(g, 0xFF17, 0x00);
        gb_write8(g, 0xFF18, 0x00);
        /* Trigger */
        gb_write8(g, 0xFF19, 0x87);
        /* Channel must NOT be enabled since DAC is off */
        ASSERT_EQ(gb_read8(g, 0xFF26) & 0x02, 0x00);

        /* Tick and check output stays silent */
        gb_tick(g, 8192);
        float buf[512];
        int n = gb_audio_read(g, buf, 512);
        ASSERT_TRUE(n > 0);
        /* All samples must be ~0 (DAC off = silence) */
        int all_silent = 1;
        for (int i = 0; i < n; i++) {
            if (buf[i] > 0.01f || buf[i] < -0.01f) { all_silent = 0; break; }
        }
        ASSERT_TRUE(all_silent);
        gb_free(g);
    }

    {   /* Test 4: Envelope decreases volume over time */
        GB *g = fresh();
        gb_apu_set_sample_rate(g, 48000);
        gb_write8(g, 0xFF26, 0x80);
        /* NR21: duty=50% */
        gb_write8(g, 0xFF16, 0x80);
        /* NR22: vol=15 (0xF0..0xFF), dir=decrease (bit3=0), period=1 (bits 2-0=001) => 0xF1 */
        gb_write8(g, 0xFF17, 0xF1);
        gb_write8(g, 0xFF18, 0x00);
        /* NR24: trigger, no length-enable, freq_hi=7 */
        gb_write8(g, 0xFF19, 0x87);
        ASSERT_TRUE(gb_read8(g, 0xFF26) & 0x02);

        /* Measure initial peak positive sample: at vol=15, duty HIGH gives
           dac_float(15)/4 = +0.25. Duty LOW gives dac_float(0)/4 = -0.25.
           The peak POSITIVE sample reflects the high-duty amplitude. */
        static float drain[8192];
        gb_tick(g, 65536);  /* one env tick: vol 15->14 */
        int n_before = gb_audio_read(g, drain, 8192);
        float max_before = -2.0f;
        for (int i = 0; i < n_before; i += 2) {
            if (drain[i] > max_before) max_before = drain[i];
        }

        /* Envelope period=1: each env tick decreases vol by 1 (64Hz = step 7 of FS).
           FS step at 512Hz = 8192 T-cycles. Env clocked at step 7 = every 8 FS steps.
           8 FS steps * 8192 T-cycles = 65536 T-cycles per env tick.
           Advance 7 more env ticks (draining each time to avoid ring overflow):
           vol goes from 14 down to 7.
           At vol=7: dac_float(7)/4 = (7/7.5-1)/4 ≈ -0.017 (less than +0.14 at vol=14). */
        float max_after = -2.0f;
        for (int ei = 0; ei < 7; ei++) {
            gb_tick(g, 65536);
            int nd = gb_audio_read(g, drain, 8192);
            if (ei == 6) {  /* last tick: vol=7 */
                for (int i = 0; i < nd; i += 2) {
                    if (drain[i] > max_after) max_after = drain[i];
                }
            }
        }

        /* Peak positive amplitude must have decreased significantly.
           max_before ≈ 0.217 (vol=14), max_after ≈ 0.017 (vol=7). */
        ASSERT_TRUE(max_after < max_before - 0.1f);
        gb_free(g);
    }

    /* ---- Task 3: Wave channel CH3 tests ---- */

    {   /* Test 1: CH3 oscillates with alternating wave pattern, NR52 bit2 set */
        GB *g = fresh();
        gb_apu_set_sample_rate(g, 48000);
        gb_write8(g, 0xFF26, 0x80);  /* power on */

        /* Fill wave RAM: alternating 0x00 and 0xFF bytes => samples alternate 0,0,15,15 pattern.
           Actually 0x00 = two 0-samples, 0xFF = two 15-samples: gives 0,0,15,15,...repeating.
           Each byte at FF30+n: 0x00->nibbles(0,0), 0xFF->nibbles(15,15). */
        for (int i = 0; i < 16; i++) {
            gb_write8(g, 0xFF30 + i, (i & 1) ? 0xFF : 0x00);
        }
        /* NR30: DAC on */
        gb_write8(g, 0xFF1A, 0x80);
        /* NR32: output level 100% (bits 6-5 = 01 => 0x20) */
        gb_write8(g, 0xFF1C, 0x20);
        /* NR33/NR34: mid frequency. freq11 = 0x400 = 1024.
           Timer reload = (2048 - 1024) * 2 = 2048 T-cycles per step.
           32 steps per full wave period = 2048 * 32 = 65536 T-cycles per period. */
        gb_write8(g, 0xFF1D, 0x00);         /* NR33: freq lo = 0x00 */
        gb_write8(g, 0xFF1E, 0x84);         /* NR34: trigger(bit7) + freq_hi=4 => freq11=0x400 */

        /* NR52 bit2 should be set after trigger */
        ASSERT_TRUE(gb_read8(g, 0xFF26) & 0x04);

        /* Advance 2 full wave periods to get a good sample spread */
        gb_tick(g, 65536 * 2);

        float buf[4096];
        int n = gb_audio_read(g, buf, 4096);
        ASSERT_TRUE(n > 0);

        /* Find max and min in left channel (even indices) */
        float max_val = -2.0f, min_val = 2.0f;
        for (int i = 0; i < n; i += 2) {
            if (buf[i] > max_val) max_val = buf[i];
            if (buf[i] < min_val) min_val = buf[i];
        }
        /* At 100% level, samples alternate 0 and 15.
           dac_float(0)/4 = -0.25; dac_float(15)/4 = +0.25 (before mixing with silent CH1/CH2).
           Expect output clearly oscillating. */
        ASSERT_TRUE(max_val > 0.1f);   /* high sample present */
        ASSERT_TRUE(min_val < -0.1f);  /* low sample present */
        gb_free(g);
    }

    {   /* Test 2: Output level 50% (NR32=0x40) produces smaller peak-to-peak than 100% (NR32=0x20).
           Uses alternating 0x00/0xFF wave: samples are 0,0,15,15 repeating.
           At 100%: dac_float(0)=-1.0, dac_float(15)=+1.0 => large peak-to-peak.
           At 50%:  dac_float(0>>1=0)=-1.0, dac_float(15>>1=7)=−0.067 => smaller peak-to-peak. */
        float pp_100pct = 0.0f;
        {
            GB *g = fresh();
            gb_apu_set_sample_rate(g, 48000);
            gb_write8(g, 0xFF26, 0x80);
            /* Wave RAM: max contrast — 0x00 and 0xFF alternating */
            for (int i = 0; i < 16; i++)
                gb_write8(g, 0xFF30 + i, (i & 1) ? 0xFF : 0x00);
            gb_write8(g, 0xFF1A, 0x80);
            gb_write8(g, 0xFF1C, 0x20);  /* 100%: bits 6-5 = 01 */
            gb_write8(g, 0xFF1D, 0x00);
            gb_write8(g, 0xFF1E, 0x84);  /* trigger, freq_hi=4 */
            gb_tick(g, 65536 * 2);
            float buf[4096];
            int n = gb_audio_read(g, buf, 4096);
            float hi = -2.0f, lo = 2.0f;
            for (int i = 0; i < n; i += 2) {
                if (buf[i] > hi) hi = buf[i];
                if (buf[i] < lo) lo = buf[i];
            }
            pp_100pct = hi - lo;
            gb_free(g);
        }

        /* Run at 50% */
        float pp_50pct = 0.0f;
        {
            GB *g = fresh();
            gb_apu_set_sample_rate(g, 48000);
            gb_write8(g, 0xFF26, 0x80);
            for (int i = 0; i < 16; i++)
                gb_write8(g, 0xFF30 + i, (i & 1) ? 0xFF : 0x00);
            gb_write8(g, 0xFF1A, 0x80);
            gb_write8(g, 0xFF1C, 0x40);  /* 50%: bits 6-5 = 10 */
            gb_write8(g, 0xFF1D, 0x00);
            gb_write8(g, 0xFF1E, 0x84);  /* trigger, freq_hi=4 */
            gb_tick(g, 65536 * 2);
            float buf[4096];
            int n = gb_audio_read(g, buf, 4096);
            float hi = -2.0f, lo = 2.0f;
            for (int i = 0; i < n; i += 2) {
                if (buf[i] > hi) hi = buf[i];
                if (buf[i] < lo) lo = buf[i];
            }
            pp_50pct = hi - lo;
            gb_free(g);
        }

        /* 50% level shifts samples right by 1 => peak-to-peak is smaller than at 100% */
        ASSERT_TRUE(pp_100pct > 0.1f);               /* 100% has real amplitude */
        ASSERT_TRUE(pp_50pct > 0.0f);                /* 50% has non-zero amplitude */
        ASSERT_TRUE(pp_50pct < pp_100pct - 0.05f);   /* 50% clearly smaller peak-to-peak */
    }

    {   /* Test 3: DAC off (NR30=0x00) + trigger => NR52 bit2 stays 0, output silent */
        GB *g = fresh();
        gb_apu_set_sample_rate(g, 48000);
        gb_write8(g, 0xFF26, 0x80);
        for (int i = 0; i < 16; i++)
            gb_write8(g, 0xFF30 + i, 0xFF);  /* all samples = 15 */
        gb_write8(g, 0xFF1A, 0x00);  /* DAC off */
        gb_write8(g, 0xFF1C, 0x20);  /* 100% level */
        gb_write8(g, 0xFF1D, 0x00);
        gb_write8(g, 0xFF1E, 0x84);  /* trigger attempt */
        /* CH3 must NOT be enabled: DAC is off */
        ASSERT_EQ(gb_read8(g, 0xFF26) & 0x04, 0x00);

        /* Output must be silent */
        gb_tick(g, 65536);
        float buf[4096];
        int n = gb_audio_read(g, buf, 4096);
        ASSERT_TRUE(n > 0);
        int all_silent = 1;
        for (int i = 0; i < n; i++) {
            if (buf[i] > 0.01f || buf[i] < -0.01f) { all_silent = 0; break; }
        }
        ASSERT_TRUE(all_silent);
        gb_free(g);
    }

    {   /* Test 4: Length counter disables CH3 after expiry */
        GB *g = fresh();
        gb_apu_set_sample_rate(g, 48000);
        gb_write8(g, 0xFF26, 0x80);
        /* NR31: length load = 255 => counter = 256 - 255 = 1 (expires after 1 length clock) */
        gb_write8(g, 0xFF1B, 255);
        gb_write8(g, 0xFF1A, 0x80);   /* DAC on */
        gb_write8(g, 0xFF1C, 0x20);   /* 100% level */
        gb_write8(g, 0xFF1D, 0x00);
        /* NR34: trigger(bit7) + length-enable(bit6) + freq_hi=4 => 0xC4 */
        gb_write8(g, 0xFF1E, 0xC4);
        /* Channel should be enabled right after trigger */
        ASSERT_TRUE(gb_read8(g, 0xFF26) & 0x04);

        /* Advance enough T-cycles to fire at least 2 length clocks (= 2 * 8192 * 2 T-cycles).
           Length is clocked at 256Hz: every other FS step (FS steps 0,2,4,6).
           Each FS step = 8192 T-cycles. One length clock = 8192 * 2 T-cycles.
           2 length clocks = 32768 * 2 T-cycles to be safe. */
        gb_tick(g, 65536);
        /* CH3 must be disabled now */
        ASSERT_EQ(gb_read8(g, 0xFF26) & 0x04, 0x00);
        gb_free(g);
    }

    TEST_MAIN_END();
}
