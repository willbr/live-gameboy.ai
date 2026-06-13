#include "debug.h"
#include "gb.h"
#include <stdlib.h>
#include <string.h>

GbDebug *gb_debug_attach(GB *gb) {
    if (gb->dbg) return gb->dbg;
    GbDebug *d = (GbDebug *)calloc(1, sizeof(GbDebug));
    gb->dbg = d;
    return d;
}

void gb_debug_detach(GB *gb) {
    free(gb->dbg);
    gb->dbg = NULL;
}

int gb_debug_find_bp(GB *gb, uint8_t bank, uint16_t addr) {
    GbDebug *d = gb->dbg;
    if (!d) return -1;
    bool banked = (addr >= 0x4000 && addr < 0x8000);
    for (int i = 0; i < d->bp_count; i++) {
        if (!d->bp[i].enabled) continue;
        if (d->bp[i].addr != addr) continue;
        if (banked && d->bp[i].bank != bank) continue;
        return i;
    }
    return -1;
}

int gb_debug_toggle_bp(GB *gb, uint8_t bank, uint16_t addr) {
    GbDebug *d = gb->dbg;
    if (!d) return -1;
    int existing = gb_debug_find_bp(gb, bank, addr);
    if (existing >= 0) {
        /* remove by compaction */
        for (int i = existing; i < d->bp_count - 1; i++) d->bp[i] = d->bp[i + 1];
        d->bp_count--;
        return -1;
    }
    if (d->bp_count >= DBG_MAX_BP) return -1;
    d->bp[d->bp_count] = (Breakpoint){ bank, addr, true };
    return d->bp_count++;
}

int gb_debug_add_wp(GB *gb, uint16_t addr, bool on_read, bool on_write) {
    GbDebug *d = gb->dbg;
    if (!d || d->wp_count >= DBG_MAX_WP) return -1;
    d->wp[d->wp_count] = (Watchpoint){ addr, on_read, on_write, true };
    return d->wp_count++;
}

void gb_debug_clear_wp(GB *gb, int index) {
    GbDebug *d = gb->dbg;
    if (!d || index < 0 || index >= d->wp_count) return;
    for (int i = index; i < d->wp_count - 1; i++) d->wp[i] = d->wp[i + 1];
    d->wp_count--;
}

void gb_debug_resume(GB *gb) {
    GbDebug *d = gb->dbg;
    if (!d) return;
    d->hit = false;
    d->hit_kind = DBG_NONE;
    d->skip_next_bp = true;
}

bool gb_debug_check_bp(GB *gb) {
    GbDebug *d = gb->dbg;
    bool skip = d->skip_next_bp;
    d->skip_next_bp = false;
    if (skip || d->bp_count == 0) return false;
    uint16_t pc = gb->cpu.pc;
    int idx = gb_debug_find_bp(gb, gb->rom_bank, pc);
    if (idx < 0) return false;
    d->hit = true;
    d->hit_kind = DBG_BREAKPOINT;
    d->hit_addr = pc;
    d->hit_pc = pc;
    return true;
}

void gb_debug_check_wp(GB *gb, uint16_t addr, bool is_write) {
    GbDebug *d = gb->dbg;
    for (int i = 0; i < d->wp_count; i++) {
        Watchpoint *w = &d->wp[i];
        if (!w->enabled || w->addr != addr) continue;
        if (is_write ? !w->on_write : !w->on_read) continue;
        d->hit = true;
        d->hit_kind = is_write ? DBG_WATCH_WRITE : DBG_WATCH_READ;
        d->hit_addr = addr;
        d->hit_pc = gb->cpu.pc;
        return;
    }
}
