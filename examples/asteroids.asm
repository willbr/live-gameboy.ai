; asteroids.asm — Asteroids for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/asteroids.asm
; 2. HOT-RELOAD FLIGHT FEEL (F5): UpdateShip (in the main-loop zone below)
;    holds the flight constants grouped under "; TWEAK + F5":
;      THRUST   — how hard Up accelerates the ship
;      FRICTION — how fast momentum bleeds off (drag)
;    Edit them, press F5 — your ship keeps its position/velocity/heading and
;    instantly flies with the new feel.
; 3. RESKIN (paint): click VRAM tile 1 (ship), tile 2 (rock) or tile 3
;    (bullet) in the tile viewer and paint it — every matching sprite changes
;    on screen live (ship.2bpp / rock.2bpp / bullet.2bpp are paintable).
; 4. TELEPORT (OAM edit): edit sprite 0's Y/X in the OAM panel to fling the
;    ship across the field by hand. Use F8 (soft reset) if you edit init code.
; 5. TUNE THE SOUND (F5): edit the `ld de, $....` pitch in UpdateBullet
;    (FIRE) or in BlowUp (EXPLODE), press F5, hear it change.
; ======================================================================
;
; Controls: Left/Right = rotate, Up = thrust (momentum carries you),
;           A = fire a bullet in the way the ship is pointing.
; Ship and asteroids wrap around the screen edges.
;
; Positions are 8.8 fixed point (hi byte = pixel, lo byte = sub-pixel) so
; momentum and slow drift look smooth. Velocities are signed bytes added to
; the lo byte each frame. There is no multiply on the SM83, so the 8 headings
; use a precomputed signed dx,dy velocity table (HeadVel).

SECTION "code", ROM0

; ----- WRAM layout (game vars live above the OAM shadow at $C000..$C09F) -----
; Ship (8.8 fixed point position):
SHIPX   EQU $C0A0   ; 2 bytes  lo,hi
SHIPY   EQU $C0A2   ; 2 bytes  lo,hi
SHIPVX  EQU $C0A4   ; signed velocity (added to SHIPX lo each frame)
SHIPVY  EQU $C0A5   ; signed velocity
HEAD    EQU $C0A6   ; heading 0..7 (0=up, then clockwise)
ROTCD   EQU $C0A7   ; rotate cooldown (debounce so one tap = one step)
FIRECD  EQU $C0A8   ; fire cooldown (debounce A)
; Bullet (8.8 fixed point):
BULX    EQU $C0A9   ; 2 bytes
BULY    EQU $C0AB   ; 2 bytes
BULVX   EQU $C0AD   ; signed
BULVY   EQU $C0AE   ; signed
BULLIFE EQU $C0AF   ; frames remaining (0 = inactive)
; Asteroids: 3 of them, 6 bytes each: xlo,xhi,ylo,yhi,vx,vy
AST0    EQU $C0B0
AST1    EQU $C0B6
AST2    EQU $C0BC
ASTCNT  EQU $C0C2   ; live asteroid count (for the SFX-on-respawn nicety)

Main:
    ld sp, $FFFE

    ; --- LCD off so we can touch VRAM ---
    xor a
    ldh ($40), a              ; LCDC = 0

    ; --- Load sprite tiles: ship=tile1, rock=tile2, bullet=tile3 ---
    ld hl, .Tiles
    ld de, $8010             ; tile 1
    ld bc, $0030            ; 3 tiles * 16 bytes
    call .CopyBC

    ; --- Clear BG tilemap to blank tile 0 (black space) ---
    ld hl, $9800
    ld bc, $0400
