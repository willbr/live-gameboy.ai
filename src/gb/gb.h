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
    uint8_t mbc_type;       /* cart header 0x147 */
    uint8_t rom_bank;       /* current bank mapped at 0x4000-0x7FFF */
    uint8_t vram[0x2000];
    uint8_t wram[0x2000];
    uint8_t oam[0xA0];
    uint8_t hram[0x7F];
    uint8_t io[0x80];      /* raw backing for not-yet-modeled IO regs */
    uint8_t ie, iflag;     /* FFFF, FF0F (iflag upper 3 bits read as 1) */

    /* ppu */
    uint8_t lcdc, stat, scy, scx, ly, lyc, bgp, obp0, obp1, wy, wx;
    int      ppu_mode;        /* 0 HBlank, 1 VBlank, 2 OAM, 3 Draw */
    int      ppu_dot;         /* dot counter within the current scanline (0..455) */
    bool     stat_line;       /* previous STAT interrupt line level (for edge detect) */
    uint8_t  framebuffer[160 * 144];   /* shade 0..3 per pixel */
    int      win_line;        /* window internal line counter */
    bool     frame_ready;     /* set true when a full frame finishes (line 144 reached) */
    /* mode-3 fifo working state (declared here so the whole struct stays serializable) */
    int      fx;              /* current screen X being produced (0..159) in mode 3 */
    int      discard;         /* SCX fine-scroll pixels to discard at start of line */
    int      fetch_step;      /* bg fetcher step 0..7 */
    int      fetch_x;         /* bg fetcher tile-column counter for this line */
    uint8_t  bg_fifo_c[8];    /* bg fifo colors */
    int      bg_fifo_n;       /* bg fifo count */
    bool     window_active;   /* window currently driving the fetcher this line */
    bool     win_started;     /* window was shown on this line (for win_line increment) */
    uint8_t  oam_dma_src;     /* high byte of last DMA source */

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

/* internal: ppu (ppu.c) */
void    gb_ppu_tick(GB *gb, int tcycles);
uint8_t gb_ppu_read(GB *gb, uint16_t addr);   /* FF40-FF4B, FF46 */
void    gb_ppu_write(GB *gb, uint16_t addr, uint8_t v);
bool    gb_ppu_vram_blocked(const GB *gb);     /* true => CPU VRAM access denied */
bool    gb_ppu_oam_blocked(const GB *gb);      /* true => CPU OAM access denied */
void    gb_ppu_reset(GB *gb);
const uint8_t *gb_framebuffer(const GB *gb);    /* 160*144 shades, for the shell */

/* internal: timer (timer.c) */
uint8_t gb_timer_read(GB *gb, uint16_t addr);
void    gb_timer_write(GB *gb, uint16_t addr, uint8_t v);
void    gb_timer_tick(GB *gb, int tcycles);

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
