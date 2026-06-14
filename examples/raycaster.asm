; raycaster.asm — first-person pseudo-3D maze for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/raycaster.asm
; 2. HOT-RELOAD THE LENS (F5): CastAndDraw holds the renderer. The grouped
;    `; TWEAK + F5` constants are the lens — MAXSTEPS (max view distance), the
;    PerpStep table (field-of-view / fan width) and the WallHeights curve
;    (depth feel). Edit one, press F5: the view changes while your
;    position/heading persist (e.g. widen PerpStep for a fish-eye).
; 3. RESKIN (paint): select VRAM tile 1 (near wall), 2 (far wall), 3 (ceiling)
;    or 4 (floor) in the tile viewer and paint it — the whole 3D view reskins
;    live (rc_wall_lt / rc_wall_dk / rc_ceiling / rc_floor .2bpp).
; 4. REBUILD THE WORLD (F8): edit the Maze DB rows (1=wall 0=empty, 8x8) or the
;    start cell at $C0A0/$C0A1. Maze + tiles load once in Main, so use F8
;    (soft reset) for those edits.
; 5. Controls: Left/Right = turn 90 degrees, Up = step forward, Down = step
;    back. Walls block movement.
;
; ---------------------------------------------------------------------
; TECHNIQUE / HONESTY NOTE
; ---------------------------------------------------------------------
; This is NOT a per-pixel DDA raycaster (impossible at 60fps in pure SM83
; with no multiply). It is an honest pseudo-3D column caster:
;   * Fixed 8x8 maze grid (Maze DB, 1=wall 0=empty).
;   * Player heading is one of 4 cardinal directions (N/E/S/W) so the forward
;     and lateral axes are just signed cell steps — no trig.
;   * One ray per BG tile COLUMN (20 columns). The ray steps in 8.8 fixed
;     point: a CONSTANT forward delta (64 = 1/4 cell) plus a per-column
;     PERPENDICULAR delta (PerpStep table) so the rays fan out like a camera.
;     NO multiply: the lateral offset accumulates by repeated addition.
;   * Because the forward component is a constant 1/4 cell, the STEP COUNT to
;     the first wall is the PERPENDICULAR distance (no fish-eye), and the fine
;     1/4-cell resolution gives smoothly tapering wall heights.
;   * Distance indexes WallHeights -> wall column height in PIXELS. Each column
;     is drawn into the BG tilemap ($9800) centred on the horizon: a ceiling
;     run, the wall (near = light tile, far = dark tile for depth shading), and
;     a floor run. The ceiling/wall and wall/floor boundaries land on an exact
;     PIXEL row by using 28 sub-tile "transition" tiles (tiles 5..32: 7 split
;     positions each for ceil/near, ceil/far, near/floor, far/floor), so the
;     diagonal edges step by 1px instead of a whole 8px tile.
;   * VRAM is only writable outside PPU mode 3, and a full 20x18 redraw is far
;     bigger than one VBlank's window, so the redraw is bracketed by LCD-off /
;     LCD-on. It only runs when the view changes (on move/turn), not per frame.
; The view genuinely re-casts every time the player moves or turns; turning
; only redraws (no move), moving re-casts from the new cell.
; ---------------------------------------------------------------------
;
; Memory map (WRAM vars):
;   $C0A0 px   (player cell X, 0..7)
;   $C0A1 py   (player cell Y, 0..7)
;   $C0A2 dir  (0=N 1=E 2=S 3=W)
;   $C0A3 inputLatch (debounce: keys held last frame)

SECTION "code", ROM0

MAXSTEPS  EQU 28        ; max ray steps before "open" (4 steps = 1 cell)  ; TWEAK + F5
NEAR_CUT  EQU 12        ; dist (1/4 cells) below this = bright near-wall tile  ; TWEAK + F5

