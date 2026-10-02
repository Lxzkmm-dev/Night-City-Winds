# Night City Winds — 0.5.1

A dynamic, physics-based wind for Cyberpunk 2077 (2.31), built from a reverse-engineering of the
game's physics and particle engines. One wind field drives everything: cars, loose props, V on
foot (opt-in), foliage, cloth, and smoke. Other mods can read the wind through the redscript API
and the native plugin.

## What it does

- **Wind field** (`NCWWind`, redscript): wind states (calm, breeze, windy, gale, storm) drawn from
  each weather's own odds, easing between them; direction and the daily rise and fall taken from
  the weather's original wind curves; gusts, turbulence, a boundary layer near the ground and
  shelter behind buildings (rays). Works with every vanilla weather and all 70 Nova City 2 states.
- **Cars** (plugin): the game's own air-drag function is detoured to act on the airspeed
  (velocity minus wind), so headwinds, tailwinds and crosswinds change how cars drive.
- **Props** (plugin): aerodynamic drag on every loose PhysX body near the player, added before
  each PhysX step; sleeping bodies wake only when the wind could plausibly move them.
- **Visuals** (redscript): the live wind is written into every weather state's `WindAreaSettings`
  curves, which the engine uses for foliage, cloth, particles and water, so the visuals match the
  physics.
- **Smoke**: the engine's own particle wind moves smoke once an emitter has a wind influence, a
  drag record it can read, no fixed motion curve and no collision module. `smoke\build_smoke.py`
  patches the game's smoke effects (and those of installed smoke mods) accordingly into an archive
  built for your own install. The plugin also hooks the emitter setup and can add its own wind
  step to CPU-simulated smoke (off by default; the game's wind looks better).
- **Wind Probe** (CET + redscript, dev only): a window to pin the wind, force a state, toggle each
  layer, tune gains and read live stats and diagnostics.

## Layout

| Path | What |
| --- | --- |
| `plugin/` | RED4ext plugin (C++20): `src/`, `CMakeLists.txt`, `build.ps1` (builds and installs the DLL) |
| `mod/` | The mod's scripts: `r6/scripts/NightCityWinds` (wind field, natives, diagnostics), `r6/scripts/WindProbe` and `bin/.../WindProbe` (the probe) |
| `smoke/` | `build_smoke.py` (the smoke patch pipeline), `modlist_first.py` (load-order helper), the analysis scripts used to decode the cooked particle data |
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
- Smoke archive: `python smoke\build_smoke.py` (defaults: wind influence floor 0.2, drag 0.15).
  It extracts the smoke effects from the game and from every enabled mod, patches them and installs
  `!!!!!NightCityWinds_SmokeWind.archive`. The result contains modified copies of other
  mods' files and is for your own install only; ship a vanilla-only build (`--vanilla-only`) or
  nothing. If your modpack ships an `archive\pc\mod\modlist.txt`, our archive must be its first
  line (`python smoke\modlist_first.py`, `--undo` to restore).

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

## Status of 0.5.1

Working in game: car drag wind, prop wind, wind states, visual sync (trees, cloth, smoke), Nova
City 2 weather, player wind (opt-in), smoke following the wind including Exploded Vehicles'
plumes (through the per-install archive). Known limits: the smoke archive is per-install; the
native route for smoke (setting wind influence and skipping collision at load, no files) is the
next step; see `docs/SMOKE_WIND_ANALYSIS.md`.
