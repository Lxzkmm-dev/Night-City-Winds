"""List direct calls (E8), jmps (E9 tail), RIP-relative refs (strings/data) inside a function, in order.
Heuristic (byte-pattern based, no disassembler): may show false positives inside other instructions.
Usage: fn.py <func_rva_hex> [more...]"""
import sys, struct, re
from xref import p, d, tbytes, TEXT_VA, TEXT_VSZ, func_of, hashes

def cstr_at(rva):
    o = p.off(rva)
    if o is None:
        return None
    e = d.find(b"\0", o, o + 120)
    if e < 0:
        return None
    s = d[o:e]
    if len(s) >= 3 and all(32 <= c < 127 for c in s):
        return s.decode()
    return None

def dump(start):
    fn = func_of(start)
    if not fn:
        print("no function at %08X" % start); return
    b, e = fn
    h = hashes.get(b)
    print("== func %08X-%08X hash=%s size=%d" % (b, e, h[0] if h else "-", e - b))
    seg = tbytes[b - TEXT_VA:e - TEXT_VA]
    items = []
    for i in range(len(seg) - 5):
        op = seg[i]
        if op == 0xE8:
            tgt = b + i + 5 + struct.unpack_from("<i", seg, i + 1)[0]
            if TEXT_VA <= tgt < TEXT_VA + TEXT_VSZ and func_of(tgt) and func_of(tgt)[0] == tgt:
                hh = hashes.get(tgt)
                items.append((b + i, "call %08X hash=%s" % (tgt, hh[0] if hh else "-")))
        if op in (0x48, 0x4C) and i + 7 <= len(seg) and seg[i + 1] in (0x8D, 0x8B) and (seg[i + 2] & 0xC7) == 0x05:
            tgt = b + i + 7 + struct.unpack_from("<i", seg, i + 3)[0]
            s = cstr_at(tgt)
            if s:
                items.append((b + i, "ref  %08X \"%s\"" % (tgt, s)))
            elif TEXT_VA <= tgt < TEXT_VA + TEXT_VSZ:
                hh = hashes.get(tgt)
                items.append((b + i, "lea  fn %08X hash=%s" % (tgt, hh[0] if hh else "-")))
            else:
                items.append((b + i, "data %08X" % tgt))
        if op == 0xFF and seg[i + 1] in (0x15,):
            tgt = b + i + 6 + struct.unpack_from("<i", seg, i + 2)[0]
            items.append((b + i, "icall [%08X]" % tgt))
    for at, s in items:
        print("  +%04X %s" % (at - b, s))

if __name__ == "__main__":
    for a in sys.argv[1:]:
        dump(int(a, 16))
