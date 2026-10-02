"""Print the VelocityOverLife curve keys of each emitter in an effect JSON.
Usage: vol.py <effect.json> [emitter name substring]"""
import json, sys

d = json.load(open(sys.argv[1], encoding="utf-8"))
want = sys.argv[2] if len(sys.argv) > 2 else ""
for h in d["Data"]["RootChunk"].get("emitters") or []:
    e = (h or {}).get("Data") or {}
    if want not in str(e.get("editorName")):
        continue
    for m in e.get("modules") or []:
        md = (m or {}).get("Data") or {}
        if md.get("$type") != "CParticleModificatorVelocityOverLife":
            continue
        v = (md.get("velocity") or {}).get("Data") or {}
        print(e.get("editorName"), "| modulate", md.get("modulate"), "| scale", md.get("scale"), "| enabled", md.get("isEnabled", 1), "|", v.get("$type"))
        print(json.dumps({k: x for k, x in v.items() if k != "$type"})[:1500])
