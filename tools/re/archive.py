"""Read-only reader for Cyberpunk 2077 .archive (RDAR) files, using the game's own Oodle DLL.

Usage:
  archive.py <file.archive> list                  file count, sizes
  archive.py <file.archive> grep <regex>          decompress every file, print regex matches (ASCII)
  archive.py <file.archive> dump <outdir> [regex] write every file (or those whose bytes match) as <hash>.bin
"""
import ctypes, os, re, struct, sys

OODLE = r"G:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\oo2ext_7_win64.dll"
_oodle = None


def oodle():
    global _oodle
    if _oodle is None:
        _oodle = ctypes.WinDLL(OODLE)
        f = _oodle.OodleLZ_Decompress
        f.restype = ctypes.c_int64
        f.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64, ctypes.c_int, ctypes.c_int,
                      ctypes.c_int, ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                      ctypes.c_int64, ctypes.c_int]
    return _oodle


def kraken(blob, size):
    out = ctypes.create_string_buffer(size)
    n = oodle().OodleLZ_Decompress(blob, len(blob), out, size, 1, 0, 0, None, 0, None, None, None, 0, 3)
    if n != size:
        raise ValueError("oodle returned %d, expected %d" % (n, size))
    return out.raw


class Archive:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        magic, ver, idx_pos, idx_size = struct.unpack_from("<IIQI", self.d, 0)
        if magic != 0x52414452:  # 'RDAR'
            raise ValueError("not an RDAR archive")
        o = idx_pos
        ft_off, ft_size, crc, n_files, n_segs, n_deps = struct.unpack_from("<IIQIII", self.d, o)
        o += 28
        self.files = []
        for i in range(n_files):
            h, ts, n_inline, s0, s1, d0, d1 = struct.unpack_from("<QqIIIII", self.d, o)
            self.files.append((h, s0, s1))
            o += 56
        self.segs = []
        for i in range(n_segs):
            self.segs.append(struct.unpack_from("<QII", self.d, o))
            o += 16

    def segment(self, i):
        off, zsize, size = self.segs[i]
        raw = self.d[off:off + zsize]
        if zsize == size:
            return raw
        if raw[:4] == b"KARK":
            return kraken(raw[8:], struct.unpack_from("<I", raw, 4)[0])
        return kraken(raw, size)

    def file(self, k):
        h, s0, s1 = self.files[k]
        return h, b"".join(self.segment(i) for i in range(s0, s1))


if __name__ == "__main__":
    a = Archive(sys.argv[1])
    mode = sys.argv[2]
    if mode == "list":
        print(len(a.files), "files,", len(a.segs), "segments")
    elif mode == "grep":
        rx = re.compile(sys.argv[3].encode())
        for k in range(len(a.files)):
            h, data = a.file(k)
            hits = sorted(set(m.group().decode("latin1") for m in rx.finditer(data)))
            if hits:
                print("%016X %8d  %s" % (h, len(data), " ".join(hits)))
    elif mode == "dump":
        outdir = sys.argv[3]
        rx = re.compile(sys.argv[4].encode()) if len(sys.argv) > 4 else None
        os.makedirs(outdir, exist_ok=True)
        for k in range(len(a.files)):
            h, data = a.file(k)
            if rx is None or rx.search(data):
                open(os.path.join(outdir, "%016X.bin" % h), "wb").write(data)
