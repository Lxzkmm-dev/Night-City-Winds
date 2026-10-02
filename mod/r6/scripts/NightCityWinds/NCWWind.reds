// =============================================================================
// NIGHT CITY WINDS - THE WIND FIELD
//
// The air over Night City, for anything that wants it:
//   NCWWind.Get(game).At(position, height)   wind velocity there (m/s, world)
//   NCWWind.Get(game).Exposure(position)     0.25 (sheltered) to 1 (open)
// and, every frame:
//   - the wind at the player (or the car they're in) to the native plugin (NCW_SetWind), which
//     applies it to cars' drag and to loose PhysX props around the player
//   - a push on V while airborne (PSMImpulse)
//   - the same wind written into the weather's own WindAreaSettings curves, which drive the
//     game's foliage, cloth, particles (smoke, fire) and water, so the visuals match the physics
//
// The model (started from Mechs of Night City's CMWind):
//   - WIND STATES: calm, breeze, windy, gale, storm. Each weather has its own odds; a state
//     lasts a few minutes, then the next is drawn. The mean eases between them (~25 s)
//   - direction and the daily rise and fall from the weather's original WindAreaSettings
//     curves (copied when loaded, before we start writing to them), with a slow wander
//   - gusts every few seconds, veering a little, travelling downwind as fronts; turbulence
//     over a few metres
//   - height: slowed near the ground (power law, 10 m reference)
//   - shelter: a building or wall upwind breaks it (rays, a few times a second), its wake
//     recovering over 60 m and churning (extra turbulence) in the lee
// Other mods and tests can pin the wind (SetOverride), force a state (ForceState) or scale it.
// =============================================================================
module NightCityWinds

public class NCWWind extends ScriptableSystem {
  // ---- the mean wind (m/s at 10 m), gusts and turbulence as fractions of it, eased ----
  private let m_speed: Float;
  private let m_gust: Float;
  private let m_turb: Float;
  private let m_toSpeed: Float;
  private let m_toGust: Float;
  private let m_toTurb: Float;
  private let m_seed: Float;
  private let m_last: Float;
  private let m_checkAt: Float;
  private let m_ready: Bool;
  // ---- the weather and the wind state ----
  private let m_weather: CName;
  private let m_weatherKind: Int32;   // NCWWind.Kind* below
  private let m_state: Int32;         // 0 calm, 1 breeze, 2 windy, 3 gale, 4 storm
  private let m_stateUntil: Float;
  private let m_forcedState: Int32;   // -1 = none
  // ---- the weather states' original wind curves, copied once loaded (flat arrays) ----
  private let m_tokens: array<ref<ResourceToken>>;
  private let m_tokenNames: array<CName>;
  private let m_loadedHashes: array<Uint64>;    // envparams already queued (by path hash)
  private let m_scannedDef: Bool;
  private let m_prevWeather: CName;
  private let m_params: array<ref<worldEnvironmentAreaParameters>>; // kept loaded
  private let m_cName: array<CName>;            // per curve set: the weather state
  private let m_cKind: array<Int32>;            // and its weather kind
  private let m_cSettings: array<ref<WindAreaSettings>>; // the live object we write to
  private let m_cSStart: array<Int32>;
  private let m_cSCount: array<Int32>;
  private let m_cDStart: array<Int32>;
  private let m_cDCount: array<Int32>;
  private let m_cMean: array<Float>;            // the strength curve's average
  private let m_sT: array<Float>;
  private let m_sV: array<Float>;
  private let m_dT: array<Float>;
  private let m_dV: array<Vector4>;
  private let m_active: Int32;                  // curve set of the current weather, or -1
  // ---- visuals ----
  private let m_visualOn: Bool;
  private let m_visualGain: Float;
  private let m_visualAt: Float;
  private let m_visualStrength: Float;
  private let m_visualDirty: Bool;
  private let m_visualAllAt: Float;
  private let m_prevActive: Int32;
  // ---- the per-frame push ----
  private let m_ticking: Bool;
  private let m_gen: Int32;
  private let m_exposure: Float;
  private let m_exposureAt: Float;
  private let m_pushed: Vector4;
  private let m_originP: Vector4;              // where the wind is sampled for the physics
  // ---- V on foot ----
  private let m_playerOn: Bool;
  private let m_prevPos: Vector4;
  private let m_prevAt: Float;
  private let m_playerVel: Vector4;
  private let m_airborne: Bool;
  private let m_playerPush: Vector4;
  // ---- other mods ----
  private let m_override: Bool;
  private let m_overrideWind: Vector4;
  private let m_scale: Float;
  private let m_enabled: Bool;

  public static func Get(game: GameInstance) -> ref<NCWWind> {
    return GameInstance.GetScriptableSystemsContainer(game).Get(n"NightCityWinds.NCWWind") as NCWWind;
  }

  private func OnAttach() -> Void {
    this.m_seed = RandRangeF(0.0, 1000.0);
    this.m_ready = false;
    this.m_scale = 1.0;
    this.m_enabled = true;
    this.m_exposure = 1.0;
    this.m_playerOn = false; // Omar (2026-10-01): not a fan of V being pushed; opt-in
    this.m_visualOn = true;
    this.m_visualGain = 1.0;
    this.m_forcedState = -1;
    this.m_active = -1;
    this.m_prevActive = -1;
    this.m_state = 1;
    // every environment definition the game loads (base, Phantom Liberty, Dogtown, Nova City's
    // master envs, other weather mods): queue its weather states' parameters
    GameInstance.GetCallbackSystem().RegisterCallback(n"Resource/PostLoad", this, n"OnEnvDefinitionLoaded")
      .AddTarget(ResourceTarget.Type(n"worldEnvironmentDefinition"));
  }

