; snake.asm — Snake for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/snake.asm
; 2. HOT-RELOAD THE RULE (F5): StepSnake holds the movement/step rate.
;    Change the "cp 16" throttle in the main loop to speed up / slow down,
;    press F5 — the snake keeps its current length/position.
; 3. RESKIN (paint): paint any snake tile in the TILE editor and every matching
;    cell updates live — 1 body-H, 2 apple, 3-6 head (R/L/U/D), 7-10 tail
;    (R/L/U/D), 11 body-V, 12-15 corners (RU/RD/LU/LD). The snake is drawn as a
;    tube: straight pieces plus bends at every turn. (snakebodyh/v, apple,
;    snakehead_*, snaketail_*, snakecorner_* .2bpp)
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

    ; tiles 1..15 -> $8010 (16 bytes each, contiguous):
    ;   1 body-H  2 apple  3-6 head R/L/U/D  7-10 tail R/L/U/D
    ;   11 body-V  12-15 corners RU/RD/LU/LD
    ld hl, .Tiles
    ld de, $8010
    ld bc, $00F0             ; 15 tiles * 16 bytes
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

    ; --- body ring buffer ($C100, 256 entries x 2 bytes = the tilemap address
    ;     of each segment). $C008 = head index. Fill every slot with a harmless
    ;     off-screen scratch cell ($9BFF) so a tail-erase never corrupts the map
    ;     even right after a hand-edit of the length byte. ---
    ld hl, $C100
    ld bc, $0200
.fillbuf:
    ld a, $FF
    ld (hl+), a              ; addr lo
    ld a, $9B
    ld (hl+), a              ; addr hi
    dec bc
    dec bc
    ld a, b
    or c
    jr nz, .fillbuf

    ; seed the starting snake tail->head: (8,9)(9,9)(10,9), head index = 2.
    ; dir is already 0 (right), so every seed records dir=right.
    ld b, 8                  ; slot 0 = tail
    ld c, 9
    xor a
    call SetSeg
    xor a
    call SlotCellAddr
    ld a, 7                  ; tail-right tile
    ld (hl), a
    ld b, 9                  ; slot 1 = body
    ld c, 9
    ld a, 1
    call SetSeg
    ld a, 1
    call SlotCellAddr
    ld a, 1                  ; body tile
    ld (hl), a
    ld b, 10                 ; slot 2 = head
    ld c, 9
    ld a, 2
    call SetSeg
    ld a, 2
    call SlotCellAddr
    ld a, 3                  ; head-right tile
    ld (hl), a
    ld a, 2
    ld ($C008), a            ; headIdx

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

.Tiles:
    incbin "examples/snakebodyh.2bpp"    ; 1  body horizontal
    incbin "examples/apple.2bpp"         ; 2  apple
    incbin "examples/snakehead_r.2bpp"   ; 3  head right
    incbin "examples/snakehead_l.2bpp"   ; 4  head left
    incbin "examples/snakehead_u.2bpp"   ; 5  head up
    incbin "examples/snakehead_d.2bpp"   ; 6  head down
    incbin "examples/snaketail_r.2bpp"   ; 7  tail right
    incbin "examples/snaketail_l.2bpp"   ; 8  tail left
    incbin "examples/snaketail_u.2bpp"   ; 9  tail up
    incbin "examples/snaketail_d.2bpp"   ; 10 tail down
    incbin "examples/snakebodyv.2bpp"    ; 11 body vertical
    incbin "examples/snakecorner_ru.2bpp"; 12 corner right+up
    incbin "examples/snakecorner_rd.2bpp"; 13 corner right+down
    incbin "examples/snakecorner_lu.2bpp"; 14 corner left+up
    incbin "examples/snakecorner_ld.2bpp"; 15 corner left+down
; ---- BodyLUT[d_in*4 + d_out] -> body tile (1=H 11=V 12=RU 13=RD 14=LU 15=LD)
;      d_in = direction this cell was entered, d_out = direction it is left.
;      (dirs: R=0 L=1 U=2 D=3; opposite-pair entries are unreachable.) ----
BodyLUT:
    db 1, 1, 14, 15          ; d_in=R: RR=H RL=- RU=LU RD=LD
    db 1, 1, 12, 13          ; d_in=L: LR=- LL=H LU=RU LD=RD
    db 13, 15, 11, 11        ; d_in=U: UR=RD UL=LD UU=V UD=-
    db 12, 14, 11, 11        ; d_in=D: DR=RU DL=LU DU=- DD=V

; ---- CellAddr: HL = $9800 + C*32 + B  (B=x, C=y). Clobbers A; preserves BC. ----
CellAddr:
    ld a, c
    ld l, a
    ld h, 0
    add hl, hl              ; *2
    add hl, hl              ; *4
    add hl, hl              ; *8
    add hl, hl              ; *16
    add hl, hl              ; *32 -> y*32
    push bc
    ld a, b
    ld c, a
    ld b, 0
    add hl, bc             ; + x
    ld bc, $9800
    add hl, bc             ; + base
    pop bc
    ret

