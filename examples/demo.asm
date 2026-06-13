; demo.asm — scrolling background demo for live-gameboy IDE
;
; Displays a horizontally-striped tile across the background, then once
; per frame (synced to VBlank) increments a WRAM counter and writes it
; to SCY so the stripes visibly scroll UP the screen.
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
    ; Fill the WHOLE BG tilemap ($9800..$9BFF, 1024 bytes) with tile 0.
    ; NOTE: A is reset to 0 INSIDE the loop because the bc==0 test below
    ; ("ld a,b / or c") clobbers A — writing the tile index from A each
    ; iteration must therefore happen after a fresh "xor a".
    ; ---------------------------------------------------------------
    ld hl, $9800
    ld bc, $0400
.fillmap:
    xor a                 ; tile index 0 (also cleared every iteration)
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .fillmap

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
    ; Main loop: once per frame, increment counter and write it to SCY
    ; so the horizontal stripes visibly scroll UP the screen.
    ;   - SCY (vertical scroll) IS visible on horizontal stripes
    ;     (SCX would be invisible: the pattern is constant along X).
    ;   - We wait for VBlank (LY == 144) each iteration so the scroll
    ;     advances ~60 px/sec instead of cycling 0..255 in microseconds.
    ; ---------------------------------------------------------------
.loop:
.waitvbl:
    ldh a, ($44)          ; LY
    cp $90                ; 144 = first VBlank line
    jr nz, .waitvbl       ; spin until LY == 144

    ld a, ($C000)
    inc a
    ld ($C000), a
    ldh ($42), a          ; SCY = counter -> stripes scroll vertically

.waitend:
    ldh a, ($44)          ; wait for VBlank to end so we update once per frame
    cp $90
    jr z, .waitend
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