.clrmap:
    xor a
    ld (hl+), a
    dec bc
    ld a, b
    or c
    jr nz, .clrmap

    ; --- Palettes: BGP=$E4, OBP0=$E4 ---
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

    ; --- Assign sprite tiles in the shadow OAM ---
    ; sprite 0 = ship (tile 1), sprite 1 = bullet (tile 3),
    ; sprites 2,3,4 = asteroids (tile 2)
    ld a, $01
    ld ($C002), a            ; ship tile
    ld a, $03
    ld ($C006), a            ; bullet tile
    ld a, $02
    ld ($C00A), a            ; asteroid 0 tile
    ld ($C00E), a            ; asteroid 1 tile
    ld ($C012), a            ; asteroid 2 tile

    ; --- Init ship: centre of screen, no velocity, pointing up ---
    xor a
    ld (SHIPX), a            ; X lo
    ld a, 80
    ld (SHIPX+1), a          ; X hi = 80
    xor a
    ld (SHIPY), a            ; Y lo
    ld a, 72
    ld (SHIPY+1), a          ; Y hi = 72
    xor a
    ld (SHIPVX), a
    ld (SHIPVY), a
    ld (HEAD), a             ; heading 0 = up
    ld (ROTCD), a
    ld (FIRECD), a
    ld (BULLIFE), a          ; bullet inactive

    ; --- Init asteroids: spread out with slow signed drift ---
    ;   AST0 at (24,24) drifting right+down
    ld hl, AST0
    ld a, 0
    ld (hl+), a             ; xlo
    ld a, 24
    ld (hl+), a             ; xhi
    ld a, 0
    ld (hl+), a             ; ylo
    ld a, 24
    ld (hl+), a             ; yhi
    ld a, $30
    ld (hl+), a             ; vx = +48/256 px/frame
    ld a, $20
    ld (hl+), a             ; vy = +32/256
    ;   AST1 at (130,40) drifting left+down
    ld hl, AST1
    ld a, 0
    ld (hl+), a
    ld a, 130
    ld (hl+), a
    ld a, 0
    ld (hl+), a
    ld a, 40
    ld (hl+), a
    ld a, $D0               ; vx = -48/256 (signed)
    ld (hl+), a
    ld a, $28
    ld (hl+), a
    ;   AST2 at (60,120) drifting right+up
    ld hl, AST2
    ld a, 0
    ld (hl+), a
    ld a, 60
    ld (hl+), a
    ld a, 0
    ld (hl+), a
    ld a, 120
    ld (hl+), a
    ld a, $38
    ld (hl+), a
    ld a, $D8               ; vy = -40/256 (signed)
    ld (hl+), a

    ld a, 3
    ld (ASTCNT), a

    ; --- Sound: power on APU, full volume, route all channels both sides ---
    ld a, $80
    ldh ($26), a             ; NR52 = APU on (write FIRST)
    ld a, $77
    ldh ($24), a             ; NR50 = master volume L/R
    ld a, $FF
    ldh ($25), a             ; NR51 = all channels to both speakers

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
    call UpdateShip
    call UpdateBullet
    call UpdateAsteroids
    call Collide
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

.Tiles:
    incbin "examples/ship.2bpp"      ; tile 1
    incbin "examples/rock.2bpp"      ; tile 2
    incbin "examples/bullet.2bpp"    ; tile 3

; ====================== INPUT ======================
; ReadInput: reads both joypad groups into RAM-free locals via B.
; Sets heading on Left/Right (one step per press, debounced by ROTCD),
; thrust flag handled in UpdateShip by re-reading? No — we stash the raw
; direction byte at $C0C3 and the action byte at $C0C4 for this frame so the
; gameplay procedures can consult them.
DIRBITS EQU $C0C3   ; this frame's direction nibble (0=pressed), bits: 0 R,1 L,2 U,3 D
ACTBITS EQU $C0C4   ; this frame's action nibble, bits: 0 A,1 B,2 Sel,3 Start

ReadInput:
    ; --- directions ---
    ld a, $20               ; select directions (bit5=1, bit4=0)
    ldh ($00), a
    ldh a, ($00)            ; settle
    ldh a, ($00)
    and $0F
    ld (DIRBITS), a
    ; --- actions ---
    ld a, $10               ; select actions (bit4=1, bit5=0)
    ldh ($00), a
    ldh a, ($00)
    ldh a, ($00)
    and $0F
    ld (ACTBITS), a
    ld a, $30               ; deselect
    ldh ($00), a

    ; --- rotate (one heading step per press, debounced) ---
    ld a, (ROTCD)
    or a
    jr z, .rotready
    dec a
    ld (ROTCD), a
    ret                     ; still cooling down: ignore rotate this frame
