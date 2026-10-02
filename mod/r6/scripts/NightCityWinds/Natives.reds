// =============================================================================
// NIGHT CITY WINDS - native bindings (red4ext\plugins\NightCityWinds)
//
// The plugin applies the wind to the game's physics; scripts tell it what the wind is.
//   NCW_SetWind(wind)            world wind velocity in m/s (W ignored): the air's own motion,
//                                so a wind blowing toward +X is (speed, 0, 0)
//   NCW_GetWind()                the wind the physics is using now
//   NCW_SetEnabled(enabled)      false = vanilla physics
//   NCW_IsVehicleHookActive()    false when this game version's car drag function wasn't found
//   NCW_GetVehicleDragStats()    (drag calls so far, last ground speed, last airspeed,
//                                last airResistanceFactor), for diagnostics
// =============================================================================

public static native func NCW_SetWind(wind: Vector4) -> Void
public static native func NCW_GetWind() -> Vector4
public static native func NCW_SetEnabled(enabled: Bool) -> Void
public static native func NCW_IsVehicleHookActive() -> Bool
public static native func NCW_GetVehicleDragStats() -> Vector4

// the prop layer: drag on loose PhysX bodies (props, debris) around the wind origin
//   NCW_SetWindOrigin(origin)               where the wind was sampled (the player / their car)
//   NCW_SetPropWind(enabled, radius, scale) radius in m (default 50), scale 1 = as computed
//   NCW_IsPropHookActive()                  true once the PhysX scene is hooked (after loading in)
//   NCW_GetPropWindStats()                  (PhysX steps seen, dynamic actors, pushed last step,
//                                           largest force last step in N)
public static native func NCW_SetWindOrigin(origin: Vector4) -> Void
public static native func NCW_SetPropWind(enabled: Bool, radius: Float, scale: Float) -> Void
// for mods that fly their own physics bodies and apply the wind themselves: the prop layer
// skips bodies inside this sphere for the next 0.5 s. Call every frame, one stable id per body.
public static native func NCW_IgnoreNear(id: Int32, position: Vector4, radius: Float) -> Void
public static native func NCW_IsPropHookActive() -> Bool
// a line in the plugin's log (red4ext\logs\nightcitywinds-*.log)
public static native func NCW_Log(message: String) -> Void
public static native func NCW_GetPropWindStats() -> Vector4

// the smoke layer: the plugin's own modifier inside the engine's particle simulation. On tagged
// smoke it damps sideways motion so the game's own wind sets the drift; the optional push
// (off by default) sets the horizontal velocity to the wind outright on every world-space
// emitter with a wind influence.
//   NCW_SetSmokeWind(enabled, gain)   the push; gain 1 = the wind x the emitter's influence
//   NCW_GetSmokeWindStats()           (emitter setups seen, emitters given our modifier,
//                                     push steps in thousands, emitters tagged late)
public static native func NCW_SetSmokeWind(enabled: Bool, gain: Float) -> Void
public static native func NCW_GetSmokeWindStats() -> Vector4

// smoke tagging at load (what the smoke archive did, done in memory): the plugin classifies the
// system's emitters by effect path and emitter name and remembers them; their runtime emitters
// then get a wind influence floor, no VelocityOverLife curve, a birth point no higher than
// 2.5 m, and are kept out of PhysX. Call from a Resource/PostLoad callback and nothing else there.
//   NCW_TagSmokeSystem(system, path)
//   NCW_SetSmokeTagFloor(floor)      default 0.2, the game's own top value for body smoke
//   NCW_GetSmokeTagStats()           (systems tagged, emitters tagged, physics pools cleared,
//                                    emitter setups matched + weak matches / 1000)
public static native func NCW_TagSmokeSystem(system: ref<CParticleSystem>, path: String) -> Void
public static native func NCW_SetSmokeTagFloor(floor: Float) -> Void
public static native func NCW_GetSmokeTagStats() -> Vector4

// 0.7.0
//   NCW_Version()                      the plugin's version, "0.7.0"
//   NCW_IsSmokeHookActive()            false when this game version's particle setup wasn't found
//   NCW_SetVehicleAero(enabled, sideGain, yawLever, liftArea)
//                                      crosswind side force (sideGain x the game's drag, default 1),
//                                      the yaw it gives (centre of pressure yawLever m ahead of the
//                                      centre of mass, default 0.5) and lift (Cl x top area in m2,
//                                      default 2.4; only the wind's share)
//   NCW_GetVehicleAeroStats()          (last side force N, yaw torque N m, lift N, 0)
public static native func NCW_Version() -> String
public static native func NCW_IsSmokeHookActive() -> Bool
public static native func NCW_SetVehicleAero(enabled: Bool, sideGain: Float, yawLever: Float, liftArea: Float) -> Void
public static native func NCW_GetVehicleAeroStats() -> Vector4

// overpass rays: tagged smoke simulates on the CPU, where nothing collides, so the plugin samples
// each puff once a quarter second and NCWParticles casts a short ray along its motion from the
// frame tick; a hit retires the puff (what the designers' killOnCollision did).
//   NCW_SmokeRaySamples()              (position, velocity) pairs sampled since the last call
//   NCW_SmokeRayHit(index)             the pair at `index` hit something
//   NCW_GetSmokeRayStats()             (puffs sampled, puffs retired, survey entries, 0)
public static native func NCW_SetSmokeRays(enabled: Bool) -> Void
public static native func NCW_SmokeRaySamples() -> array<Vector4>
public static native func NCW_SmokeRayHit(index: Int32) -> Void
public static native func NCW_GetSmokeRayStats() -> Vector4

// the coverage survey (dev): while on, every loaded particle system that got no smoke tag is
// recorded with its emitters' names and wind influence; the dump logs them on the main thread
public static native func NCW_SetSmokeSurvey(enabled: Bool) -> Void
public static native func NCW_DumpSmokeSurvey() -> Int32
