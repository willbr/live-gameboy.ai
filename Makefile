CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -O2 -g
BUILD   = build

GB_SRC  = $(wildcard src/gb/*.c)
GB_OBJ  = $(GB_SRC:src/gb/%.c=$(BUILD)/gb/%.o)

TESTS   = $(wildcard tests/test_*.c)
TESTBIN = $(TESTS:tests/%.c=$(BUILD)/%)

all: test

$(BUILD)/gb/%.o: src/gb/%.c src/gb/gb.h | $(BUILD)/gb
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%: tests/%.c $(GB_OBJ) tests/test.h | $(BUILD)
	$(CC) $(CFLAGS) $< $(GB_OBJ) -o $@

$(BUILD) $(BUILD)/gb:
	mkdir -p $@

test: $(TESTBIN)
	@rc=0; for t in $(TESTBIN); do ./$$t || rc=1; done; exit $$rc

$(BUILD)/blargg: tests/blargg.c $(GB_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< $(GB_OBJ) -o $@

roms:
	mkdir -p roms
	test -d roms/gb-test-roms || git clone --depth 1 https://github.com/retrio/gb-test-roms roms/gb-test-roms

blargg: $(BUILD)/blargg
	./$(BUILD)/blargg roms/gb-test-roms/cpu_instrs/cpu_instrs.gb
	./$(BUILD)/blargg roms/gb-test-roms/instr_timing/instr_timing.gb

$(BUILD)/dmg_acid2: tests/dmg_acid2.c $(GB_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< $(GB_OBJ) -o $@

acid2: $(BUILD)/dmg_acid2
	./$(BUILD)/dmg_acid2 roms/dmg-acid2.gb

clean:
	rm -rf $(BUILD)

.PHONY: all test blargg roms clean acid2
