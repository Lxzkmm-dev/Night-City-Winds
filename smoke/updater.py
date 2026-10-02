"""Decode the cooked particle updater buffer (rendRenderParticleUpdaterData.data) record by record."""
import base64, json, struct, sys

def emitters(p):
    return json.load(open(p, encoding="utf-8"))["Data"]["RootChunk"]["emitters"]

def dump(buf, modif_offset):
    print("   %d bytes, modifOffset %d" % (len(buf), modif_offset))
    i = 0
    n = 0
    while i < len(buf):
        tag = "MOD" if i >= modif_offset else "ini"
        if i + 4 > len(buf):
            print("   @%3d %s tail %s" % (i, tag, buf[i:].hex()))
            break
        a, b, size = buf[i], buf[i + 1], struct.unpack_from("<H", buf, i + 2)[0]
        if size < 4 or i + size > len(buf):
            # not a record header: show one byte and move on (a separator/flag byte)
            print("   @%3d %s byte %02x" % (i, tag, buf[i]))
            i += 1
            continue
        body = buf[i + 4:i + size]
        floats = [round(x, 4) for x in struct.unpack_from("<%df" % (len(body) // 4), body)] if len(body) >= 4 else []
        print("   @%3d %s rec type %d/%d size %3d floats %s" % (i, tag, a, b, size, floats[:12]))
        i += size
        n += 1

for path in sys.argv[1:]:
    for h in emitters(path):
        e = h["Data"]
        mods = [((m or {}).get("Data") or {}).get("$type", "").replace("CParticle", "") for m in e.get("modules") or []]
        blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
        upd = blob.get("updaterData") or {}
        info = (blob.get("header") or {}).get("emitterInfo") or {}
        raw = base64.b64decode((upd.get("data") or {}).get("Bytes") or "")
        print("=" * 100)
        print(path.split("\\")[-1], e.get("editorName"), "mask 0x%X" % int(info.get("modifierSetMask") or 0),
              "nmod", info.get("numModifiers"), "init 0x%X" % int(info.get("initializerSetMask") or 0), "ninit", info.get("numInitializers"))
        print("  ", [m for m in mods])
        dump(raw, int(upd.get("modifOffset") or 0))
