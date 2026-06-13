; demo.asm — scrolling background demo for live-gameboy IDE
;
; Displays a recognisable striped tile on the background, then in the
; main loop increments a WRAM counter and writes it to SCX so the
; background visibly scrolls every frame.
;
; Syntax: RGBDS-inspired SM83 as understood by gbasm / asm_assemble().
;   - (addr) for memory indirect
;   - SECTION "name", ROM0 for section directives
;   - Main: is the global entry point (live_new patches 0x100 -> JP Main)

SECTION "code", ROM0

Main:
    ld sp, $FFFE

    ; ---------------------------------------------------------------
    ; Disable LCD so we can safely write VRAM
    ; ---------------------------------------------------------------
    xor a
    ldh ($40), a          ; LCDC = 0 (LCD off)

    ; ---------------------------------------------------------------
    ; Copy 16-byte stripe tile to VRAM $8000 (tile 0)
    ;   Pattern: alternating $FF / $00 rows -> horizontal stripes
    ;   In 2bpp: byte0=plane0 bit7..0, byte1=plane1 bit7..0
    ;   $FF,$FF = all pixels color 3 (dark)
    ;   $00,$00 = all pixels color 0 (light)
    ; ---------------------------------------------------------------
    ld hl, .TileData
    ld de, $8000
    ld bc, $0010
.copyloop:
    ld a, (hl+)
    ld (de), a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .copyloop

    ; ---------------------------------------------------------------
    ; Fill the top two rows of tilemap ($9800..$9800+64) with tile 0
    ; ---------------------------------------------------------------
    ld hl, $9800
    ld bc, $0040
    xor a
.fillmap:
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .fillmap
    xor a                 ; restore A=0 for tile index

    ; ---------------------------------------------------------------
    ; Set BGP = $E4  (color 0=white, 1=lgrey, 2=dgrey, 3=black)
    ; ---------------------------------------------------------------
    ld a, $E4
    ldh ($47), a

    ; ---------------------------------------------------------------
    ; Enable LCD: LCDC=$91 (LCD on, BG tile data $8000, BG map $9800, BG on)
    ; ---------------------------------------------------------------
    ld a, $91
    ldh ($40), a

    ; ---------------------------------------------------------------
    ; Initialise WRAM scroll counter at $C000 to 0
    ; ---------------------------------------------------------------
    xor a
    ld ($C000), a

    ; ---------------------------------------------------------------
    ; Main loop: increment counter, write to SCX, loop forever
    ; ---------------------------------------------------------------
.loop:
    ld a, ($C000)
    inc a
    ld ($C000), a
    ldh ($43), a          ; SCX = counter -> background scrolls
    jr .loop

    ; ---------------------------------------------------------------
    ; Tile data: 8 rows of 2bpp (16 bytes total)
    ;   Rows 0,2,4,6: $FF,$FF -> color 3 (solid dark stripe)
    ;   Rows 1,3,5,7: $00,$00 -> color 0 (solid light stripe)
    ; ---------------------------------------------------------------
.TileData:
    db $FF,$FF   ; row 0 — dark
    db $00,$00   ; row 1 — light
    db $FF,$FF   ; row 2 — dark
    db $00,$00   ; row 3 — light
    db $FF,$FF   ; row 4 — dark
    db $00,$00   ; row 5 — light
    db $FF,$FF   ; row 6 — dark
    db $00,$00   ; row 7 — light
