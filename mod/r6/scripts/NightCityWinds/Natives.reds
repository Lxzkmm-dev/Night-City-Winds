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
// logs a particle system's emitters (wind influence, local space, cooked simulation signature,
// module classes), read natively; main thread only
public static native func NCW_DumpParticles(system: ref<CParticleSystem>, label: String) -> Void
public static native func NCW_GetPropWindStats() -> Vector4

// the smoke layer: the plugin's own wind step inside the engine's particle simulation, on every
// world-space emitter with a wind influence (no patched files needed for it to move)
//   NCW_SetSmokeWind(enabled, gain)   gain 1 = the wind x the emitter's influence
//   NCW_GetSmokeWindStats()           (emitter setups seen, emitters given wind,
//                                     particle steps in thousands, last dt)
public static native func NCW_SetSmokeWind(enabled: Bool, gain: Float) -> Void
public static native func NCW_GetSmokeWindStats() -> Vector4