.rotready:
    ld a, (DIRBITS)
    bit 1, a                ; Left pressed?
    jr nz, .notLeft
    ld a, (HEAD)
    dec a
    and 7                   ; wrap 0..7
    ld (HEAD), a
    ld a, 4
    ld (ROTCD), a
    ret
.notLeft:
    bit 0, a                ; Right pressed?
    jr nz, .rotDone
    ld a, (HEAD)
    inc a
    and 7
    ld (HEAD), a
    ld a, 4
    ld (ROTCD), a
.rotDone:
    ret

; ====================== HEADINGS ======================
; HeadVel: 8 headings, 2 signed bytes each (dx, dy), magnitude ~24/256 along
; the facing direction. Heading 0 = up; clockwise. Diagonals use ~17 so the
; speed is roughly even. These are the unit "facing" vectors used for thrust
; and for bullet velocity (bullets are scaled up).
;          dx   dy
HeadVel:
    db   $00, $E8           ; 0 up        ( 0,-24)
    db   $11, $EF           ; 1 up-right  (+17,-17)
    db   $18, $00           ; 2 right     (+24, 0)
    db   $11, $11           ; 3 dn-right  (+17,+17)
    db   $00, $18           ; 4 down      ( 0,+24)
    db   $EF, $11           ; 5 dn-left   (-17,+17)
    db   $E8, $00           ; 6 left      (-24, 0)
    db   $EF, $EF           ; 7 up-left   (-17,-17)

; HeadVecPtr: In A=heading -> HL points at HeadVel[heading].
; Clobbers A,DE; preserves B,C.
HeadVecPtr:
    add a, a               ; heading*2
    ld e, a
    ld d, 0
    ld hl, HeadVel
    add hl, de
    ret

; ====================== SHIP ======================
; UpdateShip — the signature live-edit surface. Applies thrust along the
; current heading, bleeds velocity (friction), integrates the 8.8 position,
; and wraps at the screen edges.
UpdateShip:
    ; ---------------- TWEAK + F5 (flight feel) ----------------
    ; THRUST:   how much of the facing vector is added per thrust frame.
    ;           Higher = snappier acceleration. (shift count below)
    ; FRICTION: drag applied every frame regardless of thrust. Higher =
    ;           momentum dies faster. We subtract velocity>>FRICTION.
THRUST   EQU 3              ; facing vector >> THRUST added on Up   ; TWEAK + F5
FRICTION EQU 6              ; velocity     >> FRICTION bled / frame  ; TWEAK + F5
    ; ---------------------------------------------------------

    ; --- thrust on Up ---
    ld a, (DIRBITS)
    bit 2, a               ; Up pressed?
    jr nz, .noThrust
    ld a, (HEAD)
    call HeadVecPtr        ; HL -> dx,dy
    ; dx
    ld a, (hl)
    call .scaleThrust      ; A = signed dx >> THRUST
    ld b, a
    ld a, (SHIPVX)
    add a, b
    ld (SHIPVX), a
    inc hl
    ; dy
    ld a, (hl)
    call .scaleThrust
    ld b, a
    ld a, (SHIPVY)
    add a, b
    ld (SHIPVY), a
.noThrust:

    ; --- friction: vx -= vx >> FRICTION (signed) ---
    ld a, (SHIPVX)
    call .applyFriction
    ld (SHIPVX), a
    ld a, (SHIPVY)
    call .applyFriction
    ld (SHIPVY), a

    ; --- integrate X (8.8): pos += sign-extend(vx) ---
    ld a, (SHIPVX)
    ld b, a                ; b = vx (signed)
    ld a, (SHIPX)          ; lo
    add a, b
    ld (SHIPX), a
    ld a, (SHIPX+1)        ; hi
    ld c, 0
    jr nc, .xcarry
    inc c
