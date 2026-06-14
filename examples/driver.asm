; driver.asm — top-down road racer for the live-gameboy IDE
;
; ============================ TRY THIS LIVE ============================
; 1. Run:  ./live-gameboy-ide examples/driver.asm
; 2. HOT-RELOAD THE FEEL (F5): Drive holds the driving constants grouped
;    under "; TWEAK + F5" — SCROLL_SPEED (how fast the road rushes up),
;    STEER_SPEED (how quick the car slides), ACCEL/BRAKE deltas. Edit any
;    of them, press F5 — the road keeps scrolling and your car keeps its
;    position/speed, but the handling changes instantly.
; 3. RESKIN (paint): click a VRAM tile in the tile viewer and paint it —
;    tile 1 road, 2 grass, 3 lane-dash, 4 kerb, 5 your car, 6 rival car
;    (road/grass/lane/edge/car/rival .2bpp are all paintable assets).
; 4. TELEPORT (OAM edit): edit sprite 0's X in the OAM panel to shove your
;    car across the road by hand.
; 5. TUNE THE SOUND (F5): edit the CRASH / GRASS pitch `ld de,$....`
;    constants in Drive, press F5, hear it change.
;    Use F8 (soft reset) if you edit Main / the road-building init code.
; ======================================================================
;
; Controls: Left/Right steer the car across the road. Up accelerate,
; Down brake (both change how fast the road scrolls). Drive onto the grass
; and you slow down with a rumble; hit a rival car and you crash (reset to
; the road, penalty SFX). Rivals spawn at the top via a DIV-register RNG.

SECTION "code", ROM0

; ---- road geometry (tile columns; screen is 20 tiles / 160px wide) ----
; cols 0..5 grass | 6 kerb | 7..12 road (9 lane-dash) | 13 kerb | 14..19 grass
ROAD_LEFT_PX  EQU 56          ; leftmost road pixel  (col 7 * 8)
ROAD_RIGHT_PX EQU 96          ; rightmost car X so the 8px car stays on road
CAR_Y_PX      EQU 120         ; car sits near the bottom

Main:
    ld sp, $FFFE

    ; --- LCD off so we can touch VRAM ---
    xor a
    ldh ($40), a              ; LCDC = 0

    ; --- Load tiles 1..6 -> $8010 (16 bytes each, contiguous) ---
    ;   1 road  2 grass  3 lane-dash  4 kerb  5 car  6 rival
    ld hl, .Tiles
    ld de, $8010
    ld bc, $0060             ; 6 tiles * 16 bytes
    call .CopyBC

    ; --- Build the road into BG tilemap $9800 (32 cols x 32 rows) ---
    ; Draw all 32 rows the same so the road is seamless as it wraps under SCY.
    ld hl, $9800
    ld c, 32                  ; row counter
.row:
    ld b, 0                   ; column counter (0..31)
.col:
    ld a, b
    ; default tile = grass (2)
    ld d, 2
    cp 6
    jr z, .kerb               ; col 6 -> kerb
    cp 13
    jr z, .kerb               ; col 13 -> kerb
    cp 7
    jr c, .put                ; col <7 -> grass
    cp 14
    jr nc, .put              ; col >=14 -> grass
    ; cols 7..13 are the road band; col 9 is the lane dash
    ld d, 1                   ; road
    cp 9
    jr nz, .put
    ; lane dash only on some rows for a dashed centre line
    ld a, c
    and 3
    jr nz, .put              ; 3 of 4 rows: plain road
    ld d, 3                   ; lane dash tile
    jr .put
.kerb:
    ld d, 4
