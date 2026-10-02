"""Check every emitter in a folder of effect JSONs: popcount(modifierSetMask) == numModifiers ==
number of enabled Modificator modules, and, for emitters with Acceleration first, that its two
records parse. Run on the patched build before packing. Usage: consistency.py <dir>"""
import base64, glob, json, os, struct, sys
from collections import Counter

c = Counter()
bad = []
for f in glob.glob(os.path.join(sys.argv[1], "**", "*.json"), recursive=True):
    d = json.load(open(f, encoding="utf-8"))
    for h in d["Data"]["RootChunk"].get("emitters") or []:
        e = (h or {}).get("Data") or {}
        blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
        info = (blob.get("header") or {}).get("emitterInfo") or {}
        if "modifierSetMask" not in info:
            continue
        mask = int(info["modifierSetMask"])
        n = int(info.get("numModifiers") or 0)
        # Collision has no mask bit (it lives in updaterData's collision fields)
        mods = sum(1 for m in e.get("modules") or []
                   if ((m or {}).get("Data") or {}).get("$type", "").startswith("CParticleModificator")
                   and ((m or {}).get("Data") or {}).get("$type") != "CParticleModificatorCollision"
                   and ((m or {}).get("Data") or {}).get("isEnabled", 1))
        ok = bin(mask).count("1") == n == mods
        if ok and mask & 0b1111 == 0b1000:
            upd = blob.get("updaterData") or {}
            raw = base64.b64decode(((upd.get("data") or {}).get("Bytes")) or "")
            off = int(upd.get("modifOffset") or 0)
            k1, s1 = struct.unpack_from("<HH", raw, off)
            k2, s2 = struct.unpack_from("<HH", raw, off + s1)
            ok = (s1 == 28 if k1 == 2 else s1 == 4 + 12 * k1) and (s2 == 12 if k2 == 2 else s2 == 4 + 4 * k2)
        c["ok" if ok else "MISMATCH"] += 1
        if not ok:
            bad.append((os.path.basename(f), e.get("editorName"), hex(mask), n, mods))
print(dict(c))
for b in bad[:20]:
    print("  ", b)
