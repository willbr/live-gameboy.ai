#ifndef GB_DEBUG_H
#define GB_DEBUG_H

#include <stdint.h>
#include <stdbool.h>

struct GB;  /* forward decl; debug.c includes gb.h */

#define DBG_MAX_BP 32
#define DBG_MAX_WP 32

typedef struct { uint8_t bank; uint16_t addr; bool enabled; } Breakpoint;
typedef struct { uint16_t addr; bool on_read, on_write, enabled; } Watchpoint;

typedef enum {
    DBG_NONE = 0,
    DBG_BREAKPOINT,
    DBG_WATCH_READ,
    DBG_WATCH_WRITE
} DbgHitKind;

typedef struct GbDebug {
    Breakpoint bp[DBG_MAX_BP]; int bp_count;
    Watchpoint wp[DBG_MAX_WP]; int wp_count;

    bool       hit;          /* set by a check; cleared by gb_debug_resume */
    DbgHitKind hit_kind;
    uint16_t   hit_addr;     /* breakpoint addr or watched addr */
    uint16_t   hit_pc;       /* PC at the moment of the hit */

    bool       skip_next_bp; /* one-shot: don't break on the bp at the current PC */
} GbDebug;

/* Lifecycle: attach allocates and wires gb->dbg; detach frees and NULLs it. */
GbDebug *gb_debug_attach(struct GB *gb);
void     gb_debug_detach(struct GB *gb);

/* Breakpoints. toggle adds if absent (returns index >= 0) or removes if present
   (returns -1). add returns -1 if the table is full. bank is ignored for
   addresses outside 0x4000-0x7FFF. */
int  gb_debug_toggle_bp(struct GB *gb, uint8_t bank, uint16_t addr);
int  gb_debug_find_bp(struct GB *gb, uint8_t bank, uint16_t addr); /* index or -1 */

/* Watchpoints. add returns index or -1 if full. */
int  gb_debug_add_wp(struct GB *gb, uint16_t addr, bool on_read, bool on_write);
void gb_debug_clear_wp(struct GB *gb, int index);

/* Clear a pending hit and arm a one-shot skip so a resume/step from a bp
   address executes that instruction instead of immediately re-breaking. */
void gb_debug_resume(struct GB *gb);

/* Hooks called from the hot path (cpu.c / bus.c). Return true from check_bp to
   tell gb_step to pause WITHOUT executing the current instruction. */
bool gb_debug_check_bp(struct GB *gb);
void gb_debug_check_wp(struct GB *gb, uint16_t addr, bool is_write);

#endif /* GB_DEBUG_H */
