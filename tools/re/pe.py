"""Minimal read-only PE parser: sections, exports, imports. Usage: pe.py <file> [exports|imports|sections]"""
import struct, sys

class PE:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        e = struct.unpack_from("<I", d, 0x3C)[0]
        self.nsec = struct.unpack_from("<H", d, e + 6)[0]
        optsz = struct.unpack_from("<H", d, e + 20)[0]
        self.opt = e + 24
        self.base = struct.unpack_from("<Q", d, self.opt + 24)[0]
        self.dirs = self.opt + 112
        self.secs = []
        so = self.opt + optsz
        for i in range(self.nsec):
            name = d[so:so + 8].rstrip(b"\0").decode()
            vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, so + 8)
            self.secs.append((name, va, vsz, rptr, rsz))
            so += 40

    def dir(self, i):
        return struct.unpack_from("<II", self.d, self.dirs + i * 8)

    def off(self, rva):
        for n, va, vsz, rp, rs in self.secs:
            if va <= rva < va + max(vsz, rs):
                return rva - va + rp
        return None

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)].decode("latin1")

    def exports(self):
        rva, sz = self.dir(0)
        if not rva:
            return []
        o = self.off(rva)
        nfun, nnames, afun, anames, aord = struct.unpack_from("<IIIII", self.d, o + 20)
        ordbase = struct.unpack_from("<I", self.d, o + 16)[0]
        res = []
        for i in range(nnames):
            nrva = struct.unpack_from("<I", self.d, self.off(anames) + i * 4)[0]
            ordi = struct.unpack_from("<H", self.d, self.off(aord) + i * 2)[0]
            frva = struct.unpack_from("<I", self.d, self.off(afun) + ordi * 4)[0]
            res.append((frva, ordi + ordbase, self.cstr(nrva)))
        return res

    def imports(self):
        rva, sz = self.dir(1)
        o = self.off(rva)
        res = []
        while True:
            ilt, ts, fc, name, iat = struct.unpack_from("<IIIII", self.d, o)
            if not name:
                break
            dll = self.cstr(name)
            t = self.off(ilt or iat)
            j = 0
            while True:
                v = struct.unpack_from("<Q", self.d, t + j * 8)[0]
                if not v:
                    break
                fn = "#%d" % (v & 0xFFFF) if v >> 63 else self.cstr((v & 0x7FFFFFFF) + 2)
                res.append((dll, fn, iat + j * 8))
                j += 1
            o += 20
        return res

if __name__ == "__main__":
    p = PE(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) > 2 else "sections"
    if mode == "sections":
        print("imagebase %X" % p.base)
        for s in p.secs:
            print("%-8s va=%08X vsz=%08X raw=%08X rsz=%08X" % s)
    elif mode == "exports":
        for r in p.exports():
            print("%08X %5d %s" % r)
    elif mode == "imports":
        for dll, fn, iat in p.imports():
            print("%s!%s iat=%08X" % (dll, fn, iat))
