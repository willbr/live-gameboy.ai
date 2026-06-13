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
; 5. TUNE THE SOUND (F5): edit the EAT-FOOD pitch in StepSnake or the
;    WRAP pitch in SfxWrap, press F5, hear it change.
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

    ; --- Sound: power on APU, full volume, route all channels both sides ---
    ld a, $80
    ldh ($26), a             ; NR52 = APU on (write FIRST)
    ld a, $77
    ldh ($24), a             ; NR50 = master volume L/R
    ld a, $FF
    ldh ($25), a             ; NR51 = all channels to both speakers

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
    ld a, $20                ; select directions
    ldh ($00), a
    ldh a, ($00)            ; settle
    ldh a, ($00)
    ld b, a                 ; preserve joypad read (handlers clobber a)
    ld a, $30
    ldh ($00), a            ; deselect
    ; bit0 Right,1 Left,2 Up,3 Down (0 = pressed)
    bit 0, b
    jr nz, .nr
    xor a                    ; dir 0 = right
    ld ($C002), a
    ret
.nr:
    bit 1, b
    jr nz, .nl
    ld a, 1
    ld ($C002), a
    ret
.nl:
    bit 2, b
    jr nz, .nu
    ld a, 2
    ld ($C002), a
    ret
.nu:
    bit 3, b
    jr nz, .nd
    ld a, 3
    ld ($C002), a
.nd:
    ret

StepSnake:
    ; 0) erase the OLD head cell first ($C000/$C001 still hold the old head).
    ;    HeadAddr clobbers bc, so do this BEFORE computing the new head into bc.
    call HeadAddr
    xor a
    ld (hl), a
    ; 1) compute next head from dir into b (x), c (y)
    ld a, ($C000)           ; headX
    ld b, a
    ld a, ($C001)           ; headY
    ld c, a
    ld a, ($C002)           ; dir
    cp 0
    jr nz, .notR
    inc b
    jr .applied
.notR:
    cp 1
    jr nz, .notL
    dec b
    jr .applied
.notL:
    cp 2
    jr nz, .notU
    dec c
    jr .applied
.notU:
    inc c                   ; dir 3 = down
.applied:
    ; 2) wrap walls: x in 0..19, y in 0..17 (underflow shows as >=200)
    ld a, b
    cp 20
    jr c, .xok
    cp 200
    jr c, .xhi
    ld b, 19
    call SfxWrap
    jr .xok
.xhi:
    ld b, 0
    call SfxWrap
.xok:
    ld a, c
    cp 18
    jr c, .yok
    cp 200
    jr c, .yhi
    ld c, 17
    call SfxWrap
    jr .yok
.yhi:
    ld c, 0
    call SfxWrap
.yok:
    ; 4) commit new head
    ld a, b
    ld ($C000), a
    ld a, c
    ld ($C001), a
    ; 5) eat food?
    ld a, ($C005)
    cp b
    jr nz, .noFood
    ld a, ($C006)
    cp c
    jr nz, .noFood
    ld a, ($C003)
    inc a
    ld ($C003), a           ; grow (length byte; visible in MEMORY panel)
    call .NewFood
    ld de, $0780            ; EAT-FOOD chime (~1024 Hz)    ; TWEAK + F5
    call SfxTone
.noFood:
    call DrawHead
    ret

; cheap pseudo-RNG -> new food cell. Uses $C007 seed.
.NewFood:
    ld a, ($C007)
    add a, a
    add a, 5
    xor 173
    ld ($C007), a
    and $1F
.modx:
    cp 20
    jr c, .xdone
    sub 20
    jr .modx
.xdone:
    ld ($C005), a
    ld a, ($C007)
    add a, 7
    ld ($C007), a
    and $1F
.mody:
    cp 18
    jr c, .ydone
    sub 18
    jr .mody
.ydone:
    ld ($C006), a
    call DrawFood
    ret

; ====================== SOUND ======================
; SfxTone — short CH1 (pulse) blip. In: D=freq hi (bits2-0), E=freq lo.
; Clobbers A; reads (preserves) D,E; preserves B,C,HL.
SfxTone:
    xor a
    ldh ($10), a             ; NR10 = no sweep
    ld a, $A0
    ldh ($11), a             ; NR11 = duty 50% + length
    ld a, $F2
    ldh ($12), a             ; NR12 = vol 15, decay (DAC on)
    ld a, e
    ldh ($13), a             ; NR13 = freq lo
    ld a, d
    or $C0
    ldh ($14), a             ; NR14 = trigger + length-enable + freq hi
    ret

; SfxWrap — soft low blip when the snake wraps a wall. Clobbers A,D,E.
SfxWrap:
    ld de, $0400             ; WRAP pitch (~128 Hz)        ; TWEAK + F5
    call SfxTone
    ret
