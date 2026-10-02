// =============================================================================
// NIGHT CITY WINDS - PARTICLE DIAGNOSTICS
//
// Smoke takes the wind through the patched effects archive (archive\pc\mod\
// !!!!!NightCityWinds_SmokeWind.archive, built by smoke\build_smoke.py) and the plugin's
// emitter hook; the renderer copies an emitter's settings when the effect loads, so nothing can
// be changed from here at runtime (docs/SMOKE_WIND_ANALYSIS.md). What is left in this service is
// the diagnostic behind the probe's "Dump smoke signatures" button: it loads a fixed set of
// effects on the main thread and has the plugin log each emitter's cooked simulation signature.
// Never do that from a resource callback: those run on the loading threads, and a build that
// logged per emitter from one crashed the game about a minute in (2026-10-01).
// =============================================================================
module NightCityWinds

public class NCWParticles extends ScriptableService {
  private let m_dumpTokens: array<ref<ResourceToken>>;
  private let m_dumpPaths: array<String>;

  public static func Get() -> ref<NCWParticles> {
    return GameInstance.GetScriptableServiceContainer().GetService(n"NightCityWinds.NCWParticles") as NCWParticles;
  }

  // Every particle system the game loads goes to the plugin's tagger (one native call, nothing
  // else: this runs on the loading threads). A ScriptableService, so it listens from game start.
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

  // Loads smoke that follows the wind and smoke that doesn't, and, once each is loaded, logs
  // its emitters' signatures to the plugin log. NCWWind calls PollDump from its frame tick.
  public func DumpSignatures() -> Void {
    let refs: array<ResRef> = [
      r"base\\fx\\environment\\smoke\\small\\e_steam_column_2x2x3m.particle",
      r"base\\fx\\environment\\smoke\\large\\e_steam_column_3x3x20m_dense.particle",
      r"base\\fx\\environment\\smoke\\huge\\e_steam_column_5x5x65.particle",
      r"base\\fx\\environment\\pyro\\tire_pile\\e_decoset_tires_burning_big_a_basic.particle",
      r"base\\fx\\vehicles\\_damage\\body\\v_damage_explode_fire_car_dark_smoke.particle",
      r"base\\fx\\vehicles\\_damage\\body\\v_damage_explode_fire_car_slow_big.particle",
      r"base\\fx\\environment\\smoke\\huge\\e_smoke_chimney_large_light_01.particle"
    ];
    for p in refs {
      ArrayPush(this.m_dumpPaths, ResRef.ToString(p));
      ArrayPush(this.m_dumpTokens, GameInstance.GetResourceDepot().LoadResource(p));
    }
    NCW_Log("signatures: dumping " + IntToString(ArraySize(refs)) + " effects");
  }

  public func PollDump() -> Void {
    let i = ArraySize(this.m_dumpTokens) - 1;
    while i >= 0 {
      let token = this.m_dumpTokens[i];
      if !IsDefined(token) || token.IsFinished() {
        let ps = IsDefined(token) ? token.GetResource() as CParticleSystem : null;
        // read natively, field by field through the engine's property table: a script read of
        // the modules crashed the game on the first emitter
        NCW_DumpParticles(ps, this.m_dumpPaths[i]);
        ArrayErase(this.m_dumpTokens, i);
        ArrayErase(this.m_dumpPaths, i);
      }
      i -= 1;
    }
  }
}
