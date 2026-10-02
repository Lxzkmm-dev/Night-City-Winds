"""Print every emitter of one effect JSON: wind influence, enabled modules with their key
parameters (drag, velocity, lifetime, spawn rate, size), and the cooked emitterInfo, to see why
an emitter does or doesn't move with the wind. Usage: emitters.py <effect.json> [--raw]"""
import json, sys

TYPE = "$type"


def ev(node):
    """Short text for an evaluator handle (const, start/end, curve)."""
    d = (node or {}).get("Data") or {}
    t = d.get(TYPE, "").replace("CEvaluator", "")
    if "Const" in t:
        return "%s=%s" % (t, d.get("value") if "value" in d else d.get("values") or d.get("value"))
    if "StartEnd" in t:
        return "%s %s->%s" % (t, d.get("start"), d.get("end"))
    if "RandomUniform" in t:
        return "%s %s..%s" % (t, d.get("min"), d.get("max"))
    if "Curve" in t:
        els = ((d.get("curves") or {}).get("Elements") or (d.get("curve") or {}).get("Elements") or [])
        return "%s(%d keys)" % (t, len(els))
    return t or "-"


def short(md):
    out = []
    for k, v in md.items():
        if k in (TYPE, "editorGroup", "editorName", "isEnabled", "seed"):
            continue
        if isinstance(v, dict) and "HandleId" in v:
            out.append("%s:%s" % (k, ev(v)))
        elif isinstance(v, (int, float, str)) and v not in ("", 0):
            out.append("%s=%s" % (k, v))
    return " ".join(out)


d = json.load(open(sys.argv[1], encoding="utf-8"))
root = d["Data"]["RootChunk"]
for h in root.get("emitters") or []:
    e = (h or {}).get("Data") or {}
    blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
    info = (blob.get("header") or {}).get("emitterInfo") or {}
    print("=" * 110)
    print("%s | wind %s | %s" % (e.get("editorName"), e.get("windInfluence"),
          " ".join("%s=%s" % (k, v) for k, v in e.items() if not isinstance(v, (dict, list)) and k not in ("editorName", "windInfluence", TYPE))))
    if "--raw" in sys.argv:
        print("   emitterInfo:", {k: v for k, v in info.items() if not isinstance(v, (dict, list))})
    for m in e.get("modules") or []:
        md = (m or {}).get("Data") or {}
        flag = "" if md.get("isEnabled", 1) else "  [DISABLED]"
        print("   %-40s %s%s" % (md.get(TYPE, "").replace("CParticle", ""), short(md)[:150], flag))
