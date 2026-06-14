; topdown.asm — a top-down adventure (Zelda / Metal Gear style) for the
; live-gameboy IDE.
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/topdown.asm
; 2. HOT-RELOAD MOVEMENT (F5): MoveHero (in the main-loop zone) holds the
;    grouped `; TWEAK + F5` constant MOVE_SPEED. Change it (try 1, 2, 4),
;    press F5 — the hero speeds up / slows down while keeping its position
;    and the guard's patrol. The wall-collision rule lives in the same
;    function (CanWalk samples the BG tilemap), so you can edit how solid
;    walls behave and reload live too.
; 3. RESHAPE THE DUNGEON (paint / BG MAP): the WALL tiles ARE the level.
;    Paint VRAM tile 2 (topdown_wall.2bpp) to reskin every wall at once,
;    or open the BG MAP panel and draw/erase wall cells (tile 2 = wall,
;    tile 1 = floor) to carve new rooms and corridors — the hero's
;    collision follows your edits instantly because it reads the live map.
; 4. RESKIN THE CAST (paint): paint tile 3 (topdown_hero) or tile 4
;    (topdown_guard) to redraw the hero / guard sprite live.
; 5. TUNE THE SOUND (F5): edit the BUMP pitch in MoveHero or the CAUGHT
;    pitch in the guard-collision check, press F5, hear it change.
; 6. F8 (soft reset) re-runs Main (tile load + dungeon layout) if you edit
;    init code or the map-building loop.
;
; Controls: D-pad walks the hero in 4 directions. Walls block you. A guard
; patrols left/right; touching it bumps you back to the start and beeps.

SECTION "code", ROM0

; ---- tile indices (in VRAM, $8000-based tiledata) ----
TILE_FLOOR  EQU 1
TILE_WALL   EQU 2
TILE_HERO   EQU 3
TILE_GUARD  EQU 4

; ---- game vars ($C0A0+) ----
; $C0A0 heroX (BG pixel x of hero top-left)
; $C0A1 heroY (BG pixel y of hero top-left)
; $C0A2 heroDir (0=down 1=up 2=left 3=right) — reserved for future facing
; $C0A3 guardX (BG pixel x of guard top-left)
; $C0A4 guardY
; $C0A5 guardDX (signed: 1 or $FF)

Main:
    ld sp, $FFFE
    xor a
    ldh ($40), a             ; LCD off so we can touch VRAM

    ; --- load tiles 1..4 contiguously at $8010 (16 bytes each) ---
    ld hl, .FloorTile
    ld de, $8010             ; tile 1
    ld bc, $0010
    call .CopyBC
    ld hl, .WallTile
    ld de, $8020             ; tile 2
    ld bc, $0010
    call .CopyBC
    ld hl, .HeroTile
    ld de, $8030             ; tile 3
    ld bc, $0010
    call .CopyBC
    ld hl, .GuardTile
    ld de, $8040             ; tile 4
    ld bc, $0010
    call .CopyBC

    ; --- fill the whole tilemap with floor (tile 1) ---
    ld hl, $9800
    ld bc, $0400
.fillfloor:
    ld a, TILE_FLOOR
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .fillfloor

    ; --- build the dungeon: border walls + a couple of interior walls.
    ;     The visible area is 20 cols (0..19) x 18 rows (0..17). We draw the
    ;     map row-by-row from a compact layout table: '#' = wall, '.' = floor.
    ;     (This loop is the F8 target — edit the layout and soft-reset.) ---
    ld hl, $9800             ; top-left of tilemap
    ld de, .Layout
.maprow:
    ld a, (de)               ; first byte of the row, 0 marks end of table
    or a
    jr z, .mapdone
    ld c, 20                 ; 20 visible columns per row
.mapcol:
    ld a, (de)
    inc de
    cp 35                    ; '#' wall marker (ASCII)
    jr nz, .floorcell
    ld a, TILE_WALL
    jr .putcell
.floorcell:
    ld a, TILE_FLOOR
.putcell:
    ld (hl+), a
    dec c
    jr nz, .mapcol
    ; advance HL past the 12 off-screen columns to next row start
    push de
    ld de, 12
    add hl, de
    pop de
    jr .maprow
.mapdone:

    ; --- palettes ---
    ld a, $E4
    ldh ($47), a             ; BGP
    ldh ($48), a             ; OBP0

    ; --- zero OAM shadow ($C000..$C09F) ---
    ld hl, $C000
    ld bc, $00A0
