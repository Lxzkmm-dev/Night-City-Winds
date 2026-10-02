import json, sys

d = json.load(open(sys.argv[1], encoding="utf-8"))
print(list(d.keys()))
root = d["Data"]["RootChunk"]
print(root.get("$type"), list(root.keys()))
em = root["emitters"][0]
print("emitter handle keys:", list(em.keys()))
inner = em.get("Data") or em
print({k: (v if not isinstance(v, (dict, list)) else type(v).__name__) for k, v in inner.items()})
print("windInfluence:", inner.get("windInfluence"), "local:", inner.get("keepSimulationLocal"))
print("material:", json.dumps(inner.get("material"))[:300])
blob = inner.get("renderResourceBlob")
print("blob:", json.dumps(blob)[:200])
b = (blob or {}).get("Data") or {}
print("blob keys:", list(b.keys()))
h = b.get("header") or {}
print("header keys:", list(h.keys()) if isinstance(h, dict) else type(h))
ei = h.get("emitterInfo") if isinstance(h, dict) else None
print("emitterInfo:", json.dumps(ei)[:800] if ei else None)