  public func PlayerWindOn() -> Bool = this.m_playerOn

  private cb func OnEnvDefinitionLoaded(event: ref<ResourceEvent>) {
    let def = event.GetResource() as worldEnvironmentDefinition;
    if !IsDefined(def) {
      return;
    }
    for st in def.weatherStates {
      this.QueueLoad(st);
    }
  }

  private func OnPlayerAttach(request: ref<PlayerAttachRequest>) -> Void {
    this.StartTicking();
  }

  private func OnDetach() -> Void {
    this.m_gen += 1;
    this.m_ticking = false;
    this.RestoreVisuals();
    NCW_SetWind(new Vector4(0.0, 0.0, 0.0, 0.0));
  }

  // ===================================================================================
  // what others read and set
  // ===================================================================================
  // the wind at `p` (m/s, world), `h` metres above the ground (below 0: not known, taken as 10 m);
  // without shelter: multiply by Exposure(p)
  public func At(p: Vector4, h: Float) -> Vector4 = this.AtTurb(p, h, 0.0)

  // the same with extra turbulence (a fraction of the mean), for a building's wake
  public func AtTurb(p: Vector4, h: Float, extraTurb: Float) -> Vector4 {
    if !this.m_enabled {
      return new Vector4(0.0, 0.0, 0.0, 0.0);
    }
    if this.m_override {
      return this.m_overrideWind;
    }
    this.Update();
    let t = this.Clock();
    let s = this.m_seed;
    let mean = this.Mean();
    if mean <= 0.01 {
      return new Vector4(0.0, 0.0, 0.0, 0.0);
    }
    // the boundary layer: slower near the ground, stronger higher up
    let z = h < 0.0 ? 10.0 : MaxF(1.0, h);
    let prof = ClampF(PowF(z / 10.0, 0.25), 0.55, 1.6);
    let g = this.GustAt(p);
    let speed = mean * prof * (1.0 + this.m_gust * 2.0 * g);
    let d = NCWWind.Dir(this.GustHeadingAt(p));
    // eddies over a few metres
    let a = mean * prof * (this.m_turb + MaxF(0.0, extraTurb));
    let ex = SinF(p.X * 0.21 + t * 1.7 + s) * SinF(p.Y * 0.17 - t * 1.3);
    let ey = SinF(p.Y * 0.23 + t * 1.1 + s) * CosF(p.X * 0.19 + t * 0.9);
    let ez = 0.5 * SinF((p.X + p.Y) * 0.15 + t * 2.1) * SinF(p.Z * 0.3 + t + s);
    return new Vector4(d.X * speed + ex * a, d.Y * speed + ey * a, ez * a, 0.0);
  }

  // How open `p` is to the wind, 0.25 (right behind a wall or building upwind) to 1 (open):
  // a ray 60 m upwind and one 3 m above it; the wake recovers with the square root of the
  // distance, so a building's lee reaches several building lengths downwind. Rays: ask a few
  // times a second, not every frame.
  public func Exposure(p: Vector4) -> Float {
    let d = NCWWind.Dir(this.Heading() + 180.0);
    let sq = GameInstance.GetSpatialQueriesSystem(this.GetGameInstance());
    let best = 1.0;
    let i = 0;
    while i < 2 {
      let from = p + new Vector4(0.0, 0.0, 1.0 + 3.0 * Cast<Float>(i), 0.0);
      let to = from + d * 60.0;
      let hit: TraceResult;
      if sq.SyncRaycastByCollisionGroup(from, to, n"Static", hit, true, false) {
        let frac = ClampF(Vector4.Distance(from, Cast<Vector4>(hit.position)) / 60.0, 0.0, 1.0);
        best = MinF(best, 0.25 + 0.75 * SqrtF(frac));
      }
      i += 1;
    }
    return best;
  }

  // the mean wind at 10 m now (m/s), before gusts and shelter
  public func Mean() -> Float {
    this.Update();
    return this.m_speed * this.DailyShape() * this.m_scale;
  }

  // the heading the wind blows toward, degrees: 0 = +Y (north), 90 = +X (east)
  public func Heading() -> Float {
    let t = this.Clock();
    let s = this.m_seed;
    let wander = 20.0 * SinF(t / 170.0 + s) + 8.0 * SinF(t / 41.0 + s * 2.0);
    if this.m_active >= 0 {
      return NCWWind.Wrap(this.CurveHeading() + wander);
    }
    return NCWWind.Wrap(s * 0.36 + 35.0 * SinF(t / 170.0 + s) + 12.0 * SinF(t / 41.0 + s * 2.0));
  }

  // the heading with the gust's veer, at the wind origin (the player or their car)
  public func GustHeading() -> Float = this.GustHeadingAt(this.m_originP)

  public func GustHeadingAt(p: Vector4) -> Float {
    let t = this.GustClock(p);
    return this.Heading() + 15.0 * SinF(t * 0.5 + this.m_seed) * this.GustAt(p);
  }

  // the gust factor now at the wind origin, 0 (lull) to about 1 (a full gust)
  public func Gust() -> Float = this.GustAt(this.m_originP)

