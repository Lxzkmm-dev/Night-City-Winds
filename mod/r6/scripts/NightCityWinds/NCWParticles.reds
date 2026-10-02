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
// =============================================================================
module NightCityWinds

public class NCWParticles extends ScriptableService {
  // A ScriptableService, so it listens from game start.
  private cb func OnLoad() {
    GameInstance.GetCallbackSystem().RegisterCallback(n"Resource/PostLoad", this, n"OnParticlesLoaded")
      .AddTarget(ResourceTarget.Type(n"CParticleSystem"));
  }

  private cb func OnParticlesLoaded(event: ref<ResourceEvent>) {
    let ps = event.GetResource() as CParticleSystem;
    if IsDefined(ps) {
      NCW_TagSmokeSystem(ps, ResRef.ToString(event.GetPath()));
    }
  }
}