Main:
    ld sp, $FFFE
    xor a
    ldh ($40), a             ; LCD off (safe to touch VRAM)

    ; --- Load tiles 1..32 -> $8010 (16 bytes each, contiguous) ---
    ;   1 near-wall 2 far-wall 3 ceiling 4 floor, then 28 edge-smoothing
    ;   transition tiles (5..32): ceil/near, ceil/far, near/floor, far/floor,
    ;   each at 7 sub-tile split rows so wall edges step by 1px, not 8px.
    ld hl, .Tiles
    ld de, $8010
    ld bc, $0200             ; 32 tiles * 16 bytes
    call .CopyBC

    ; --- Clear BG tilemap $9800..$9BFF to tile 0 ---
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

    ; --- Init player vars: stand in the corridor (cell 4,6) facing North,
    ;     looking straight down it to the far wall ---
    ld a, 4
    ld ($C0A0), a            ; px
    ld a, 6
    ld ($C0A1), a            ; py
    xor a
    ld ($C0A2), a            ; dir = North
    xor a
    ld ($C0A3), a            ; input latch

    ; --- Sound: power on APU (optional SFX on bump/turn) ---
    ld a, $80
    ldh ($26), a             ; NR52 on (write FIRST)
    ld a, $77
    ldh ($24), a             ; NR50 master vol
    ld a, $FF
    ldh ($25), a             ; NR51 route all

    ; --- First render while the LCD is still OFF, so the full 20x18 tilemap
    ;     redraw is never dropped (VRAM is only writable outside PPU mode 3). ---
    call CastAndDraw

    ; --- LCD on: tiledata $8000, BG map $9800, BG on ---
    ld a, $91
    ldh ($40), a

; -------------------- MAIN LOOP (F5 hot-reload zone) --------------------
.loop:
.waitvbl:
    ldh a, ($44)             ; LY
    cp $90                   ; 144 = VBlank start
    jr nz, .waitvbl

    call ReadInput           ; turns / moves the player; sets a "dirty" flag in B
    ; ReadInput returns A != 0 if the view must be redrawn this frame.
    or a
    jr z, .nodraw
    ; A full-screen tilemap redraw is far bigger than one VBlank's writable
    ; window, so blank the LCD (VRAM fully writable), re-cast, then re-enable.
    ; We are in VBlank here (LY=$90), the safe point to toggle the LCD.
    xor a
    ldh ($40), a             ; LCD off
    call CastAndDraw         ; <-- F5 RENDERER (re-casts all 20 columns)
    ld a, $91
    ldh ($40), a             ; LCD on
.nodraw:

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

.Tiles:
    incbin "examples/rc_wall_lt.2bpp"    ; 1 near wall (light)
    incbin "examples/rc_wall_dk.2bpp"    ; 2 far wall  (dark)
    incbin "examples/rc_ceiling.2bpp"    ; 3 ceiling
    incbin "examples/rc_floor.2bpp"      ; 4 floor
    ; 5..32: edge-smoothing transition tiles (7 sub-tile splits each):
    ;   5..11 ceil/near  12..18 ceil/far  19..25 near/floor  26..32 far/floor
    incbin "examples/rc_trans.2bpp"

; ====================== INPUT ======================
; ReadInput — edge-detected d-pad. Left/Right rotate dir; Up/Down step the
; player cell forward/back if not blocked by a wall. Returns A=1 if anything
; changed (so the loop redraws), A=0 otherwise. Uses $C0A3 as the held-latch.
ReadInput:
    ld a, $20                ; select directions
    ldh ($00), a
    ldh a, ($00)            ; settle
    ldh a, ($00)
    ld b, a                  ; b = raw read (0=pressed) bits0R 1L 2U 3D
    ld a, $30
    ldh ($00), a            ; deselect
    ; pressed mask = (~b) & $0F  (1 = pressed)
    ld a, b
    cpl
    and $0F
    ld b, a                  ; b = pressed mask
    ; edge = pressed & ~latch
    ld a, ($C0A3)
    cpl
    and b
    ld c, a                  ; c = newly-pressed edges
    ld a, b
    ld ($C0A3), a            ; store latch = current pressed
    ; --- process edges in C ---
    xor a
    ld d, a                  ; d = "changed" accumulator
    ; Right (bit0) -> turn right (dir+1)
    bit 0, c
    jr z, .noR
    ld a, ($C0A2)
    inc a
    and 3
    ld ($C0A2), a
    ld d, 1
    call SfxTurn
.noR:
    ; Left (bit1) -> turn left (dir-1)
    bit 1, c
    jr z, .noL
    ld a, ($C0A2)
    dec a
    and 3
    ld ($C0A2), a
    ld d, 1
    call SfxTurn
.noL:
    ; Up (bit2) -> step forward
    bit 2, c
    jr z, .noU
    ld e, 1                  ; e=1 forward
    call TryMove
    ld d, 1
.noU:
    ; Down (bit3) -> step back
    bit 3, c
    jr z, .noD
    ld e, 0                  ; e=0 backward
    call TryMove
    ld d, 1
.noD:
    ld a, d                  ; return changed flag
    ret