  // Gusts are fronts that travel downwind at the mean speed, not a pulse everywhere at once:
  // the gust clock at `p` runs behind by the time the front takes to get there, so a gust
  // reaches the trees upwind first, then the car, then the smoke downwind.
  public func GustAt(p: Vector4) -> Float {
    let t = this.GustClock(p);
    let s = this.m_seed;
    let g = 0.55 * SinF(t * 0.71 + s) + 0.35 * SinF(t * 1.93 + s * 2.0) + 0.25 * SinF(t * 0.29 + s * 3.0) - 0.25;
    return ClampF(g / 0.9, 0.0, 1.0);
  }

  private func GustClock(p: Vector4) -> Float {
    let d = NCWWind.Dir(this.Heading());
    let along = p.X * d.X + p.Y * d.Y;
    return this.Clock() - along / MaxF(3.0, this.m_speed);
  }

  public func Weather() -> CName {
    this.Update();
    return this.m_weather;
  }

  // the wind state: n"calm", n"breeze", n"windy", n"gale", n"storm"
  public func State() -> CName = NCWWind.StateName(this.m_state)

  // pin the wind state (n"calm" ... n"storm"); n"" or an unknown name lets the weather pick again
  public func ForceState(state: CName) -> Void {
    this.m_forcedState = NCWWind.StateIndex(state);
    this.m_stateUntil = 0.0;
  }

  // the wind the physics was last given (at the player / their car, with shelter)
  public func Pushed() -> Vector4 = this.m_pushed

  public func Sheltered() -> Float = this.m_exposure

  public func HasWeatherCurves() -> Bool = this.m_active >= 0

  // the strength last written into the weather's wind curves (the game's visual scale)
  public func VisualStrength() -> Float = this.m_visualStrength

  // write the live wind into the weather's wind curves (foliage, cloth, smoke, fire)
  public func SetVisualSync(enabled: Bool) -> Void {
    this.m_visualOn = enabled;
    if !enabled {
      this.RestoreVisuals();
    }
  }

  // multiplies the visual strength written to the curves (1 = the weather's own balance)
  public func SetVisualGain(gain: Float) -> Void {
    this.m_visualGain = MaxF(0.0, gain);
  }

  public func VisualGain() -> Float = this.m_visualGain

  // pin the wind to `wind` (m/s, world) until ClearOverride; for scenes, tests and other mods
  public func SetOverride(wind: Vector4) -> Void {
    this.m_override = true;
    this.m_overrideWind = wind;
  }

  public func ClearOverride() -> Void {
    this.m_override = false;
  }

  // scales the weather's wind (1 = as designed, 0 = still)
  public func SetScale(scale: Float) -> Void {
    this.m_scale = MaxF(0.0, scale);
  }

  public func SetEnabled(enabled: Bool) -> Void {
    this.m_enabled = enabled;
    NCW_SetEnabled(enabled);
    if !enabled {
      this.RestoreVisuals();
    }
  }

  // wind on V on foot (pushes while jumping, falling or gliding; nothing while grounded)
  public func SetPlayerWind(enabled: Bool) -> Void {
    this.m_playerOn = enabled;
  }

  public func PlayerAirborne() -> Bool = this.m_airborne

  // the velocity change the wind gave V last frame (m/s)
  public func PlayerPush() -> Vector4 = this.m_playerPush

  // ===================================================================================
  // the frame tick
  // ===================================================================================
  private func StartTicking() -> Void {
    if this.m_ticking {
      return;
    }
    this.m_ticking = true;
    this.m_gen += 1;
    this.Next();
  }

  private func Next() -> Void {
    let cb = new NCWWindTick();
    cb.sys = this;
    cb.gen = this.m_gen;
    GameInstance.GetDelaySystem(this.GetGameInstance()).DelayCallback(cb, 0.0, false);
  }

  public func Tick(gen: Int32) -> Void {
    if gen != this.m_gen {
      return;
    }
    let player = GetPlayer(this.GetGameInstance());
    if IsDefined(player) {
      let p = player.GetWorldPosition();
      let veh = GetMountedVehicle(player);
      if IsDefined(veh) {
        p = veh.GetWorldPosition();
      }
      let now = this.Clock();
      if now >= this.m_exposureAt {
        this.m_exposureAt = now + 0.25;
        // shelter eases in and out over about a second, so passing a building doesn't snap
        this.m_exposure += (this.Exposure(p) - this.m_exposure) * 0.3;
      }
      this.m_originP = p;
      // a car's body sits about 1 m up: the ground's boundary layer slows the wind there; in
      // a building's wake the mean drops (shelter) and the eddies grow (wake turbulence)
      let w = this.AtTurb(p, 1.5, 0.6 * (1.0 - this.m_exposure));
      if !this.m_override {
        w = w * this.m_exposure;
      }
      this.m_pushed = w;
      NCW_SetWindOrigin(p);
      NCW_SetWind(w);
      // overpass rays for smoke, on the main thread
      let particles = NCWParticles.Get();
      if IsDefined(particles) {
        particles.RayTick(this.GetGameInstance());
      }
      if !IsDefined(veh) {
        this.PlayerWind(player, now);
      } else {
        this.m_prevAt = 0.0;
        this.m_airborne = false;
        this.m_playerPush = new Vector4(0.0, 0.0, 0.0, 0.0);
      }
      if now >= this.m_visualAt {
        this.m_visualAt = now + 0.1;
        this.SyncVisuals();
      }
    }
    this.Next();
  }

