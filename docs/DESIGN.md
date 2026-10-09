# PizzaRivals — Design Doc

> Play Pizza Tower as a Rivals of Aether character: real RoA movement and attacks, any stock or
> workshop character, switchable in-game, inside real Pizza Tower levels with PT enemies, doors and
> escape sequences.

Status: draft v0.1 · 2026-10-03. Modelled on SkyCraft (`../SkyCraft/docs/DESIGN.md`).

## 1. Core principle (same as SkyCraft)

**Neither game is rewritten.** RoA runs its own engine: physics, state machine, hitboxes, the GML
interpreter that executes workshop characters. Pizza Tower runs its own world: rooms, enemies,
doors, score, escape timer. The two bridges only **translate**:

- PT tells RoA *what the world is shaped like* and *what button state the player is in*.
- RoA tells PT *where the player is*, *what to draw*, and *what it hit*.

If we find ourselves re-implementing a RoA mechanic in PT GML, or a PT mechanic in C++, the design
has gone wrong. This is also why **all 98 workshop characters + stock cast work with zero
per-character code**: their GML runs inside RoA unchanged.

## 2. Why two processes (and not a GML port)

| Fact | Consequence |
|---|---|
| RoA is **32-bit YYC** (`RivalsofAether.exe`, PE32 x86). Pizza Tower is **64-bit**. | They cannot share a process. Shared memory IPC, exactly like SkyCraft. |
| RoA characters are plain `.gml` + PNGs, executed by RoA's in-engine interpreter (`gml_parser_run`). PT's GML has no runtime `eval`. | Hosting characters in PT would need a full GML interpreter *plus* a clone of the RoA engine API (hundreds of built-ins, hitbox/window/article systems). That is "rewriting the other game". |
| RoA has a mod loader (hook any builtin by name, call builtins, MinHook). | We can drive a real RoA match from a DLL. |
| PT has a full decomp; no mod loader. | We edit the decomp and build it (a modified standalone game), plus a small native extension for IPC. |

Considered and rejected: (a) transpiling character GML into PT GML at build time (breaks on
workshop updates, huge API surface); (b) PT-authoritative movement with RoA only supplying
animations (loses what makes the characters themselves: dashes, double jumps, specials that move you).

## 3. Components

```
┌──────── RivalsofAether.exe (x86, hidden window, muted) ───────┐      ┌──────── PizzaTower.exe (x64, built from decomp) ────────┐
│ roa-bridge/  (mods/pizzarivals.dll via roa-mod-loader)         │      │ pt-bridge/extension  (pizzarivals_pt.dll, GM extension) │
│                                                                │      │ pt-bridge/gml        (patches into the decomp)          │
│  MatchBoot      ─ start 1P match / training room, no menus     │      │                                                         │
│  CharSwitch     ◀ SELECT_CHARACTER → reload player             │ ◀──  │  obj_rivals_controller  ─ owns the bridge, char menu    │
│  InputBridge    ◀ PRInput → player input state                 │ ◀──  │  scr_rivals_input       ─ PT keys → PRInput             │
│  CollisionMirror◀ PRSolidSet → par_block instances             │ ◀──  │  scr_rivals_collision   ─ PT solids/slopes → PRSolidSet │
│  StateExport    ─ x,y,hsp,vsp,state,dir ───────────────────────┼───▶  │  state_rivals           ─ puppet follows RoA position   │
│  HitboxExport   ─ active hitboxes ─────────────────────────────┼───▶  │  scr_rivals_hitboxes    ─ RoA hitbox → PT damage volume │
│  FrameCapture   ─ player layer RGBA → shared memory ───────────┼───▶  │  draw: pixels → sprite/surface at the puppet            │
│  HurtApplier    ◀ PRHurt → RoA hit pipeline                    │ ◀──  │  scr_hurtplayer hook → PR_EV_PLAYER_HURT                │
└────────────────────────────────────────────────────────────────┘      └─────────────────────────────────────────────────────────┘
                       shared memory  Local\PizzaRivals_v1  (protocol/pizzarivals_protocol.h)
```

Layout of this repo:

