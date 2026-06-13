#include "gb.h"

uint8_t gb_timer_read(GB *gb, uint16_t addr) { (void)gb; (void)addr; return 0xFF; }
void    gb_timer_write(GB *gb, uint16_t addr, uint8_t v) { (void)gb; (void)addr; (void)v; }
void    gb_timer_tick(GB *gb, int tcycles) { (void)gb; (void)tcycles; }