; TryMove — step the player one cell. In: E=1 forward, E=0 backward.
; Computes target cell from dir, checks Maze; commits only if empty.
; Clobbers A,B,C,H,L.
TryMove:
    ; fetch dx = DirDX[dir] -> D
    ld hl, DirDX
    ld a, ($C0A2)
    add a, l
    ld l, a
    ld a, h
    adc a, 0
    ld h, a
    ld d, (hl)               ; d = dx (signed -1/0/1)
    ; fetch dy = DirDY[dir] -> C
    ld hl, DirDY
    ld a, ($C0A2)
    add a, l
    ld l, a
    ld a, h
    adc a, 0
    ld h, a
    ld c, (hl)               ; c = dy
    ; backward? (E==0) negate both
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
    add a, d                 ; tx = px + dx
    ld b, a                  ; b = tx
    ld a, ($C0A1)
    add a, c                 ; ty = py + dy
    ld c, a                  ; c = ty
    ; bounds 0..7 (underflow shows >=200)
    ld a, b
    cp 8
    jr nc, .blocked
    ld a, c
    cp 8
    jr nc, .blocked
    ; check Maze[ty*8 + tx]
    ld a, c
    add a, a
    add a, a
    add a, a                 ; ty*8
    add a, b                 ; + tx
    ld l, a
    ld h, 0
    ld de, Maze
    add hl, de
    ld a, (hl)
    or a
    jr nz, .blocked          ; wall -> reject
    ; commit
    ld a, b
    ld ($C0A0), a
    ld a, c
    ld ($C0A1), a
    ret
.blocked:
    call SfxBump
    ret

; ====================== RENDERER (F5) ======================
; CastAndDraw — cast one ray per BG column (20) and paint the column into the
; tilemap. Fine 8.8 fixed-point stepping, no multiply. Constants below are the
; live-edit lens.  ; TWEAK + F5
;
; Each column's ray starts at the player cell centre and advances by a constant
; FORWARD delta (64 = 1/4 cell per step) plus a per-column PERPENDICULAR delta
; (PerpStep table) so the 20 rays fan out like a camera. Because the forward
; component is a constant 1/4 cell, the STEP COUNT is the perpendicular distance
; (no fish-eye), and the fine 1/4-cell resolution gives smooth wall heights. The
; ray steps until it lands in a wall; the step count indexes WallHeights.
;
; WRAM scratch: $C0B0 col, $C0B2/3 posX, $C0B4/5 posY, $C0B6/7 dX, $C0B8/9 dY,
;               $C0BA wall tile.  B (across the step loop) = distance index.
CastAndDraw:
    xor a
    ld ($C0B0), a            ; col = 0
.colLoop:
    ; --- PerpStep[col] -> E (signed lateral delta per step) ---
    ld a, ($C0B0)
    ld l, a
    ld h, 0
    ld de, PerpStep          ; ; TWEAK + F5 (per-column fan / FOV)
    add hl, de
    ld a, (hl)
    ld e, a
    ; --- per-step dX (B) and dY (C) from dir; FWD = 64 ---
    ld a, ($C0A2)
    or a
    jr nz, .dE
    ld b, e                  ; N: dX=+perp, dY=-64
    ld c, -64
    jr .haveDelta
.dE:
    cp 1
    jr nz, .dS
    ld b, 64                 ; E: dX=+64, dY=+perp
    ld c, e
    jr .haveDelta
.dS:
    cp 2
    jr nz, .dW
    xor a                    ; S: dX=-perp, dY=+64
    sub e
    ld b, a
    ld c, 64
    jr .haveDelta
.dW:
    ld b, -64               ; W: dX=-64, dY=-perp
    xor a
    sub e
    ld c, a
.haveDelta:
    ; --- ray pos = cell centre (8.8): posX = px*256+128, posY = py*256+128 ---
    ld a, ($C0A0)
    ld ($C0B3), a
    ld a, 128
    ld ($C0B2), a
    ld a, ($C0A1)
    ld ($C0B5), a
    ld a, 128
    ld ($C0B4), a
    ; sign-extend dX (B) -> $C0B6/7
    ld a, b
    ld ($C0B6), a
    add a, a
    sbc a, a
    ld ($C0B7), a
    ; sign-extend dY (C) -> $C0B8/9
    ld a, c
    ld ($C0B8), a
    add a, a
    sbc a, a
    ld ($C0B9), a

    ; --- step the ray; B counts steps = perpendicular distance ---
    ld b, 0
