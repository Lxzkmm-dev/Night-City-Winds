"""One line per emitter: wind, local/world, lifetime, size, count, and whether it has an enabled
VelocityOverLife / Drag / Acceleration, plus the top mask bit. Usage: brief.py <effect.json>..."""
import json, sys


def mod(e, cls):
    return next((((m or {}).get("Data") or {}) for m in e.get("modules") or []
                 if ((m or {}).get("Data") or {}).get("$type") == cls and ((m or {}).get("Data") or {}).get("isEnabled", 1)), None)


def rng(ev):
    d = (ev or {}).get("Data") or {}
    if "value" in d and not isinstance(d["value"], dict):
        return "%g" % float(d["value"])
    if "min" in d:
        return "%g-%g" % (float(d["min"]), float(d["max"]))
    return d.get("$type", "?").replace("CEvaluator", "")


for p in sys.argv[1:]:
    d = json.load(open(p, encoding="utf-8"))
    print("##", p.split("\\")[-1])
    for h in d["Data"]["RootChunk"].get("emitters") or []:
        e = (h or {}).get("Data") or {}
        info = (((e.get("renderResourceBlob") or {}).get("Data") or {}).get("header") or {}).get("emitterInfo") or {}
        mask = int(info.get("modifierSetMask") or 0)
        life = rng((mod(e, "CParticleInitializerLifeTime") or {}).get("lifeTime"))
        size = mod(e, "CParticleInitializerSize") or {}
        drag = mod(e, "CParticleModificatorDrag")
        print("  %-28s wind %.2f %-5s life %-9s size x%-6s max %-4s VOL %-3s ACC %-3s drag %-6s topbit %d" % (
            e.get("editorName"), float(e.get("windInfluence") or 0), "LOCAL" if e.get("keepSimulationLocal") else "world",
            life, "%g" % float(size.get("scale") or 1), e.get("maxParticles"),
            "yes" if mod(e, "CParticleModificatorVelocityOverLife") else "-",
            "yes" if mod(e, "CParticleModificatorAcceleration") else "-",
            "%g" % float(drag.get("scale") or 0) if drag else "-", mask.bit_length() - 1))
