#include "test.h"
#include "../src/ide/ide.h"
#include "../src/gb/gb.h"
#include "../src/gb/debug.h"

int main(void) {
    IdeState *s = ide_new("examples/hello.asm");
    ASSERT_TRUE(s != NULL);
    GB *g = ide_gb(s);
    ASSERT_TRUE(g != NULL);
    ASSERT_TRUE(ide_debug(s) != NULL);       /* ide_new attaches a debugger */
    ASSERT_EQ(ide_exec_mode(s), EXEC_RUNNING);

    /* pause, then single-step one instruction: PC must change, mode returns to PAUSED */
    ide_pause(s);
    ASSERT_EQ(ide_exec_mode(s), EXEC_PAUSED);
    uint16_t pc0 = g->cpu.pc;
    uint64_t cyc0 = g->cycles;
    ide_step_insn(s);
    ide_run_slice(s);
    ASSERT_TRUE(g->cpu.pc != pc0);
    ASSERT_TRUE(g->cycles > cyc0);
    ASSERT_EQ(ide_exec_mode(s), EXEC_PAUSED);

    /* step one scanline: ly advances */
    uint8_t ly0 = g->ly;
    ide_step_line(s);
    ide_run_slice(s);
    ASSERT_TRUE(g->ly != ly0 || g->frame_ready);

    ide_free(s);

    /* a write watchpoint pauses a RUNNING session: hello.asm writes serial FF01.
     * Use a fresh session so the serial writes haven't happened yet. */
    {
        IdeState *s2 = ide_new("examples/hello.asm");
        ASSERT_TRUE(s2 != NULL);
        GB *g2 = ide_gb(s2);
        gb_debug_add_wp(g2, 0xFF01, false, true);
        ASSERT_EQ(ide_exec_mode(s2), EXEC_RUNNING);
        int slices = 0;
        while (ide_exec_mode(s2) == EXEC_RUNNING && slices < 600) { ide_run_slice(s2); slices++; }
        ASSERT_EQ(ide_exec_mode(s2), EXEC_PAUSED);
        ASSERT_EQ(ide_debug(s2)->hit_kind, DBG_WATCH_WRITE);
        ASSERT_EQ(ide_debug(s2)->hit_addr, 0xFF01);
        ide_free(s2);
    }
    TEST_MAIN_END();
}
