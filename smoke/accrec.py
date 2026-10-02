"""Find CParticleModificatorAcceleration's mask bit and cooked record: for emitters whose only
modifier bits below Acceleration's are absent, print the module fields and the first modifier
record. Usage: accrec.py <dir of effect JSONs> [max]"""
import base64, glob, json, os, struct, sys
from collections import Counter

root = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 12
shown = 0
sizes = Counter()
for f in glob.glob(os.path.join(root, "**", "*.json"), recursive=True):
    d = json.load(open(f, encoding="utf-8"))
    for h in d["Data"]["RootChunk"].get("emitters") or []:
        e = (h or {}).get("Data") or {}
        acc = next((((m or {}).get("Data") or {}) for m in e.get("modules") or []
                    if ((m or {}).get("Data") or {}).get("$type") == "CParticleModificatorAcceleration"
                    and ((m or {}).get("Data") or {}).get("isEnabled", 1)), None)
        if not acc:
            continue
        blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
        info = (blob.get("header") or {}).get("emitterInfo") or {}
        upd = blob.get("updaterData") or {}
        mask = int(info.get("modifierSetMask") or 0)
        low = mask & 0b111
        if low:  # something before bit 3 too
            sizes["has bits 0-2: %s" % bin(low)] += 1
            continue
        if not mask & 8:
            sizes["ACC enabled but bit 3 clear"] += 1
            continue
        raw = base64.b64decode(((upd.get("data") or {}).get("Bytes")) or "")
        off = int(upd.get("modifOffset") or 0)
        a, b, size = raw[off], raw[off + 1], struct.unpack_from("<H", raw, off + 2)[0]
        ev = (acc.get("acceleration") or {}).get("Data") or {}
        sizes["first rec %d/%d size %d, evaluator %s, worldSpace=%s" % (a, b, size, ev.get("$type"), acc.get("worldSpace"))] += 1
        if shown < limit:
            shown += 1
            print(os.path.basename(f), e.get("editorName"))
            print("   module:", {k: v for k, v in acc.items() if k != "acceleration"})
            print("   accel :", json.dumps(ev)[:300])
            body = raw[off + 4: off + 4 + 64]
            print("   rec @%d: %s | floats %s" % (off, raw[off:off + 4].hex(), [round(x, 4) for x in struct.unpack_from("<%df" % (len(body) // 4), body)]))
for k, v in sizes.most_common():
    print("%5d  %s" % (v, k))