.xcarry:
    ; add carry, and sign-extend a negative vx as a borrow on the hi byte
    add a, c
    bit 7, b
    jr z, .xpos
    dec a                  ; vx negative -> hi -= 1 (the add only handled carry)
.xpos:
    ld (SHIPX+1), a

    ; --- integrate Y ---
    ld a, (SHIPVY)
    ld b, a
    ld a, (SHIPY)
    add a, b
    ld (SHIPY), a
    ld a, (SHIPY+1)
    ld c, 0
    jr nc, .ycarry
    inc c
.ycarry:
    add a, c
    bit 7, b
    jr z, .ypos
    dec a
.ypos:
    ld (SHIPY+1), a

    ; --- wrap X hi into 0..159, Y hi into 0..143 ---
    ld a, (SHIPX+1)
    cp 160
    jr c, .xwok
    cp 200                 ; underflow (negative) shows as large value
    jr c, .xwrhi           ; 160..199 -> went off right
    add a, 160             ; negative -> +160 (wrap to right side)
    ld (SHIPX+1), a
    jr .xwok
.xwrhi:
    sub 160
    ld (SHIPX+1), a
.xwok:
    ld a, (SHIPY+1)
    cp 144
    jr c, .ywok
    cp 200
    jr c, .ywrhi
    add a, 144
    ld (SHIPY+1), a
    jr .ywok
.ywrhi:
    sub 144
    ld (SHIPY+1), a
.ywok:
    ret

; scaleThrust: In A=signed byte -> A = A >> THRUST keeping sign.
; Clobbers nothing else of interest (uses A).
.scaleThrust:
    push bc
    ld b, THRUST
.stLoop:
    sra a                  ; arithmetic shift right (preserves sign)
    dec b
    jr nz, .stLoop
    pop bc
    ret

; applyFriction: In A=signed velocity -> A = v - (v >> FRICTION).
; Clobbers B (restored). Keeps sign.
.applyFriction:
    push bc
    ld c, a                ; c = v
    ld b, FRICTION
.frLoop:
    sra a
    dec b
    jr nz, .frLoop         ; a = v >> FRICTION
    ld b, a
    ld a, c
    sub b                  ; v - (v>>FRICTION)
    pop bc
    ret

; ====================== BULLET ======================
; UpdateBullet — fire on A (debounced), move the bullet, despawn at lifetime.
UpdateBullet:
    ; --- decay fire cooldown ---
    ld a, (FIRECD)
    or a
    jr z, .fireReady
    dec a
    ld (FIRECD), a
    jr .moveBullet
.fireReady:
    ; fire only if no bullet currently alive (one shot at a time, clean demo)
    ld a, (BULLIFE)
    or a
    jr nz, .moveBullet
    ld a, (ACTBITS)
    bit 0, a               ; A pressed?
    jr nz, .moveBullet
    ; spawn: bullet starts at ship position, velocity = facing vector (scaled)
    ld a, (SHIPX)
    ld (BULX), a
    ld a, (SHIPX+1)
    ld (BULX+1), a
    ld a, (SHIPY)
    ld (BULY), a
    ld a, (SHIPY+1)
    ld (BULY+1), a
    ld a, (HEAD)
    call HeadVecPtr        ; HL -> dx,dy
    ; bullet velocity = facing vector << 1 (faster than the ship)
    ld a, (hl)
    sla a
    ld (BULVX), a
    inc hl
    ld a, (hl)
    sla a
    ld (BULVY), a
    ld a, 60
    ld (BULLIFE), a        ; lives ~1s
    ld a, 8
    ld (FIRECD), a         ; brief re-fire debounce
    ld de, $0780           ; FIRE pitch (~1024 Hz)        ; TWEAK + F5
    call SfxTone
.moveBullet:
    ld a, (BULLIFE)
    or a
    ret z                  ; no bullet
    dec a
    ld (BULLIFE), a
    ; integrate X (8.8 signed)
    ld a, (BULVX)
    ld b, a
    ld a, (BULX)
    add a, b
    ld (BULX), a
    ld a, (BULX+1)
    ld c, 0
    jr nc, .bxc
    inc c