  // V is a character controller, not a rigid body: the wind reaches V as a locomotion impulse
  // (PSMImpulse, a velocity change), only while airborne, where the game isn't driving V's
  // velocity from the stick. Drag on a standing person: Cd 1.0, about 0.7 m2, 80 kg.
  private func PlayerWind(player: ref<PlayerPuppet>, now: Float) -> Void {
    let p = player.GetWorldPosition();
    let dt = now - this.m_prevAt;
    if this.m_prevAt <= 0.0 || dt <= 0.0 || dt > 0.25 {
      this.m_prevPos = p;
      this.m_prevAt = now;
      this.m_playerVel = new Vector4(0.0, 0.0, 0.0, 0.0);
      return;
    }
    let v = (p - this.m_prevPos) / dt;
    this.m_playerVel = this.m_playerVel + (v - this.m_playerVel) * 0.5;
    this.m_prevPos = p;
    this.m_prevAt = now;
    this.m_airborne = !this.OnGround(p);
    this.m_playerPush = new Vector4(0.0, 0.0, 0.0, 0.0);
    if !this.m_playerOn || !this.m_enabled || !this.m_airborne {
      return;
    }
    let w = this.At(p, 1.7);
    if !this.m_override {
      w = w * this.m_exposure;
    }
    let rel = w - this.m_playerVel;
    let speed = Vector4.Length(rel);
    if speed < 0.5 {
      return;
    }
    // F = 0.5 rho Cd A |rel| rel; dv = F / m dt
    let dv = rel * (0.5 * 1.2 * 1.0 * 0.7 * speed / 80.0 * dt);
    this.m_playerPush = dv;
    let evt = new PSMImpulse();
    evt.id = n"impulse";
    evt.impulse = dv;
    player.QueueEvent(evt);
  }

  // something solid within 0.6 m under V's feet (the world, a car or a prop)
  private func OnGround(p: Vector4) -> Bool {
    let sq = GameInstance.GetSpatialQueriesSystem(this.GetGameInstance());
    let from = p + new Vector4(0.0, 0.0, 0.3, 0.0);
    let to = p - new Vector4(0.0, 0.0, 0.6, 0.0);
    let hit: TraceResult;
    if sq.SyncRaycastByCollisionGroup(from, to, n"Static", hit, true, false) {
      return true;
    }
    if sq.SyncRaycastByCollisionGroup(from, to, n"Vehicle", hit, true, false) {
      return true;
    }
    return sq.SyncRaycastByCollisionGroup(from, to, n"Dynamic", hit, true, false);
  }

  // ===================================================================================
  // visuals: the live wind into the weather's own WindAreaSettings
  // ===================================================================================
  // Every 0.1 s, each weather state's primary WindAreaSettings gets the current wind: its
  // strength curve set flat to (that state's original average) x (the wind now / that
  // weather's design speed), so a weather keeps its own art balance and moves with the wind
  // state and the gusts; its direction curve set flat to the current heading. All states are
  // written (not just the active one) so a weather transition blends between two up-to-date
  // curves. Point counts are never changed (no reallocation under the renderer).
  private func SyncVisuals() -> Void {
    if !this.m_visualOn || !this.m_enabled || ArraySize(this.m_cSettings) == 0 {
      return;
    }
    let speedNow = this.Mean() * (1.0 + this.m_gust * 2.0 * this.Gust());
    if this.m_override {
      speedNow = Vector4.Length(this.m_overrideWind);
    }
    let heading = this.GustHeading();
    if this.m_override && speedNow > 0.01 {
      heading = Rad2Deg(AtanF(this.m_overrideWind.X, this.m_overrideWind.Y));
    }
    let d = NCWWind.Dir(heading);
    let dir = new Vector4(d.X, d.Y, 0.0, 0.0);
    // the current and the previous weather every time (a transition blends those two); every
    // other state once every 2 s, so a switch to any of them starts from fresh curves
    // (Nova City 2 brings ~60 states with wind curves)
    let now = this.Clock();
    let all = now >= this.m_visualAllAt;
    if all {
      this.m_visualAllAt = now + 2.0;
    }
    let k = 0;
    while k < ArraySize(this.m_cSettings) {
      // by name: the base game, Phantom Liberty and Nova City can each have a file for a state
      if all || Equals(this.m_cName[k], this.m_weather) || Equals(this.m_cName[k], this.m_prevWeather) {
        this.WriteCurves(k, speedNow, dir);
      }
      k += 1;
    }
    this.m_visualDirty = true;
  }

