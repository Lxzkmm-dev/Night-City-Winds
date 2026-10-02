import re, sys, json
src = r"F:\Cyberpunk 2077\mods\Codeware\red4ext\plugins\Codeware\Scripts\Codeware.Global.reds"
txt = open(src, encoding="utf-8").read().splitlines()
types = {}
cur = None
hdr = re.compile(r'^(?:public )?(?:abstract )?(?:final )?(?:native |importonly )*(class|struct|enum)\s+(\w+)(?:\s+extends\s+(\w+))?')
for i, line in enumerate(txt):
    m = hdr.match(line)
    if m:
        cur = {"kind": m.group(1), "name": m.group(2), "base": m.group(3), "line": i + 1, "body": []}
        types[m.group(2)] = cur
        if line.rstrip().endswith("{}"):
            cur = None
        continue
    if cur is not None:
        if line.startswith("}"):
            cur = None
        else:
            cur["body"].append(line.strip())
json.dump(types, open(sys.argv[1], "w"), indent=0)
print(len(types))
