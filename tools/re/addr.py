"""Load cyberpunk2077_addresses.json into a TSV: hash, rva, symbol. Then grep it with addr.py grep <regex>."""
import json, re, sys, os
J = r"G:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\cyberpunk2077_addresses.json"
TSV = os.path.join(os.path.dirname(__file__), "symbols.tsv")
# Section bases from pe.py (va): .text 0x1000 .rdata 0x2A8D000 .data 0x32E6000
SEC = {1: 0x1000, 2: 0x2A8D000, 3: 0x32E6000, 4: 0x4927000, 5: 0x4BE7000}

def build():
    d = json.load(open(J))
    with open(TSV, "w", encoding="utf-8") as o:
        for a in d["Addresses"]:
            s, off = a["offset"].split(":")
            rva = SEC.get(int(s), 0) + int(off, 16)
            o.write("%s\t%08X\t%s\n" % (a["hash"], rva, a.get("symbol", "")))
    print(len(d["Addresses"]), {k: v for k, v in d.items() if k != "Addresses"})

def grep(pat):
    r = re.compile(pat)
    for line in open(TSV, encoding="utf-8"):
        if r.search(line.split("\t", 2)[2]):
            sys.stdout.write(line)

if __name__ == "__main__":
    if sys.argv[1] == "build":
        build()
    else:
        grep(sys.argv[2])
