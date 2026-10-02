"""Find direct callers (E8 rel32) of a target RVA and show the ~12 instructions before each call.
Usage: callers.py <target_rva_hex> [context]"""
import sys, struct, re
import capstone
from xref import tbytes, TEXT_VA, func_of, hashes
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
tgt = int(sys.argv[1], 16)
ctx = int(sys.argv[2]) if len(sys.argv) > 2 else 12
res = []
for m in re.finditer(rb"\xE8", tbytes):
    i = m.start()
    if i + 5 > len(tbytes):
        break
    if TEXT_VA + i + 5 + struct.unpack_from("<i", tbytes, i + 1)[0] == tgt:
        res.append(TEXT_VA + i)
print("callers:", len(res))
for at in res:
    fn = func_of(at)
    h = hashes.get(fn[0]) if fn else None
    print("== call @%08X in %08X hash=%s" % (at, fn[0] if fn else 0, h[0] if h else "-"))
    if fn:
        code = tbytes[fn[0] - TEXT_VA:at + 5 - TEXT_VA]
        ins = list(md.disasm(code, fn[0]))
        for x in ins[-ctx:]:
            print("   %08X %s %s" % (x.address, x.mnemonic, x.op_str))