; ---- SetSeg: record a segment. In: A=ring slot, B=x, C=y. Stores the cell
;      address (addrbuf $C100, 2B/slot) and current dir ($C002 -> dirbuf $C300,
;      1B/slot). Does NOT draw. Clobbers A,D,E,HL; preserves B,C. ----
SetSeg:
    push af                 ; slot
    call CellAddr           ; HL = cell address (preserves B,C)
    ld d, h
    ld e, l                 ; DE = cell address
    pop af                  ; slot
    push af
    ld l, a
    ld h, 0
    add hl, hl              ; slot*2
    push bc
    ld bc, $C100
    add hl, bc             ; HL = &addrbuf[slot]
    pop bc
    ld a, e
    ld (hl+), a             ; addr lo
    ld a, d
    ld (hl), a              ; addr hi
    pop af                  ; slot
    ld l, a
    ld h, 0
    push bc
    ld bc, $C300
    add hl, bc             ; HL = &dirbuf[slot]
    pop bc
    ld a, ($C002)           ; current dir
    ld (hl), a
    ret

; ---- SlotCellAddr: In A=slot -> HL = that slot's tilemap address (from
;      addrbuf). Clobbers A,D,E; preserves B,C. ----
SlotCellAddr:
    ld e, a
    ld d, 0
    ld hl, $C100
    add hl, de
    add hl, de             ; HL = &addrbuf[slot]
    ld a, (hl+)
    ld e, a                ; lo
    ld a, (hl)
    ld d, a                ; hi
    ld h, d
    ld l, e                ; HL = cell address
    ret

; ---- SlotDir: In A=slot -> A = dirbuf[slot]. Clobbers HL; preserves B,C,D,E. ----
SlotDir:
    ld l, a
    ld h, 0
    push bc
    ld bc, $C300
    add hl, bc
    pop bc
    ld a, (hl)
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
    ; 1) the old head is now a body segment. Choose straight vs corner from
    ;    BodyLUT[ entry-dir*4 + exit-dir ]: entry-dir is the dir stored for the
    ;    cell, exit-dir is the current dir (where the new head is going).
    ld a, ($C008)           ; old head slot
    call SlotDir            ; A = entry dir
    add a, a
    add a, a                ; *4
    ld c, a
    ld a, ($C002)           ; current (exit) dir
    add a, c
    ld c, a
    ld b, 0
    ld hl, BodyLUT
    add hl, bc
    ld c, (hl)              ; body tile -> C
    ld a, ($C008)
    call SlotCellAddr       ; HL = old head cell (preserves B,C)
    ld a, c
    ld (hl), a
    ; 2) compute next head from dir into b (x), c (y)
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
    ; 3) wrap walls: x in 0..19, y in 0..17 (underflow shows as >=200)
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
    ; 4) commit new head position
    ld a, b
    ld ($C000), a
    ld a, c
    ld ($C001), a
    ; 5) advance the ring head, record the segment (addr+dir), draw HEAD tile.
    ld a, ($C008)
    inc a
    ld ($C008), a           ; headIdx++ (byte wraps at 256)
    call SetSeg             ; stores addr+dir for slot=headIdx; preserves b,c
    ld a, ($C008)
    call SlotCellAddr       ; HL = new head cell (preserves b,c)
    ld a, ($C002)
    add a, 3                ; head tile = 3 + dir (R/L/U/D)
    ld (hl), a
    ; 6) eat food?  (b=x, c=y still hold the new head)
    ld a, ($C005)
    cp b
    jr nz, .noFood
    ld a, ($C006)
    cp c
    jr nz, .noFood
    ; ate: grow -- bump length and KEEP the tail where it is (skip erase below)
    ld a, ($C003)
    inc a
    ld ($C003), a           ; grow (length byte; visible in MEMORY panel)
    call .NewFood
    ld de, $0780            ; EAT-FOOD chime (~1024 Hz)    ; TWEAK + F5
    call SfxTone
    ret
.noFood:
    ; 7) blank the cell the tail just vacated: slot (headIdx - length) & $FF
    ld a, ($C003)           ; length
    ld b, a
    ld a, ($C008)           ; headIdx
    sub b                   ; old tail slot
    call SlotCellAddr       ; HL = vacated cell
    xor a
    ld (hl), a              ; blank tile 0
    ; 8) the cell ahead of it becomes the new tail: slot T = (headIdx-length+1).
    ;    The tail caps toward its only neighbour (slot T+1), so it faces that
    ;    neighbour's stored direction -- correct even when the snake turned here.
    ld a, ($C003)
    ld b, a
    ld a, ($C008)
    sub b
    inc a
    inc a                   ; slot T+1 (the segment toward the head)
    call SlotDir            ; A = dir of the neighbour
    add a, 7                ; tail tile = 7 + dir (R/L/U/D)
    ld b, a                 ; stash tile (b no longer needed)
    ld a, ($C003)
    ld c, a
    ld a, ($C008)
    sub c
    inc a                   ; slot T (the tail cell itself)
    call SlotCellAddr       ; HL = new tail cell (preserves b)
    ld a, b
    ld (hl), a
    ret

; cheap pseudo-RNG -> new apple cell. Uses $C007 seed. Rerolls (up to 16x) if
; the chosen cell is occupied, so the apple never lands on the snake (where the
; tail would later blank it).
.NewFood:
    ld b, 16                 ; retry budget
.nfTry:
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
    dec b
    jr z, .nfPlace           ; out of retries -> place it anyway
    push bc                  ; preserve retry counter
    ld a, ($C006)
    ld c, a                  ; y
    ld a, ($C005)
    ld b, a                  ; x
    call CellAddr            ; HL = cell address
    pop bc
    ld a, (hl)
    or a
    jr nz, .nfTry            ; cell not blank -> reroll
.nfPlace:
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
