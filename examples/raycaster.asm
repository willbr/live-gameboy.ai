; raycaster.asm — first-person bitmap raycaster for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/raycaster.asm
; 2. HOT-RELOAD THE LENS (F5): CastColumns is the renderer. The grouped
;    `; TWEAK + F5` constants are the lens — MAXSTEPS (max view distance), the
;    PerpStep table (field-of-view / fan width) and the WallHeights curve
;    (depth feel). Edit one, press F5: the view changes while your
;    position/heading persist (e.g. widen PerpStep for a fish-eye).
; 3. REBUILD THE WORLD (F8): edit the Maze DB rows (1=wall 0=empty, 8x8) or the
;    start cell at $C0A0/$C0A1. Maze loads once in Main, so use F8.
; 4. Controls: Left/Right = turn 90 degrees, Up = step forward, Down = step
;    back. Walls block movement.
;
; ---------------------------------------------------------------------
; TECHNIQUE / HONESTY NOTE — BITMAP DISPLAY
; ---------------------------------------------------------------------
; The DMG has no real bitmap mode, so we use the TILE DATA as a framebuffer:
;   * A 16x9 block of unique tiles (indices 0..143) is the framebuffer — a
;     128x72 pixel bitmap. (Tilemap bytes are 8-bit, so only 256 tiles are
;     addressable; 128x72 leaves room for a border tile.)
;   * A STAT/HBlank interrupt (vector $0048) rewrites SCY every scanline so each
;     of the 72 source rows is shown on TWO screen lines (line-doubling),
;     stretching the bitmap to the full 144px height; a VBlank interrupt ($0040)
;     resets SCY at the top of each frame. SCX centres it horizontally (128
;     wide, 16px border each side). The ISRs sit in fixed-address sections
;     (SECTION ... ROM0[$0040]/[$0048]) — gbasm supports located sections.
;   * The raycaster casts one ray per 2 screen columns (64 rays, shared with
;     the odd neighbour). Each ray steps in 8.8 fixed point — a constant
;     forward delta (128 = 1/2 cell) plus a per-column perpendicular delta
;     (PerpStep) so the rays fan like a camera. NO multiply. The forward
;     component is constant, so the step count is the PERPENDICULAR distance
;     (no fish-eye); it indexes WallHeights -> wall pixel height. The column is
;     filled ceiling / wall (near=light, far=dark) / floor at exact pixels.
;   * The redraw (~0.4s) runs into the WRAM shadow with the LCD ON — the screen
;     keeps showing the previous frame, no blank — and only the fast shadow->
;     VRAM blit briefly blanks the LCD (~1 frame). Only re-renders on move/turn.
; ---------------------------------------------------------------------
;
; Memory map (WRAM vars):
;   $C0A0 px   (player cell X, 0..7)      $C0A1 py  (player cell Y, 0..7)
;   $C0A2 dir  (0=N 1=E 2=S 3=W)          $C0A3 inputLatch
;   $C0B0..B7 colStart[8]  $C0B8..BF colEnd[8]  $C0C0..C7 colColor[8]
;   $C0C8..CF ray-cast scratch   $C0D0..DA render loop scratch