.put:
    ld a, d
    ld (hl+), a
    inc b
    ld a, b
    cp 32
    jr nz, .col
    dec c
    jr nz, .row

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

    ; --- Sprite tiles: car=tile5 (sprite 0), rival=tile6 (sprite 1) ---
    ld a, 5
    ld ($C002), a            ; car tile
    ld a, 6
    ld ($C006), a            ; rival tile

    ; --- Init game vars ($C0A0+) ---
    ld a, 72
    ld ($C0A0), a            ; carX  (middle of the road)
    ld a, 2
    ld ($C0A1), a            ; speed (current scroll step per frame)
    xor a
    ld ($C0A2), a            ; scy   (BG vertical scroll accumulator)
    ld a, 80
    ld ($C0A3), a            ; rivalX
    ld a, 200
    ld ($C0A4), a            ; rivalY (>=160 == off-screen / inactive)

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
    call Drive
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
    incbin "examples/road.2bpp"     ; 1 road surface
    incbin "examples/grass.2bpp"    ; 2 grass / verge
    incbin "examples/lane.2bpp"     ; 3 lane dash
    incbin "examples/edge.2bpp"     ; 4 kerb / road edge
    incbin "examples/car.2bpp"      ; 5 player car
    incbin "examples/rival.2bpp"    ; 6 rival car

; ====================== GAMEPLAY ======================

; ReadInput — d-pad: Left/Right steer (set $C0A5 dir flag), Up/Down adjust
; speed directly. (0 = pressed.) Clobbers A,B.
ReadInput:
    ld a, $20                ; select direction keys
    ldh ($00), a
    ldh a, ($00)            ; settle
    ldh a, ($00)
    ld b, a                  ; preserve joypad read
    ld a, $30
    ldh ($00), a            ; deselect
    ; bit0 Right, 1 Left, 2 Up, 3 Down (0 = pressed)
    xor a
    ld ($C0A5), a            ; steer dir: 0=none
    bit 1, b
    jr nz, .nl
    ld a, $FF               ; -1 = steer left
    ld ($C0A5), a
.nl:
    bit 0, b
    jr nz, .nr
    ld a, 1                  ; +1 = steer right
    ld ($C0A5), a
.nr:
    ld a, 1
    ld ($C0A6), a           ; throttle flag default = coast
    bit 2, b
    jr nz, .nu
    ld a, 2                  ; accelerate
    ld ($C0A6), a
.nu:
    bit 3, b
    jr nz, .nd
    xor a                    ; brake
    ld ($C0A6), a
.nd:
    ret

; Drive — the F5 hot-reload surface. Scroll the road, steer/clamp the car,
; manage speed, move the rival down, and check collisions.
Drive:
    ; ----- TWEAK + F5 : the driving feel lives here -----
    ; (edit any of these, press F5; road + car keep their state)
.STEER_SPEED  EQU 2          ; px the car slides per frame when steering
.MAX_SPEED    EQU 5          ; fastest road scroll (px/frame)
.MIN_SPEED    EQU 1          ; slowest (braking floor)
.GRASS_SPEED  EQU 1          ; speed clamp while off-road on the grass
.RIVAL_SPEED  EQU 1          ; extra px/frame the rival drifts down
.CRASH_PITCH  EQU $0500      ; crash SFX pitch
.GRASS_PITCH  EQU $0380      ; off-road rumble pitch
    ; ----------------------------------------------------

    ; --- adjust speed from throttle flag ($C0A6: 0 brake,1 coast,2 accel) ---
    ld a, ($C0A6)
    cp 2
    jr nz, .notAccel
    ld a, ($C0A1)
    cp .MAX_SPEED
    jr nc, .speedDone
    inc a
    ld ($C0A1), a
    jr .speedDone
.notAccel:
    or a
    jr nz, .speedDone        ; coast: leave speed as-is
    ld a, ($C0A1)            ; brake
    cp .MIN_SPEED + 1
    jr c, .speedDone
    dec a
    ld ($C0A1), a
.speedDone:

    ; --- steer the car (carX += STEER_SPEED * dir), then clamp to road ---
    ld a, ($C0A5)            ; dir (-1/0/+1, signed)
    or a
    jr z, .noSteer
    ld b, a                  ; sign in b
    ld a, ($C0A0)            ; carX
    bit 7, b
    jr z, .steerRight
    sub .STEER_SPEED         ; left
    jr .clampX
.steerRight:
    add a, .STEER_SPEED
.clampX:
    ; clamp carX to [ROAD_LEFT_PX, ROAD_RIGHT_PX]
    cp ROAD_LEFT_PX
    jr nc, .notTooLeft
    ld a, ROAD_LEFT_PX
.notTooLeft:
    cp ROAD_RIGHT_PX + 1
    jr c, .storeX
    ld a, ROAD_RIGHT_PX
.storeX:
    ld ($C0A0), a
