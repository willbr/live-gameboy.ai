; hello.asm — "Hello" serial output demo for gbasm
;
; Writes "Hello" to the Game Boy serial port (FF01/FF02).
; Load the resulting .gb in ./live-gameboy or any emulator;
; serial output appears in the emulator's console if it supports
; the blargg-style serial-to-stdout convention.
;
; Entry layout:
;   The assembled ROM has code at 0x0150 (default org).
;   The header entry point at 0x0100 is auto-patched by the
;   assembler to hold a JP $0150 so the CPU reaches our code.
;   NOTE: this demo relies on a post-assembly JP patch in
;   test_e2e_serial — for a standalone ROM you would use a
;   SECTION "entry",ROM0 at 0x0100 and ds to skip the header.

Main:
    ; --- Send 'H' ---
    ld a, $48
    ldh ($01), a    ; SB = 'H'
    ld a, $81
    ldh ($02), a    ; SC trigger

    ; --- Send 'e' ---
    ld a, $65
    ldh ($01), a
    ld a, $81
    ldh ($02), a

    ; --- Send 'l' ---
    ld a, $6C
    ldh ($01), a
    ld a, $81
    ldh ($02), a

    ; --- Send 'l' ---
    ld a, $6C
    ldh ($01), a
    ld a, $81
    ldh ($02), a

    ; --- Send 'o' ---
    ld a, $6F
    ldh ($01), a
    ld a, $81
    ldh ($02), a

Done:
    jr Done         ; spin forever
