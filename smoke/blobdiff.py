"""Show the cooked render blob (header.emitterInfo masks + updaterData) of emitters with and
without a Drag module, to see how a modifier is represented in the cooked data."""
import json, sys

def emitters(p):
    return json.load(open(p, encoding="utf-8"))["Data"]["RootChunk"]["emitters"]

def brief(v, depth=0):
    if isinstance(v, dict):
        return {k: brief(x, depth + 1) for k, x in v.items() if depth < 5}
    if isinstance(v, list):
        return ["...%d items" % len(v)] if len(v) > 8 else [brief(x, depth + 1) for x in v]
    if isinstance(v, str) and len(v) > 80:
        return v[:80] + "...(%d chars)" % len(v)
    return v

for path in sys.argv[1:]:
    for h in emitters(path):
        e = h["Data"]
        mods = [((m or {}).get("Data") or {}).get("$type", "").replace("CParticle", "") for m in e.get("modules") or []]
        blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
        info = (blob.get("header") or {}).get("emitterInfo") or {}
        print("=" * 120)
        print(path.split("\\")[-1], e.get("editorName"), "drag" if any("Drag" in m for m in mods) else "NO DRAG")
        print("  modules:", mods)
        print("  numModifiers", info.get("numModifiers"), "modifierSetMask", info.get("modifierSetMask"),
              "numInitializers", info.get("numInitializers"), "initializerSetMask", info.get("initializerSetMask"))
        print("  updaterData:", json.dumps(brief(blob.get("updaterData")))[:1500])
        print("  gpuSimShaders:", json.dumps(brief(blob.get("gpuSimShaders")))[:300])