  // one weather state's curves: strength flat at its scaled level, direction flat at `dir`
  private func WriteCurves(k: Int32, speedNow: Float, dir: Vector4) -> Void {
    let w = this.m_cSettings[k];
    // Quadratic, like the drag the wind exerts. Two scales, the larger wins:
    //  - physical: 20 at a 21 m/s storm. The engine's SpeedTree update takes the blended wind
    //    strength x 0.05, clamped to 1 (hash 1291658409), so 20 is full foliage sway; vanilla's
    //    sandstorm uses exactly 20, its sunny 0.06 (still trees)
    //  - the weather's own art: its curve average at its design speed, so a weather authored
    //    windier than the physics (a sandstorm's dust) keeps its look
    let design = NCWWind.DesignSpeed(this.m_cKind[k]);
    let ratio = speedNow / MaxF(0.5, design);
    let art = this.m_cMean[k] * ratio * ratio;
    let phys = 20.0 * (speedNow / 21.0) * (speedNow / 21.0);
    // capped at 25: trees are already at full sway at 20, and particles (fire especially) shear
    // apart well past the strongest the game's own weathers use (sandstorm, 20)
    let s = ClampF(MaxF(art, phys) * this.m_visualGain, 0.0, 25.0);
    if k == this.m_active {
      this.m_visualStrength = s;
    }
    let n = CurveDataFloat.GetSize(w.strength);
    let i = 0u;
    while i < n {
      let t: Float;
      let v: Float;
      CurveDataFloat.GetPointValue(w.strength, i, t, v);
      CurveDataFloat.SetPointValue(w.strength, i, t, s);
      i += 1u;
    }
    let m = CurveDataVector4.GetSize(w.direction);
    let j = 0u;
    while j < m {
      let t: Float;
      let v: Vector4;
      CurveDataVector4.GetPointValue(w.direction, j, t, v);
      CurveDataVector4.SetPointValue(w.direction, j, t, dir);
      j += 1u;
    }
  }

  // puts every curve back as the game had it
  private func RestoreVisuals() -> Void {
    if !this.m_visualDirty {
      return;
    }
    this.m_visualDirty = false;
    let k = 0;
    while k < ArraySize(this.m_cSettings) {
      let w = this.m_cSettings[k];
      let i = 0;
      while i < this.m_cSCount[k] && i < Cast<Int32>(CurveDataFloat.GetSize(w.strength)) {
        let idx = this.m_cSStart[k] + i;
        CurveDataFloat.SetPointValue(w.strength, Cast<Uint32>(i), this.m_sT[idx], this.m_sV[idx]);
        i += 1;
      }
      let j = 0;
      while j < this.m_cDCount[k] && j < Cast<Int32>(CurveDataVector4.GetSize(w.direction)) {
        let idx = this.m_cDStart[k] + j;
        CurveDataVector4.SetPointValue(w.direction, Cast<Uint32>(j), this.m_dT[idx], this.m_dV[idx]);
        j += 1;
      }
      k += 1;
    }
  }

  // ===================================================================================
  // the weather and the wind state
  // ===================================================================================
  private func Clock() -> Float = EngineTime.ToFloat(GameInstance.GetEngineTime(this.GetGameInstance()))

  private func Update() -> Void {
    let now = this.Clock();
    if !this.m_ready || now >= this.m_checkAt {
      this.m_checkAt = now + 2.0;
      this.LoadCurves();
      this.ReadWeather(now);
    }
    this.PollCurves();
    if now >= this.m_stateUntil || (this.m_forcedState >= 0 && this.m_forcedState != this.m_state) {
      this.PickState(now);
    }
    if !this.m_ready {
      this.m_ready = true;
      this.m_speed = this.m_toSpeed;
      this.m_gust = this.m_toGust;
      this.m_turb = this.m_toTurb;
      this.m_last = now;
      return;
    }
    let dt = ClampF(now - this.m_last, 0.0, 5.0);
    this.m_last = now;
    let k = 1.0 - ExpF(-dt / 25.0);
    this.m_speed += (this.m_toSpeed - this.m_speed) * k;
    this.m_gust += (this.m_toGust - this.m_gust) * k;
    this.m_turb += (this.m_toTurb - this.m_turb) * k;
  }

  private func ReadWeather(now: Float) -> Void {
    let ws = GameInstance.GetWeatherSystem(this.GetGameInstance());
    if !IsDefined(ws) {
      return;
    }
    let st = ws.GetWeatherState();
    if !IsDefined(st) {
      return;
    }
    if Equals(st.name, this.m_weather) && this.m_ready {
      return;
    }
    this.m_prevWeather = this.m_weather;
    this.m_weather = st.name;
    this.m_weatherKind = NCWWind.KindOf(st.name);
    this.m_prevActive = this.m_active;
    this.m_active = this.CurveSetOf(st.name);
    // a new weather draws a new wind state at once (the mean still eases over ~25 s)
    this.m_stateUntil = 0.0;
    // a weather that isn't in any definition we've seen (quest weather): load it too
    this.QueueLoad(st);
  }

  // draws the next wind state from the weather's odds (or takes the forced one)
  private func PickState(now: Float) -> Void {
    let state = this.m_forcedState;
    if state < 0 {
      let w = NCWWind.Odds(this.m_weatherKind);
      let total = w.X + w.Y + w.Z + w.W + NCWWind.StormOdds(this.m_weatherKind);
      let r = RandRangeF(0.0, MaxF(0.0001, total));
      if r < w.X {
        state = 0;
      } else if r < w.X + w.Y {
        state = 1;
      } else if r < w.X + w.Y + w.Z {
        state = 2;
      } else if r < w.X + w.Y + w.Z + w.W {
        state = 3;
      } else {
        state = 4;
      }
    }
    this.m_state = state;
    this.m_stateUntil = now + RandRangeF(90.0, 300.0);
    // each state's speed varies a little each time it comes round
    let jitter = RandRangeF(0.8, 1.2);
    switch state {
      case 0: this.Want(1.0 * jitter, 0.25, 0.1); break;
      case 1: this.Want(4.0 * jitter, 0.3, 0.15); break;
      case 2: this.Want(8.0 * jitter, 0.4, 0.2); break;
      case 3: this.Want(14.0 * jitter, 0.55, 0.3); break;
      default: this.Want(21.0 * jitter, 0.7, 0.4);
    }
  }

