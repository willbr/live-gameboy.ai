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

    TEST_MAIN_END();
}
