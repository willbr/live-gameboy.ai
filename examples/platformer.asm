; platformer.asm — a single-screen platformer for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/platformer.asm
; 2. HOT-RELOAD THE FEEL (F5): UpdatePlayer (in the main-loop zone below)
;    holds the grouped, commented feel constants GRAVITY / JUMP_IMPULSE /
;    MOVE_SPEED / TERMINAL_VEL. Tweak any of them, press F5 — the hero keeps
;    its exact position/velocity while the jump arc / run speed change live.
; 3. EDIT THE LEVEL LIVE: the platforms ARE the BG tilemap. Open the BG MAP
;    panel (or paint VRAM tile 2 = ground.2bpp) — draw a new ledge by hand
;    and the hero will land on it. Use F8 (soft reset) to re-run InitLevel.
; 4. RESKIN (paint): paint VRAM tile 1 (hero.2bpp) or tile 2 (ground.2bpp)
;    in the TILE editor — the hero / platforms restyle live.
; 5. TUNE THE SOUND (F5): edit the JUMP pitch in UpdatePlayer or the LAND
;    pitch in SfxLand, press F5, hear it change.
;
; Controls: Left/Right = run, A or Up = jump (only while standing on ground).
;
; Honest scope note: collision is a readable "sample the tilemap cell" model.
; Gravity/landing use the cell under the hero's feet; horizontal movement uses
; the cell at the hero's mid-height ahead of the leading edge. One 8x8 hero,
; 8x8 solid tiles, single screen (no scrolling). Simple on purpose.

SECTION "code", ROM0

Main:
    ld sp, $FFFE
    xor a
    ldh ($40), a             ; LCD off so we can touch VRAM

    ; tiles: 1=hero @ $8010, 2=ground @ $8020 (16 bytes each)
    ld hl, .HeroTile
    ld de, $8010
    ld bc, $0010
    call .CopyBC
    ld hl, .GroundTile
    ld de, $8020
    ld bc, $0010
    call .CopyBC

    ; clear BG tilemap $9800..$9BFF to blank tile 0 (open sky)
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

    ; zero OAM shadow $C000..$C09F (all sprites off-screen)
    ld hl, $C000
    ld bc, $00A0
.clroam:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clroam

    ; hero is sprite 0, tile 1
    ld a, 1
    ld ($C002), a            ; sprite 0 tile = hero

    ; init game vars (pixel coords; DrawSprites adds the OAM +8/+16 offsets)
    ld a, 24
    ld ($C0A0), a            ; playerX
    ld a, 0
    ld ($C0A1), a            ; playerY (start in the air -> falls onto a ledge)
    xor a
    ld ($C0A2), a            ; velY (signed; 0 = at rest)
    ld ($C0A3), a            ; onGround flag (0 = airborne)

    call InitLevel

    ; --- Sound: power on APU, full volume, route all channels both sides ---
    ld a, $80
    ldh ($26), a             ; NR52 = APU on (write FIRST)
    ld a, $77
    ldh ($24), a             ; NR50 = master volume L/R
    ld a, $FF
    ldh ($25), a             ; NR51 = all channels to both speakers

    ld a, $93                ; LCD on, tiledata $8000, OBJ on, BG on
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
    call UpdatePlayer
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

.HeroTile:
    incbin "examples/hero.2bpp"
.GroundTile:
    incbin "examples/ground.2bpp"

; ====================== LEVEL (F8 target) ======================
; Lay solid ground (tile 2) into the BG tilemap. A floor across the bottom plus
; two floating ledges. Edit this and press F8, or paint the BG MAP live.
InitLevel:
    ; --- floor: bottom two rows (16,17) all the way across (20 cols) ---
    ld hl, $9A00             ; $9800 + 16*32  (row 16)
    ld c, 2                  ; 2 rows
.floorRow:
    ld b, 20
.floorCol:
    ld a, 2
    ld (hl+), a
    dec b
    jr nz, .floorCol
    ld de, 12                ; 32 - 20 to reach next row start
    add hl, de
    dec c
    jr nz, .floorRow

    ; --- low ledge: row 13, cols 3..8 ---
    ld hl, $99A3             ; $9800 + 13*32 + 3
    ld b, 6