.stepLoop:
    ; posX += dX
    ld a, ($C0B2)
    ld l, a
    ld a, ($C0B6)
    add a, l
    ld ($C0B2), a
    ld a, ($C0B3)
    ld h, a
    ld a, ($C0B7)
    adc a, h
    ld ($C0B3), a
    ; posY += dY
    ld a, ($C0B4)
    ld l, a
    ld a, ($C0B8)
    add a, l
    ld ($C0B4), a
    ld a, ($C0B5)
    ld h, a
    ld a, ($C0B9)
    adc a, h
    ld ($C0B5), a
    ; cell = (posY.hi)*8 + posX.hi ; bounds + wall test
    ld a, ($C0B3)            ; cellX
    cp 8
    jr nc, .hit              ; out of maze -> wall at this distance
    ld d, a
    ld a, ($C0B5)            ; cellY
    cp 8
    jr nc, .hit
    add a, a
    add a, a
    add a, a                 ; cellY*8
    add a, d                 ; + cellX
    ld l, a
    ld h, 0
    ld de, Maze
    add hl, de
    ld a, (hl)
    or a
    jr nz, .hit              ; wall
    inc b
    ld a, b
    cp MAXSTEPS
    jr c, .stepLoop
    ld b, MAXSTEPS           ; no hit within range -> far wall
.hit:
    ; B = distance index. H (PIXELS) = WallHeights[B].
    ld a, b
    ld l, a
    ld h, 0
    ld de, WallHeights       ; ; TWEAK + F5 (distance->pixel-height curve)
    add hl, de
    ld a, (hl)               ; a = H (wall pixel height)
    ld c, a                  ; c = H
    ; centre on the horizon (row 72): DS = 72 - H/2 ; DEp = DS + H
    srl a                    ; H/2
    ld d, a
    ld a, 72
    sub d
    ld ($C0BD), a            ; DS  (wall-top pixel)
    add a, c
    ld ($C0BE), a            ; DEp (wall-bottom pixel)
    ; --- near/far tile set by distance (solid + its two transition bases) ---
    ld a, b
    cp NEAR_CUT              ; ; TWEAK + F5 (near/far shading cutoff)
    jr nc, .farSet
    ld a, 1
    ld ($C0BA), a            ; near wall solid
    ld a, 5
    ld ($C0BB), a            ; ceil/near transition base (tiles 5..11)
    ld a, 19
    ld ($C0BC), a            ; near/floor transition base (tiles 19..25)
    jr .haveSet
.farSet:
    ld a, 2
    ld ($C0BA), a            ; far wall solid
    ld a, 12
    ld ($C0BB), a            ; ceil/far transition base (tiles 12..18)
    ld a, 26
    ld ($C0BC), a            ; far/floor transition base (tiles 26..32)
.haveSet:
    ; --- column base address $9800 + col -> HL (col<20, no hi carry) ---
    ld a, ($C0B0)
    ld l, a
    ld h, $98

    ; --- ceiling: ta = DS>>3 full ceiling tiles (tile 3) ---
    ld a, ($C0BD)
    srl a
    srl a
    srl a                    ; ta
    ld b, a
.dCeil:
    ld a, b
    or a
    jr z, .dCeilEnd
    ld a, 3
    ld (hl), a
    push bc
    ld bc, 32
    add hl, bc
    pop bc
    dec b
    jr .dCeil
.dCeilEnd:
    ; --- top transition tile if ka = DS&7 > 0 (ka px ceiling, rest wall) ---
    ld a, ($C0BD)
    and 7
    jr z, .noTop
    dec a
    ld b, a                  ; ka-1
    ld a, ($C0BB)
    add a, b                 ; topBase + (ka-1)
    ld (hl), a
    push bc
    ld bc, 32
    add hl, bc
    pop bc
.noTop:
    ; --- full wall: count = (DEp>>3) - (DS>>3) - (ka>0 ? 1 : 0) ---
    ld a, ($C0BE)
    srl a
    srl a
    srl a                    ; tb
    ld b, a
    ld a, ($C0BD)
    srl a
    srl a
    srl a                    ; ta
    ld c, a
    ld a, b
    sub c                    ; tb - ta
    ld b, a
    ld a, ($C0BD)
    and 7
    jr z, .noTopAdj
    dec b                    ; minus the top-transition row
.noTopAdj:
    ld a, ($C0BA)
    ld c, a                  ; c = wall solid tile
.dWall:
    ld a, b
    or a
    jr z, .dWallEnd
    ld (hl), c
    push bc
    ld bc, 32
    add hl, bc
    pop bc
    dec b
    jr .dWall
