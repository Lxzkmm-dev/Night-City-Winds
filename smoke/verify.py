import base64, glob, json, sys

for f in glob.glob(sys.argv[1] + r"\**\*.json", recursive=True):
    d = json.load(open(f, encoding="utf-8"))
    print(f.split("\\")[-1])
    for h in d["Data"]["RootChunk"]["emitters"]:
        e = h["Data"]
        bd = e["renderResourceBlob"]["Data"]
        info = bd["header"]["emitterInfo"]
        raw = base64.b64decode(bd["updaterData"]["data"]["Bytes"])
        mods = [((m or {}).get("Data") or {}).get("$type", "").replace("CParticle", "") for m in e["modules"]]
        print("  %-22s wind %.2f blob %.2f mask %s nmod %s tail %s last %s" % (
            e["editorName"], e["windInfluence"], info["windInfluence"], hex(int(info["modifierSetMask"])),
            info["numModifiers"], raw[-12:].hex(), mods[-1]))
