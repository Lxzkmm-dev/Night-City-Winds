"""Disassemble a function of Cyberpunk2077.exe (by RVA) with capstone, annotating:
- call/jmp targets with their address-library hash,
- RIP-relative refs with strings or data RVAs.
Usage: dis.py <rva_hex> [--max N]
"""
import sys, struct
import capstone
from capstone import x86
from xref import p, d, tbytes, TEXT_VA, TEXT_VSZ, func_of, hashes
from fn import cstr_at

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

def hname(t):
    h = hashes.get(t)
    if not h:
        return ""
    return " ; hash=%s%s" % (h[0], (" " + h[1]) if h[1] else "")

def dis(start, end=None, maxn=4000):
    fn = func_of(start)
    if end is None:
        end = fn[1] if fn and fn[0] == start else start + 0x200
    code = tbytes[start - TEXT_VA:end - TEXT_VA]
    base = 0x140000000
    out = []
    for ins in md.disasm(code, base + start):
        rva = ins.address - base
        s = "%08X  %-8s %s" % (rva, ins.mnemonic, ins.op_str)
        note = ""
        if ins.mnemonic in ("call", "jmp") or ins.mnemonic.startswith("j"):
            if ins.operands and ins.operands[0].type == x86.X86_OP_IMM:
                t = ins.operands[0].imm - base
                s = "%08X  %-8s %08X" % (rva, ins.mnemonic, t)
                note = hname(t)
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                t = ins.address + ins.size + op.mem.disp - base
                cs = cstr_at(t)
                note += ' ; [%08X]%s' % (t, (' "%s"' % cs) if cs else hname(t))
        out.append(s + note)
        if len(out) >= maxn:
            break
    print("\n".join(out))

if __name__ == "__main__":
    a = int(sys.argv[1], 16)
    e = int(sys.argv[2], 16) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else None
    print("== %08X%s" % (a, hname(a)))
    dis(a, e)
