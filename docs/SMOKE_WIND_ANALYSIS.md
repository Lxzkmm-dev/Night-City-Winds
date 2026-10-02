# Why smoke doesn't follow the wind: analysis

2026-10-01. The sources are: the framework's particle log from Omar's run (60 smoke, fire and vehicle effects, every emitter), static reverse engineering of the 2.31 exe, the effect files themselves (decompressed from the two smoke mods' archives), and Omar's three clips.

## 1. What we see

| Thing | Follows our wind? | Why it matters |
|---|---|---|
| Trees (SpeedTree) | **Yes**, full sway in a storm | The live wind (strength and direction) reaches the renderer correctly |
| Prop smoke and steam (vents, columns, tyre pile) | **Yes, but faintly** | Particles do read the same live wind |
| Car fire and explosion smoke | **No**, the column stays vertical | Something about these specific effects blocks it |
| Fire flames | Leaned, and sheared off when the wind was extreme | Fixed by capping visual strength at 25 |

So the wind itself works. The question is narrower: **why some particle effects use it and others don't.**

## 2. How a particle gets wind in this engine

1. The weather's `WindAreaSettings` (strength and direction) are blended into the frame's environment. Trees and prop smoke prove our values arrive.
2. Each particle **emitter** has a `windInfluence` multiplier that says how much of that wind it takes:
   - on the asset at `CParticleEmitter + 0x110`;
   - on the cooked render data the renderer uses, at `rendRenderParticleBlob.header.emitterInfo + 0x24`.
   - These offsets come from the engine's own property registration (functions hash 58529680 and 124067489).
3. **Default 0 = no wind at all.** The value is only stored in the file when an artist set it.
4. Each emitter's simulation is described in the cooked data by a **modifier set mask**, an **initializer set mask** and a **simulation hash**.
   - The particle files contain **no shader code** (checked in all three files I extracted).
   - The exe references `particleSimulationCompiler.exe`.
   - So the per-emitter simulation programs are **precompiled offline** and looked up by that hash, not built from the editable fields at runtime.

## 3. Evidence from your run

Authored wind influence of the effects you were near (the log records the values before our change):

| Effect | Emitters | Wind influence as shipped | Local-space emitters |
|---|---|---|---|
| `v_damage_explode_fire_car_dark_smoke` (**replaced by Exploded Vehicles Smoke Overhaul**) | 12 | all **0** | **6 of 12** |
| `v_damage_explode_fire_car_slow_big` | 6 | one at 0.2, the rest 0 | 4 of 6 |
| `v_damage_explode_fire_car` / `_front_car` / `_back_car` | 7 / 4 / 5 | one 0.05, the rest 0 | most |
| `e_fire_medium_car_wreck_01` (**replaced by Ultra Fog**) | 6 | all 0 | 4 of 6 |
| `e_smoke_chimney_large_light_01` | 3 | all 0 | all 3 |
| `e_steam_column_2x2x3m` (Ultra Fog) | 4 | **0.10** | none |
| `e_steam_column_3x3x20m_dense` (Ultra Fog) | 7 | **0.2 to 0.6** | all 7 |
| `e_decoset_tires_burning_big_a_basic` | 6 | **0.2** on its two smoke emitters | 4 of 6 |

- **Ultra Fog Steam Smoke and Bloom** replaces 37 of the 60 logged effects, and **Exploded Vehicles Smoke Overhaul** replaces the car's dark smoke. Their files are what's live, and they kept wind influence at 0 on the car smoke.
- **The pattern:** the smoke that responded (vents, steam columns, the tyre pile) is exactly the smoke that **shipped with wind influence above 0**. Car and explosion smoke shipped at **0** on every world-space emitter, with about half its emitters in **local space**.

## 4. Candidate causes, ranked

### A. Wind is compiled into the simulation program, so raising `windInfluence` at runtime does nothing. **Most likely.**

- **For:**
  - The simulation is precompiled per emitter and looked up by hash.
  - Every effect that responded had wind authored in.
  - Every car smoke emitter we raised from 0 to 0.5 at load (both the asset and the cooked copy) still didn't move.
  - Explanation: if the offline compiler only emits the wind term when `windInfluence > 0`, an emitter authored at 0 has no wind code to scale.
- **Against:** none yet.
- **The decisive check (logging added in this build):** each emitter's line now includes its modifier mask, initializer mask and simulation hash next to its wind influence.
  - If emitters that shipped with wind show a **mask bit or hash family the 0-wind ones lack**, A is confirmed.
  - If the masks are identical, A is out, and the cause is D or a runtime read we're missing.

### B. Local-space simulation

- About half the car smoke emitters, and whole effects like chimney smoke, set `keepSimulationLocal`. Their particles live in the emitter's own frame and follow the car. World wind isn't applied to them, which is also why the framework skips them.
- So even with A solved, these emitters wouldn't drift. Their world-space siblings would lean while they don't, which reads as a column that barely bends.

### C. The smoke's own motion overpowers the wind

- The car smoke emitters run **VelocityOverLife**, **Drag**, **Turbulize** and **Noise** modules. Velocity-over-life drives the rise every frame, and drag pulls velocity back toward the authored motion.
- With authored influences of 0.1 to 0.6, this is why prop smoke leans only "faintly". It shrinks the effect but can't make it zero, so it doesn't explain the car smoke.

### D. Timing: the renderer copies the values before our change

- The boost runs in `Resource/PostLoad` on a loader thread. If the renderer has already built the effect's render object (an effect preloaded before the framework started listening, for example), it uses the old values.
- **Mostly ruled out:** the log shows the callback firing for the car effects at their first load in this session, and the listener now starts with the game.
- **Still possible:** the renderer reads the cooked emitter data during load, before `PostLoad`.

### E. The wind is too weak for smoke

- **Ruled out:** the same strength gives trees full sway, and prop smoke visibly reacts.

## 5. What each cause means for the fix

| If the cause is | Fix | Cost |
|---|---|---|
| **A** (compiled) | Ship our own versions of the key smoke effects with wind influence authored in and recooked (WolvenKit), so the simulation compiler includes wind. They must be **merged with Exploded Vehicles Smoke Overhaul and Ultra Fog** (compatibility patches for those two), or they'll override each other by load order. | Asset work, no code. The only route that changes compiled behaviour without shader patching. |
| **A**, native alternative | Find the renderer's emitter setup and the simulation-program lookup, and force the wind variant. Only possible if a wind-enabled variant exists for that emitter's other modules. | Deep reverse engineering; may not exist |
| **B** (local space) | Leave local emitters alone on moving cars. For static wrecks and fires, the patched assets could switch smoke emitters to world space. | Asset work |
| **C** (modules) | In patched assets, weaken drag and velocity-over-life on smoke so the wind reads | Asset work, tuning |
| **D** (timing) | Native hook before render creation instead of `PostLoad` | Moderate |

## 5a. Result of the decisive check (signature dump, 2026-10-01, 7 effects, 43 emitters)

- **A is ruled out.**
  - Every emitter has `simulationType 0` and `simulationHash 0`: CPU simulation, no precompiled simulation program.
  - The modifier masks encode the module set (velocity-over-life, drag, turbulize and so on), with no wind bit. The masks of emitters authored with wind and those at 0 differ only by their modules.
  - `windInfluence` is a plain per-emitter value the CPU updater reads, so raising it works. The car smoke's world-space emitters were indeed raised to 0.5 by the boost (visible in the dump).
- **B is the cause.**
  - The emitters that make up the visible columns are **local-space and authored at 0**: car dark smoke e3–e5, e7 and e8 (spawn circles with turbulize, noise and drag); car slow_big e0, e2, e4 and e5; all three chimney emitters.
  - The framework skipped local emitters on the assumption that local space ignores wind. The game's own `e_steam_column_3x3x20m_dense` refutes that: it authors 0.2 to 0.6 on emitters that are **all local**. Artists wouldn't set it if local emitters ignored wind.
- **Fix:** the boost now includes local-space emitters, in both the asset and the cooked blob.
  - Risk: on a moving car, local particles travel with the car, so wind shows as a lean in the car's frame. That's acceptable for a burning wreck, which doesn't move.
- **C still applies to magnitude:** drag, turbulize and velocity-over-life shape how far a 0.5 influence leans the column. The probe's floor slider (0 to 3) tunes it.

## 5c. In-memory boost including local emitters: still no lean. The fix is file patching.

- **Result:** with local emitters boosted, car smoke still rose straight up.
- **What the files show:** the dark smoke column's own smoke emitters (`smoke_base`, `smoke_column_medium`, `smoke_column`) are **world-space**, and the in-memory boost had set them to 0.5 (confirmed by the dump). So the renderer doesn't read the loaded resource's emitter values after load. It takes its own copy while loading. Effects whose **files** carry wind (steam vents) do lean.
- **Fix:** `smoke\build_smoke.py` patches the files.
  1. It extracts the effects in the smoke-relevant fx folders from the game and from every enabled mod archive (WolvenKit CLI 9.0.1). The winning version is the one the game loads first; the first build assumed alphabetical order, which was wrong for Omar's setup (see 5d).
  2. It raises `windInfluence` to 0.5 on emitters whose editor name says smoke, steam, dust, fume, vapor, cloud, haze, mist, fog, soot or ash, but not fire, flame, spark, ember, glow, light, debris, chunk, flash, blast, shock, heat or distortion. Both the emitter and the cooked `emitterInfo` are changed.
  3. It packs the result as `!!!!!NightCityWinds_SmokeWind.archive`, which loads before `!!!_Ultra_Fog_Lite`.
- **First build:** 389 effects (190 vanilla, 197 from mods, mostly Ultra Fog, and 2 added by Exploded Vehicles, the car and truck dark smoke). Round trip verified: 0.5 on both the emitter and the blob.
- **Re-run the script after updating any smoke mod.** The in-memory boost is now off by default.

## 5d. Car smoke and Exploded Vehicles: drag, then load order (2026-10-01)

- **Drag:** the car's dark smoke column still rose straight after patching because its emitters have no Drag module, and particles take wind only through drag. The build now appends one (mask bit 32, a 12-byte `02 00 0C 00` record, coefficient 0.15). With Exploded Vehicles disabled, car smoke leaned in game.
- **With Exploded Vehicles enabled it didn't, and the cause was load order, not the patch.** CyberVision ships `archive\pc\mod\modlist.txt` (in the enabled mods "04 - PATH TRACING Very High - RTX 5070 TI" and "Load Order .Json files MENU CONFIG", about 2,100 lines). Archives it lists load first, in its order; unlisted ones follow; the first copy of a file wins. Ours wasn't listed, so it lost every conflict whatever its name (renaming to `zzzzz_` couldn't help).
  - It also means the 272 patched effects that `!!!_Ultra_Fog_Lite` ships never applied in Omar's setup; only files no listed mod touches (vanilla car smoke, steam vents) did.
- **Conflicts among the mods themselves:** only `v_damage_explode_car.particle`, held by Exploded Vehicles (line 106) and EffectsRenderingFix (line 2011). The game uses Exploded Vehicles'; the alphabetical build had patched EffectsRenderingFix's.
- **Fix:**
  - `build_smoke.py` now resolves each file's winner from the effective `modlist.txt` and archive indexes (`archive_load_order`, `archive_hashes`), and warns when our archive isn't the list's first line.
  - `smoke\modlist_first.py` puts `!!!!!NightCityWinds_SmokeWind.archive` on line 1 of each enabled `modlist.txt`, with backups in `backups\modlist` (`--undo` restores them). It edits the modpack's files, so it needs Omar's go-ahead, and a modpack update will undo it.
- **Rebuild (same settings, game-accurate sources):** 389 effects. Five files were held by two mods: `v_damage_explode_car` goes to Exploded Vehicles, and four (two tire blowouts, the biohazard grenade, `lib_vehicle_crash`) go to Ultra Fog over EffectsRenderingFix. The car explosion source is byte-identical to Exploded Vehicles' file.
- **Known gap:** emitters that carry a *disabled* Drag module (in `v_damage_explode_car`, `kickup_dust` and `smoke_side`) are skipped by the "already has drag" check, so they still get no wind. Fix later by counting only enabled Drag modules.
- **For release:** players without a `modlist.txt` load alphabetically, so the `!!!!!` name wins there. Modpacks that ship a list need our line added; document that in the mod's install notes.

## 5e. First working run (Exploded Vehicles on): the still column and the runaway smoke (2026-10-01, 21:50)

With our archive first in CyberVision's list, car smoke followed the wind. Omar's video (30 m/s crosswind pinned in the probe) showed two faults:

- **The tall thick column hung still** while the rest of the plume blew away. Cause: its emitters (`smoke_column`, `smoke_column_medium`, 5–10 s and 18–20 s lifetimes) carry a `VelocityOverLife` curve with `modulate` on, which sets the particle's velocity from its initial velocity every frame. Drag's wind contribution is written over before it accumulates, so the column never bends, whatever its wind influence. The same construction is in every vanilla tall column (`e_smoke_column_dark_4x4x35`, `e_steam_column_5x5x65`, ...), and in the game those have wind influence 0 on exactly those emitters: the designers knew.
  - **Fix:** `build_smoke.py` removes VelocityOverLife from a smoke emitter that has no drag of its own (`drop_vol`: the cooked record is the first modifier record, `u16 samples, u16 size = 4 + 12·samples`; checked on all 455 emitters with the module, `volrec.py`) and gives it a weak drag instead, `k = min(0.15, 0.33 / mean lifetime)`, so a particle still rises about 85% as far as before and bends more the longer it lives. Emitters whose designer gave them both keep their curve, since replacing it with a strong drag would stop them rising. 80 effects changed this way, every one got its drag.
- **Car and tire (prop) smoke blew away faster than it was made.** Cause: 0.5 wind influence. The game's own smoke uses 0.1–0.2 (`survey.py`: 77 emitters at 0.1, 40 at 0.2, 22 at 0.5, and the 0.5s are thin steam detail), tuned for its windiest weather at strength 20, and we drive the curves to 20+ in a storm. So our smoke took 2.5–5× the designer's wind.
  - **Fix:** floor lowered to 0.2 (the designers' top value for body smoke). The probe's gain slider stays for tuning.
- Both fixes are file-side; no plugin or script change.
- **Result (22:09):** car smoke near the ground "much better"; the thick high smoke still static. The weak drag (k = 0.017 for the 20 s column) kept the climb but gave it almost no grip on the wind.
- **Second fix: buoyancy.** The game's own buoyant smoke is `Acceleration` up plus `Drag` 0.25–1.5 (a particle settles at a/k up and at the wind's speed across). `vol_to_buoyancy` now replaces the curve with exactly that: Acceleration (0, 0, k·rise) with k = 1, where rise is the initial Z velocity times the curve's mean Z multiplier (1.6 m/s for the car's column), plus Drag 1. Acceleration is mask bit 3, cooked as a direction record `02 00 1C 00` + (min xyz, max xyz) and a scale record `02 00 0C 00` + (min, max) (`accrec.py`, 615 emitters); an existing Acceleration is rewritten in place. Emitters where bits 0 or 2 are set, or whose Acceleration isn't constant, keep their curve rather than get the weak-drag fallback (which could also have unleashed an Acceleration the curve used to cancel). 92 emitters in 65 effects; `consistency.py` checks mask, count and records on all 397 changed emitters.
- **Result (23:39):** the car's plume bends, but a second cloud still "originates in the air". Cause: `smoke_column` is born at a constant `InitializerPosition` of Z = 10 m above the effect, so however the wind carries its particles, new ones keep appearing at that fixed point in the sky. **Fix:** `lower_spawn` caps the birth point of columns we convert at 2.5 m (cooked as a `02 00 1C 00` + xyz,xyz initializer record, matched by exact bytes). Applies to the car and truck dark smoke and three vanilla 50 m columns.

## 5f. Diagnostic: the plume ignores wind even at 1.0; embedded particles (2026-10-02, 00:02)

- With `--column-wind 1.0` (only the converted columns at full wind; install confirmed in `install_when_closed.log` before the run) the plume stayed dead straight. The lowered origin did show, so our file was loaded.
- **Embedded particles:** EV's `v_damage_body_big_fire_car.effect` references 4 particle files; three (`..._slow_big`, `..._front_car`, `..._fire_car`, `..._back_car`: Flags `Embedded`) are copies stored inside the .effect's `EmbeddedFiles`. Only `..._dark_smoke` (Flags `Default`) is loaded from the depot, so it's the only one our archive reaches. The car smoke that visibly follows the wind is the embedded `smoke_big` (designer wind 0.2, designer drag 1) and `smoke_1_002` (0.05, drag 1), unpatched.
- **Implication:** every confirmed lean so far is designer drag plus designer or raised wind influence (steam vents, embedded car smoke). No emitter with a Drag (or Acceleration) we **added** has been seen to take wind, though the record bytes match the designer's exactly (`02 00 0C 00` + coefficient pair, mask bit 32, count +1). Something else the renderer builds per emitter is missing or cached. Next: trace the CPU particle simulation natively (where Drag and windInfluence are consumed, and what decides which modifiers run).
- **Root cause found natively (2.31):** `0x41826C` builds each emitter's modifier function list from `modifierSetMask` (bit 32 → drag `0x13F65C`, bit 3 → acceleration `0x13B144`, bit 1 → VOL `0x13A28C`). The drag function reads a **float scale first**, then the coefficient evaluator record (`02 00 0C 00` + min, max), and applies quadratic damping `v' = v / (1 + k·dt·|v|)`, k = scale × coefficient; it has no wind term. Designer drags are cooked `[scale][02 00 0C 00][1,1]` (523 of 719 top-bit cases have the module scale there; the rest are curve evaluators). Ours lacked the float, so the engine read the record header as a ~1e-39 scale and skipped the drag: **every drag we added did nothing**, and the column with our Acceleration and no working drag accelerated straight up without limit. Fixed in `add_drag`; buoyancy now uses a = k·rise·|rise| for the quadratic drag. VOL multiplies the particle velocity component-wise per frame. Wind itself is applied outside the modifiers (not yet located).
- **Test after the drag fix (00:21):** drag now works, the plume still doesn't lean. So nothing in the files carries wind to that emitter; Omar chose option B (native).
- **Option B, first build (plugin 0.3.0, `plugin\src\SmokeWind.cpp`):** detours the emitter setup (hash 2428183128, `0x417F6C`). After it returns, for a world-space emitter whose cooked `windInfluence` > 0, it reallocates the modifier list with the engine's own allocator (hash 2087392471) and free (hash 2786008195), appends `SmokeModifier`, and bumps the count at `+0x198`. Emitter info offsets come from RTTI (`rendRenderParticleBlobEmitterInfo`), anchored on the setup's read of `modifierSetMask` at desc `+0x88` and checked against `numModifiers` (`-8`) and `initializerSetMask` (`+8`). The modifier gets (particle, emitter+0x10, context, life fraction in xmm3) and moves the particle: `pos += wind × influence × gain × ramp(life/0.1) × dt`, dt = context `+0x54`. Per-emitter influence lives in a 65,536-slot lock-free table keyed by emitter pointer. The first six samples log context `+0x50/+0x54/+0x90`, position, velocity and wind, to confirm the layout. Wind Probe: "smoke wind in the engine" checkbox, gain 0–5, stats line. Classification still relies on wind influence (the smoke archive gives smoke 0.2); name-based tagging comes next so the archive can go.
- **First run (00:34):** hook fine, 52 of 1,089 emitters given wind, 762k steps, dt 0.023 (context `+0x54` confirmed; `+0x50` = 1.0, a scale; `+0x90` ≈ 0, not wind). Samples rising at 1.3–1.7 m/s = the converted column, so it was hooked. The plume still didn't drift. Likely cause: emitters with a Collision module (the column) take positions each frame from an external buffer (the loop's `rdi` path, `0x1399F2`: pos from `+0x20`, vel from `+0xC20` per slot), overwriting a position nudge, while velocity changes (Acceleration did make it rise) carry through. **Second build:** steer horizontal velocity toward wind × influence × gain at 1.5/s instead of moving the position; logs a trace every 3 s (velocity left vs found a frame later).
- **Second run (00:50):** the trace shows the velocity found a frame later equals the one *before* our change: the particle keeps a persistent velocity at `+0x20` and the loop copies it into the working one at `+0x14` every frame (`0x1399DE` → `0x139B75`); the engine's drag writes both. **Third build:** write `+0x20` too. Also: the wind during that run was only 2–3 m/s ("windy"), so the target drift was ~0.5 m/s; test in "storm".
- **Fourth run (01:13–01:22):** low smoke around the car visibly drifts (Omar confirmed); the tall plume still doesn't. Traced particles (near the ground) hold ~5 m/s downwind in a 30 m/s wind (30 × 0.2 × gain 1 = 6 m/s target). No plume particle was traced, so "hooked" was never shown for the plume.
- **What the plume emitter is:** `smoke_column` (20 particles, 18–20 s) carries an enabled `CParticleModificatorCollision`; `smoke_column_medium` (10, 5–10 s) does not. The cooked `updaterData` collision fields are identical (all zero) with and without the module across the whole corpus, so collision isn't cooked there; the game side sets it up. In the sim loop, an emitter with a non-null `[arg5 + 0x38]` pool takes each particle's position (`pool + idx·12 + 0x20`) and working velocity (`+0xC20`) from a PhysX read buffer every frame (256 particles per 0x1820 block, collided-flags bitfield at +0), instead of integrating `pos += vel·dt` and restoring `+0x14` from `+0x20`. The loop's tail only writes the particle count to `pool + 0x309C`; no velocity write-back to PhysX was found in the loop or its caller (`0x138300`). Hypothesis: modifiers' velocity writes never reach PhysX-simulated (collision) emitters, so neither our modifier nor the designer's Drag moves them; the medium column (no collision) does drift.
- **The decisive run (01:35 build):** one build that logs everything about the plume: at setup, every emitter with the plume's modifier mask (0x10A102028) with each skip reason, its engine modifier list as game-relative RVAs, the full `rendRenderParticleBlobEmitterInfo` and `rendRenderParticleUpdaterData` via RTTI (blob base confirmed against the setup's own reads: header @?, emitterInfo @? → 0x40), raw hex of the blob and the runtime emitter; one line per hooked emitter; then a 150-frame trace of one plume particle at a time (6 per run): the whole 0x98-byte particle once, context floats, emitter flags, and per frame the position, working velocity `+0x14` and base velocity `+0x20` before our write, what we wrote last frame, `+0x54/+0x58/+0x88`, the collided flag, the count, the cursor, dt and the wind. Setting changes are logged too. If `+0x14` next frame ≠ what we wrote → PhysX owns the motion; fix = make the column CPU-simulated (disable its Collision module in the archive build) or feed PhysX.
- **Decisive run (01:47–01:52):** setup lines captured (the per-frame lines were lost: RED4ext replaced the log on Omar's relaunch at 21:54; copy logs to `F:\2077 Wind Dev\logs` from now on). Findings:
  - The car plume's two emitters were both hooked: `B299E150` (20 particles, `smoke_column`, has Collision) and `B2996950` (10, `smoke_column_medium`, no Collision); list count 7 → 8, our modifier last. Engine modifier list for the plume mask: `game+13b144` (Acceleration), `+13b2f0`, `+13b06c`, `+13f5f8`, `+134bf0`, `+139194`, `+13f65c` (Drag).
  - Six particle traces were taken, from four plume-like emitters; **none from the tall column**, though it had twice the medium column's particles in the trace window (≈1/9 by chance). Its particles are most likely never run through the modifier list.
  - The medium column's particles carried our velocity in both `+0x14` and `+0x20` at trace start: (−0.327, −1.897) = 2.12 × the engine's own per-emitter wind term, so our write persists on CPU-simulated emitters, and the low smoke visibly drifts (Omar).
  - **The engine's vanilla wind:** context `+0x90` is a per-emitter vector (wind × influence; (−0.154, −0.894) in the storm, ≈0 in calm) that loop 2 adds to the position each frame (`pos += dt · ctx+0x90`). So vanilla particle wind is pure advection by wind × windInfluence, where the engine's wind vector is a few m/s at most.
  - Runtime emitter layout (from the hex dumps): `+0x18` records pointer, `+0x40` modifOffset, `+0x7C` collisionRadius, `+0x84` maxCollisions, `+0x98` eventFrequency, `+0x9C` eventProbability, `+0xA8` diffuseWrapFactor, `+0xAC` backLightingFactor, `+0xB8` maxParticles, `+0xBC` blob version, `+0xD8` windInfluence, `+0xDC` simulationType, `+0xE0` flags (bit 1 = keepSimulationLocal), `+0x190/+0x198` modifier list/count, `+0x1A0` mask. The tall and medium columns' runtime emitters differ only in maxParticles: nothing in the cooked blob marks collision; the game side reads the Collision module itself.
  - Particle layout: `+0x0` pos, `+0xC` age, `+0x10` 1/lifetime, `+0x14` working velocity, `+0x20` base velocity, `+0x2C/+0x30` rotation?, `+0x34..0x40` working size/alpha (restored from `+0x44..0x50`), `+0x5C/0x60` from `+0x64/0x68`, `+0x6C` rotation, `+0x70/+0x74` rotation rate, `+0x84` = 999, `+0x94` seed.
- **The fix (02:00):** `build_smoke.py` removes `CParticleModificatorCollision` from every smoke emitter it patches. The columns become ordinary CPU emitters, which the trace proved take our velocity. Cost: `killOnCollision` is gone, so column smoke passes through overpasses instead of dying on them.
- **Confirmed (02:10):** the plume drifts with the wind, with the plugin's smoke wind on and off. Off, the drift is the engine's own advection (wind × influence 0.2 from the weather curves our visual sync writes); on, the plugin's push adds to it and is tunable from the probe. So the complete chain for a smoke column is: wind influence > 0, a working Drag record, no VelocityOverLife, no Collision module, and a load order that lets our file win.
- **Omar (02:11): it looks better with the plugin's push off.** The engine's wind is additive advection (`pos += wind × influence × dt`) over each puff's own motion, so spread and turbulence survive; our step sets the horizontal velocity outright past 10% of life, so puffs move in lockstep. Default switched to off (plugin `Settings.enabled{false}`, probe checkbox unchecked). If a native push returns, it should be a relative-air drag, not an override. Consequence for the runtime plan: only the influence (settable in the setup hook at emitter `+0xD8`) and the collision module need native handling; VOL, drag and buoyancy are look changes.
- **0.6.0 (02:40): the archive's work moved into the plugin.** The outer particle update computes each emitter's advection vector as `ctx+0x90 = [runtimeEmitter+0xD8] × worldWind[+0xC0..0xC8]` (0x1385F6–0x13863B), so the runtime emitter's `+0xD8` is the influence the engine's own wind uses. `plugin/src/SmokeTag.cpp`: a `Resource/PostLoad` callback on `CParticleSystem` (one native call, `NCW_TagSmokeSystem`) classifies emitters with the archive's path and name rules, fingerprints each smoke emitter's cooked blob (masks, counts, maxParticles, simulationHash, and strongly also the updater record bytes; wind influence excluded), stores a 0.2 floor in a lock-free table, writes the floor on the script-visible emitter and blob, and removes `CParticleModificatorCollision` from the emitter's `modules` in memory. The emitter setup hook fingerprints the render-side blob (a copy made before the callback, which is why the old script boost never worked) and writes the floor into `+0xD8`. Test: archive parked, burning car in a storm. Open: whether the in-memory collision removal is early enough for the game-side PhysX setup.
- **Cleanup (02:20):** the plugin's plume diagnostics (fingerprinting, hex dumps, per-frame traces) are removed again; it logs the layout check, the first 60 hooked emitters and setting changes. The particle and emitter layouts they established are recorded above.
- **Third run (01:05, 30 m/s pinned):** our velocity now persists (found ≈ left), but horizontal speed settled near 1.2 m/s against a 6 m/s target: the effect's quadratic drag (k = 1) damps toward zero every frame and out-pulled our 1.5/s easing. **Fourth build:** set the horizontal velocity outright to wind × influence × gain once past the jet phase (blend over the first 10% of life). The loop moves the particle by it next frame before drag acts again.
- Patching inside .effect `EmbeddedFiles` is also needed for effects that embed their particles (EV does; check vanilla effects too).

## 5b. Incident: crash about a minute after boot (2026-10-01, 16:24 and 16:25 boots)

- **What crashed:** the build that logged every emitter (module class names, masks, hashes) from inside the `Resource/PostLoad` callback. The two boots that ran it crashed about a minute in. Omar then disabled the mod, and the next boot was fine.
- **What I changed:** the load callback runs on the game's loading threads, so it now does only the minimum (the path test and the boost, no logging). Signature diagnostics moved to the main thread, on request: the probe's "Dump smoke signatures" button loads a fixed set of effects and logs them from the frame tick.
- **Rule from now on:** no script work beyond the essentials inside resource callbacks.

## 6. Next step: one run, no guessing

1. Re-enable the mod and restart.
2. In the Wind Probe window, press **Dump smoke signatures**. The log does the rest.
3. I compare masks between emitters that shipped with wind and those at 0.
   - If A is confirmed, the plan is patched smoke assets with compatibility versions for your two smoke mods.
   - If not, I trace the renderer's emitter setup natively (D).
