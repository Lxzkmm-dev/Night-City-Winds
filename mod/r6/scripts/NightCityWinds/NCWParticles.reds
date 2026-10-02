// =============================================================================
// NIGHT CITY WINDS - PARTICLES
//
// Smoke takes the wind through the plugin alone (0.6.0; the patched effects archive is retired):
// every particle system the game loads is handed to the plugin's tagger, which classifies its
// emitters by effect path and emitter name and remembers them. When the renderer later sets an
// emitter up, the plugin recognises it and gives it a wind influence floor, drops its fixed
// velocity curve, lowers its birth point and keeps it out of PhysX, so the engine's own wind
// advection moves it (docs/SMOKE_WIND_ANALYSIS.md). Nothing else may run in the callback: it
// runs on the loading threads, and a build that logged per emitter from one crashed the game
// about a minute in (2026-10-01).
//
// 0.7.0: overpass rays. Smoke kept out of PhysX never collides, so each puff is sampled by the
// plugin once a quarter second and RayTick (called from NCWWind's frame tick, main thread)
// casts a short ray along its motion; a hit retires the puff, as the designers' killOnCollision
// did. At most 96 rays a frame.
// =============================================================================
module NightCityWinds

public class NCWParticles extends ScriptableService {
  private let m_raysOn: Bool;

  public static func Get() -> ref<NCWParticles> {
    return GameInstance.GetScriptableServiceContainer().GetService(n"NightCityWinds.NCWParticles") as NCWParticles;
  }

  // A ScriptableService, so it listens from game start.
  private cb func OnLoad() {
    this.m_raysOn = true;
    GameInstance.GetCallbackSystem().RegisterCallback(n"Resource/PostLoad", this, n"OnParticlesLoaded")
      .AddTarget(ResourceTarget.Type(n"CParticleSystem"));
  }

  private cb func OnParticlesLoaded(event: ref<ResourceEvent>) {
    let ps = event.GetResource() as CParticleSystem;
    if IsDefined(ps) {
      NCW_TagSmokeSystem(ps, ResRef.ToString(event.GetPath()));
    }
  }

  public func SetRays(enabled: Bool) -> Void {
    this.m_raysOn = enabled;
    NCW_SetSmokeRays(enabled);
  }

  public func RaysOn() -> Bool = this.m_raysOn

  // the frame's rays: from each sampled puff along the way it will move in the next 0.3 s
  // (at least 0.75 m, so a slow puff drifting into a ceiling is caught too), static world only
  public func RayTick(game: GameInstance) -> Void {
    if !this.m_raysOn {
      return;
    }
    let samples = NCW_SmokeRaySamples();
    let n = ArraySize(samples) / 2;
    if n == 0 {
      return;
    }
    let sq = GameInstance.GetSpatialQueriesSystem(game);
    let i = 0;
    while i < n {
      let p = samples[i * 2];
      let v = samples[i * 2 + 1];
      let step = v * 0.3;
      let len = Vector4.Length(step);
      if len < 0.75 {
        step = len > 0.01 ? step * (0.75 / len) : new Vector4(0.0, 0.0, 0.75, 0.0);
      }
      let hit: TraceResult;
      if sq.SyncRaycastByCollisionGroup(p, p + step, n"Static", hit, true, false) {
        NCW_SmokeRayHit(i);
      }
      i += 1;
    }
  }
}