  private func Want(speed: Float, gust: Float, turb: Float) -> Void {
    this.m_toSpeed = speed;
    this.m_toGust = gust;
    this.m_toTurb = turb;
  }

  // ---- loading every weather state's parameters and copying their wind curves ----
  private func LoadCurves() -> Void {
    if this.m_scannedDef {
      return;
    }
    let ws = GameInstance.GetWeatherSystem(this.GetGameInstance());
    if !IsDefined(ws) {
      return;
    }
    let def = ws.GetEnvironmentDefinition();
    if !IsDefined(def) {
      return;
    }
    this.m_scannedDef = true;
    for st in def.weatherStates {
      this.QueueLoad(st);
    }
  }

  // once per envparams file (several definitions can share the vanilla states' files)
  private func QueueLoad(st: ref<worldWeatherState>) -> Void {
    if !IsDefined(st) || ResourceRef.IsEmpty(st.environmentAreaParameters) {
      return;
    }
    let hash = ResourceRef.GetHash(st.environmentAreaParameters);
    if ArrayContains(this.m_loadedHashes, hash) {
      return;
    }
    ArrayPush(this.m_loadedHashes, hash);
    ArrayPush(this.m_tokenNames, st.name);
    ArrayPush(this.m_tokens, GameInstance.GetResourceDepot().LoadResource(ResourceRef.GetPath(st.environmentAreaParameters)));
  }

  private func PollCurves() -> Void {
    let i = ArraySize(this.m_tokens) - 1;
    while i >= 0 {
      let token = this.m_tokens[i];
      if IsDefined(token) && token.IsFinished() {
        let params = token.GetResource() as worldEnvironmentAreaParameters;
        if IsDefined(params) {
          ArrayPush(this.m_params, params);
          this.CopyCurves(this.m_tokenNames[i], params);
        }
        this.m_tokens[i] = null;
        if Equals(this.m_tokenNames[i], this.m_weather) {
          this.m_active = this.CurveSetOf(this.m_weather);
        }
      }
      i -= 1;
    }
  }

  // the state's primary WindAreaSettings (enabled, with a direction): its original points
  private func CopyCurves(name: CName, params: ref<worldEnvironmentAreaParameters>) -> Void {
    for s in params.renderAreaSettings.areaParameters {
      let w = s as WindAreaSettings;
      if IsDefined(w) && w.enable && CurveDataVector4.GetSize(w.direction) > 0u && CurveDataFloat.GetSize(w.strength) > 0u {
        let probe = NCWWind.SampleV(w.direction, 12.0);
        if AbsF(probe.X) + AbsF(probe.Y) > 0.001 {
          ArrayPush(this.m_cName, name);
          ArrayPush(this.m_cKind, NCWWind.KindOf(name));
          ArrayPush(this.m_cSettings, w);
          ArrayPush(this.m_cSStart, ArraySize(this.m_sT));
          ArrayPush(this.m_cDStart, ArraySize(this.m_dT));
          let n = CurveDataFloat.GetSize(w.strength);
          let sum = 0.0;
          let i = 0u;
          while i < n {
            let t: Float;
            let v: Float;
            CurveDataFloat.GetPointValue(w.strength, i, t, v);
            ArrayPush(this.m_sT, t);
            ArrayPush(this.m_sV, v);
            sum += v;
            i += 1u;
          }
          ArrayPush(this.m_cSCount, Cast<Int32>(n));
          ArrayPush(this.m_cMean, sum / Cast<Float>(Cast<Int32>(n)));
          let m = CurveDataVector4.GetSize(w.direction);
          let j = 0u;
          while j < m {
            let t: Float;
            let v: Vector4;
            CurveDataVector4.GetPointValue(w.direction, j, t, v);
            ArrayPush(this.m_dT, t);
            ArrayPush(this.m_dV, v);
            j += 1u;
          }
          ArrayPush(this.m_cDCount, Cast<Int32>(m));
          return;
        }
      }
    }
  }

  private func CurveSetOf(name: CName) -> Int32 {
    let k = 0;
    while k < ArraySize(this.m_cName) {
      if Equals(this.m_cName[k], name) {
        return k;
      }
      k += 1;
    }
    return -1;
  }

  // the weather's original strength curve's rise and fall through the day, 0.3 to 1.7 around 1
  private func DailyShape() -> Float {
    let k = this.m_active;
    if k < 0 || this.m_cMean[k] <= 0.0001 {
      return 1.0;
    }
    let v = this.SampleStored(this.m_cSStart[k], this.m_cSCount[k], this.Hour());
    return ClampF(v / this.m_cMean[k], 0.3, 1.7);
  }

  private func CurveHeading() -> Float {
    let k = this.m_active;
    let d = this.SampleStoredV(this.m_cDStart[k], this.m_cDCount[k], this.Hour());
    return Rad2Deg(AtanF(d.X, d.Y));
  }

  private func Hour() -> Float {
    let gt = GameInstance.GetTimeSystem(this.GetGameInstance()).GetGameTime();
    return Cast<Float>(GameTime.Hours(gt)) + Cast<Float>(GameTime.Minutes(gt)) / 60.0;
  }

