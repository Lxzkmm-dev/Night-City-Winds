"""Generic string -> referencing function -> vtables containing that function, for any x64 DLL/EXE.
Usage: dllxref.py <pe_path> <substring> [--dis N]
Prints each string containing <substring>, the functions (by .pdata) that reference it via RIP-relative
LEA/MOV, and every 8-byte slot in .rdata that points at those functions (vtable candidates, with slot index
relative to the run of code pointers around it)."""
import sys, struct, bisect, re
import pefile, capstone

path, needle = sys.argv[1], sys.argv[2].encode()
ndis = int(sys.argv[sys.argv.index("--dis") + 1]) if "--dis" in sys.argv else 0
pe = pefile.PE(path, fast_load=False)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
tva, tsz = text.VirtualAddress, text.Misc_VirtualSize
funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [f[0] for f in funcs]
def func_of(r):
    i = bisect.bisect_right(starts, r) - 1
    return funcs[i] if i >= 0 and funcs[i][0] <= r < funcs[i][1] else None
exports = {}
if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
    for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if s.name:
            exports[s.address] = s.name.decode()

hits = [m.start() for m in re.finditer(re.escape(needle), data)]
strs = set()
for h in hits:
    s = h
    while s > 0 and 32 <= data[s - 1] < 127:
        s -= 1
    strs.add(s)
code = data[tva:tva + tsz]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
rdata = [s for s in pe.sections if s.Name.startswith(b".rdata")][0]
rd = data[rdata.VirtualAddress:rdata.VirtualAddress + rdata.Misc_VirtualSize]
for s in sorted(strs):
    e = data.index(b"\0", s)
    print("== %08X %r" % (s, data[s:e][:160]))
    fns = set()
    for m in re.finditer(rb"[\x48\x4C][\x8D\x8B][\x05\x0D\x15\x1D\x25\x2D\x35\x3D]", code):
        i = m.start()
        if i + 7 <= len(code) and tva + i + 7 + struct.unpack_from("<i", code, i + 3)[0] == s:
            f = func_of(tva + i)
            print("   ref @%08X in func %s" % (tva + i, "%08X-%08X" % f if f else "?"))
            if f:
                fns.add(f[0])
    for f in fns:
        tgt = struct.pack("<Q", base + f)
        for m in re.finditer(re.escape(tgt), rd):
            off = m.start()
            # find run of code pointers around it
            k = off
            while k >= 8:
                v = struct.unpack_from("<Q", rd, k - 8)[0] - base
                if not (tva <= v < tva + tsz):
                    break
                k -= 8
            print("   func %08X%s in table @%08X slot %d (table starts @%08X)" % (f, " [" + exports[f] + "]" if f in exports else "", rdata.VirtualAddress + off, (off - k) // 8, rdata.VirtualAddress + k))
        if ndis:
            fb, fe = func_of(f)
            for x in list(md.disasm(data[fb:fe], base + fb))[:ndis]:
                print("      %08X %s %s" % (x.address - base, x.mnemonic, x.op_str))
