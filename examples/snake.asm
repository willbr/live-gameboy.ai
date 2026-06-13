; snake.asm — Snake for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/snake.asm
; 2. HOT-RELOAD THE RULE (F5): StepSnake holds the movement/step rate.
;    Change the "cp 16" throttle in the main loop to speed up / slow down,
;    press F5 — the snake keeps its current length/position.
; 3. RESKIN (paint): paint VRAM tile 1 (body) or tile 2 (food) in the TILE
;    editor — every body/food cell updates live (snakebody/food .2bpp).
; 4. GROW BY HAND (memory edit): bump the length byte at $C003 in the
;    MEMORY panel. Use F8 if you edit init code.
;
; Controls: D-pad steers. Walls wrap (edit StepSnake to make them deadly).

SECTION "code", ROM0

Main:
    ld sp, $FFFE
    xor a
    ldh ($40), a             ; LCD off

    ; tile 1 = body @ $8010, tile 2 = food @ $8020
    ld hl, .BodyTile
    ld de, $8010
    ld bc, $0010
    call .CopyBC
    ld hl, .FoodTile
    ld de, $8020
    ld bc, $0010
    call .CopyBC

    ; clear tilemap to blank tile 0
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

    ; init vars: head at (10,9), moving right, length 3
    ld a, 10
    ld ($C000), a            ; headX
    ld a, 9
    ld ($C001), a            ; headY
    xor a
    ld ($C002), a            ; dir = 0 (right)
    ld a, 3
    ld ($C003), a            ; length
    xor a
    ld ($C004), a            ; tick
    ld a, $13
    ld ($C007), a            ; rng seed (nonzero)

    ; food at (15,9)
    ld a, 15
    ld ($C005), a
    ld a, 9
    ld ($C006), a

    ; draw initial head + food so the boot screen is non-blank
    call DrawHead
    call DrawFood

    ld a, $91                ; LCD on, tiledata $8000, BG on (no sprites)
    ldh ($40), a

.loop:
.waitvbl:
    ldh a, ($44)
    cp $90
    jr nz, .waitvbl

    call ReadInput

    ; --- throttle: only step every 16 frames ---
    ld a, ($C004)
    inc a
    ld ($C004), a
    cp 16
    jr c, .skipstep
    xor a
    ld ($C004), a
    call StepSnake
.skipstep:

.waitend:
    ldh a, ($44)
    cp $90
    jr z, .waitend
    jr .loop

.CopyBC:
    ld a, (hl+)
    ld (de), a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .CopyBC
    ret

.BodyTile:
    incbin "examples/snakebody.2bpp"
.FoodTile:
    incbin "examples/food.2bpp"

; ---- HL = $9800 + headY*32 + headX ----
HeadAddr:
    ld a, ($C001)           ; headY
    ld l, a
    ld h, 0
    add hl, hl              ; *2
    add hl, hl              ; *4
    add hl, hl              ; *8
    add hl, hl              ; *16
    add hl, hl              ; *32
    ld a, ($C000)           ; headX
    ld c, a
    ld b, 0
    add hl, bc
    ld bc, $9800
    add hl, bc
    ret

DrawHead:
    call HeadAddr
    ld a, 1                 ; body tile
    ld (hl), a
    ret

DrawFood:
    ld a, ($C006)           ; foodY
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    ld a, ($C005)           ; foodX
    ld c, a
    ld b, 0
    add hl, bc
    ld bc, $9800
    add hl, bc
    ld a, 2                 ; food tile
    ld (hl), a
    ret

; ====================== GAMEPLAY (stubs, filled in Task 5) ======================
ReadInput:
    ret
StepSnake:
    ret
