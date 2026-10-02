"""Side by side: the motion modules of smoke emitters that follow the wind vs ones that don't."""
import json, sys

def load(p):
    return json.load(open(p, encoding="utf-8"))["Data"]["RootChunk"]["emitters"]

def summarize(v, depth=0):
    if isinstance(v, dict):
        t = v.get("$type", "")
        keys = {k: summarize(x, depth + 1) for k, x in v.items() if k not in ("$type", "HandleId") and depth < 4}
        return {"$": t, **keys} if t else keys
    if isinstance(v, list):
        return [summarize(x, depth + 1) for x in v[:6]]
    return v

WANT = ("Velocity", "Acceleration", "Drag", "Turbulize", "Noise", "LifeTime")
for path, names in [(sys.argv[1], sys.argv[2].split(",")), (sys.argv[3], sys.argv[4].split(","))]:
    print("=" * 100)
    print(path.split("\\")[-1])
    for h in load(path):
        e = h["Data"]
        if e.get("editorName") not in names:
            continue
        print("-- emitter", e["editorName"], "wind", e.get("windInfluence"), "local", e.get("keepSimulationLocal"))
        for m in e.get("modules") or []:
            d = (m or {}).get("Data") or {}
            t = d.get("$type", "")
            if any(w in t for w in WANT):
                s = json.dumps(summarize({k: v for k, v in d.items() if k not in ("editorName", "editorGroup", "isEnabled")}))
                print("   ", t.replace("CParticle", ""), s[:700])
