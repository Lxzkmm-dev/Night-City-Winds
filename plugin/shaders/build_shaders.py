"""Builds the shader patches the plugin embeds (REDGraph client): foliage wind (0.8.0), smoke (0.9.0).

The patch is the game's own wind-impulse "shapes" compute shader (m_windImpulseVolumeUpdate_Shapes,
HASH b88af93e421f88ee278f2ba00f214d6d) with one edit: where it stores each cell of the 64^3 wind
impulse volume, it also adds a cell of Night City Winds' local wind, read from a 16 x 16 grid in a
constant buffer at b3 (REDGraph leases b3 into the engine's own descriptor table). Everything else
is the vanilla shader's bytecode, so its resource layout and behaviour stay the engine's.

Nothing of CDPR's is kept in the repository: the vanilla bytecode is extracted from the installed
game's shader cache at build time, edited, assembled and validated, and written as a C++ header
into plugin/build/generated (ignored by git).

    python -I build_shaders.py --game "G:\\...\\Cyberpunk 2077" --out ..\\build\\generated

Needs REDGraph's SDK tools (deps/REDGraph/sdk/tools) and a DXC with dxcompiler.dll + dxil.dll
(the Windows SDK's bin/<ver>/x64 works). Standard library only.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
TOOLS = HERE.parent.parent / "deps" / "REDGraph" / "sdk" / "tools"
SHAPES = "b88af93e421f88ee278f2ba00f214d6d"

# --- the edit ------------------------------------------------------------------------------------
# Cell (x, y, z) of the volume is thread (%24, %25, %26); the shader has just summed the impulse
# shapes into (%296..%299), faded them toward the volume's edges by %295, added the previous
# frame's cell (%301..%304) as (%305..%308) and stores that. We add grid[(x >> 2) + (y >> 2) * 16]
# x %295 (the same edge fade): one entry per 2 m x 2 m column, the plugin's wind deviation there.

TYPE_ANCHOR = "%hostlayout.SharedPixelConsts = type"
TYPE_LINE = "%hostlayout.NCWGrid = type { [256 x <4 x float>] }\n"

HANDLE_ANCHOR = ("  %7 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 12, i1 false)"
                 "  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)\n")
HANDLE_LINE = "  %ncw.h = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 1, i32 3, i1 false)\n"

STORE_OLD = ("  call void @dx.op.textureStore.f32(i32 67, %dx.types.Handle %1, i32 %24, i32 %25, i32 %26, "
             "float %305, float %306, float %307, float %308, i8 15)")
STORE_NEW = """  %ncw.ix = lshr i32 %24, 2
  %ncw.iy = lshr i32 %25, 2
  %ncw.iys = shl i32 %ncw.iy, 4
  %ncw.ixy = or i32 %ncw.ix, %ncw.iys
  %ncw.i = and i32 %ncw.ixy, 255
  %ncw.r = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %ncw.h, i32 %ncw.i)
  %ncw.x = extractvalue %dx.types.CBufRet.f32 %ncw.r, 0
  %ncw.y = extractvalue %dx.types.CBufRet.f32 %ncw.r, 1
  %ncw.z = extractvalue %dx.types.CBufRet.f32 %ncw.r, 2
  %ncw.w = extractvalue %dx.types.CBufRet.f32 %ncw.r, 3
  %ncw.fx = fmul fast float %ncw.x, %295
  %ncw.fy = fmul fast float %ncw.y, %295
  %ncw.fz = fmul fast float %ncw.z, %295
  %ncw.fw = fmul fast float %ncw.w, %295
  %ncw.ox = fadd fast float %305, %ncw.fx
  %ncw.oy = fadd fast float %306, %ncw.fy
  %ncw.oz = fadd fast float %307, %ncw.fz
  %ncw.ow = fadd fast float %308, %ncw.fw
  call void @dx.op.textureStore.f32(i32 67, %dx.types.Handle %1, i32 %24, i32 %25, i32 %26, float %ncw.ox, float %ncw.oy, float %ncw.oz, float %ncw.ow, i8 15)"""

CB_LIST_OLD = "!17 = !{!18}\n"
CB_LIST_NEW = "!17 = !{!18, !100}\n"
CB_META = "!100 = !{i32 1, %hostlayout.NCWGrid* undef, !\"\", i32 0, i32 3, i32 1, i32 4096, null}\n"


def replace_once(text, old, new, what):
    if text.count(old) != 1:
        raise SystemExit(f"edit anchor '{what}' found {text.count(old)} times (the game's shader changed?)")
    return text.replace(old, new)


def edit(ll: str) -> str:
    if f"; shader hash: {SHAPES}" not in ll:
        raise SystemExit("not the shapes shader")
    ll = replace_once(ll, TYPE_ANCHOR, TYPE_LINE + TYPE_ANCHOR, "type")
    ll = replace_once(ll, HANDLE_ANCHOR, HANDLE_ANCHOR + HANDLE_LINE, "handle")
    ll = replace_once(ll, STORE_OLD, STORE_NEW, "store")
    ll = replace_once(ll, CB_LIST_OLD, CB_LIST_NEW, "cbuffer list")
    return ll.rstrip("\n") + "\n" + CB_META


# --- 0.9.0: volumetric smoke through the fog's own particle path ----------------------------------
# The volumetric fog's lighting CS comes in pairs: m_volumetricFogLighting[_DistantShadow] reads a
# StructuredBuffer<VolumetricGPUParticle> at t6 (count in the fog constants, cb6 register 16 .z)
# and blends each particle's density and colour into the froxel before lighting it; the
# _NoParticles twins - the only ones 2.31 runs - skip that loop. We register the WITH-particles
# bytecode as the patch of each NoParticles shader, with its particle reads moved to a cbuffer at
# b3 that the plugin fills: register 0 .z = the count (as the engine's, a float), then two
# registers a particle: (x, y, z, radius) and (rgb8 + density8 << 24, falloff8, 0, 0) as uints.
SMOKE_PAIRS = [
    # (engine NoParticles shader the patch replaces, the with-particles shader it is built from, symbol)
    ("81702d32ff55a1141aeb46556d215e3d", "d038ce3d637f0275f0452b265e937e4a", "SmokeRaster"),
    ("61f9ce9cb0eff83f521aadb878669662", "d39d549f9ac175d0f239658582f5c9b3", "SmokeDistantShadow"),
]
SMOKE_MAX = 128
SMOKE_REGS = 272  # 1 + 2 x 128 = 257 registers, rounded up to a multiple of 256 bytes (4352 B)
SMOKE_TYPE = f"%hostlayout.NCWSmoke = type {{ [{SMOKE_REGS} x <4 x float>] }}\n"

RE_PARTICLE_META = re.compile(r'^!\d+ = !\{i32 (\d+), %"class\.StructuredBuffer<VolumetricGPUParticle>"\* undef, '
                              r'!"", i32 0, i32 6, i32 1, i32 12, i32 0, !\d+\}$', re.M)
RE_RESOURCES = re.compile(r"^!dx\.resources = !\{!(\d+)\}$", re.M)


def smoke_edit(ll: str, source_hash: str) -> str:
    if f"; shader hash: {source_hash}" not in ll:
        raise SystemExit(f"not {source_hash}")
    # the particle buffer's handle
    m = RE_PARTICLE_META.findall(ll)
    if len(m) != 1:
        raise SystemExit("particle buffer declaration not found")
    hp = re.findall(rf"^  (%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 0, i32 {m[0]}, i32 6, "
                    r"i1 false\)", ll, re.M)
    if len(hp) != 1:
        raise SystemExit("particle buffer handle not found")
    # the fog constants' handle (cb6)
    hf = re.findall(r"^  (%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 \d+, i32 6, "
                    r"i1 false\)", ll, re.M)
    if len(hf) != 1:
        raise SystemExit("fog constants handle not found")

    # the six particle reads -> two cbuffer registers
    loads = list(re.finditer(rf"^  (%\d+) = call %dx\.types\.ResRet\.(f32|i32) @dx\.op\.bufferLoad\.\2\(i32 68, "
                             rf"%dx\.types\.Handle {hp[0]}, i32 (%\d+), i32 (\d+)\)[^\n]*\n"
                             r"  (%\d+) = extractvalue %dx\.types\.ResRet\.\2 \1, 0\n", ll, re.M))
    if sorted(int(x.group(4)) for x in loads) != [0, 4, 8, 12, 16, 20] or len({x.group(3) for x in loads}) != 1:
        raise SystemExit(f"particle reads not as expected ({[x.group(4) for x in loads]})")
    idx = loads[0].group(3)
    out, last = [], 0
    for k, x in enumerate(loads):
        off = int(x.group(4))
        reg, comp = ("%ncw.r0", off // 4) if off < 16 else ("%ncw.r1", (off - 16) // 4)
        t = x.group(2)
        new = ""
        if k == 0:
            new += f"  %ncw.b = shl i32 {idx}, 1\n  %ncw.r0 = add i32 %ncw.b, 1\n  %ncw.r1 = add i32 %ncw.b, 2\n"
        new += (f"  {x.group(1)} = call %dx.types.CBufRet.{t} @dx.op.cbufferLoadLegacy.{t}(i32 59, "
                f"%dx.types.Handle %ncw.h, i32 {reg})\n"
                f"  {x.group(5)} = extractvalue %dx.types.CBufRet.{t} {x.group(1)}, {comp}\n")
        out.append(ll[last:x.start()])
        out.append(new)
        last = x.end()
    out.append(ll[last:])
    ll = "".join(out)

    # the particle count (cb6 register 16 .z, read before and at the end of the loop) -> ours
    count = re.compile(rf"^  (%\d+) = call %dx\.types\.CBufRet\.f32 @dx\.op\.cbufferLoadLegacy\.f32\(i32 59, "
                       rf"%dx\.types\.Handle {hf[0]}, i32 16\)(  ;[^\n]*)?\n  (%\d+) = extractvalue "
                       r"%dx\.types\.CBufRet\.f32 \1, 2\n  (%\d+) = fptoui float \3 to i32\n", re.M)
    sites = count.findall(ll)
    if len(sites) != 2:
        raise SystemExit(f"particle count reads: {len(sites)}, expected 2")
    ll = count.sub(lambda c: c.group(0).replace(f"Handle {hf[0]}, i32 16)", "Handle %ncw.h, i32 0)", 1), ll)

    # our cbuffer: a type, a handle at the top of the entry, a CBV declaration at b3
    ll = replace_once(ll, TYPE_ANCHOR, SMOKE_TYPE + TYPE_ANCHOR, "type")
    first = re.search(r"^  %1 = call %dx\.types\.Handle @dx\.op\.createHandle\([^\n]*\n", ll, re.M)
    res = RE_RESOURCES.search(ll)
    lists = re.search(rf"^!{res.group(1)} = !\{{(!\d+|null), (!\d+|null), (!\d+), (!\d+|null)\}}$", ll, re.M)
    cbv_list = lists.group(3)
    cbv = re.search(rf"^{re.escape(cbv_list)} = !\{{([^}}]*)\}}$", ll, re.M)
    n_cbv = len(cbv.group(1).split(","))
    ll = (ll[:first.end()] +
          f"  %ncw.h = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 {n_cbv}, i32 3, i1 false)\n" +
          ll[first.end():])
    ll = ll.replace(cbv.group(0), f"{cbv_list} = !{{{cbv.group(1)}, !1000}}", 1)
    return (ll.rstrip("\n") + "\n" +
            f"!1000 = !{{i32 {n_cbv}, %hostlayout.NCWSmoke* undef, !\"\", i32 0, i32 3, i32 1, "
            f"i32 {SMOKE_REGS * 16}, null}}\n")


def run(*args):
    r = subprocess.run([sys.executable, "-I", *map(str, args)], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"{args[0]} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True, help="the Cyberpunk 2077 install (holds engine/)")
    ap.add_argument("--dxc", required=True, help="folder with dxc.exe, dxcompiler.dll, dxil.dll")
    ap.add_argument("--out", required=True, help="folder for the NCW*.hpp headers")
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        run(TOOLS / "rg_shader_cache.py", "extract", SHAPES, "--game", a.game, "--out", t)
        vanilla = t / f"{SHAPES}.dxil"
        run(TOOLS / "rg_dxil.py", "dis", vanilla, t / "vanilla.ll", "--dxc", a.dxc)
        ll = (t / "vanilla.ll").read_text(encoding="utf-8")
        (t / "patched.ll").write_text(edit(ll), encoding="utf-8")
        print(run(TOOLS / "rg_dxil.py", "asm", t / "patched.ll", t / "patched.dxil", "--dxc", a.dxc).strip())
        print(run(TOOLS / "rg_dxil.py", "header", t / "patched.dxil", out / "NCWFoliagePatch.hpp",
                  "--symbol", "FoliagePatch", "--namespace", "NCW::Shaders").strip())

        for target, source, symbol in SMOKE_PAIRS:
            run(TOOLS / "rg_shader_cache.py", "extract", source, "--game", a.game, "--out", t)
            run(TOOLS / "rg_dxil.py", "dis", t / f"{source}.dxil", t / f"{source}.ll", "--dxc", a.dxc)
            ll = (t / f"{source}.ll").read_text(encoding="utf-8")
            (t / f"{symbol}.ll").write_text(smoke_edit(ll, source), encoding="utf-8")
            print(run(TOOLS / "rg_dxil.py", "asm", t / f"{symbol}.ll", t / f"{symbol}.dxil", "--dxc", a.dxc).strip())
            print(run(TOOLS / "rg_dxil.py", "header", t / f"{symbol}.dxil", out / f"NCW{symbol}.hpp",
                      "--symbol", symbol, "--namespace", "NCW::Shaders").strip())


if __name__ == "__main__":
    main()