```
protocol/      shared C header + x86/x64 layout test (done)
roa-bridge/    x86 DLL for roa-mod-loader (CMake, MSVC, MinHook via the loader)
pt-bridge/
  extension/   x64 GM extension DLL: opens the shared memory, exposes pr_* functions to GML
  gml/         scripts/objects to drop into the OpenTower decomp (kept as a patch set, not a fork)
characters/    catalog.json (generated) + stock.json
tools/         build_catalog.py (done), packaging, fake peers for testing each side alone
```

## 4. Characters: one catalog, no per-character code

`tools/build_catalog.py` scans `…/workshop/content/383980/*/config.ini` (type 0 = character,
2 = stage) and merges `characters/stock.json` into `characters/catalog.json`. Today: **18 stock + 98
workshop** characters, all with `init.gml`. Ids: `stock:kragg`, `workshop:1866016173`.

Switching = `PR_EV_SELECT_CHARACTER{id}` → RoA bridge tears down the player and loads the character
through RoA's own loader (stock = native object; workshop = the same path the in-game CSS uses,
`search_workshop_chars` / `get_workshop_data`). Result comes back as `CHAR_READY` / `CHAR_FAILED`
(a broken workshop character reports an error and PT keeps the previous one). The PT selector menu is
generated from the catalog (portraits from `charselect.png`), so new Workshop subscriptions appear
after re-running the scanner (later: the bridge rescans on launch).

## 5. Coordinates and scale

