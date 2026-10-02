"""Read-only static xref tool for Cyberpunk2077.exe (no disassembler needed).

- Finds RIP-relative LEA/MOV references (48/4C 8D|8B modrm[mod=00,rm=101] disp32) to given strings.
- Maps each reference to its containing function via .pdata RUNTIME_FUNCTION entries.
- Maps the function start RVA to its address-library hash (cyberpunk2077_addresses.json).
- For native-registration sites, lists other LEA targets in the same function that point into .text
  (candidate native implementations registered next to the name).

Usage: xref.py "<string1>" "<string2>" ...
"""
import struct, sys, bisect, os, re
from pe import PE

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = r"G:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe"
p = PE(EXE)
d = p.d
secs = {s[0]: s for s in p.secs}
text = secs[".text"]
TEXT_VA, TEXT_VSZ, TEXT_RAW = text[1], text[2], text[3]
tbytes = d[TEXT_RAW:TEXT_RAW + text[4]]

# .pdata -> function ranges
pd_rva, pd_sz = p.dir(3)
po = p.off(pd_rva)
funcs = []
for i in range(pd_sz // 12):
    b, e, u = struct.unpack_from("<III", d, po + i * 12)
    funcs.append((b, e))
funcs.sort()
fstarts = [f[0] for f in funcs]

def func_of(rva):
    i = bisect.bisect_right(fstarts, rva) - 1
    if i >= 0 and funcs[i][0] <= rva < funcs[i][1]:
        # chained unwind info can split a function; walk back while previous chunk ends where this starts
        return funcs[i]
    return None

# address library
hashes = {}
for line in open(os.path.join(HERE, "symbols.tsv"), encoding="utf-8"):
    h, rva, sym = line.rstrip("\n").split("\t")
    hashes[int(rva, 16)] = (h, sym)

def rva_of_file(off):
    for n, va, vsz, rp, rs in p.secs:
        if rp <= off < rp + rs:
            return off - rp + va

def find_string_rvas(s):
    out = []
    pat = s.encode() + b"\0"
    start = 0
    while True:
        i = d.find(pat, start)
        if i < 0:
            break
        if i == 0 or d[i - 1] == 0 or True:
            # require the string to start at a boundary (previous byte NUL) for exact match
            if d[i - 1] == 0:
                out.append(rva_of_file(i))
        start = i + 1
    return out

# Precompute all RIP-relative lea/mov targets once (cached)
CACHE = os.path.join(HERE, "riprefs.bin")
def build_refs():
    import array
    refs = array.array("Q")
    rx = re.compile(rb"[\x48\x4C][\x8D\x8B][\x05\x0D\x15\x1D\x25\x2D\x35\x3D]", re.S)
    n = len(tbytes)
    for m in rx.finditer(tbytes):
        i = m.start()
        if i + 7 > n:
            continue
        disp = struct.unpack_from("<i", tbytes, i + 3)[0]
        insn_rva = TEXT_VA + i
        tgt = insn_rva + 7 + disp
        refs.append((tgt << 32) | insn_rva)
    refs = sorted(refs)
    with open(CACHE, "wb") as f:
        f.write(array.array("Q", refs).tobytes())

def load_refs():
    import array
    if not os.path.exists(CACHE):
        build_refs()
    a = array.array("Q")
    a.frombytes(open(CACHE, "rb").read())
    return a

REFS = load_refs()
KEYS = [x >> 32 for x in REFS]

def refs_to(tgt):
    i = bisect.bisect_left(KEYS, tgt)
    out = []
    while i < len(KEYS) and KEYS[i] == tgt:
        out.append(REFS[i] & 0xFFFFFFFF)
        i += 1
    return out

def text_targets_in(fn):
    """LEA targets inside function fn that point into .text (candidate function pointers)."""
    b, e = fn
    res = []
    seg = tbytes[b - TEXT_VA:e - TEXT_VA]
    for m in re.finditer(rb"[\x48\x4C]\x8D[\x05\x0D\x15\x1D\x25\x2D\x35\x3D]", seg):
        i = m.start()
        if i + 7 > len(seg):
            continue
        disp = struct.unpack_from("<i", seg, i + 3)[0]
        tgt = b + i + 7 + disp
        if TEXT_VA <= tgt < TEXT_VA + TEXT_VSZ:
            res.append((b + i, tgt))
    return res

def describe(rva):
    fn = func_of(rva)
    if not fn:
        return "  ref @%08X (no pdata function)" % rva
    h = hashes.get(fn[0])
    return "  ref @%08X in func %08X-%08X  hash=%s %s" % (rva, fn[0], fn[1], h[0] if h else "-", h[1] if h and h[1] else "")

if __name__ == "__main__":
    near = "--near" in sys.argv
    for s in [a for a in sys.argv[1:] if not a.startswith("--")]:
        srvas = find_string_rvas(s)
        print("== %r at %s" % (s, ",".join("%08X" % r for r in srvas)))
        for sr in srvas:
            for r in refs_to(sr):
                print(describe(r))
                if near:
                    fn = func_of(r)
                    if fn:
                        # show text pointers loaded within +-0x40 bytes of the string ref
                        for at, tgt in text_targets_in(fn):
                            if abs(at - r) <= 0x30:
                                h = hashes.get(tgt)
                                print("      lea text %08X @%08X hash=%s" % (tgt, at, h[0] if h else "-"))
