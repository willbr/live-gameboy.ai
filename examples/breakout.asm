; breakout.asm — Breakout for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/breakout.asm
; 2. EDIT THE LEVEL LIVE: the brick field IS the BG tilemap. Open the
;    BG MAP panel (or paint VRAM tile 3) — knock out a brick by hand, or
;    repaint tile 3 (brick.2bpp) to reskin every brick at once.
; 3. HOT-RELOAD PHYSICS (F5): UpdateBall holds the bounce math; edit the
;    reflect values / speed and press F5 mid-game.
; 4. F8 (soft reset) re-runs InitBricks to refill the field.
;
; Controls: Left/Right move the paddle.

SECTION "code", ROM0

Main:
    ld sp, $FFFE
    xor a
    ldh ($40), a             ; LCD off

    ; tiles: 1=ball @ $8010, 2=paddle @ $8020, 3=brick @ $8030
    ld hl, .BallTile
    ld de, $8010
    ld bc, $0010
    call .CopyBC
    ld hl, .PaddleTile
    ld de, $8020
    ld bc, $0010
    call .CopyBC
    ld hl, .BrickTile
    ld de, $8030
    ld bc, $0010
    call .CopyBC

    ; clear tilemap
    ld hl, $9800
    ld bc, $0400
.clrmap:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clrmap

    ld a, $E4
    ldh ($47), a             ; BGP
    ldh ($48), a             ; OBP0

    ; zero OAM shadow
    ld hl, $C000
    ld bc, $00A0
.clroam:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clroam

    ; sprite tiles: ball=1, paddle halves=2
    ld a, 1
    ld ($C002), a
    ld a, 2
    ld ($C006), a
    ld ($C00A), a

    ; vars
    ld a, 80
    ld ($C0A0), a            ; ballX
    ld a, 100
    ld ($C0A1), a            ; ballY
    ld a, 1
    ld ($C0A2), a            ; ballDX
    ld a, $FF
    ld ($C0A3), a            ; ballDY = -1 (upward)
    ld a, 72
    ld ($C0A4), a            ; padX

    call InitBricks

    ld a, $93                ; LCD on, OBJ on, BG on
    ldh ($40), a

.loop:
.waitvbl:
    ldh a, ($44)
    cp $90
    jr nz, .waitvbl

    ld a, $C0
    ldh ($46), a             ; OAM DMA

    call ReadInput
    call UpdateBall
    call DrawSprites

.waitend:
    ldh a, ($44)
    cp $90
    jr z, .waitend
    jr .loop

; helper + tile data live in Main's local scope (called from Main)
.CopyBC:
    ld a, (hl+)
    ld (de), a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .CopyBC
    ret

.BallTile:
    incbin "examples/ball.2bpp"
.PaddleTile:
    incbin "examples/paddle.2bpp"
.BrickTile:
    incbin "examples/brick.2bpp"

; --- fill tilemap rows 2..5 with brick tile (tile 3) ---
InitBricks:
    ld hl, $9840             ; $9800 + 2*32  (row 2)
    ld c, 4                  ; 4 rows
.row:
    ld b, 20                 ; 20 columns visible
.col:
    ld a, 3
    ld (hl+), a
    dec b
    jr nz, .col
    ; advance HL to next row start: 32 - 20 = 12 more
    ld de, 12
    add hl, de
    dec c
    jr nz, .row
    ret

DrawSprites:
    ld a, ($C0A1)
    add a, 16
    ld ($C000), a           ; ball Y
    ld a, ($C0A0)
    add a, 8
    ld ($C001), a           ; ball X
    ; paddle at bottom (OAM Y=152)
    ld a, 152
    ld ($C004), a
    ld ($C008), a
    ld a, ($C0A4)
    add a, 8
    ld ($C005), a           ; paddle-left X
    add a, 8
    ld ($C009), a           ; paddle-right X (8 px right)
    ret

; ====================== GAMEPLAY ======================
ReadInput:
    ld a, $20                ; directions
    ldh ($00), a
    ldh a, ($00)
    ldh a, ($00)
    bit 1, a                 ; Left
    jr nz, .nl
    ld a, ($C0A4)
    cp 2
    jr c, .nl
    dec a
    dec a
    ld ($C0A4), a
.nl:
    ld a, $20
    ldh ($00), a
    ldh a, ($00)
    ldh a, ($00)
    bit 0, a                 ; Right
    jr nz, .nr
    ld a, ($C0A4)
    cp 144
    jr nc, .nr
    inc a
    inc a
    ld ($C0A4), a
.nr:
    ld a, $30
    ldh ($00), a
    ret

UpdateBall:
    ; X axis
    ld a, ($C0A2)
    ld b, a
    ld a, ($C0A0)
    add a, b
    ld ($C0A0), a
    cp 8
    jr nc, .xhi
    ld a, 1
    ld ($C0A2), a
    jr .yax
.xhi:
    cp 152
    jr c, .yax
    ld a, $FF
    ld ($C0A2), a
.yax:
    ; Y axis
    ld a, ($C0A3)
    ld b, a
    ld a, ($C0A1)
    add a, b
    ld ($C0A1), a
    cp 8
    jr nc, .ylo
    ld a, 1
    ld ($C0A3), a
    jr .brick
.ylo:
    cp 140
    jr c, .brick
    ld a, $FF
    ld ($C0A3), a
.brick:
    call .BrickHit
    ret

; map ball (pixel) -> tilemap cell; if it's a brick (tile 3), clear it
; and reflect DY. col = ballX/8, row = ballY/8.
.BrickHit:
    ld a, ($C0A1)           ; ballY
    srl a
    srl a
    srl a                   ; /8 -> row
    ld d, a                 ; row
    ld a, ($C0A0)           ; ballX
    srl a
    srl a
    srl a                   ; /8 -> col
    ld e, a                 ; col
    ; HL = $9800 + row*32 + col
    ld a, d
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl              ; *32
    ld a, e
    ld c, a
    ld b, 0
    add hl, bc
    ld bc, $9800
    add hl, bc
    ld a, (hl)
    cp 3                    ; brick?
    ret nz
    xor a
    ld (hl), a             ; clear brick cell (live tilemap edit by the game)
    ; reflect DY: going up ($FF) -> down (1); going down (1) -> up ($FF)
    ld a, ($C0A3)
    cp 1
    jr z, .setUp
    ld a, 1
    ld ($C0A3), a
    ret
.setUp:
    ld a, $FF
    ld ($C0A3), a
    ret