Both are Y-down pixel worlds, so no axis flip (cf. SkyCraft's Z-up→Y-up). A single `PR_SCALE`
(default 1.0, tunable) converts RoA px ↔ PT px. PT's Peppino is roughly the size of a RoA
character, so 1.0 should read correctly; verify in Phase 1.

## 6. The player (RoA authoritative, PT puppet)

1. RoA integrates the character's physics each frame from `PRInput`.
2. Every PT step, the extension reads `PRPlayerState`; the PT player object (the **puppet**) is
   moved to that position. The puppet stays a real PT `obj_player` in a custom state
   (`states.rivals`) so enemies, doors, triggers, collectibles, camera, score and escape logic all
   keep working, same reasoning as SkyCraft's PlayerPuppet.
3. PT-owned events override RoA: room change / door / tube / cutscene set `PR_PT_PAUSED|HIDDEN` and
   `warp` so RoA teleports to the new spawn.

## 7. Collision mirror (SkyCraft's CollisionField)

RoA's physics asks "is there a solid here" via instances of its block objects. PT's solids are
`obj_solid`, `obj_slope`, platforms and tile collision. Each step (cheap, diffed by `revision`) PT
exports the solids within ~2 screens of the puppet as `PRSolid` rects (+ slope kinds); RoA creates/
moves/destroys matching `par_block` / jump-through / slope instances. RoA's own movement code then
runs unchanged against PT geometry. Steep slopes: PT slopes are 45°; RoA handles ramps via block
masks, to be verified (spike S3).

## 8. Rendering

RoA renders **only the player layer** (character, articles, hit FX, character `pre_draw/post_draw`
output; no stage, no HUD) into a transparent surface, camera locked on the player. Frame size is the
PT view (960×540). The bridge copies RGBA into the double-buffered shared pixel area (~2 MB/frame;
CPU copy is fine at this size, no GPU interop needed). PT's extension uploads it with
`buffer`→`surface`/`sprite` and `draw_surface` at `puppet − (W/2, H/2)`. Because RoA draws the
character itself, **palette shaders, custom draw code and effects match the real game**.
PT-side extras: PT lighting/depth is not applied (v1); player is drawn at the puppet's depth.

Audio: RoA is muted in v1. v2 forwards `PR_EV_SOUND` and plays the RoA sound files through PT's FMOD
or `audio_play_sound`.

## 9. Combat

- **Player → PT enemy.** Each frame RoA exports its active hitboxes (`PRHitbox`: rect, damage,
  angle, power, hitpause). PT spawns short-lived hit volumes at those rects that call PT's own enemy
  damage/stun/launch code (so toppin drops, points, combo and enemy death animations are PT's).
  PT acks with `HIT_CONNECTED{id}`, RoA applies hitpause so attacks feel like RoA.
- **PT enemy → player.** `scr_hurtplayer` is hooked in the `states.rivals` case: instead of PT's hurt
  state it sends `PLAYER_HURT{damage, angle, power}`; RoA applies it through its hit pipeline
  (hitstun, DI, parry/shield still work), PT's HP/points loss rules stay PT's.
- **Decision (user, 2026-10-03): a PT hit does both.** PT's own hurt tail runs (points/combo/style
  loss, hurt counters, invulnerability) *and* RoA adds percent (`RIVALS_HURT_DAMAGE`) and flings the
  character (`RIVALS_HURT_POWER`, angle away from the attacker). Implemented in
  `scr_hurtplayer` -> `rivals_on_hurt` -> `PR_EV_PLAYER_HURT`.
- Death/KO: PT's rules for now (RoA percent is mirrored for display only).

## 10. IPC

Identical shape to SkyCraft's: header with magic/version/PIDs/heartbeats, seqlock latest-value slots
for per-frame data (input, PT state, player state, hitboxes, solids), two SPSC rings for events, a
pixel area. Fixed little-endian structs, no pointers. Heartbeat loss: RoA freezes and hides; PT puppet
falls back to normal Peppino. Authoritative definition: `protocol/pizzarivals_protocol.h`.

## 11. Launching

v1: `PizzaRivals` launcher starts RoA (via `RivalsofAether.exe`, loader auto-loads
`mods/pizzarivals.dll`), the DLL hides the window and boots straight into a match, then starts the
modded Pizza Tower (or PT starts RoA itself through the extension). Both exit together.

## 12. Spikes (risk-first; do these before building features)

| # | Spike | Question | Fallback |
|---|---|---|---|
| S1 | **Headless match boot** | From `mods/pizzarivals.dll`, can we start a 1-player match/training with chosen character with no menu, window hidden, and step it? Entry points live in the decomp (`init_training_mode`, `restart_match`, `GAME_START`; GAME_START failed to decompile → verify at runtime with the loader's logger). | Hook CSS functions and auto-press buttons via synthetic input. |
| S2 | **Player-layer capture** | Draw only the player layer (+articles/FX) to a transparent surface and read pixels. | Capture full frame with a flat chroma background and key it out. |
| S3 | **Collision mirror** | Create `par_block`/slope instances at runtime; do characters walk on them (incl. ledges, platforms, walljumps)? | Build a custom RoA workshop *stage* that embeds a large synthetic room. |
| S4 | **PT build** | Install GameMaker 2023.1.1.62, run `PTdecompiler.csx` (decomp ships without sprites/audio), get the unmodified decomp to build and run; confirm extension DLL loading (x64). | — (prerequisite) |
| S5 | **Hit export** | Enumerate live hitbox instances each frame (`create_hitbox` is script `gml_Script_create_hitbox`). | Hook `create_hitbox`/hitbox update and track ourselves. |

## 13. Phases (each ends playable)

0. **Link** — S4 + both peers handshake over shared memory; fake peers (`tools/`) test each side alone.
1. **Walk** — S1+S2+S3: Peppino's puppet moves with a RoA character on PT geometry; sprite drawn.
2. **Switch** — character menu from catalog; stock + workshop swapping; load-failure handling.
3. **Fight** — hitbox export, hurt path, hitpause, combo/score integration.
4. **PT integration** — doors, tubes, cutscenes, escape, hurt/death, pause, taunts as PT taunts.
5. **Polish** — audio bridge, lighting/depth on the sprite, auto-launch, packaging/installer, per-
   character quirks list (characters that rely on stage objects, teams, or CPU opponents).

## 14. Known risks

- Characters assuming an opponent/stage (CPU dummy, `get_stage_data`, ledges). Mitigation: spawn an
  invisible dummy opponent in the match; keep a quirks table.
- RoA decomp is incomplete (439 scripts failed incl. `GAME_START`, `NetworkCreate`): boot logic is
  discovered at runtime via loader hooks rather than read from source.
- Pixel scale mismatch between games: handled by `PR_SCALE`.
- Online/rollback code paths in RoA must be fully bypassed (offline match only); SmokeAPI is already
  installed on this copy.
- Licensing: the OpenTower decomp is CC BY 4.0 (credit required) and ships no assets; this repo
  distributes only bridge code and patch sets, never game assets.

## 15. Findings log (2026-10-03)

- **S4 done.** The decomp builds with GameMaker LTS 2022.0.3.99 via Igor (`tools/build_pt.sh`),
  despite the README preferring 2022.0.1.30. Note: Igor `Package` did not refresh the `.win`; `Run`
  does. Patches are applied to a *copy* (`C:\pt`) by `pt-bridge/install.py`, idempotently.
- PT hooks used: new `states.rivals` (appended to the enum, so existing values don't shift), a case
  in `obj_player` Step_0 that skips PT's collision (RoA owns position), a branch in `scr_hurtplayer`,
  and a persistent `obj_rivals_controller` created from the title room code.
- PT enemy damage = putting the enemy in `states.hit` with `hitX/hitY/hitLag/hithsp/hitvsp`
  (same as `scr_pistolcollision`); PT's `scr_enemy_hit` then does launch/kill/combo/points.
- RoA `data.win` keeps asset name tables (OBJT/ROOM/SCPT/SPRT) even though the code is YYC; dumped to
  `docs/roa_asset_names.json`. Useful names: objects `oPlayer`, `pHitBox`, `par_block`,
  `par_jumpthrough`; rooms `training_charselect_room`, `stage_training_clean`, `GamePlay`;
  scripts include `restart_match`, `init_training_mode`, `GAME_START`.
- The RoA exe string table contains global/instance variable names (e.g. `training_mode`,
  `char_num`, `char_map`), so `variable_global_get/exists` can be probed by name. The loader
  exports (mangled C++ names) are resolved with `GetProcAddress`, so the bridge builds without the
  loader's import lib; the loader provides a per-frame D3D11 Present callback that runs on the game
  thread, which is where bridge code calls GML builtins.

## 16. Findings log, part 2 (2026-10-03, end-to-end working)

**Status: Zetterburn plays inside Pizza Tower.** Input -> RoA physics/attacks -> state, hitboxes and pixels back into PT all work.

How the RoA bridge (`roa-bridge/src/bridge_main.cpp`) gets there:
- GML is only called from inside the game's own script context: the `floor` builtin detour is the per-frame tick
  (D3D Present only sets a flag). Calling GML from Present crashed RoA ("argument is not provided to script").
- **Input**: RoA resolves buttons per player in `player_controls_update` (RVA 0x9391B0), which copies `pc_*` globals into the
  player instance (`attack_down/_pressed/_counter`, ...). We hook it and write the player's variables right after it; directions
  go through `pc_*` (hooks on `controls_update` 0x8FDB30 and `controls_intake` 0x68F710 too). Slot 1 is player 1 (arrays are 1-based).
- **Match boot**: set `training_mode`, `player_select[1]`, `player_connected[1]`, `player_stock[1]`, then `room_goto`. Boot room is
  `stage_custom_clean`; the stage art is removed by deactivating `obj_stage_main`, `obj_custom_stage`, `depth_of_five/ten`, `HUD`,
  and the room background is set to magenta (chroma key). The training stage's art could not be hidden (tiles/backgrounds).
- **No blast zones**: `death_update` (RVA 0x7454B0) is swallowed while a bridged match runs. **Camera** already follows the player.
- **Position**: RoA snaps the player's `x` back on the next frame (y is writable), so we never teleport. PT and RoA coordinates are
  related by an offset (`g_ox/g_oy`), re-derived on PT's warp request once the spawn animation is over (`PR_ROA_WARPING` flag).
- **Collision mirror**: PT solids -> pooled `solid_32_obj`/`jumpthrough_32_obj` instances moved/scaled each change; the stage's own blocks are parked.
- **Pixels**: backbuffer -> staging texture -> 960x540 RGBA centred on the character, magenta keyed to alpha 0.
- **Window**: RoA moves off-screen and is muted when the match is ready (`mods/pizzarivals_showwindow.txt` disables that).
- Hitboxes: `pHitBox` instances expose `bbox_*`, `damage`, `kb_angle`, `kb_value`, `hitpause`, `type`, `player`.