.bxc:
    add a, c
    bit 7, b
    jr z, .bxp
    dec a
.bxp:
    ld (BULX+1), a
    ; integrate Y
    ld a, (BULVY)
    ld b, a
    ld a, (BULY)
    add a, b
    ld (BULY), a
    ld a, (BULY+1)
    ld c, 0
    jr nc, .byc
    inc c
.byc:
    add a, c
    bit 7, b
    jr z, .byp
    dec a
.byp:
    ld (BULY+1), a
    ; despawn if it left the screen
    ld a, (BULX+1)
    cp 160
    jr nc, .bulOff
    ld a, (BULY+1)
    cp 144
    jr nc, .bulOff
    ret
.bulOff:
    xor a
    ld (BULLIFE), a
    ret

; ====================== ASTEROIDS ======================
; UpdateAsteroids — drift each live asteroid in a straight line, wrap edges.
UpdateAsteroids:
    ld hl, AST0
    call .one
    ld hl, AST1
    call .one
    ld hl, AST2
    call .one
    ret
; .one: HL -> asteroid (xlo,xhi,ylo,yhi,vx,vy). Skips if "dead" (vx=vy=0).
.one:
    ; dead asteroids have vx=0 and vy=0 and are parked off-screen at xhi=200
    ld a, (hl)             ; we won't early-out on velocity; a parked asteroid
                           ; has xhi=200 (off screen) and zero velocity so it
                           ; simply stays there. Continue integrating harmlessly.
    push hl
    ; X
    inc hl                 ; -> xhi
    inc hl
    inc hl
    inc hl                 ; -> vx
    ld a, (hl)             ; vx
    ld d, a
    pop hl
    push hl
    ; integrate X lo/hi with signed d
    ld a, (hl)             ; xlo
    add a, d
    ld (hl), a
    inc hl                 ; xhi
    ld a, (hl)
    ld c, 0
    jr nc, .axc
    inc c
.axc:
    add a, c
    bit 7, d
    jr z, .axp
    dec a
.axp:
    ; wrap xhi into 0..159 (parked asteroids at 200 stay parked: skip wrap if vx=0&vy=0)
    ld (hl), a
    pop hl
    push hl
    ; Y: advance HL to ylo
    inc hl
    inc hl                 ; -> ylo
    ; fetch vy
    push hl
    inc hl
    inc hl
    inc hl                 ; -> vy
    ld a, (hl)
    ld d, a
    pop hl                 ; -> ylo
    ld a, (hl)
    add a, d
    ld (hl), a
    inc hl                 ; yhi
    ld a, (hl)
    ld c, 0
    jr nc, .ayc
    inc c
.ayc:
    add a, c
    bit 7, d
    jr z, .ayp
    dec a
.ayp:
    ld (hl), a
    pop hl
    ; now wrap both axes, but leave parked asteroids (vx=vy=0) alone
    push hl
    inc hl
    inc hl
    inc hl
    inc hl                 ; vx
    ld a, (hl)
    inc hl                 ; vy
    or (hl)                ; vx | vy
    pop hl
    or a
    ret z                  ; parked (dead) asteroid: don't wrap it on-screen
    ; wrap X
    inc hl                 ; xhi
    ld a, (hl)
    cp 160
    jr c, .wxok
    cp 200
    jr c, .wxhi
    add a, 160
    ld (hl), a
    jr .wxok
.wxhi:
    sub 160
    ld (hl), a
.wxok:
    inc hl
    inc hl                 ; yhi (xhi -> ylo -> yhi)
    ld a, (hl)
    cp 144
    jr c, .wyok
    cp 200
    jr c, .wyhi
    add a, 144
    ld (hl), a
    jr .wyok
.wyhi:
    sub 144
    ld (hl), a
.wyok:
    ret

