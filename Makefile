CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -O2 -g
BUILD   = build

GB_SRC  = $(wildcard src/gb/*.c)
GB_OBJ  = $(GB_SRC:src/gb/%.c=$(BUILD)/gb/%.o)

# Assembler library objects — exclude gbasm.c (it defines main())
ASM_SRC = $(filter-out src/asm/gbasm.c,$(wildcard src/asm/*.c))
ASM_OBJ = $(ASM_SRC:src/asm/%.c=$(BUILD)/asm/%.o)

# Live-patching engine objects
LIVE_SRC = $(wildcard src/live/*.c)
LIVE_OBJ = $(LIVE_SRC:src/live/%.c=$(BUILD)/live/%.o)

# IDE UI objects — exclude main.c (doesn't exist yet; filter-out for safety)
IDE_SRC = $(filter-out src/ide/main.c,$(wildcard src/ide/*.c))
IDE_OBJ = $(IDE_SRC:src/ide/%.c=$(BUILD)/ide/%.o)

TESTS   = $(wildcard tests/test_*.c)
TESTBIN = $(TESTS:tests/%.c=$(BUILD)/%)

all: test

$(BUILD)/gb/%.o: src/gb/%.c src/gb/gb.h | $(BUILD)/gb
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/asm/%.o: src/asm/%.c src/asm/asm.h | $(BUILD)/asm
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/live/%.o: src/live/%.c src/live/live.h | $(BUILD)/live
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ide/%.o: src/ide/%.c src/ide/ui.h | $(BUILD)/ide
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%: tests/%.c $(GB_OBJ) $(ASM_OBJ) $(LIVE_OBJ) $(IDE_OBJ) tests/test.h | $(BUILD)
	$(CC) $(CFLAGS) $< $(GB_OBJ) $(ASM_OBJ) $(LIVE_OBJ) $(IDE_OBJ) -lz -o $@

$(BUILD) $(BUILD)/gb $(BUILD)/asm $(BUILD)/live $(BUILD)/ide:
	mkdir -p $@

test: $(TESTBIN)
	@rc=0; for t in $(TESTBIN); do ./$$t || rc=1; done; exit $$rc

$(BUILD)/blargg: tests/blargg.c $(GB_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< $(GB_OBJ) -o $@

$(BUILD)/blargg_sound: tests/blargg_sound.c $(GB_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< $(GB_OBJ) -o $@

roms:
	mkdir -p roms
	test -d roms/gb-test-roms || git clone --depth 1 https://github.com/retrio/gb-test-roms roms/gb-test-roms

blargg: $(BUILD)/blargg
	./$(BUILD)/blargg roms/gb-test-roms/cpu_instrs/cpu_instrs.gb
	./$(BUILD)/blargg roms/gb-test-roms/instr_timing/instr_timing.gb
	./$(BUILD)/blargg roms/gb-test-roms/mem_timing/mem_timing.gb

sound: $(BUILD)/blargg_sound
	./$(BUILD)/blargg_sound "roms/gb-test-roms/dmg_sound/rom_singles/01-registers.gb"

$(BUILD)/dmg_acid2: tests/dmg_acid2.c $(GB_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< $(GB_OBJ) -o $@

acid2: $(BUILD)/dmg_acid2
	./$(BUILD)/dmg_acid2 roms/dmg-acid2.gb

# --- gbasm CLI ---
gbasm: src/asm/gbasm.c $(ASM_OBJ) $(GB_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< $(ASM_OBJ) $(GB_OBJ) -o $@

# --- assembler demo: assemble the hello example ---
asm-demo: gbasm
	./gbasm examples/hello.asm -o $(BUILD)/hello.gb --sym $(BUILD)/hello.sym
	@echo "asm-demo: OK ($(BUILD)/hello.gb)"

# --- SDL shell (separate from the SDL-free core tests) ---
SDL_CFLAGS = $(shell pkg-config --cflags sdl3)
SDL_LIBS   = $(shell pkg-config --libs sdl3)
SHELL_SRC  = $(wildcard src/shell/*.c)

live-gameboy: $(SHELL_SRC) $(GB_OBJ)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(SHELL_SRC) $(GB_OBJ) $(SDL_LIBS) -lz -o $@

# --- IDE shell (SDL3 interactive IDE) ---
live-gameboy-ide: src/ide/main.c $(IDE_OBJ) $(GB_OBJ) $(ASM_OBJ) $(LIVE_OBJ)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) src/ide/main.c $(IDE_OBJ) $(GB_OBJ) $(ASM_OBJ) $(LIVE_OBJ) $(SDL_LIBS) -lz -o $@

# headless screenshot for verification/CI (still links SDL but never opens a window)
shell-shot: live-gameboy
	./live-gameboy --shot roms/dmg-acid2.gb build/shell-acid2.png 60 2

# IDE headless screenshot gate (no window required)
ide-shot: live-gameboy-ide
	./live-gameboy-ide --ide-shot examples/demo.asm build/ide.png 60

clean:
	rm -rf $(BUILD) live-gameboy live-gameboy-ide gbasm

.PHONY: all test blargg roms clean acid2 shell-shot sound gbasm asm-demo live-gameboy-ide ide-shot