.noSteer:

    ; --- scroll the road: scy += speed each frame -> road rushes up ---
    ld a, ($C0A1)            ; speed
    ld b, a
    ld a, ($C0A2)            ; scy
    add a, b
    ld ($C0A2), a
    ldh ($42), a             ; SCY

    ; --- rival car: move it down with the scroll; respawn when it passes ---
    ld a, ($C0A4)            ; rivalY
    cp 160
    jr c, .rivalActive
    ; inactive: maybe spawn using DIV-register RNG
    ldh a, ($04)             ; DIV (free-running) as cheap RNG
    and $1F                  ; 0..31
    cp 4
    jr nc, .rivalDone        ; only spawn ~1/8 of the inactive frames
    ; spawn at top, random-ish X across the road band (56..96)
    ldh a, ($04)
    and $1F                  ; 0..31
    add a, ROAD_LEFT_PX      ; 56..87 -> roughly on the road
    ld ($C0A3), a            ; rivalX
    xor a
    ld ($C0A4), a            ; rivalY = 0 (top)
    jr .rivalDone
.rivalActive:
    ; rival descends at (speed + RIVAL_SPEED)
    ld b, a                  ; rivalY
    ld a, ($C0A1)            ; speed
    add a, .RIVAL_SPEED
    add a, b
    ld ($C0A4), a            ; new rivalY (may exceed 160 -> goes inactive)
.rivalDone:

    ; --- collision: axis-distance check car vs rival (within 8px both axes) ---
    ld a, ($C0A4)            ; rivalY
    cp 160
    jr nc, .offRoadCheck     ; rival inactive -> skip
    ; |rivalY - CAR_Y_PX| < 8 ?
    ld b, a
    ld a, CAR_Y_PX
    sub b
    bit 7, a
    jr z, .ypos
    cpl
    inc a                    ; abs
.ypos:
    cp 8
    jr nc, .offRoadCheck
    ; |rivalX - carX| < 8 ?
    ld a, ($C0A3)            ; rivalX
    ld b, a
    ld a, ($C0A0)            ; carX
    sub b
    bit 7, a
    jr z, .xpos
    cpl
    inc a
.xpos:
    cp 8
    jr nc, .offRoadCheck
    ; CRASH: penalty -> drop speed to MIN, send rival away, SFX
    ld a, .MIN_SPEED
    ld ($C0A1), a
    ld a, 200
    ld ($C0A4), a            ; rival off-screen
    ld de, .CRASH_PITCH      ; CRASH pitch              ; TWEAK + F5
    call SfxTone
    ret

.offRoadCheck:
    ; Off-road? (this build keeps the car clamped on tarmac, so the grass
    ; rumble fires only if a live OAM/memory edit shoves carX off the band.)
    ld a, ($C0A0)            ; carX
    cp ROAD_LEFT_PX
    jr c, .onGrass
    cp ROAD_RIGHT_PX + 1
    jr nc, .onGrass
    ret                      ; on the road -> done
.onGrass:
    ld a, ($C0A1)
    cp .GRASS_SPEED + 1
    jr c, .grassSfx
    ld a, .GRASS_SPEED
    ld ($C0A1), a            ; grass slows you down
.grassSfx:
    ld de, .GRASS_PITCH      ; off-road rumble pitch    ; TWEAK + F5
    call SfxTone
    ret

; DrawSprites — write shadow OAM for the car (sprite 0) and rival (sprite 1).
DrawSprites:
    ; car -> sprite 0
    ld a, CAR_Y_PX
    add a, 16
    ld ($C000), a           ; carY (fixed)
    ld a, ($C0A0)           ; carX
    add a, 8
    ld ($C001), a
    ; rival -> sprite 1
    ld a, ($C0A4)           ; rivalY
    add a, 16
    ld ($C004), a
    ld a, ($C0A3)           ; rivalX
    add a, 8
    ld ($C005), a
    ret

; ====================== SOUND ======================
; SfxTone — short CH1 (pulse) blip. In: D=freq hi (bits2-0), E=freq lo.
; Clobbers A; reads (preserves) D,E; preserves B,C,HL. Freq is the live-edit
; surface (see CRASH/GRASS pitch constants in Drive).
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