  // linear between the stored keys, held flat outside them
  private func SampleStored(start: Int32, count: Int32, x: Float) -> Float {
    if count <= 0 {
      return 0.0;
    }
    if count == 1 || x <= this.m_sT[start] {
      return this.m_sV[start];
    }
    let i = 1;
    while i < count {
      let a = start + i - 1;
      let b = start + i;
      if x <= this.m_sT[b] {
        return this.m_sV[a] + (this.m_sV[b] - this.m_sV[a]) * (x - this.m_sT[a]) / MaxF(0.0001, this.m_sT[b] - this.m_sT[a]);
      }
      i += 1;
    }
    return this.m_sV[start + count - 1];
  }

  private func SampleStoredV(start: Int32, count: Int32, x: Float) -> Vector4 {
    if count <= 0 {
      return new Vector4(0.0, 1.0, 0.0, 0.0);
    }
    if count == 1 || x <= this.m_dT[start] {
      return this.m_dV[start];
    }
    let i = 1;
    while i < count {
      let a = start + i - 1;
      let b = start + i;
      if x <= this.m_dT[b] {
        let k = (x - this.m_dT[a]) / MaxF(0.0001, this.m_dT[b] - this.m_dT[a]);
        return this.m_dV[a] + (this.m_dV[b] - this.m_dV[a]) * k;
      }
      i += 1;
    }
    return this.m_dV[start + count - 1];
  }

  // ===================================================================================
  // tables
  // ===================================================================================
  // ---- weather kinds -----------------------------------------------------------------
  //  0 clear       1 smog/pollution  2 fog/mist     3 clouds/overcast  4 heavy clouds
  //  5 rain        6 toxic rain      7 storm        8 sandstorm        9 hot and still (haze,
  //  humid, arid)  10 windy          11 drizzle     12 heavy rain      13 wet fog
  // Every vanilla state and every Nova City 2 state (70, from its nova_city_master_env_v002)
  // is listed by name; anything else (quest weathers, other weather mods) goes by keywords.
  public static func KindOf(weather: CName) -> Int32 {
    let n = StrLower(NameToString(weather));
    if StrBeginsWith(n, "24h_weather_") {
      n = StrAfterFirst(n, "24h_weather_");
    }
    let k = "|" + n + "|";
    if StrContains("|sandstorm|sandstorm_old|vanilla_sandstorm|", k) { return 8; }
    if StrContains("|storm|", k) { return 7; }
    if StrContains("|toxic_rain|vanilla_toxic_rain|", k) { return 6; }
    if StrContains("|rain_alt_2|drizzle_heavy|downpour|", k) { return 12; }
    if StrContains("|rain|vanilla_rain|rain_alt_1|rain_q|rain_q_alt|rain_3q|rain_wip|q306_rainy_night|", k) { return 5; }
    if StrContains("|drizzle|drizzle_light|light_rain|distant_rain|q302_light_rain|", k) { return 11; }
    if StrContains("|windy|sunny_windy|", k) { return 10; }
    if StrContains("|fog_wet|fog_rain|haze_rain|", k) { return 13; }
    if StrContains("|fog|vanilla_fog|mist|fog_heavy|fog_dense|fog_thick|fog_dark_dense|fog_haze|silent_hill|pollution_fog|", k) { return 2; }
    if StrContains("|pollution|vanilla_pollution|smog|haze_pollution|haze_smog|", k) { return 1; }
    if StrContains("|drought|arid|humid|muggy|haze|haze_heavy|dew|", k) { return 9; }
    if StrContains("|heavy_clouds|vanilla_heavy_clouds|heavy_clouds_dense|", k) { return 4; }
    if StrContains("|light_clouds|cloudy|vanilla_light_clouds|vanilla_cloudy|overcast|overcast_light|overcast_broken|courier_clouds|gloomy|meme_clouds|sa_courier_clouds|q306_epilogue_cloudy_morning|", k) { return 3; }
    if StrContains("|sunny|sunny_old|clear|sunny_sunset|sunn_e3|vanilla_sunny|default|sky_softbox|sky_softbox_clear|blackout|test|", k) { return 0; }
    // keywords, strongest first
    if StrContains(n, "sandstorm") || StrContains(n, "dust") { return 8; }
    if StrContains(n, "storm") || StrContains(n, "thunder") || StrContains(n, "hurricane") || StrContains(n, "typhoon") { return 7; }
    if StrContains(n, "toxic") || StrContains(n, "acid") { return 6; }
    if StrContains(n, "downpour") || StrContains(n, "heavy_rain") || StrContains(n, "monsoon") { return 12; }
    if StrContains(n, "windy") || StrContains(n, "gust") || StrContains(n, "breez") || StrContains(n, "gale") { return 10; }
    if StrContains(n, "drizzle") || StrContains(n, "light_rain") || StrContains(n, "shower") { return 11; }
    if StrContains(n, "rain") || StrContains(n, "wet") { return 5; }
    if StrContains(n, "fog") || StrContains(n, "mist") { return 2; }
    if StrContains(n, "smog") || StrContains(n, "pollution") { return 1; }
    if StrContains(n, "haze") || StrContains(n, "humid") || StrContains(n, "muggy") || StrContains(n, "arid") || StrContains(n, "drought") { return 9; }
    if StrContains(n, "heavy_cloud") || StrContains(n, "dense_cloud") { return 4; }
    if StrContains(n, "cloud") || StrContains(n, "overcast") || StrContains(n, "gloom") { return 3; }
    return 0;
  }