.clroam:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clroam

    ; sprite tiles: sprite 0 = hero, sprite 1 = guard
    ld a, TILE_HERO
    ld ($C002), a
    ld a, TILE_GUARD
    ld ($C006), a

    ; --- init game vars: hero spawns just inside the top-left room ---
    ld a, 16
    ld ($C0A0), a            ; heroX (tile col 2)
    ld a, 16
    ld ($C0A1), a            ; heroY (tile row 2)
    xor a
    ld ($C0A2), a            ; heroDir = down
    ; guard patrols along an open corridor
    ld a, 88
    ld ($C0A3), a            ; guardX
    ld a, 80
    ld ($C0A4), a            ; guardY
    ld a, 1
    ld ($C0A5), a            ; guardDX = +1

    ; --- Sound: power on APU, full volume, route all channels both sides ---
    ld a, $80
    ldh ($26), a             ; NR52 = APU on (write FIRST)
    ld a, $77
    ldh ($24), a             ; NR50 = master volume L/R
    ld a, $FF
    ldh ($25), a             ; NR51 = all channels to both speakers

    ; --- LCD on: BG + OBJ on, tiledata $8000 ---
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
    call MoveHero
    call MoveGuard
    call CheckCaught
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

.FloorTile:
    incbin "examples/topdown_floor.2bpp"
.WallTile:
    incbin "examples/topdown_wall.2bpp"
.HeroTile:
    incbin "examples/topdown_hero.2bpp"
.GuardTile:
    incbin "examples/topdown_guard.2bpp"

; --- dungeon layout: 20 chars per row, '#' wall '.' floor. One DB per row.
;     A trailing 0 byte ends the table. The hero spawns at col 2,row 2. ---
.Layout:
    db "####################"   ; row 0  top wall
    db "#..................#"   ; row 1
    db "#..................#"   ; row 2  (hero spawns here)
    db "#......######......#"   ; row 3
    db "#......#....#......#"   ; row 4
    db "#......#....#......#"   ; row 5
    db "#..................#"   ; row 6
    db "#..................#"   ; row 7
    db "#..####......####..#"   ; row 8
    db "#.....#......#.....#"   ; row 9
    db "#.....#......#.....#"   ; row 10
    db "#..................#"   ; row 11
    db "#......######......#"   ; row 12
    db "#......#....#......#"   ; row 13
    db "#..................#"   ; row 14
    db "#..................#"   ; row 15
    db "#..................#"   ; row 16
    db "####################"   ; row 17 bottom wall
    db 0                        ; end of table

; ====================== GAMEPLAY PROCEDURES ======================

; ReadInput — read the d-pad into B ($C0A6 latch). bit0 Right,1 Left,2 Up,
; 3 Down (0 = pressed), matching the FF00 select-direction read.
ReadInput:
    ld a, $20                ; select directions
    ldh ($00), a
    ldh a, ($00)             ; settle
    ldh a, ($00)
    ld ($C0A6), a            ; stash raw read for MoveHero
    ld a, $30
    ldh ($00), a             ; deselect
    ret

; -------------------- MoveHero (F5 SIGNATURE) --------------------
; Walks the hero one axis at a time, blocked by wall tiles. The movement
; feel lives entirely in the grouped TWEAK constant below — edit it, hit F5.
MOVE_SPEED  EQU 2            ; pixels per frame                ; TWEAK + F5
MoveHero:
    ld a, ($C0A6)            ; raw d-pad read (0 = pressed)
    ld b, a
    ; ---- Right (bit0) ----
    bit 0, b
    jr nz, .notRight
    ld a, ($C0A0)
    add a, MOVE_SPEED
    ld c, a                  ; candidate X
    ld a, ($C0A1)
    ld d, a                  ; current Y
    call CanWalk
    jr nz, .notRight
    ld a, c
    ld ($C0A0), a
    ld a, 3
    ld ($C0A2), a           ; face right
.notRight:
    ; ---- Left (bit1) ----
    ld a, ($C0A6)
    ld b, a
    bit 1, b
    jr nz, .notLeft
    ld a, ($C0A0)
    sub MOVE_SPEED
    ld c, a
    ld a, ($C0A1)
    ld d, a
    call CanWalk
    jr nz, .notLeft
    ld a, c
    ld ($C0A0), a
    ld a, 2
    ld ($C0A2), a           ; face left
.notLeft:
    ; ---- Up (bit2) ----
    ld a, ($C0A6)
    ld b, a
    bit 2, b
    jr nz, .notUp
    ld a, ($C0A0)
    ld c, a                  ; X unchanged
    ld a, ($C0A1)
    sub MOVE_SPEED
    ld d, a                  ; candidate Y
    call CanWalk
    jr nz, .notUp
    ld a, d
    ld ($C0A1), a
    ld a, 1
    ld ($C0A2), a           ; face up
.notUp:
    ; ---- Down (bit3) ----
    ld a, ($C0A6)
    ld b, a
    bit 3, b
    jr nz, .notDown
    ld a, ($C0A0)
    ld c, a
    ld a, ($C0A1)
    add a, MOVE_SPEED
    ld d, a
    call CanWalk
    jr nz, .notDown
    ld a, d
    ld ($C0A1), a
    xor a
    ld ($C0A2), a           ; face down