.dWallEnd:
    ; --- bottom transition tile if kb = DEp&7 > 0 (kb px wall, rest floor) ---
    ld a, ($C0BE)
    and 7
    jr z, .noBot
    dec a
    ld b, a                  ; kb-1
    ld a, ($C0BC)
    add a, b                 ; botBase + (kb-1)
    ld (hl), a
    push bc
    ld bc, 32
    add hl, bc
    pop bc
.noBot:
    ; --- floor: count = 18 - (DEp>>3) - (kb>0 ? 1 : 0) ---
    ld a, ($C0BE)
    srl a
    srl a
    srl a                    ; tb
    ld b, a
    ld a, 18
    sub b                    ; 18 - tb
    ld b, a
    ld a, ($C0BE)
    and 7
    jr z, .noBotAdj
    dec b                    ; minus the bottom-transition row
.noBotAdj:
.dFloor:
    ld a, b
    or a
    jr z, .dFloorEnd
    ld a, 4
    ld (hl), a
    push bc
    ld bc, 32
    add hl, bc
    pop bc
    dec b
    jr .dFloor
.dFloorEnd:

    ; --- next column ---
    ld a, ($C0B0)
    inc a
    ld ($C0B0), a
    cp 20
    jp c, .colLoop
    ret

; ====================== DATA TABLES ======================
; Maze: 8x8, 1=wall 0=empty. Outer ring is wall; some inner walls form a maze.
; (Edit this and the start cell at $C0A0/$C0A1, then F8 to rebuild the world.)
Maze:
    db 1,1,1,1,1,1,1,1
    db 1,1,1,1,0,1,1,1     ; vvv 1-wide corridor at x=4: the side walls
    db 1,1,1,1,0,1,1,1     ;     recede toward the far wall straight ahead
    db 1,1,1,1,0,1,1,1
    db 1,1,1,1,0,1,1,1
    db 1,1,1,1,0,1,1,1
    db 1,1,1,1,0,1,1,1     ; player starts at (4,6), facing the far wall
    db 1,1,1,1,1,1,1,1

; Direction deltas (dir 0=N 1=E 2=S 3=W), signed cell steps.
DirDX:
    db 0, 1, 0, -1
DirDY:
    db -1, 0, 1, 0

; Slope[col] — signed 8.8 lateral cells per forward cell, one per BG column.
; Linear camera plane: ~0 at centre, +/-112 (~tan 24deg) at the edges, so the
; 20 columns fan out evenly. Widen for a fish-eye, narrow for a telephoto.
; (Keep entries within signed-byte range -128..127.)  ; TWEAK + F5
; PerpStep[col] — signed 8.8 lateral delta per forward step, one per BG column.
; ~0 at the centre, +/-32 (forward delta is 64, so ~tan 27deg) at the edges, so
; the 20 rays fan evenly. Widen for fish-eye, narrow for telephoto.  ; TWEAK + F5
PerpStep:
    db -32,-29,-25,-22,-19,-15,-12,-8,-5,-2,2,5,8,12,15,19,22,25,29,32

; WallHeights[dist 0..MAXSTEPS] -> wall height in PIXELS (dist in 1/4 cells).
; Smooth perspective curve (near=tall, ~576/(dist+4)), precomputed so there is
; no divide at runtime. The drawer renders pixel-accurate edges using the
; transition tiles, so this can be any 8..144.  ; TWEAK + F5
WallHeights:
    db 128,112,99,88,80,72,66,60,56,52,48,45,42,40,38,36,34,32,31,30,28,27,26,25,24,24,23,22,22

; ====================== SOUND ======================
; SfxTone — short CH1 (pulse) blip. In: D=freq hi (bits2-0), E=freq lo.
; Clobbers A; preserves B,C,HL (and D,E inputs).
SfxTone:
    xor a
    ldh ($10), a             ; NR10 no sweep
    ld a, $A0
    ldh ($11), a             ; NR11 duty + length
    ld a, $F2
    ldh ($12), a             ; NR12 vol 15, decay
    ld a, e
    ldh ($13), a             ; NR13 freq lo
    ld a, d
    or $C0
    ldh ($14), a             ; NR14 trigger + len-enable + freq hi
    ret

; SfxTurn — soft click when turning.  ; TWEAK + F5
SfxTurn:
    push de
    ld de, $0700             ; TURN pitch
    call SfxTone
    pop de
    ret

; SfxBump — low thud when a wall blocks movement.  ; TWEAK + F5
SfxBump:
    push de
    ld de, $0380             ; BUMP pitch (low)
    call SfxTone
    pop de
    ret