; ====================== COLLISION ======================
; Collide — bullet vs each asteroid (axis-distance check). On a hit: park the
; asteroid off-screen (dead), kill the bullet, play the explosion SFX.
Collide:
    ld a, (BULLIFE)
    or a
    ret z                  ; no bullet, nothing to hit
    ld hl, AST0
    call .check
    ld hl, AST1
    call .check
    ld hl, AST2
    call .check
    ret
; .check: HL -> asteroid. If alive and within range of the bullet, blow it up.
.check:
    ; alive? (vx|vy != 0)
    push hl
    inc hl
    inc hl
    inc hl
    inc hl                 ; vx
    ld a, (hl)
    inc hl
    or (hl)                ; vy
    pop hl
    ret z                  ; dead asteroid: skip
    ; |bulX.hi - astX.hi| <= 6 ?
    ld a, (BULX+1)
    push hl
    inc hl                 ; -> xhi
    sub (hl)
    pop hl
    call .absA
    cp 7
    ret nc                 ; too far in X
    ; |bulY.hi - astY.hi| <= 6 ?
    ld a, (BULY+1)
    push hl
    inc hl
    inc hl
    inc hl                 ; -> yhi
    sub (hl)
    pop hl
    call .absA
    cp 7
    ret nc                 ; too far in Y
    ; HIT! park the asteroid off-screen and clear its velocity (dead)
    push hl
    ld a, 200
    inc hl
    ld (hl), a             ; xhi = 200 (off screen)
    inc hl
    inc hl                 ; yhi
    ld (hl), a             ; yhi = 200
    inc hl
    xor a
    ld (hl+), a            ; vx = 0
    ld (hl), a             ; vy = 0
    pop hl
    ; kill bullet
    xor a
    ld (BULLIFE), a
    call BlowUp
    ret
; .absA: A = |A| (signed). Clobbers nothing else.
.absA:
    bit 7, a
    ret z
    cpl
    inc a
    ret

; ====================== DRAW ======================
; DrawSprites — copy fixed-point hi bytes (pixels) into the OAM shadow with the
; +16/+8 hardware offsets. Off-screen / dead things get Y=0 (hidden).
DrawSprites:
    ; ship -> sprite 0
    ld a, (SHIPY+1)
    add a, 16
    ld ($C000), a
    ld a, (SHIPX+1)
    add a, 8
    ld ($C001), a
    ; bullet -> sprite 1 (hide if inactive)
    ld a, (BULLIFE)
    or a
    jr nz, .bulOn
    xor a
    ld ($C004), a          ; Y=0 hides it
    jr .ast
.bulOn:
    ld a, (BULY+1)
    add a, 16
    ld ($C004), a
    ld a, (BULX+1)
    add a, 8
    ld ($C005), a
.ast:
    ; asteroids -> sprites 2,3,4
    ld hl, AST0
    ld de, $C008
    call .drawAst
    ld hl, AST1
    ld de, $C00C
    call .drawAst
    ld hl, AST2
    ld de, $C010
    call .drawAst
    ret
; .drawAst: HL -> asteroid, DE -> OAM entry (Y,X). Dead (xhi=200) -> hidden.
.drawAst:
    inc hl                 ; -> xhi
    ld a, (hl)
    cp 200
    jr nz, .alive
    ; dead: hide (Y=0)
    xor a
    ld (de), a
    ret
.alive:
    ld b, a                ; b = xhi
    inc hl
    inc hl                 ; -> yhi
    ld a, (hl)
    add a, 16
    ld (de), a             ; OAM Y
    inc de
    ld a, b
    add a, 8
    ld (de), a             ; OAM X
    ret

; ====================== SOUND ======================
; BlowUp — explosion noise on CH4. Clobbers A.
BlowUp:
    ld a, $00
    ldh ($20), a           ; NR41 length
    ld a, $F1
    ldh ($21), a           ; NR42 vol 15, decay        ; TWEAK + F5
    ld a, $33
    ldh ($22), a           ; NR43 noise freq (rumble)  ; TWEAK + F5
    ld a, $C0
    ldh ($23), a           ; NR44 trigger + length
    ret

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
