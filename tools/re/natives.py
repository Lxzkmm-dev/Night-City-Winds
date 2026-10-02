"""Resolve script native names to their native thunk via the static registration pattern:
   lea rcx,"Name" ; call CName ; ... lea rdx, <thunk> ; ... call rtti::NativeMemberFunction ctor (003CDF14)
                                                    or rtti::NativeGlobalFunction ctor (00C896E4)
Then report which buffered-state IDs (writer 00240998 / reader 0023F168 via 008E763C-like helpers) each thunk uses.
Usage: natives.py Name1 Name2 ...
"""
import sys, struct
import capstone
from capstone import x86
from xref import find_string_rvas, refs_to, func_of, tbytes, TEXT_VA, hashes
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
CTORS = {0x003CDF14: "member", 0x00C896E4: "global"}
BASE = 0

def insns(b, e):
    return list(md.disasm(tbytes[b - TEXT_VA:e - TEXT_VA], b))

def thunks_for(name):
    out = []
    for sr in find_string_rvas(name):
        for r in refs_to(sr):
            fn = func_of(r)
            if not fn:
                continue
            ins = insns(*fn)
            last_lea = None
            for x in ins:
                if x.mnemonic == "lea" and x.operands[1].type == x86.X86_OP_MEM and x.operands[1].mem.base == x86.X86_REG_RIP:
                    t = x.address + x.size + x.operands[1].mem.disp
                    if TEXT_VA <= t < TEXT_VA + len(tbytes):
                        last_lea = t
                if x.mnemonic == "call" and x.operands[0].type == x86.X86_OP_IMM and x.operands[0].imm in CTORS:
                    out.append((fn[0], last_lea, CTORS[x.operands[0].imm]))
                    break
    return out

def calls_in(fn_rva, depth=1):
    fn = func_of(fn_rva)
    if not fn:
        return []
    res = []
    ins = insns(*fn)
    for i, x in enumerate(ins):
        if x.mnemonic in ("call", "jmp") and x.operands and x.operands[0].type == x86.X86_OP_IMM:
            t = x.operands[0].imm
            if not (TEXT_VA <= t < TEXT_VA + len(tbytes)):
                continue
            # collect immediate stores to [rsp+0x20..0x40] just before the call
            imms = []
            for y in ins[max(0, i - 14):i]:
                if y.mnemonic == "mov" and len(y.operands) == 2 and y.operands[0].type == x86.X86_OP_MEM and y.operands[0].mem.base == x86.X86_REG_RSP and y.operands[1].type == x86.X86_OP_IMM:
                    imms.append("[rsp+%X]=%X" % (y.operands[0].mem.disp, y.operands[1].imm))
            h = hashes.get(t)
            res.append("%s %08X hash=%s %s" % (x.mnemonic, t, h[0] if h else "-", " ".join(imms)))
    return res

if __name__ == "__main__":
    for n in sys.argv[1:]:
        for reg, th, kind in thunks_for(n):
            h = hashes.get(th) if th else None
            print("== %s: reg %08X -> %s thunk %08X hash=%s" % (n, reg, kind, th or 0, h[0] if h else "-"))
            if th:
                for c in calls_in(th):
                    print("     " + c)