.ledgeA:
    ld a, 2
    ld (hl+), a
    dec b
    jr nz, .ledgeA

    ; --- high ledge: row 10, cols 12..17 ---
    ld hl, $994C             ; $9800 + 10*32 + 12
    ld b, 6
.ledgeB:
    ld a, 2
    ld (hl+), a
    dec b
    jr nz, .ledgeB
    ret

; ====================== GAMEPLAY ======================
; ReadInput — set move intent in WRAM. $C0A4 = moveDX (signed: 0/+/-),
; $C0A5 = jump request (1 if A or Up pressed this frame). Clobbers A,B.
ReadInput:
    xor a
    ld ($C0A4), a            ; default: no horizontal move
    ld ($C0A5), a            ; default: no jump request

    ; --- direction keys (Right/Left/Up) ---
    ld a, $20
    ldh ($00), a
    ldh a, ($00)             ; settle
    ldh a, ($00)
    ld b, a                  ; b = direction nibble (0 = pressed)

    bit 0, b                 ; Right
    jr nz, .notRight
    ld a, 1
    ld ($C0A4), a            ; moveDX = +1 (scaled by MOVE_SPEED below)
.notRight:
    bit 1, b                 ; Left
    jr nz, .notLeft
    ld a, $FF
    ld ($C0A4), a            ; moveDX = -1
.notLeft:
    bit 2, b                 ; Up = jump
    jr nz, .notUp
    ld a, 1
    ld ($C0A5), a
.notUp:

    ; --- action keys (A) ---
    ld a, $10
    ldh ($00), a
    ldh a, ($00)             ; settle
    ldh a, ($00)
    bit 0, a                 ; A = jump
    jr nz, .notA
    ld a, 1
    ld ($C0A5), a
.notA:
    ld a, $30
    ldh ($00), a             ; deselect
    ret

; ---------------------------------------------------------------------------
; UpdatePlayer — the SIGNATURE F5 hook. All the "feel" lives here.
; Apply gravity -> integrate Y -> resolve ground; then horizontal move with a
; wall check; then jump if standing. Hero state (pos/vel) survives a hot-reload
; because this runs every frame in the loop, so tweaks take effect instantly.
; ---------------------------------------------------------------------------
UpdatePlayer:
    ; ===================== TWEAK + F5 (the feel) =====================
GRAVITY       EQU 1          ; velY gained per frame (pull-down)   ; TWEAK + F5
JUMP_IMPULSE  EQU $FB        ; initial velY on jump (-5, signed)   ; TWEAK + F5
MOVE_SPEED    EQU 2          ; horizontal pixels per frame         ; TWEAK + F5
TERMINAL_VEL  EQU 4          ; max downward velY (clamp)           ; TWEAK + F5
    ; ================================================================

    ; ---------- vertical: gravity + integrate + land ----------
    ld a, ($C0A2)            ; velY (signed)
    add a, GRAVITY
    ; clamp downward speed to TERMINAL_VEL (only when moving down, velY < 128)
    bit 7, a
    jr nz, .velStored        ; negative (rising): no terminal clamp
    cp TERMINAL_VEL + 1
    jr c, .velStored
    ld a, TERMINAL_VEL
.velStored:
    ld ($C0A2), a

    ; tentative new Y = playerY + velY
    ld b, a                  ; b = velY (signed)
    ld a, ($C0A1)
    add a, b
    ld ($C0A1), a            ; commit Y first; landing check corrects it

    ; assume airborne until proven grounded this frame
    xor a
    ld ($C0A3), a

    ; only resolve ground while falling or resting (velY >= 0)
    ld a, ($C0A2)
    bit 7, a
    jr nz, .skidDone         ; rising -> skip ground resolve

    ; is the tile under the hero's feet solid?
    call FeetSolid           ; A = tile under feet (mid-bottom)
    cp 2
    jr nz, .skidDone         ; empty -> keep falling

    ; landed: snap Y up to the top of that tile row, zero velY, set grounded.
    ; feet row top = ((playerY + 8) & ~7); place feet there -> playerY = top-8.
    ld a, ($C0A1)
    add a, 8                 ; feet pixel
    and $F8                  ; round down to tile boundary (top of feet tile)
    sub 8                    ; back to player top
    ld ($C0A1), a
    xor a
    ld ($C0A2), a            ; velY = 0
    inc a
    ld ($C0A3), a            ; onGround = 1
    ld de, $0400             ; LAND pitch is in SfxLand; this is a soft thud
    call SfxLand
