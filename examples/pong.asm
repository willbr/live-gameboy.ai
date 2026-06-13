; pong.asm — Pong for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/pong.asm
; 2. HOT-RELOAD PHYSICS (F5): edit the bounce math / speeds in UpdateBall
;    (in the main-loop zone below), press F5 — the rally keeps going at the
;    new speed, score/positions intact.
; 3. RESKIN (paint): click VRAM tile 1 (ball) or tile 2 (paddle) in the
;    tile viewer, paint it in the TILE editor — the ball/paddle changes on
;    screen live (ball.2bpp / paddle.2bpp are paintable assets).
; 4. TELEPORT (OAM edit): edit sprite 0's Y/X in the OAM panel to move the
;    ball by hand. Use F8 (soft reset) if you change Main/init code.
; ======================================================================
;
; Controls: Up/Down = move left paddle. Right paddle is a simple AI.

SECTION "code", ROM0

Main:
    ld sp, $FFFE

    ; --- LCD off so we can touch VRAM ---
    xor a
    ldh ($40), a              ; LCDC = 0

    ; --- Load ball tile -> $8010 (tile 1), paddle tile -> $8020 (tile 2) ---
    ld hl, .BallTile
    ld de, $8010
    ld bc, $0010
    call .CopyBC
    ld hl, .PaddleTile
    ld de, $8020
    ld bc, $0010
    call .CopyBC

    ; --- Clear BG tilemap $9800..$9BFF to tile 0 (blank court) ---
    ld hl, $9800
    ld bc, $0400
.clrmap:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clrmap

    ; --- Palettes: BGP and OBP0 = $E4 ---
    ld a, $E4
    ldh ($47), a             ; BGP
    ldh ($48), a             ; OBP0

    ; --- Zero the OAM shadow $C000..$C09F (all sprites off-screen) ---
    ld hl, $C000
    ld bc, $00A0
.clroam:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clroam

    ; --- Sprite tiles: ball=tile1, paddles=tile2 ---
    ld a, $01
    ld ($C002), a            ; ball tile
    ld a, $02
    ld ($C006), a            ; Lpad-top tile
    ld ($C00A), a            ; Lpad-bot tile
    ld ($C00E), a            ; Rpad-top tile
    ld ($C012), a            ; Rpad-bot tile

    ; --- Init game vars ---
    ld a, 80
    ld ($C0A0), a            ; ballX
    ld a, 72
    ld ($C0A1), a            ; ballY
    ld a, 1
    ld ($C0A2), a            ; ballDX = +1
    ld a, 1
    ld ($C0A3), a            ; ballDY = +1
    ld a, 64
    ld ($C0A4), a            ; lPadY
    ld a, 64
    ld ($C0A5), a            ; rPadY

    ; --- LCD on: LCDC=$93 (LCD on, tiledata $8000, OBJ on, BG on) ---
    ld a, $93
    ldh ($40), a

; -------------------- MAIN LOOP (F5 hot-reload zone) --------------------
.loop:
.waitvbl:
    ldh a, ($44)             ; LY
    cp $90                   ; 144 == VBlank start
    jr nz, .waitvbl

    ld a, $C0                ; OAM DMA from $C000
    ldh ($46), a

    call ReadInput
    call UpdateAI
    call UpdateBall
    call DrawSprites

.waitend:
    ldh a, ($44)
    cp $90
    jr z, .waitend
    jr .loop

; --- helper: copy BC bytes HL->DE ---
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

; ====================== GAMEPLAY PROCEDURES ======================
; (full bodies added in Task 3; stubs here so the boot test links/draws)

ReadInput:
    ; select the direction keys
    ld a, $20
    ldh ($00), a
    ldh a, ($00)            ; ignore (let it settle)
    ldh a, ($00)
    ; bit2 = Up, bit3 = Down (0 = pressed)
    bit 2, a
    jr nz, .notUp
    ld a, ($C0A4)
    cp 8
    jr c, .notUp            ; clamp at top
    dec a
    dec a                   ; speed 2 px/frame
    ld ($C0A4), a
.notUp:
    ld a, $20
    ldh ($00), a
    ldh a, ($00)
    ldh a, ($00)
    bit 3, a
    jr nz, .notDown
    ld a, ($C0A4)
    cp 120
    jr nc, .notDown         ; clamp at bottom (144-24)
    inc a
    inc a
    ld ($C0A4), a
.notDown:
    ld a, $30
    ldh ($00), a
    ret

UpdateAI:
    ; right paddle chases the ball's Y, 1 px/frame
    ld a, ($C0A1)           ; ballY
    ld b, a
    ld a, ($C0A5)           ; rPadY
    cp b
    jr z, .aiDone
    jr c, .aiDown
    dec a
    ld ($C0A5), a
    ret
.aiDown:
    inc a
    ld ($C0A5), a
.aiDone:
    ret

UpdateBall:
    ; --- X axis ---
    ld a, ($C0A2)           ; ballDX (signed: 1 or $FF)
    ld b, a
    ld a, ($C0A0)           ; ballX
    add a, b
    ld ($C0A0), a
    ; bounce off left paddle zone (ballX <= 24) -> DX = +1
    cp 24
    jr nc, .checkRight
    ld a, 1
    ld ($C0A2), a
    jr .yaxis
.checkRight:
    ; bounce off right paddle zone (ballX >= 144) -> DX = -1 ($FF)
    cp 144
    jr c, .yaxis
    ld a, $FF
    ld ($C0A2), a
.yaxis:
    ; --- Y axis ---
    ld a, ($C0A3)           ; ballDY
    ld b, a
    ld a, ($C0A1)           ; ballY
    add a, b
    ld ($C0A1), a
    ; bounce off top (ballY <= 8) -> DY = +1
    cp 8
    jr nc, .checkBottom
    ld a, 1
    ld ($C0A3), a
    ret
.checkBottom:
    ; bounce off bottom (ballY >= 136) -> DY = -1
    cp 136
    ret c
    ld a, $FF
    ld ($C0A3), a
    ret

DrawSprites:
    ; ball -> sprite 0
    ld a, ($C0A1)            ; ballY
    add a, 16
    ld ($C000), a
    ld a, ($C0A0)            ; ballX
    add a, 8
    ld ($C001), a
    ; left paddle -> sprites 1,2 (X=16)
    ld a, ($C0A4)            ; lPadY
    add a, 16
    ld ($C004), a           ; Lpad-top Y
    add a, 8
    ld ($C008), a           ; Lpad-bot Y (8 px below)
    ld a, 16
    ld ($C005), a           ; Lpad-top X
    ld ($C009), a           ; Lpad-bot X
    ; right paddle -> sprites 3,4 (X=152)
    ld a, ($C0A5)            ; rPadY
    add a, 16
    ld ($C00C), a
    add a, 8
    ld ($C010), a
    ld a, 152
    ld ($C00D), a
    ld ($C011), a
    ret
