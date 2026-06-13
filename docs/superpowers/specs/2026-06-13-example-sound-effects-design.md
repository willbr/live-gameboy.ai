# Design: Sound effects for the example games

Date: 2026-06-13
Status: approved (design), pending implementation plan
Builds on: 2026-06-13-example-games-design.md (Pong, Snake, Breakout)

## Goal

Add gameplay sound effects to the three example games, wired to key events, and
make each effect a **live-coding hook** (tweakable pitch/duration constants with
a `TRY THIS LIVE` recipe) — consistent with the gallery's purpose of showcasing
the live-coding IDE. The DMG APU is fully implemented (4 channels, envelopes,
sweep, length counters, NR52 power; passes blargg `dmg_sound` register tests), so
this is asm-only work in the example files plus tests.

## Non-goals

- No background music, no shared sound engine/include (keep the established
  self-contained per-game style). CH2/CH3 are left unused.
- No emulator/APU changes. No changes to the test harness (`gb_tick` already
  advances the APU, and NR52 exposes per-channel status).

## Sound approach & channel allocation

Each game powers on the APU once in `Main` init:
- `NR52` ($FF26) = `$80` — APU on.
- `NR50` ($FF24) = `$77` — master volume (left/right ~max).
- `NR51` ($FF25) = `$FF` — route all channels to both speakers.

Then short SFX are triggered from gameplay procedures via two tiny per-game
helper routines:
- **`SfxTone`** — CH1 pulse+sweep (`NR10`–`NR14`): a short tonal blip. The caller
  loads the frequency (and the routine sets duty/envelope/length-enable + trigger).
  Used for hits, bounces, chimes.
- **`SfxNoise`** — CH4 noise (`NR41`–`NR44`): a percussive burst. Used for
  destroy/break events.

Both set the length-enable bit (NR14/NR44 bit6) with a short length so each effect
auto-stops — no manual note-off. Envelope decay (NR12/NR42) gives a natural fade.

Helper shape (illustrative; finalized in the plan):
- `SfxTone` expects the 11-bit frequency in a register pair / two bytes, writes
  `NR10=$00` (no sweep), `NR11=duty|length`, `NR12=envelope`, `NR13=freq lo`,
  `NR14=$C0|freq hi` (trigger + length-enable).
- `SfxNoise` writes `NR41=length`, `NR42=envelope`, `NR43=clock/divisor`,
  `NR44=$C0` (trigger + length-enable).

## Per-game sounds (event → channel)

**Pong** — 3 sounds, all CH1 (pitch distinguishes):
- Paddle hit — short high blip, fired in `UpdateBall` when DX reflects at a paddle
  zone (`$C0A2` set to `$01`/`$FF`).
- Wall bounce (top/bottom) — short lower blip, fired when DY reflects.
- Score cue — distinct third blip (quick descending/again-different pitch) fired on
  the far X-edge reflect. Pong currently reflects at the X zones rather than
  resetting, so this reuses that reflect site as the "point" cue; no gameplay
  change.

**Snake** — 2 sounds:
- Eat food — rising CH1 chime, fired in `StepSnake`'s grow branch (head == food).
- Wall wrap — soft low CH1 blip, fired in the `.xhi`/`.yhi` (or equivalent) wrap
  branches. (Snake has no death — walls wrap — so this is the natural second event.)

**Breakout** — 3 sounds:
- Brick break — CH4 noise burst, fired in `.BrickHit` when a tile-3 cell is cleared.
- Paddle hit — CH1 blip, fired when the ball reflects at the bottom paddle zone.
- Wall bounce — soft CH1 blip on side/top reflect.

Each event already has a clear hook point in the existing gameplay procedures;
wiring is loading the effect's constants then `call SfxTone` / `call SfxNoise`.

## Live-edit hook

- Each SFX's tweakable constants (frequency/pitch, duty, length, envelope) live in
  a clearly-labeled block at or near the call site, inside the main-loop
  (F5-hot-reloadable) zone — NOT in one-time init — so editing them and pressing
  F5 changes the sound mid-game with state intact.
- Each game's `TRY THIS LIVE` header comment gains a sound line, e.g.:
  - Pong: "edit the paddle-hit pitch in `UpdateBall`, press F5, hear it change."
  - Snake: "edit the eat-food chime pitch in `StepSnake`, F5."
  - Breakout: "edit the brick-break noise (`NR43` clock) in `.BrickHit`, F5."
- The APU power-on stays in init (F8 territory); only the per-effect parameters are
  the F5 surface.

## Testing

Per game, scripted-gameplay C tests (using the existing `tests/example_run.h`
harness + `gb_set_buttons`) that drive the game to each event and verify the
expected channel fires:
- Poll `NR52` ($FF26) once per frame across the run, OR-ing the channel-status
  bits (bit0=CH1, bit1=CH2, bit3=CH4) into an accumulator.
- Assert the expected channel bit was observed active at least once after the
  triggering event. Examples:
  - Pong: run ~200 frames (ball bounces off walls/paddles) → assert CH1 was active.
  - Snake: steer head into the food cell → assert CH1 was active (eat-food chime).
  - Breakout: run until a brick clears (reuse the existing brick-count check) →
    assert CH4 (noise) was active.
- A baseline assertion that `NR52` reads `$F0`+ (APU powered, top bit set) after
  init confirms power-on.
- Rationale: triggers set the channel-enabled bit and NR52 status synchronously on
  the APU register write; the length counter clears it after the effect, and SFX
  last several frames, so once-per-frame polling reliably catches the event. This
  ties the sound to actually firing during real gameplay, not just to register
  pokes — guarding against the "passes for the wrong reason" failure mode.

## Integration

- Modify `examples/pong.asm`, `examples/snake.asm`, `examples/breakout.asm`:
  add APU power-on in init, the `SfxTone`/`SfxNoise` helper(s), the per-event
  calls, and the `TRY THIS LIVE` sound lines.
- Add per-game sound tests (extend the existing `tests/test_example_*.c`, or add
  focused cases) — boot/gameplay tests must stay green.
- README: add a one-line note that the examples have tweakable sound effects (and
  which key/registers to edit), folded into the existing Example games section.
- All existing tests remain green; `make examples` still builds bootable ROMs.

## Open considerations

- Exact frequencies/lengths are tuned for "pleasant retro blip"; the plan picks
  concrete starting values (they're the live-edit surface, so approximate is fine).
- Keep SFX short (length ~0.1–0.25s) so rapid events (Pong rallies, Breakout brick
  runs) don't drone. Re-triggering each event restarts the channel, which is the
  desired behavior.
- Snake's per-step movement tick was considered and rejected (rhythmic ticking is
  annoying at ~8 steps/sec); eat-food + wall-wrap are the chosen events.