.skidDone:

    ; ---------- horizontal: move with a wall check ----------
    ld a, ($C0A4)            ; moveDX (0 / +1 / -1)
    or a
    jr z, .jumpPhase

    bit 7, a
    jr nz, .moveLeft
    ; ---- moving right ----
    call WallAheadRight
    or a
    jr nz, .jumpPhase        ; blocked by wall
    ld a, ($C0A0)
    cp 160 - 8               ; screen right edge
    jr nc, .jumpPhase
    add a, MOVE_SPEED
    ld ($C0A0), a
    jr .jumpPhase
.moveLeft:
    call WallAheadLeft
    or a
    jr nz, .jumpPhase        ; blocked by wall
    ld a, ($C0A0)
    cp MOVE_SPEED
    jr c, .jumpPhase         ; at left edge
    sub MOVE_SPEED
    ld ($C0A0), a

    ; ---------- jump (only when standing on ground) ----------
.jumpPhase:
    ld a, ($C0A5)            ; jump request
    or a
    ret z
    ld a, ($C0A3)            ; onGround?
    or a
    ret z
    ld a, JUMP_IMPULSE
    ld ($C0A2), a            ; velY = upward impulse
    xor a
    ld ($C0A3), a            ; leaving the ground
    ld de, $0780             ; JUMP pitch (~1024 Hz)          ; TWEAK + F5
    call SfxTone
    ret

; ---- FeetSolid: A = the BG tile under the hero's feet centre.
;      col = (playerX + 4) / 8, row = (playerY + 8) / 8. Clobbers A,B,C,D,E,HL. ----
FeetSolid:
    ld a, ($C0A1)
    add a, 8                 ; just below the hero's bottom edge
    ld d, a                  ; feet pixel Y
    ld a, ($C0A0)
    add a, 4                 ; hero horizontal centre
    ld e, a                  ; centre pixel X
    jr TileAt

; ---- WallAheadRight: A = tile just past the hero's right edge at mid-height.
;      col = (playerX + 8) / 8, row = (playerY + 4) / 8 -> nonzero(=2) if solid. ----
WallAheadRight:
    ld a, ($C0A1)
    add a, 4
    ld d, a
    ld a, ($C0A0)
    add a, 8                 ; right edge
    ld e, a
    jr TileAt

; ---- WallAheadLeft: A = tile just left of the hero's left edge at mid-height. ----
WallAheadLeft:
    ld a, ($C0A1)
    add a, 4
    ld d, a
    ld a, ($C0A0)
    dec a                    ; one pixel left of the left edge
    ld e, a
    jr TileAt

; ---- TileAt: In D = pixel Y, E = pixel X. Out A = tilemap byte at that pixel.
;      HL = $9800 + (Y/8)*32 + (X/8). Off-screen (>=160/144) reads as 0 (empty).
;      Clobbers A,B,C,H,L; preserves D,E. ----
TileAt:
    ld a, d
    srl a
    srl a
    srl a                    ; row = Y/8
    ld l, a
    ld h, 0
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl
    add hl, hl              ; row*32
    ld a, e
    srl a
    srl a
    srl a                    ; col = X/8
    ld c, a
    ld b, 0
    add hl, bc             ; + col
    ld bc, $9800
    add hl, bc             ; + base
    ld a, (hl)
    ret

DrawSprites:
    ld a, ($C0A1)            ; playerY
    add a, 16
    ld ($C000), a           ; OAM 0 Y
    ld a, ($C0A0)            ; playerX
    add a, 8
    ld ($C001), a           ; OAM 0 X
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

; SfxLand — soft low thud when the hero lands. Clobbers A,D,E.
SfxLand:
    ld de, $0380             ; LAND pitch (~98 Hz)            ; TWEAK + F5
    call SfxTone
    ret
