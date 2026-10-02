"""Infer which modifierSetMask bit belongs to which modifier class, from many emitters.
For each modifier class, the bit(s) set in every emitter that has it (and that are absent in
emitters without it)."""
import glob, json, os, sys
from collections import defaultdict

root = sys.argv[1]
rows = []
for f in glob.glob(os.path.join(root, "**", "*.json"), recursive=True):
    try:
        d = json.load(open(f, encoding="utf-8"))
    except Exception:
        continue
    for h in d["Data"]["RootChunk"].get("emitters") or []:
        e = (h or {}).get("Data") or {}
        blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
        info = (blob.get("header") or {}).get("emitterInfo") or {}
        if "modifierSetMask" not in info:
            continue
        mods = set()
        for m in e.get("modules") or []:
            md = (m or {}).get("Data") or {}
            t = md.get("$type", "")
            if t.startswith("CParticleModificator") and md.get("isEnabled", 1):
                mods.add(t.replace("CParticleModificator", ""))
        rows.append((int(info["modifierSetMask"]), int(info.get("numModifiers") or 0), mods, f, e.get("editorName")))

print("emitters:", len(rows))
classes = sorted(set().union(*[r[2] for r in rows]))
for c in classes:
    have = [r[0] for r in rows if c in r[2]]
    lack = [r[0] for r in rows if c not in r[2]]
    always = ~0
    for m in have:
        always &= m
    never = 0
    for m in lack:
        never |= m
    bits = [b for b in range(64) if (always >> b) & 1 and not (never >> b) & 1]
    print("%-28s n=%4d  bits only-with-it: %s" % (c, len(have), bits))
# does popcount(mask) == numModifiers?
ok = sum(1 for r in rows if bin(r[0]).count("1") == r[1])
print("popcount(mask)==numModifiers in %d of %d" % (ok, len(rows)))
