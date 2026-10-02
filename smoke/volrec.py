"""Check where VelocityOverLife's cooked record sits: for every emitter with an enabled VOL,
the lowest modifier bit, and the first modifier record's header (kind, size) against the
evaluator type and sample count. Usage: volrec.py <dir of effect JSONs>"""
import base64, glob, json, os, struct, sys
from collections import Counter

c = Counter()
ex = {}
for f in glob.glob(os.path.join(sys.argv[1], "**", "*.json"), recursive=True):
    d = json.load(open(f, encoding="utf-8"))
    for h in d["Data"]["RootChunk"].get("emitters") or []:
        e = (h or {}).get("Data") or {}
        vol = next((((m or {}).get("Data") or {}) for m in e.get("modules") or []
                    if ((m or {}).get("Data") or {}).get("$type") == "CParticleModificatorVelocityOverLife"
                    and ((m or {}).get("Data") or {}).get("isEnabled", 1)), None)
        blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
        info = (blob.get("header") or {}).get("emitterInfo") or {}
        upd = blob.get("updaterData") or {}
        mask = int(info.get("modifierSetMask") or 0)
        if not vol:
            if mask & 2:
                c["bit1 set without enabled VOL"] += 1
            continue
        if not mask & 2:
            c["VOL enabled but bit1 clear"] += 1
            continue
        if mask & 1:
            c["bit0 also set"] += 1
            continue
        raw = base64.b64decode(((upd.get("data") or {}).get("Bytes")) or "")
        off = int(upd.get("modifOffset") or 0)
        kind, size = raw[off], struct.unpack_from("<H", raw, off + 2)[0]
        ev = (vol.get("velocity") or {}).get("Data") or {}
        t = ev.get("$type", "").replace("CEvaluator", "")
        n = ev.get("numberOfCurveSamples")
        key = "%s samples=%s modulate=%s -> kind %d size %d" % (t, n, vol.get("modulate"), kind, size)
        c[key] += 1
        ex.setdefault(key, (os.path.basename(f), e.get("editorName")))
for k, v in sorted(c.items(), key=lambda t: -t[1]):
    print("%5d  %s   e.g. %s" % (v, k, ex.get(k, "")))
