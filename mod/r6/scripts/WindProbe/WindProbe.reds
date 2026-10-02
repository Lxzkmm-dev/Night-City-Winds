// =============================================================================
// NIGHT CITY WINDS - RE PROBE HELPERS (dev only)
//
// CET can't pass struct fields by script_ref (ResourceRef.GetPath, CurveDataFloat.GetSize, ...),
// so the WindProbe CET mod calls these instead. Nothing here changes game state.
// =============================================================================

public abstract class WindProbeRS {
  // starts (or finds) the load of a weather state's environment area parameters
  public static func ParamsToken(st: ref<worldWeatherState>) -> ref<ResourceToken> {
    let path = ResourceRef.GetPath(st.environmentAreaParameters);
    return GameInstance.GetResourceDepot().LoadResource(path);
  }

  // every WindAreaSettings in a loaded worldEnvironmentAreaParameters, with its curves
  public static func WindReport(res: ref<CResource>) -> String {
    let params = res as worldEnvironmentAreaParameters;
    if !IsDefined(params) {
      return "not a worldEnvironmentAreaParameters";
    }
    let out = "";
    let count = 0;
    for s in params.renderAreaSettings.areaParameters {
      count += 1;
      let w = s as WindAreaSettings;
      if IsDefined(w) {
        out += "WindAreaSettings enable=" + (w.enable ? "true" : "false") + "\n";
        let n = CurveDataFloat.GetSize(w.strength);
        out += "  strength: " + IntToString(Cast<Int32>(n)) + " points\n";
        let i = 0u;
        while i < n {
          let t: Float;
          let v: Float;
          CurveDataFloat.GetPointValue(w.strength, i, t, v);
          out += "    t=" + FloatToStringPrec(t, 3) + " strength=" + FloatToStringPrec(v, 3) + "\n";
          i += 1u;
        }
        let m = CurveDataVector4.GetSize(w.direction);
        out += "  direction: " + IntToString(Cast<Int32>(m)) + " points\n";
        let j = 0u;
        while j < m {
          let t: Float;
          let d: Vector4;
          CurveDataVector4.GetPointValue(w.direction, j, t, d);
          out += "    t=" + FloatToStringPrec(t, 3) + " dir=(" + FloatToStringPrec(d.X, 3) + ", " + FloatToStringPrec(d.Y, 3) + ", " + FloatToStringPrec(d.Z, 3) + ", " + FloatToStringPrec(d.W, 3) + ")\n";
          j += 1u;
        }
      }
    }
    if StrLen(out) == 0 {
      return "no WindAreaSettings (" + IntToString(count) + " area settings)";
    }
    return out;
  }
}
