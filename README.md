# Night City Winds — 0.7.0

A dynamic, physics-based wind for Cyberpunk 2077 (2.31), built from a reverse-engineering of the
game's physics and particle engines. One wind field drives everything: cars, loose props, V on
foot (opt-in), foliage, cloth, and smoke. Other mods can read the wind through the redscript API
and the native plugin.

## What it does

- **Wind field** (`NCWWind`, redscript): wind states (calm, breeze, windy, gale, storm) drawn from
  each weather's own odds, easing between them; direction and the daily rise and fall taken from
  the weather's original wind curves; gusts that travel downwind as fronts, turbulence, a
  boundary layer near the ground, and shelter behind buildings (rays) whose wake recovers over
  60 m and churns in the lee. Works with every vanilla weather and all 70 Nova City 2 states.
- **Cars** (plugin): the game's own air-drag function is detoured to act on the airspeed
  (velocity minus wind), so headwinds, tailwinds and crosswinds change how cars drive. On top
  of that isotropic drag, 0.7.0 adds the crosswind side force a car's flank takes, the yaw it
  gives (the centre of pressure ahead of the centre of mass) and the wind's share of lift, each
  capped at a quarter of the car's weight.
- **Props** (plugin): aerodynamic drag on every loose PhysX body near the player, added before
  each PhysX step; sleeping bodies wake only when the wind could plausibly move them.
- **Visuals** (redscript): the live wind is written into every weather state's `WindAreaSettings`
  curves, which the engine uses for foliage, cloth, particles and water, so the visuals match the
  physics.
- **Smoke** (plugin + redscript, no patched files): every particle system the game loads is
  classified by effect path and emitter name; emitters that are smoke, steam or dust get, when the
  renderer sets them up, a wind influence floor (the engine's own particle wind multiplies by it),
  their fixed velocity curve stubbed, a birth point no higher than 2.5 m, sideways drag, and are
  kept out of PhysX. Works on vanilla smoke and on other mods' smoke (Exploded Vehicles' plumes
  included) with no load-order dependence. Each emitter is classed (body smoke, steam, dust,
  column, exhaust) and takes a floor and drag to match; puffs that would pass through an
  overpass are retired by short rays cast from the frame tick. The plugin can also push smoke
  itself (off by default; the game's wind looks better).
- **Wind Probe** (CET + redscript, dev only): a window to pin the wind, force a state, toggle each
  layer, tune gains and read live stats and diagnostics.

## Layout

| Path | What |
| --- | --- |
| `plugin/` | RED4ext plugin (C++20): `src/`, `CMakeLists.txt`, `build.ps1` (builds and installs the DLL) |
| `mod/` | The mod's scripts: `r6/scripts/NightCityWinds` (wind field, natives, diagnostics), `r6/scripts/WindProbe` and `bin/.../WindProbe` (the probe) |
| `smoke/` | Legacy (0.5.x): `build_smoke.py` (the retired smoke archive pipeline), `modlist_first.py` (load-order helper), the analysis scripts used to decode the cooked particle data |
| `tools/re/` | Read-only reverse-engineering tools for `Cyberpunk2077.exe` and the archives (address-library symbols, xrefs, disassembly, RDAR reader) |
| `docs/` | `PHYSICS_RE_FINDINGS.md` (the engine findings, hookable functions by hash), `SMOKE_WIND_ANALYSIS.md` (the smoke investigation), `PRIOR_ART.md` |
| `install.ps1` | Copies `mod/` into the Mod Organizer mod folder |

Until 0.5.0 the mod was called Cyberpunk Wind Framework; the natives were `CWF_*` and the classes `CWFWind` and `CWFParticles`. 0.5.1 renamed them (`NCW_*`, `NCWWind`, `NCWParticles`, module `NightCityWinds`); the Mod Organizer folder can keep whatever name you gave it.\n\nNot in the repository: `deps/` (RED4ext.SDK, PhysX 3.4 headers, WolvenKit CLI), extracted game
and mod files, built archives and DLLs. See below.

## Building

Requirements: Visual Studio 2026 (MSVC, CMake, Ninja), Python 3.11, and in `deps/`:
`RED4ext.SDK-master` (header-only), `physx34` (PhysX 3.4 public headers: `PhysX_3.4/Include`,
`PxShared/include`), `wolvenkit` (WolvenKit CLI 9.0.1). The game paths at the top of
`smoke/build_smoke.py` and in `plugin/build.ps1` are the author's; change them for your install.

- Plugin: `powershell -ExecutionPolicy Bypass -File plugin\build.ps1` (add `-NoInstall` to only build).
- Scripts: `powershell -ExecutionPolicy Bypass -File install.ps1`.
- Smoke archive (retired in 0.6.0, kept for reference): `python smoke\build_smoke.py` built a
  per-install archive of patched smoke effects. It is no longer needed; remove any
  `!!!!!NightCityWinds_SmokeWind.archive` and any line for it in a modpack's
  `archive\pc\mod\modlist.txt` (`python smoke\modlist_first.py --undo`).

Requires RED4ext, redscript, Codeware and (for the probe) Cyber Engine Tweaks.

## Using the wind from another mod

```swift
let wind = NCWWind.Get(game);
let v = wind.At(position, heightAboveGround);   // m/s, world space
let open = wind.Exposure(position);             // 0.25 sheltered .. 1 open
wind.SetOverride(new Vector4(10.0, 0.0, 0.0, 0.0)); wind.ClearOverride();
wind.ForceState(n"storm");
```

A mod that moves its own physics bodies calls `NCW_IgnoreNear(id, position, radius)` every frame
so the prop layer leaves them alone. All natives are listed in `mod/r6/scripts/NightCityWinds/Natives.reds`.

## Status of 0.7.0

Working in game (0.6.0): car drag wind, prop wind, wind states, visual sync (trees, cloth,
smoke), Nova City 2 weather, player wind (opt-in), smoke following the wind including Exploded
Vehicles' plumes, all from the plugin and scripts. How it was found: `docs/SMOKE_WIND_ANALYSIS.md`
section 5g.

New in 0.7.0, to be confirmed in game: car side force, yaw and lift; overpass rays for smoke;
smoke classes; gust fronts and building wakes; the coverage survey (probe: record, then dump to
the plugin log) for smoke emitters the tagger misses; `NCW_Version()` and `NCW_IsSmokeHookActive()`
plus a one-line hook summary at startup in the plugin log.