; --- interrupt vectors (fixed-address sections; need gbasm's ROM0[$addr]) ---
SECTION "vec_vblank", ROM0[$0040]
    jp VBlankISR             ; VBlank: reset SCY=0 for the top of the next frame
SECTION "vec_stat", ROM0[$0048]
    jp StatISR               ; STAT mode-0 (HBlank): line-double via SCY

; Plain ROM0: gbasm auto-places the code above the header and the vectors
; defined above, so it can't stomp them.
SECTION "code", ROM0

MAXSTEPS  EQU 16        ; max ray steps before "open" (2 steps = 1 cell)  ; TWEAK + F5
NEAR_CUT  EQU 6         ; dist (1/2 cells) below this = bright near-wall tile  ; TWEAK + F5
BORDER    EQU 144       ; tile index used for the left/right border
SCYACC    EQU $C0E0     ; WRAM: running SCY value the HBlank ISR walks down
SHADOW    EQU $C400     ; WRAM shadow framebuffer (rendered with LCD on)

Main:
    ld sp, $FFFE
    xor a
    ldh ($40), a             ; LCD off (safe to touch VRAM)

    ; --- border tile (tile 144) -> $8900 : solid color 2 (dark frame) ---
    ld hl, $8900
    ld b, 8
.bord:
    xor a
    ld (hl+), a              ; plane0 = 0
    ld a, $FF
    ld (hl+), a             ; plane1 = $FF -> color 2
    dec b
    jr nz, .bord

    ; --- fill the whole tilemap with the border tile ---
    ld hl, $9800
    ld bc, $0400
.clrmap:
    ld a, BORDER
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clrmap

    ; --- lay the 16x9 framebuffer tile indices at rows 0..8, cols 0..15 ---
    ;     cell (row,col) -> tile row*16 + col
    ld hl, $9800
    xor a
    ld ($C0D2), a            ; row = 0
.mapRow:
    push hl
    ld a, ($C0D2)
    swap a                   ; row*16 (row<=8 -> <=128)
    ld c, a                  ; base tile index for this row
    ld b, 16
.mapCol:
    ld a, c
    ld (hl+), a
    inc c
    dec b
    jr nz, .mapCol
    pop hl
    push de
    ld de, 32
    add hl, de
    pop de
    ld a, ($C0D2)
    inc a
    ld ($C0D2), a
    cp 9
    jr c, .mapRow

    ; --- palette, scroll (SCX centres the 128-wide bitmap), player, APU ---
    ld a, $E4
    ldh ($47), a             ; BGP
    ld a, 240
    ldh ($43), a             ; SCX = -16 -> bitmap (BG col 0..127) shows at x16..143
    xor a
    ldh ($42), a             ; SCY = 0

    ld a, 3
    ld ($C0A0), a            ; px
    ld a, 6
    ld ($C0A1), a            ; py
    xor a
    ld ($C0A2), a            ; dir = North
    ld ($C0A3), a            ; input latch

    ld a, $80
    ldh ($26), a             ; NR52 APU on
    ld a, $77
    ldh ($24), a             ; NR50 master vol
    ld a, $FF
    ldh ($25), a             ; NR51 route all

    ; --- first render into the shadow buffer, then blit to VRAM (LCD off) ---
    call CastColumns
    call BlitShadow

    ; --- arm the line-doubling interrupts ---
    xor a
    ld (SCYACC), a           ; SCY accumulator = 0
    ldh ($42), a             ; SCY = 0 (line 0 of the first frame)
    ld a, $08
    ldh ($41), a             ; STAT: enable mode-0 (HBlank) interrupt source
    ld a, $03
    ldh ($FF), a             ; IE = VBlank + STAT
    xor a
    ldh ($0F), a             ; clear pending IF

    ; --- LCD on: tiledata $8000, BG map $9800, BG on ---
    ld a, $91
    ldh ($40), a
    ei                       ; the HBlank/VBlank ISRs now line-double every frame

; -------------------- MAIN LOOP --------------------
; The interrupts handle the display (line-doubling). The loop reads input once
; per frame; on a move it RE-RENDERS INTO THE SHADOW WITH THE LCD ON (the screen
; keeps showing the previous frame — no blank), then briefly blanks the LCD only
; for the fast shadow->VRAM blit (~1 frame), so there is no long flicker.
.loop:
    halt                     ; sleep until an interrupt (VBlank or HBlank)
    ldh a, ($44)
    cp 144
    jr c, .loop              ; only act during VBlank (LY >= 144)

    call ReadInput           ; turns / moves; A != 0 if the view must redraw
    or a
    jr z, .noRedraw
    call CastColumns         ; <-- F5 RENDERER -> shadow buffer (LCD stays on)
    di
    xor a
    ldh ($40), a             ; LCD off (only for the quick blit)
    call BlitShadow          ; shadow -> VRAM tile data
    xor a
    ld (SCYACC), a           ; reset the line-double accumulator
    ldh ($42), a             ; SCY = 0 for line 0 of the resumed frame
    ld a, $91
    ldh ($40), a             ; LCD on
    ei
.noRedraw:
    ; wait out the rest of VBlank so we handle input once per frame
.we:
    ldh a, ($44)
    cp 144
    jr nc, .we
    jr .loop

; ====================== SHADOW BLIT ======================
; BlitShadow — copy the 144-tile shadow framebuffer ($C400) to VRAM tile data
; ($8000). 2304 bytes; called with the LCD off so writes always land. ~14ms.
BlitShadow:
    ld hl, SHADOW
    ld de, $8000
    ld c, 144                ; 144 tiles, 16 bytes each (unrolled to cut overhead)
.blit:
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    ld a, (hl+)
    ld (de), a
    inc de
    dec c
    jr nz, .blit
    ret

; ====================== LINE-DOUBLING ISRs ======================
; The 128x72 bitmap is stretched to 144px by showing each source row on two
; screen lines: scanline N is drawn with SCY = -ceil(N/2). The VBlank ISR
; resets SCY to 0 at the top of the frame; the HBlank (STAT mode-0) ISR walks
; SCY down by 1 every two scanlines.
VBlankISR:
    push af
    xor a
    ldh ($42), a             ; SCY = 0 for line 0
    ld (SCYACC), a           ; accumulator = 0
    pop af
    reti

StatISR:                     ; fires at each visible line's HBlank (after draw)
    push af
    ldh a, ($44)             ; LY of the line just drawn
    and 1
    jr nz, .odd              ; only step every other line
    ld a, (SCYACC)
    dec a                    ; advance to the next source row
    ld (SCYACC), a
    ldh ($42), a             ; SCY for the NEXT scanline
.odd:
    pop af
    reti

; ====================== INPUT ======================
; ReadInput — edge-detected d-pad. Left/Right rotate dir; Up/Down step the
; player cell. Returns A=1 if anything changed (redraw), else 0. $C0A3 = latch.
ReadInput:
    ld a, $20
    ldh ($00), a
    ldh a, ($00)
    ldh a, ($00)
    ld b, a
    ld a, $30
    ldh ($00), a
    ld a, b
    cpl
    and $0F
    ld b, a                  ; b = pressed mask (1=pressed)
    ld a, ($C0A3)
    cpl
    and b
    ld c, a                  ; c = newly-pressed edges
    ld a, b
    ld ($C0A3), a            ; latch = current pressed
    xor a
    ld d, a                  ; d = changed accumulator
    bit 0, c                 ; Right -> turn right
    jr z, .noR
    ld a, ($C0A2)
    inc a
    and 3
    ld ($C0A2), a
    ld d, 1
    call SfxTurn
.noR:
    bit 1, c                 ; Left -> turn left
    jr z, .noL
    ld a, ($C0A2)
    dec a
    and 3
    ld ($C0A2), a
    ld d, 1
    call SfxTurn
.noL:
    bit 2, c                 ; Up -> step forward
    jr z, .noU
    push bc                  ; TryMove clobbers C (edge mask) — preserve it
    ld e, 1
    call TryMove
    pop bc
    ld d, 1
.noU:
    bit 3, c                 ; Down -> step back
    jr z, .noD
    ld e, 0
    call TryMove
    ld d, 1
.noD:
    ld a, d
    ret

; TryMove — step the player one cell. In: E=1 forward, E=0 backward.
; Commits only if the target cell is empty. Clobbers A,B,C,D,H,L.
TryMove:
    ld hl, DirDX
    ld a, ($C0A2)
    add a, l
    ld l, a
    ld a, h
    adc a, 0
    ld h, a
    ld d, (hl)               ; dx
    ld hl, DirDY
    ld a, ($C0A2)
    add a, l
    ld l, a
    ld a, h
    adc a, 0
    ld h, a
    ld c, (hl)               ; dy
    ld a, e
    or a
    jr nz, .fwd
    xor a
    sub d
    ld d, a
    xor a
    sub c
    ld c, a
.fwd:
    ld a, ($C0A0)
    add a, d
    ld b, a                  ; tx
    ld a, ($C0A1)
    add a, c
    ld c, a                  ; ty
    ld a, b
    cp 8
    jr nc, .blocked
    ld a, c
    cp 8
    jr nc, .blocked
    ld a, c
    add a, a
    add a, a
    add a, a
    add a, b
    ld l, a
    ld h, 0
    ld de, Maze
    add hl, de
    ld a, (hl)
    or a
    jr nz, .blocked
    ld a, b
    ld ($C0A0), a
    ld a, c
    ld ($C0A1), a
    ret
.blocked:
    call SfxBump
    ret

; ====================== RAY CAST ======================
; CastRay — cast one column ray. In: E = PerpStep (signed lateral delta/step).
; Out: B = distance (steps, 0..MAXSTEPS). Clobbers A,C,D,E,H,L. (B is result.)
CastRay:
    ld a, ($C0A2)            ; dir -> per-step dX(B), dY(C); FWD=128 (1/2 cell)
    or a
    jr nz, .cE
    ld b, e
    ld c, -128
    jr .cd
.cE:
    cp 1
    jr nz, .cS
    ld b, 128
    ld c, e
    jr .cd
.cS:
    cp 2
    jr nz, .cW
    xor a
    sub e
    ld b, a
    ld c, 128
    jr .cd
.cW:
    ld b, -128
    xor a
    sub e
    ld c, a
.cd:
    ld a, ($C0A0)            ; pos = cell centre (8.8)
    ld ($C0C9), a            ; posX hi
    ld a, 128
    ld ($C0C8), a            ; posX lo
    ld a, ($C0A1)
    ld ($C0CB), a            ; posY hi
    ld a, 128
    ld ($C0CA), a            ; posY lo
    ld a, b                  ; sign-extend dX -> $C0CC/CD
    ld ($C0CC), a
    add a, a
    sbc a, a
    ld ($C0CD), a
    ld a, c                  ; sign-extend dY -> $C0CE/CF
    ld ($C0CE), a
    add a, a
    sbc a, a
    ld ($C0CF), a
    ld b, 0                  ; b = step count
.rl:
    ld a, ($C0C8)            ; posX += dX
    ld l, a
    ld a, ($C0CC)
    add a, l
    ld ($C0C8), a
    ld a, ($C0C9)
    ld h, a
    ld a, ($C0CD)
    adc a, h
    ld ($C0C9), a
    ld a, ($C0CA)            ; posY += dY
    ld l, a
    ld a, ($C0CE)
    add a, l
    ld ($C0CA), a
    ld a, ($C0CB)
    ld h, a
    ld a, ($C0CF)
    adc a, h
    ld ($C0CB), a
    ld a, ($C0C9)            ; cellX
    cp 8
    jr nc, .rhit
    ld d, a
    ld a, ($C0CB)            ; cellY
    cp 8
    jr nc, .rhit
    add a, a
    add a, a
    add a, a
    add a, d
    ld l, a
    ld h, 0
    ld de, Maze
    add hl, de
    ld a, (hl)
    or a
    jr nz, .rhit
    inc b
    ld a, b
    cp MAXSTEPS
    jr c, .rl
    ld b, MAXSTEPS
.rhit:
    ret

; ====================== RENDERER (F5) ======================
; CastColumns — render the 128x72 bitmap. For each of 16 tile columns: cast the
; 8 rays it covers, then build its 9 framebuffer tiles. Ceiling=color0,
; floor=color3, near wall=color1, far wall=color2; wall centred on row 36.
CastColumns:
    xor a
    ld ($C0D0), a            ; tx = 0
.txLoop:
    ; --- cast the 8 rays for this tile column ---
    xor a
    ld ($C0D1), a            ; j = 0
    ld a, $FF
    ld ($C0D8), a            ; minStart = 255
    xor a
    ld ($C0D9), a            ; maxEnd = 0
.castJ:
    ld a, ($C0D0)            ; x = tx*8 + j
    add a, a
    add a, a
    add a, a
    ld c, a
    ld a, ($C0D1)
    add a, c
    ld l, a
    ld h, 0
    ld de, PerpStep          ; ; TWEAK + F5 (per-column fan / FOV)
    add hl, de
    ld a, (hl)
    ld e, a                  ; perp
    call CastRay             ; -> B = dist
    ld a, b                  ; H (pixels) = WallHeights[dist]
    ld l, a
    ld h, 0
    ld de, WallHeights       ; ; TWEAK + F5 (distance->pixel-height curve)
    add hl, de
    ld a, (hl)
    ld c, a                  ; c = H
    srl a                    ; H/2
    ld e, a
    ld a, 36
    sub e                    ; drawStart = 36 - H/2
    ld d, a                  ; d = drawStart
    add a, c                 ; drawEnd = drawStart + H
    ld e, a                  ; e = drawEnd
    ; store colStart[j], colEnd[j]
    ld a, ($C0D1)
    add a, $B0
    ld l, a
    ld h, $C0
    ld a, d
    ld (hl), a               ; colStart[j]
    inc l
    ld (hl), a               ; colStart[j+1] (cast every 2px; share the odd col)
    ld a, ($C0D1)
    add a, $B8
    ld l, a
    ld a, e
    ld (hl), a               ; colEnd[j]
    inc l
    ld (hl), a               ; colEnd[j+1]
    ; minStart / maxEnd
    ld a, d
    ld hl, $C0D8
    cp (hl)
    jr nc, .noMin
    ld (hl), a
.noMin:
    ld a, e
    ld hl, $C0D9
    cp (hl)
    jr c, .noMax
    ld (hl), a
.noMax:
    ; colColor[j] = near/far by dist (B)
    ld a, b
    cp NEAR_CUT              ; ; TWEAK + F5 (near/far shading cutoff)
    ld a, 1
    jr c, .nearC
    ld a, 2
.nearC:
    ld c, a
    ld a, ($C0D1)
    add a, $C0
    ld l, a
    ld h, $C0
    ld a, c
    ld (hl), a               ; colColor[j]
    inc l
    ld (hl), a               ; colColor[j+1]
    ld a, ($C0D1)
    add a, 2                 ; cast every other column (0,2,4,6)
    ld ($C0D1), a
    cp 8
    jp c, .castJ

    ; --- build the 9 framebuffer tiles for this tx ---
    xor a
    ld ($C0D2), a            ; ty = 0
.tyLoop:
    ld a, ($C0D0)            ; shadow tileAddr = SHADOW + (ty*16+tx)*16
    swap a                   ; tx*16
    ld ($C0D6), a            ; tileAddr lo
    ld a, ($C0D2)
    add a, $C4              ; hi = (SHADOW>>8) + ty   (SHADOW = $C400)
    ld ($C0D7), a
    ld a, ($C0D2)            ; tileTop = ty*8
    add a, a
    add a, a
    add a, a
    ld ($C0DA), a            ; tileTop
    ; all ceiling? (tileTop+7) < minStart
    add a, 7                 ; tileBot
    ld c, a
    ld a, ($C0D8)            ; minStart
    ld b, a
    ld a, c
    cp b
    jr nc, .notCeil          ; tileBot >= minStart -> not all ceiling
    xor a                    ; ceiling color0 -> $00
    call .Fill16
    jr .nextTy
.notCeil:
    ld a, ($C0DA)            ; tileTop
    ld c, a
    ld a, ($C0D9)            ; maxEnd
    ld b, a
    ld a, c
    cp b
    jr c, .perPixel          ; tileTop < maxEnd -> mixed
    ld a, $FF                ; floor color3 -> $FF
    call .Fill16
    jr .nextTy
.perPixel:
    xor a
    ld ($C0D3), a            ; prow = 0
.prowLoop:
    ld a, ($C0DA)            ; y = tileTop + prow
    ld c, a
    ld a, ($C0D3)
    add a, c
    ld ($C0D4), a            ; y
    ld d, 0                  ; p0
    ld e, 0                  ; p1
    ld b, 0                  ; j
.bitJ:
    ld a, b                  ; start[j]
    add a, $B0
    ld l, a
    ld h, $C0
    ld a, ($C0D4)
    cp (hl)
    jr c, .pcCeil            ; y < start -> ceiling (0)
    ld a, b                  ; end[j]
    add a, $B8
    ld l, a
    ld a, ($C0D4)
    cp (hl)
    jr nc, .pcFloor          ; y >= end -> floor (3)
    ld a, b                  ; wall color[j]
    add a, $C0
    ld l, a
    ld a, (hl)
    jr .pcHave
.pcCeil:
    xor a
    jr .pcHave
.pcFloor:
    ld a, 3
.pcHave:
    ld c, a                  ; color
    and 1
    sla d
    or d
    ld d, a                  ; p0 = (p0<<1)|bit0
    ld a, c
    and 2
    srl a
    sla e
    or e
    ld e, a                  ; p1 = (p1<<1)|bit1
    inc b
    ld a, b
    cp 8
    jr c, .bitJ
    ld a, ($C0D6)            ; write to tileAddr + prow*2
    ld c, a
    ld a, ($C0D3)
    add a, a
    add a, c
    ld l, a
    ld a, ($C0D7)
    ld h, a
    ld a, d
    ld (hl+), a             ; plane0
    ld a, e
    ld (hl), a             ; plane1
    ld a, ($C0D3)
    inc a
    ld ($C0D3), a
    cp 8
    jp c, .prowLoop
.nextTy:
    ld a, ($C0D2)
    inc a
    ld ($C0D2), a
    cp 9
    jp c, .tyLoop
    ld a, ($C0D0)
    inc a
    ld ($C0D0), a
    cp 16
    jp c, .txLoop
    ret

; .Fill16 — write A to 16 bytes at tileAddr ($C0D6/D7). Preserves A.
.Fill16:
    push af
    ld a, ($C0D6)
    ld l, a
    ld a, ($C0D7)
    ld h, a
    pop af
    ld b, 16
.f16:
    ld (hl+), a
    dec b
    jr nz, .f16
    ret

; ====================== DATA TABLES ======================
; Maze: 8x8, 1=wall 0=empty. A 1-wide corridor at x=4 with a far wall ahead.
; (Edit this and the start cell at $C0A0/$C0A1, then F8 to rebuild the world.)
Maze:
    db 1,1,1,1,1,1,1,1
    db 1,0,0,0,0,0,0,1
    db 1,0,0,1,0,0,0,1     ; an open 6x6 room with a few pillars so moving and
    db 1,0,0,0,0,1,0,1     ; turning visibly change the view (walk around them)
    db 1,0,1,0,0,0,0,1
    db 1,0,0,0,1,0,0,1
    db 1,0,0,0,0,0,0,1     ; player starts at (3,6) facing North
    db 1,1,1,1,1,1,1,1

DirDX:
    db 0, 1, 0, -1
DirDY:
    db -1, 0, 1, 0

; PerpStep[col] — signed 8.8 lateral delta per step, one per of 128 columns.
; Linear camera plane: ~0 at centre, +/-32 at the edges. (Generated.)  ; TWEAK + F5
PerpStep:
    incbin "examples/rc_perp.bin"

; WallHeights[dist 0..MAXSTEPS] -> wall height in PIXELS (dist in 1/2 cells,
; max 72 = bitmap height). Smooth perspective curve (~216/(dist+3)).  ; TWEAK + F5
WallHeights:
    db 72,54,43,36,31,27,24,22,20,18,17,16,15,14,13,12,12

; ====================== SOUND ======================
; SfxTone — short CH1 pulse blip. In: D=freq hi (bits2-0), E=freq lo.
SfxTone:
    xor a
    ldh ($10), a
    ld a, $A0
    ldh ($11), a
    ld a, $F2
    ldh ($12), a
    ld a, e
    ldh ($13), a
    ld a, d
    or $C0
    ldh ($14), a
    ret

SfxTurn:                     ; ; TWEAK + F5
    push de
    ld de, $0700
    call SfxTone
    pop de
    ret

SfxBump:                     ; ; TWEAK + F5
    push de
    ld de, $0380
    call SfxTone
    pop de
    ret