  // the wind (m/s at 10 m) each weather's own art was made for: its curves' average maps here
  public static func DesignSpeed(kind: Int32) -> Float {
    switch kind {
      case 1: return 2.0;   // smog
      case 2: return 1.5;   // fog
      case 3: return 4.5;   // clouds
      case 4: return 6.0;   // heavy clouds
      case 5: return 7.0;   // rain
      case 6: return 9.0;   // toxic rain
      case 7: return 14.0;  // storm
      case 8: return 15.0;  // sandstorm
      case 9: return 2.5;   // hot and still
      case 10: return 9.0;  // windy
      case 11: return 5.0;  // drizzle
      case 12: return 10.0; // heavy rain
      case 13: return 3.0;  // wet fog
    }
    return 3.5;             // clear
  }

  // odds of calm, breeze, windy, gale (X, Y, Z, W); storm in StormOdds
  public static func Odds(kind: Int32) -> Vector4 {
    switch kind {
      case 1: return new Vector4(0.5, 0.4, 0.1, 0.0);     // smog
      case 2: return new Vector4(0.7, 0.3, 0.0, 0.0);     // fog
      case 3: return new Vector4(0.15, 0.5, 0.3, 0.05);   // clouds
      case 4: return new Vector4(0.05, 0.3, 0.45, 0.2);   // heavy clouds
      case 5: return new Vector4(0.0, 0.2, 0.5, 0.25);    // rain
      case 6: return new Vector4(0.0, 0.1, 0.4, 0.4);     // toxic rain
      case 7: return new Vector4(0.0, 0.0, 0.2, 0.5);     // storm
      case 8: return new Vector4(0.0, 0.0, 0.1, 0.4);     // sandstorm
      case 9: return new Vector4(0.45, 0.4, 0.13, 0.02);  // hot and still
      case 10: return new Vector4(0.0, 0.15, 0.5, 0.3);   // windy
      case 11: return new Vector4(0.1, 0.45, 0.35, 0.1);  // drizzle
      case 12: return new Vector4(0.0, 0.1, 0.4, 0.4);    // heavy rain
      case 13: return new Vector4(0.3, 0.5, 0.2, 0.0);    // wet fog
    }
    return new Vector4(0.3, 0.5, 0.18, 0.02);             // clear
  }

  public static func StormOdds(kind: Int32) -> Float {
    switch kind {
      case 5: return 0.05;
      case 6: return 0.1;
      case 7: return 0.3;
      case 8: return 0.5;
      case 10: return 0.05;
      case 12: return 0.1;
    }
    return 0.0;
  }

  // a readable name for the weather kind, for the probe and logs
  public static func KindName(kind: Int32) -> String {
    switch kind {
      case 1: return "smog";
      case 2: return "fog";
      case 3: return "clouds";
      case 4: return "heavy clouds";
      case 5: return "rain";
      case 6: return "toxic rain";
      case 7: return "storm";
      case 8: return "sandstorm";
      case 9: return "hot and still";
      case 10: return "windy";
      case 11: return "drizzle";
      case 12: return "heavy rain";
      case 13: return "wet fog";
    }
    return "clear";
  }

  public func WeatherKind() -> String = NCWWind.KindName(this.m_weatherKind)

  // how many weather states' wind curves are loaded and driven (vanilla: ~13, Nova City 2: ~60)
  public func DrivenStates() -> Int32 = ArraySize(this.m_cSettings)

  public static func StateName(state: Int32) -> CName {
    switch state {
      case 0: return n"calm";
      case 1: return n"breeze";
      case 2: return n"windy";
      case 3: return n"gale";
    }
    return n"storm";
  }

  public static func StateIndex(state: CName) -> Int32 {
    let i = 0;
    while i < 5 {
      if Equals(NCWWind.StateName(i), state) {
        return i;
      }
      i += 1;
    }
    return -1;
  }

  // ===================================================================================
  // curves and angles
  // ===================================================================================
  public static func SampleV(c: CurveDataVector4, x: Float) -> Vector4 {
    let n = CurveDataVector4.GetSize(c);
    if n == 0u {
      return new Vector4(0.0, 0.0, 0.0, 0.0);
    }
    let pt: Float;
    let pv: Vector4;
    CurveDataVector4.GetPointValue(c, 0u, pt, pv);
    if x <= pt || n == 1u {
      return pv;
    }
    let i = 1u;
    while i < n {
      let t: Float;
      let v: Vector4;
      CurveDataVector4.GetPointValue(c, i, t, v);
      if x <= t {
        let k = (x - pt) / MaxF(0.0001, t - pt);
        return pv + (v - pv) * k;
      }
      pt = t;
      pv = v;
      i += 1u;
    }
    return pv;
  }

  // the horizontal unit vector a heading points along (0 = +Y, 90 = +X)
  public static func Dir(heading: Float) -> Vector4 {
    let r = Deg2Rad(heading);
    return new Vector4(SinF(r), CosF(r), 0.0, 0.0);
  }

  public static func Wrap(deg: Float) -> Float {
    let d = deg;
    while d > 180.0 {
      d -= 360.0;
    }
    while d <= -180.0 {
      d += 360.0;
    }
    return d;
  }
}

public class NCWWindTick extends DelayCallback {
  public let sys: wref<NCWWind>;
  public let gen: Int32;

  public func Call() -> Void {
    if IsDefined(this.sys) {
      this.sys.Tick(this.gen);
    }
  }
}