.notDown:
    ret

; CanWalk — can an 8x8 hero stand at pixel (C=x, D=y)?  Samples the BG
; tilemap at both the top-left and bottom-right corners; returns Z set
; (walkable) only if neither corner is a wall tile. On a wall, plays a
; BUMP blip and returns NZ. Clobbers A,E,H,L; preserves B,C,D.
CanWalk:
    ; corner 1: (x, y)
    ld a, c
    ld e, a                  ; pixel x
    ld a, d                  ; pixel y
    call TileAt             ; A = tile index at (E/8, A/8)
    cp TILE_WALL
    jr z, .blocked
    ; corner 2: (x+7, y+7)
    ld a, c
    add a, 7
    ld e, a
    ld a, d
    add a, 7
    call TileAt
    cp TILE_WALL
    jr z, .blocked
    ; both clear -> Z set (cp TILE_WALL was not equal, but we need Z=walkable)
    xor a                    ; A=0, sets Z
    ret
.blocked:
    ld de, $0600             ; BUMP pitch (~256 Hz)            ; TWEAK + F5
    call SfxTone
    ld a, 1
    or a                     ; clears Z (NZ = blocked)
    ret

; TileAt — return in A the BG tilemap tile index at pixel (E=x, A=y).
; Computes cell = $9800 + (y/8)*32 + (x/8) and reads VRAM. Clobbers H,L,E.
; (Reads VRAM directly; reads are always safe.) Preserves B,C,D.
TileAt:
    srl a
    srl a
    srl a                    ; y/8 -> tile row
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl               ; row*32
    ld a, e
    srl a
    srl a
    srl a                    ; x/8 -> tile col
    ld e, a
    push de                  ; preserve D (caller's candidate Y)
    ld d, 0
    add hl, de               ; + col
    pop de
    push bc
    ld bc, $9800
    add hl, bc
    pop bc
    ld a, (hl)
    ret

; MoveGuard — simple horizontal patrol; reverse direction at corridor bounds.
MoveGuard:
    ld a, ($C0A5)            ; guardDX (signed)
    ld b, a
    ld a, ($C0A3)            ; guardX
    add a, b
    ld ($C0A3), a
    ; reverse at left bound (24) / right bound (136)
    cp 24
    jr nc, .ckRight
    ld a, 1
    ld ($C0A5), a           ; turn right
    ret
.ckRight:
    cp 136
    ret c
    ld a, $FF
    ld ($C0A5), a           ; turn left
    ret

; CheckCaught — if hero overlaps the guard (within 6 px on both axes),
; beep and send the hero back to spawn (16,16).
CheckCaught:
    ld a, ($C0A0)           ; heroX
    ld b, a
    ld a, ($C0A3)           ; guardX
    sub b                    ; guardX - heroX
    call .AbsLt6
    ret nz                   ; far apart on X -> safe
    ld a, ($C0A1)           ; heroY
    ld b, a
    ld a, ($C0A4)           ; guardY
    sub b
    call .AbsLt6
    ret nz                   ; far apart on Y -> safe
    ; caught!
    ld a, 16
    ld ($C0A0), a
    ld a, 16
    ld ($C0A1), a
    ld de, $0780            ; CAUGHT pitch (~1024 Hz)          ; TWEAK + F5
    call SfxTone
    ret
; .AbsLt6 — in A a signed difference; return Z if |A| < 6, else NZ.
.AbsLt6:
    bit 7, a
    jr z, .pos
    cpl
    inc a                    ; A = -A (abs)
.pos:
    cp 6
    jr c, .near
    or a                     ; NZ (>=6)
    ret
.near:
    xor a                    ; Z (<6)
    ret

; DrawSprites — copy hero + guard positions into the OAM shadow (Y+16, X+8).
DrawSprites:
    ld a, ($C0A1)           ; heroY
    add a, 16
    ld ($C000), a
    ld a, ($C0A0)           ; heroX
    add a, 8
    ld ($C001), a
    ld a, ($C0A4)           ; guardY
    add a, 16
    ld ($C004), a
    ld a, ($C0A3)           ; guardX
    add a, 8
    ld ($C005), a
    ret

; ====================== SOUND ======================
; SfxTone — short CH1 (pulse) blip. In: D=freq hi (bits2-0), E=freq lo.
; Clobbers A; reads (preserves) D,E; preserves B,C,HL.
SfxTone:
    xor a
    ldh ($10), a             ; NR10 = no sweep
    ld a, $A0
    ldh ($11), a             ; NR11 = duty 50% + length (short)
    ld a, $F2
    ldh ($12), a             ; NR12 = vol 15, decay (DAC on)
    ld a, e
    ldh ($13), a             ; NR13 = freq lo
    ld a, d
    or $C0
    ldh ($14), a             ; NR14 = trigger + length-enable + freq hi
    ret
