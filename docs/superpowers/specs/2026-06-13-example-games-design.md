# Design: Live-coding example gallery — Pong, Snake, Breakout

Date: 2026-06-13
Status: approved (design), pending implementation plan

## Goal

Add three playable example games to `examples/` whose **primary purpose is to
showcase the live-coding IDE**. Gameplay is the vehicle; each game is authored
around a signature live-edit hook (hot-reload, tile painting, or OAM/memory
editing) plus secondary hooks, so a user can open the game and immediately *feel*
the live-editing workflow.

The games are: **Pong**, **Snake**, **Breakout**.

## Non-goals

- No shared engine / `include` file. Each example is self-contained (the
  assembler's include support is unproven, and shared code obscures the
  "edit *this* function" story the showcase depends on).
- No menu / multi-game ROM. Three standalone `.asm` files.
- Not a graduated assembly tutorial and not aiming for "best possible game" —
  the bar is "fun enough to play, obviously live-editable."

## Approach

Three standalone, copy-paste-able `.asm` files in `examples/`, each readable
top-to-bottom, each with a `TRY THIS LIVE` recipe in its header comment.

### Common structure (so F5 hot-reload works as advertised)

F5 hot-reload only preserves RAM/VRAM state for code that runs **in the main
loop**; init code (which only runs once) needs F8 (soft reset). Therefore every
file is structured:

- `Main:` → init: `sp` setup, LCD off, load tiles, set palettes (BGP/OBP),
  set up OAM DMA shadow, LCD on. This is the **F8** target — runs once.
- A VBlank-synced **main loop** calling small, named gameplay procedures.
  These are the **F5** targets, kept in the loop, with tweakable **constants
  grouped near the top** of the loop section so the edit target is obvious.

## The three games

### 1. `pong.asm` — sprites + physics
**Signature hook: hot-reload ball physics (F5).**

- Two paddles (player = d-pad up/down; CPU paddle tracks the ball) and a ball,
  all as **sprites** (OAM objects). Field is a dark BG with a dashed center-line
  tile. Optional middle score.
- Init: `InitOAM` sets up the objects via OAM DMA from a WRAM shadow at `$C000`.
- Main loop (VBlank-synced): `ReadInput → UpdatePaddle → UpdateAI →
  UpdateBall → Collide → CopyOAM` (DMA trigger).
- **Signature (F5):** `UpdateBall` holds `BALL_DX`/`BALL_DY` and the bounce math.
  Edit speed / add english on paddle hit, press F5 — ball changes mid-rally,
  score and positions intact.
- **Secondary:** paint the paddle tile live (reskin); edit OAM in the IDE to
  teleport the ball; tweak BGP to invert the court.

### 2. `snake.asm` — tilemap + input logic
**Signature hook: hot-reload the movement rule (F5).**

- Classic Snake on a tile grid. Body and food are drawn by writing tile indices
  into the **BG tilemap** (`$9800`), not sprites. Body cells held in a WRAM
  ring buffer.
- Init: paint border + clear field.
- Main loop, throttled to ~8 steps/sec via a frame counter:
  `ReadInput → StepSnake → CheckFood → DrawCells`.
- **Signature (F5):** `StepSnake` is the direction/growth rule. Hot-reload to
  change speed, allow/forbid reverse, or wrap-vs-die at walls — snake keeps its
  current length and position.
- **Secondary:** paint the food/body tile live (reskin); edit memory (length
  byte / head position) in the IDE to grow the snake instantly.

### 3. `breakout.asm` — tile-grid editing
**Signature hook: live-paint / edit the brick field (BG MAP + tile paint).**

- Paddle (sprite, d-pad) + ball (sprite), and a **grid of brick tiles** in the
  BG tilemap. Ball clears a brick by zeroing the corresponding tilemap cell.
- Init: `InitBricks` lays brick rows into `$9800`.
- Main loop: `ReadInput → UpdatePaddle → UpdateBall → BrickCollide → CopyOAM`.
- **Signature:** the brick field IS the tilemap, so editing the BG MAP / painting
  the brick tile in the IDE reshapes the level live — knock out a brick by hand,
  or repaint the brick tile to reskin every brick at once (VRAM provenance).
- **Secondary (F5):** `UpdateBall` physics; `BrickCollide` scoring/behavior.

## Shared conventions

- **Input:** `FF00` select-then-read (write select nibble, read low nibble;
  0 = pressed), debounced where needed.
- **VRAM safety:** all VRAM writes during VBlank or while LCD off; OAM updated
  via DMA (`FF46`) from a `$C0xx` shadow buffer.
- **Tweakables grouped:** physics/rate/level constants at the top of each loop
  section so the live-edit target is unmistakable.
- **Header recipe:** each file opens with a numbered `TRY THIS LIVE` recipe
  (3–4 steps) naming exact functions/tiles and which key to press (F5 vs F8).
- **Tile assets:** small inline `.2bpp` or in-source tile data so tiles are
  paintable in the IDE (like `demo.asm`'s `stripe.2bpp`).

## Testing

- **Boot/screenshot (per game):** assemble with `gbasm`, run headless `--shot`
  for N frames, assert a non-blank framebuffer / expected pixels (mirrors the
  existing e2e screenshot tests and `demo.asm`'s `ide-shot` path).
- **Smoke logic (per game):** run a scripted input sequence headless and assert
  a state byte changed as expected (e.g. ball moved, snake length grew, a brick
  cell cleared).
- Wire each game into the `examples/` build + CI screenshot path.

## Build / integration

- Add `pong.asm`, `snake.asm`, `breakout.asm` (plus any `.2bpp` assets) to
  `examples/`.
- Add Makefile targets to assemble each and produce a screenshot, alongside the
  existing `ide-shot` flow.
- Mention the gallery in `README.md` (and/or an `examples/README`).

## Open considerations

- AI paddle in Pong: keep trivial (track ball Y, clamped speed) to stay readable.
- Snake RNG for food placement: use a cheap LFSR or DIV-register read; document
  it so it's not mistaken for a bug.
- Order of implementation: Pong first (exercises sprites/OAM/physics — the
  broadest live-edit surface), then Snake, then Breakout.
