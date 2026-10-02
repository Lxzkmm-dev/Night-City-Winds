"""Survey emitters: of those with wind influence, how many have an enabled VelocityOverLife
(and with modulate on/off) and an enabled Drag, to judge how designers combined them.
Usage: survey.py <dir of effect JSONs> [--list]"""
import glob, json, os, sys
from collections import Counter

root = sys.argv[1]
c = Counter()
rows = []
for f in glob.glob(os.path.join(root, "**", "*.json"), recursive=True):
    d = json.load(open(f, encoding="utf-8"))
    for h in d["Data"]["RootChunk"].get("emitters") or []:
        e = (h or {}).get("Data") or {}
        w = float(e.get("windInfluence") or 0)
        mods = {}
        for m in e.get("modules") or []:
            md = (m or {}).get("Data") or {}
            if md.get("isEnabled", 1):
                mods.setdefault(md.get("$type", "").replace("CParticleModificator", "").replace("CParticleInitializer", "I:"), md)
        vol = mods.get("VelocityOverLife")
        drag = mods.get("Drag")
        key = "wind>0" if w > 0 else "wind=0"
        c[key] += 1
        if vol:
            c[key + " +VOL(modulate=%s)" % vol.get("modulate")] += 1
        if drag:
            c[key + " +Drag"] += 1
        if vol and drag:
            c[key + " +VOL+Drag"] += 1
        if w > 0:
            rows.append((w, os.path.basename(f), e.get("editorName"), bool(vol), vol.get("modulate") if vol else None,
                         float(drag.get("scale") or 0) if drag else None))
for k, v in sorted(c.items()):
    print("%-40s %d" % (k, v))
print("wind influence values (designer):", Counter(round(r[0], 2) for r in rows).most_common(12))
if "--list" in sys.argv:
    for r in sorted(rows):
        print("  %.2f  %-60s %-30s VOL=%s mod=%s drag=%s" % r)
