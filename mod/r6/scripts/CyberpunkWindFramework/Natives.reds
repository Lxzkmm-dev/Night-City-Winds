// =============================================================================
// CYBERPUNK WIND FRAMEWORK - native bindings (red4ext\plugins\CyberpunkWindFramework)
//
// The plugin applies the wind to the game's physics; scripts tell it what the wind is.
//   CWF_SetWind(wind)            world wind velocity in m/s (W ignored): the air's own motion,
//                                so a wind blowing toward +X is (speed, 0, 0)
//   CWF_GetWind()                the wind the physics is using now
//   CWF_SetEnabled(enabled)      false = vanilla physics
//   CWF_IsVehicleHookActive()    false when this game version's car drag function wasn't found
//   CWF_GetVehicleDragStats()    (drag calls so far, last ground speed, last airspeed,
//                                last airResistanceFactor), for diagnostics
// =============================================================================

public static native func CWF_SetWind(wind: Vector4) -> Void
public static native func CWF_GetWind() -> Vector4
public static native func CWF_SetEnabled(enabled: Bool) -> Void
public static native func CWF_IsVehicleHookActive() -> Bool
public static native func CWF_GetVehicleDragStats() -> Vector4

// the prop layer: drag on loose PhysX bodies (props, debris) around the wind origin
//   CWF_SetWindOrigin(origin)               where the wind was sampled (the player / their car)
//   CWF_SetPropWind(enabled, radius, scale) radius in m (default 50), scale 1 = as computed
//   CWF_IsPropHookActive()                  true once the PhysX scene is hooked (after loading in)
//   CWF_GetPropWindStats()                  (PhysX steps seen, dynamic actors, pushed last step,
//                                           largest force last step in N)
public static native func CWF_SetWindOrigin(origin: Vector4) -> Void
public static native func CWF_SetPropWind(enabled: Bool, radius: Float, scale: Float) -> Void
// for mods that fly their own physics bodies and apply the wind themselves: the prop layer
// skips bodies inside this sphere for the next 0.5 s. Call every frame, one stable id per body.
public static native func CWF_IgnoreNear(id: Int32, position: Vector4, radius: Float) -> Void
public static native func CWF_IsPropHookActive() -> Bool
// a line in the plugin's log (red4ext\logs\cyberpunkwindframework-*.log)
public static native func CWF_Log(message: String) -> Void
// logs a particle system's emitters (wind influence, local space, cooked simulation signature,
// module classes), read natively; main thread only
public static native func CWF_DumpParticles(system: ref<CParticleSystem>, label: String) -> Void
public static native func CWF_GetPropWindStats() -> Vector4

// the smoke layer: the plugin's own wind step inside the engine's particle simulation, on every
// world-space emitter with a wind influence (no patched files needed for it to move)
//   CWF_SetSmokeWind(enabled, gain)   gain 1 = the wind x the emitter's influence
//   CWF_GetSmokeWindStats()           (emitter setups seen, emitters given wind,
//                                     particle steps in thousands, last dt)
public static native func CWF_SetSmokeWind(enabled: Bool, gain: Float) -> Void
public static native func CWF_GetSmokeWindStats() -> Vector4
