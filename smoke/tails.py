"""Print the tail of the cooked updater buffer of emitters, with their Drag module settings, to
locate the drag record. Usage: tails.py <corpus dir> <max>"""
import base64, glob, json, os, struct, sys

root, limit = sys.argv[1], int(sys.argv[2])
shown = 0
for f in sorted(glob.glob(os.path.join(root, "**", "*.json"), recursive=True)):
    d = json.load(open(f, encoding="utf-8"))
    for h in d["Data"]["RootChunk"].get("emitters") or []:
        e = (h or {}).get("Data") or {}
        blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
        info = (blob.get("header") or {}).get("emitterInfo") or {}
        mask = int(info.get("modifierSetMask") or 0)
        drag = None
        for m in e.get("modules") or []:
            md = (m or {}).get("Data") or {}
            if md.get("$type") == "CParticleModificatorDrag":
                drag = md
        if not (mask >> 32) & 1 or drag is None:
            continue
        raw = base64.b64decode(((blob.get("updaterData") or {}).get("data") or {}).get("Bytes") or "")
        dc = (drag.get("dragCoefficient") or {}).get("Data") or {}
        desc = dc.get("$type", "")
        if "Const" in desc:
            desc += " value=%s" % dc.get("value")
        elif "Curve" in desc:
            els = ((dc.get("curves") or {}).get("Elements") or [])
            desc += " keys=%s" % [(round(x.get("point", 0), 3), round(x.get("value", 0), 3)) for x in els]
        print("=" * 100)
        print(os.path.basename(f), e.get("editorName"), "| drag scale", drag.get("scale"), "|", desc)
        tail = raw[-100:]
        print("   tail hex:", tail.hex())
        fl = [round(x, 4) for x in struct.unpack_from("<%df" % (len(tail) // 4), tail[len(tail) % 4:])]
        print("   tail as floats (aligned to end):", fl)
        shown += 1
        if shown >= limit:
            sys.exit(0)
