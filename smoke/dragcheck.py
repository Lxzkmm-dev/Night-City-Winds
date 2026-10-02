"""For emitters with an enabled Drag module, test hypotheses for how the drag is cooked:
the last 12 bytes of the updater buffer == record 02 00 0C 00 + (a, b), with
  (a, b) = (value*scale, value*scale) for a const coefficient, (start*scale, end*scale) for start/end.
Also report the highest mask bit, to see whether drag is always last."""
import base64, glob, json, os, struct, sys
from collections import Counter

root = sys.argv[1]
stats = Counter()
examples = []
for f in glob.glob(os.path.join(root, "**", "*.json"), recursive=True):
    d = json.load(open(f, encoding="utf-8"))
    for h in d["Data"]["RootChunk"].get("emitters") or []:
        e = (h or {}).get("Data") or {}
        blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
        info = (blob.get("header") or {}).get("emitterInfo") or {}
        mask = int(info.get("modifierSetMask") or 0)
        if not (mask >> 32) & 1:
            continue
        drag = next((((m or {}).get("Data") or {}) for m in e.get("modules") or []
                     if ((m or {}).get("Data") or {}).get("$type") == "CParticleModificatorDrag"), None)
        if not drag:
            stats["bit32 without drag module"] += 1
            continue
        raw = base64.b64decode(((blob.get("updaterData") or {}).get("data") or {}).get("Bytes") or "")
        top = mask.bit_length() - 1
        stats["highest bit %d" % top] += 1
        dc = (drag.get("dragCoefficient") or {}).get("Data") or {}
        s = float(drag.get("scale") or 1.0)
        t = dc.get("$type", "")
        last = raw[-12:]
        hdr = last[:4].hex()
        a, b = struct.unpack_from("<2f", last, 4) if len(last) == 12 else (None, None)
        if t.endswith("FloatConst"):
            v = float(dc.get("value") or 0)
            exp = [(v * s, v * s), (v, v)]
        elif t.endswith("FloatStartEnd"):
            exp = [(float(dc.get("start") or 0) * s, float(dc.get("end") or 0) * s), (float(dc.get("start") or 0), float(dc.get("end") or 0))]
        else:
            exp = []
        match = "none"
        for i, (x, y) in enumerate(exp):
            if hdr == "02000c00" and abs(a - x) < 1e-3 and abs(b - y) < 1e-3:
                match = "scaled" if i == 0 else "unscaled"
                break
        stats["%s / %s" % (t.replace("CEvaluator", ""), match)] += 1
        if match == "none" and len(examples) < 6:
            examples.append((os.path.basename(f), e.get("editorName"), t, dc.get("value"), dc.get("start"), dc.get("end"), s, hdr, a, b, top))
for k, v in sorted(stats.items()):
    print("%-50s %d" % (k, v))
for x in examples:
    print("MISS", x)
